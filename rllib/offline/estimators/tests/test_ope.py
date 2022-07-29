import copy
import os
import unittest
from pathlib import Path
from typing import Type, Union, Dict

import numpy as np
from ray.data import read_json
from ray.rllib.algorithms import AlgorithmConfig
from ray.rllib.algorithms.dqn import DQNConfig
from ray.rllib.evaluation.worker_set import WorkerSet
from ray.rllib.execution.rollout_ops import synchronous_parallel_sample
from ray.rllib.offline.dataset_reader import DatasetReader
from ray.rllib.offline.estimators import (
    DirectMethod,
    DoublyRobust,
    ImportanceSampling,
    WeightedImportanceSampling,
)
from ray.rllib.offline.estimators.fqe_torch_model import FQETorchModel
from ray.rllib.offline.estimators.tests.gridworld import GridWorldEnv, GridWorldPolicy
from ray.rllib.policy import Policy
from ray.rllib.policy.sample_batch import SampleBatch, concat_samples
from ray.rllib.utils.framework import try_import_torch
from ray.rllib.utils.numpy import convert_to_numpy
from ray.rllib.utils.test_utils import check

import ray

torch, _ = try_import_torch()


class TestOPE(unittest.TestCase):
    """Compilation tests for using OPE both standalone and in an RLlib Algorithm"""

    @classmethod
    def setUpClass(cls):
        ray.init()
        rllib_dir = Path(__file__).parent.parent.parent.parent
        train_data = os.path.join(rllib_dir, "tests/data/cartpole/small.json")
        eval_data = train_data

        env_name = "CartPole-v0"
        cls.gamma = 0.99
        n_episodes = 3
        cls.q_model_config = {"n_iters": 160}

        config = (
            DQNConfig()
            .environment(env=env_name)
            .training(gamma=cls.gamma)
            .rollouts(num_rollout_workers=3, batch_mode="complete_episodes")
            .framework("torch")
            .resources(num_gpus=int(os.environ.get("RLLIB_NUM_GPUS", 0)))
            .offline_data(input_=train_data)
            .evaluation(
                evaluation_interval=1,
                evaluation_duration=n_episodes,
                evaluation_num_workers=1,
                evaluation_duration_unit="episodes",
                evaluation_config={"input": eval_data},
                off_policy_estimation_methods={
                    "is": {"type": ImportanceSampling},
                    "wis": {"type": WeightedImportanceSampling},
                    "dm_fqe": {"type": DirectMethod},
                    "dr_fqe": {"type": DoublyRobust},
                },
            )
        )
        cls.algo = config.build()

        # Read n_episodes of data, assuming that one line is one episode
        reader = DatasetReader(read_json(eval_data))
        batches = [reader.next() for _ in range(n_episodes)]
        cls.batch = concat_samples(batches)
        cls.n_episodes = len(cls.batch.split_by_episode())
        print("Episodes:", cls.n_episodes, "Steps:", cls.batch.count)

    @classmethod
    def tearDownClass(cls):
        ray.shutdown()

    def test_ope_standalone(self):
        # Test all OPE methods standalone
        estimator = ImportanceSampling(
            policy=self.algo.get_policy(),
            gamma=self.gamma,
        )
        estimates = estimator.estimate(self.batch)
        assert estimates is not None, "IS estimator did not compute estimates"

        estimator = WeightedImportanceSampling(
            policy=self.algo.get_policy(),
            gamma=self.gamma,
        )
        estimates = estimator.estimate(self.batch)
        assert estimates is not None, "WIS estimator did not compute estimates"

        estimator = DirectMethod(
            policy=self.algo.get_policy(),
            gamma=self.gamma,
            q_model_config=self.q_model_config,
        )
        losses = estimator.train(self.batch)
        assert losses, "DM estimator did not return mean loss"
        estimates = estimator.estimate(self.batch)
        assert estimates is not None, "DM estimator did not compute estimates"

        estimator = DoublyRobust(
            policy=self.algo.get_policy(),
            gamma=self.gamma,
            q_model_config=self.q_model_config,
        )
        losses = estimator.train(self.batch)
        assert losses, "DM estimator did not return mean loss"
        estimates = estimator.estimate(self.batch)
        assert estimates is not None, "DM estimator did not compute estimates"

    def test_ope_in_algo(self):
        # Test OPE in DQN, during training as well as by calling evaluate()
        ope_results = self.algo.train()["evaluation"]["off_policy_estimator"]
        # Check that key exists AND is not {}
        assert ope_results, "Did not run OPE in training!"
        assert set(ope_results.keys()) == {
            "is",
            "wis",
            "dm_fqe",
            "dr_fqe",
        }, "Missing keys in OPE result dict"

        # Check algo.evaluate() manually as well
        ope_results = self.algo.evaluate()["evaluation"]["off_policy_estimator"]
        assert ope_results, "Did not run OPE on call to Algorithm.evaluate()!"
        assert set(ope_results.keys()) == {
            "is",
            "wis",
            "dm_fqe",
            "dr_fqe",
        }, "Missing keys in OPE result dict"


class TestFQE(unittest.TestCase):
    """Compilation and learning tests for the Fitted-Q Evaluation model"""

    @classmethod
    def setUpClass(cls) -> None:
        ray.init()
        env = GridWorldEnv()
        cls.policy = GridWorldPolicy(
            observation_space=env.observation_space,
            action_space=env.action_space,
            config={},
        )
        cls.gamma = 0.99
        # Collect single episode under optimal policy
        obs_batch = []
        new_obs = []
        actions = []
        action_prob = []
        rewards = []
        dones = []
        obs = env.reset()
        done = False
        while not done:
            obs_batch.append(obs)
            act, _, extra = cls.policy.compute_single_action(obs)
            actions.append(act)
            action_prob.append(extra["action_prob"])
            obs, rew, done, _ = env.step(act)
            new_obs.append(obs)
            rewards.append(rew)
            dones.append(done)
        cls.batch = SampleBatch(
            obs=obs_batch,
            actions=actions,
            action_prob=action_prob,
            rewards=rewards,
            dones=dones,
            new_obs=new_obs,
        )

    @classmethod
    def tearDownClass(cls) -> None:
        ray.shutdown()

    def test_fqe_compilation_and_stopping(self):
        # Test FQETorchModel for:
        # (1) Check that it does not modify the underlying batch during training
        # (2) Check that the stopping criteria from FQE are working correctly
        # (3) Check that using fqe._compute_action_probs equals brute force
        # iterating over all actions with policy.compute_log_likelihoods
        fqe = FQETorchModel(
            policy=self.policy,
            gamma=self.gamma,
        )
        tmp_batch = copy.deepcopy(self.batch)
        losses = fqe.train(self.batch)

        # Make sure FQETorchModel.train() does not modify the batch
        check(tmp_batch, self.batch)

        # Make sure FQE stopping criteria are respected
        assert (
            len(losses) == fqe.n_iters or losses[-1] < fqe.delta
        ), f"FQE.train() terminated early in {len(losses)} steps with final loss"
        f"{losses[-1]} for n_iters: {fqe.n_iters} and delta: {fqe.delta}"

        # Test fqe._compute_action_probs against "brute force" method
        # of computing log_prob for each possible action individually
        # using policy.compute_log_likelihoods
        obs = torch.tensor(self.batch["obs"], device=fqe.device)
        action_probs = fqe._compute_action_probs(obs)
        action_probs = convert_to_numpy(action_probs)

        tmp_probs = []
        for act in range(fqe.policy.action_space.n):
            tmp_actions = np.zeros_like(self.batch["actions"]) + act
            log_probs = self.policy.compute_log_likelihoods(
                actions=tmp_actions,
                obs_batch=self.batch["obs"],
            )
            tmp_probs.append(np.exp(log_probs))
        tmp_probs = np.stack(tmp_probs).T
        check(action_probs, tmp_probs, decimals=3)

    def test_fqe_optimal_convergence(self):
        # Optimal GridWorldPolicy with epsilon = 0.0 and GridWorldEnv are deterministic;
        # check that FQE converges to the true Q-values for self.batch
        q_vals = [
            -2.50,
            -1.51,
            -0.52,
            0.49,
            1.50,
            2.53,
            3.56,
            4.61,
            5.67,
            6.73,
            7.81,
            8.90,
            10.00,
        ]
        q_model_config = {
            "tau": 1.0,
            "model": {
                "fcnet_hiddens": [],
                "activation": "linear",
            },
            "lr": 0.01,
            "n_iters": 5000,
            "delta": 1e-3,
        }

        fqe = FQETorchModel(
            policy=self.policy,
            gamma=self.gamma,
            **q_model_config,
        )
        losses = fqe.train(self.batch)
        print(losses[-10:])
        assert losses[-1] < fqe.delta, "FQE loss did not converge!"
        estimates = fqe.estimate_v(self.batch)
        print(estimates)
        check(
            estimates,
            q_vals,
            decimals=1,
        )


def get_policy_batch_and_mean_std_ret(
    num_episodes: int,
    gamma: float,
    epsilon: float,
) -> (Policy, SampleBatch, float, float):
    """Return a GridWorld policy, SampleBatch, and the mean and stddev of the
    discounted episode returns over the batch.
    """
    config = (
        AlgorithmConfig()
        .rollouts(batch_mode="complete_episodes")
        .environment(disable_env_checking=True)
        .experimental(_disable_preprocessor_api=True)
        .to_dict()
    )

    env = GridWorldEnv()
    policy = GridWorldPolicy(
        env.observation_space, env.action_space, {"epsilon": epsilon}
    )
    workers = WorkerSet(
        env_creator=lambda env_config: GridWorldEnv(),
        policy_class=GridWorldPolicy,
        trainer_config=config,
        num_workers=8,
    )
    workers.foreach_policy(func=lambda policy, _: policy.update_epsilon(epsilon))
    ep_ret = []
    batches = []
    n_eps = 0
    while n_eps < num_episodes:
        batch = synchronous_parallel_sample(worker_set=workers)
        for episode in batch.split_by_episode():
            ret = 0
            for r in episode[SampleBatch.REWARDS][::-1]:
                ret = r + gamma * ret
            ep_ret.append(ret)
            n_eps += 1
        batches.append(batch)
    workers.stop()
    return policy, concat_samples(batches), np.mean(ep_ret), np.std(ep_ret)


def check_estimate(
    estimator_cls: Type[Union[DirectMethod, DoublyRobust]],
    gamma: float,
    q_model_config: Dict,
    policy: Policy,
    batch: SampleBatch,
    mean_ret: float,
    std_ret: float,
):
    # Train and estimate an estimator using the given batch and policy.
    # Assert that the 1 stddev intervals for the estimated mean return
    # and the actual mean return overlap.
    estimator = estimator_cls(
        policy=policy,
        gamma=gamma,
        q_model_config=q_model_config,
    )
    loss = estimator.train(batch)["loss"]
    estimates = estimator.estimate(batch)
    est_mean = estimates["v_target"]
    est_std = estimates["v_target_std"]
    print(f"{est_mean:.2f}, {est_std:.2f}, {mean_ret:.2f}, {std_ret:.2f}, {loss:.2f}")
    # Assert that the two mean +- stddev intervals overlap
    assert (
        est_mean - est_std <= mean_ret + std_ret
        and mean_ret - std_ret <= est_mean + est_std
    ), (
        f"DirectMethod estimate {est_mean:.2f} with stddev "
        f"{est_std:.2f} does not converge to true discounted return "
        f"{mean_ret:.2f} with stddev {std_ret:.2f}!"
    )


class TestOPELearning(unittest.TestCase):
    """Learning tests for the DirectMethod and DoublyRobust estimators"""

    @classmethod
    def setUpClass(cls):
        ray.init()
        # Epsilon-greedy exploration values
        random_eps = 0.8
        mixed_eps = 0.5
        expert_eps = 0.2
        num_episodes = 32
        cls.gamma = 0.99

        # Config settings for FQE model
        cls.q_model_config = {
            "n_iters": 600,
            "minibatch_size": 32,
            "tau": 1.0,
            "model": {
                "fcnet_hiddens": [],
                "activation": "linear",
            },
            "lr": 0.01,
        }

        (
            cls.random_policy,
            cls.random_batch,
            cls.random_reward,
            cls.random_std,
        ) = get_policy_batch_and_mean_std_ret(num_episodes, cls.gamma, random_eps)
        print(
            f"Collected random batch of {cls.random_batch.count} steps "
            f"with return {cls.random_reward} stddev {cls.random_std}"
        )

        (
            cls.mixed_policy,
            cls.mixed_batch,
            cls.mixed_reward,
            cls.mixed_std,
        ) = get_policy_batch_and_mean_std_ret(num_episodes, cls.gamma, mixed_eps)
        print(
            f"Collected mixed batch of {cls.mixed_batch.count} steps "
            f"with return {cls.mixed_reward} stddev {cls.mixed_std}"
        )

        (
            cls.expert_policy,
            cls.expert_batch,
            cls.expert_reward,
            cls.expert_std,
        ) = get_policy_batch_and_mean_std_ret(num_episodes, cls.gamma, expert_eps)
        print(
            f"Collected expert batch of {cls.expert_batch.count} steps "
            f"with return {cls.expert_reward} stddev {cls.expert_std}"
        )

    @classmethod
    def tearDownClass(cls):
        ray.shutdown()

    def test_dm_random_policy_random_data(self):
        print("Test DirectMethod on random policy on random dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.random_batch,
            self.random_reward,
            self.random_std,
        )

    def test_dm_random_policy_mixed_data(self):
        print("Test DirectMethod on random policy on mixed dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.mixed_batch,
            self.random_reward,
            self.random_std,
        )

    @unittest.skip(
        "Skipped out due to flakiness; makes sense since expert episodes"
        "are shorter than random ones, increasing the variance of the estimate"
    )
    def test_dm_random_policy_expert_data(self):
        print("Test DirectMethod on random policy on expert dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.expert_batch,
            self.random_reward,
            self.random_std,
        )

    def test_dm_mixed_policy_random_data(self):
        print("Test DirectMethod on mixed policy on random dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.random_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dm_mixed_policy_mixed_data(self):
        print("Test DirectMethod on mixed policy on mixed dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.mixed_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dm_mixed_policy_expert_data(self):
        print("Test DirectMethod on mixed policy on expert dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.expert_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dm_expert_policy_random_data(self):
        print("Test DirectMethod on expert policy on random dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.random_batch,
            self.expert_reward,
            self.expert_std,
        )

    def test_dm_expert_policy_mixed_data(self):
        print("Test DirectMethod on expert policy on mixed dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.mixed_batch,
            self.expert_reward,
            self.expert_std,
        )

    def test_dm_expert_policy_expert_data(self):
        print("Test DirectMethod on expert policy on expert dataset")
        check_estimate(
            DirectMethod,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.expert_batch,
            self.expert_reward,
            self.expert_std,
        )

    def test_dr_random_policy_random_data(self):
        print("Test DoublyRobust on random policy on random dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.random_batch,
            self.random_reward,
            self.random_std,
        )

    def test_dr_random_policy_mixed_data(self):
        print("Test DoublyRobust on random policy on mixed dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.mixed_batch,
            self.random_reward,
            self.random_std,
        )

    @unittest.skip(
        "Skipped out due to flakiness; makes sense since expert episodes"
        "are shorter than random ones, increasing the variance of the estimate"
    )
    def test_dr_random_policy_expert_data(self):
        print("Test DoublyRobust on  random policy on expert dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.random_policy,
            self.expert_batch,
            self.random_reward,
            self.random_std,
        )

    def test_dr_mixed_policy_random_data(self):
        print("Test DoublyRobust on  mixed policy on random dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.random_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dr_mixed_policy_mixed_data(self):
        print("Test DoublyRobust on  mixed policy on mixed dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.mixed_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dr_mixed_policy_expert_data(self):
        print("Test DoublyRobust on  mixed policy on expert dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.mixed_policy,
            self.expert_batch,
            self.mixed_reward,
            self.mixed_std,
        )

    def test_dr_expert_policy_random_data(self):
        print("Test DoublyRobust on  expert policy on random dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.random_batch,
            self.expert_reward,
            self.expert_std,
        )

    def test_dr_expert_policy_mixed_data(self):
        print("Test DoublyRobust on  expert policy on mixed dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.mixed_batch,
            self.expert_reward,
            self.expert_std,
        )

    def test_dr_expert_policy_expert_data(self):
        print("Test DoublyRobust on  expert policy on expert dataset")
        check_estimate(
            DoublyRobust,
            self.gamma,
            self.q_model_config,
            self.expert_policy,
            self.expert_batch,
            self.expert_reward,
            self.expert_std,
        )


if __name__ == "__main__":
    import sys

    import pytest

    sys.exit(pytest.main(["-v", __file__]))
