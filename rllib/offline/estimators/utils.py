from typing import Callable
from ray.rllib.policy import Policy
from ray.rllib.policy.sample_batch import SampleBatch
from ray.rllib.utils.typing import TensorType
import logging
from ray.rllib.utils.numpy import convert_to_numpy
from ray.rllib.utils.torch_utils import convert_to_torch_tensor
import numpy as np
from functools import partial

from gym.spaces import Discrete, Box

logger = logging.getLogger(__name__)


def action_log_likelihood(
    policy: Policy, batch: SampleBatch, actions_normalized=True
) -> TensorType:
    """Returns log likelihood for actions in given batch for policy.

    Computes likelihoods by passing the observations through the current
    policy's `compute_log_likelihoods()` method

    Args:
        policy: Policy to compute actions for
        batch: The SampleBatch or MultiAgentBatch to calculate action
            log likelihoods from. This batch/batches must contain OBS
            and ACTIONS keys.
        actions_normalized: Whether the actions are normalized between [-1,1].
            This is usually True for batches generated by an RLlib algorithm.

    Returns:
        The probabilities of the actions in the batch, given the
        observations and the policy.
    """
    num_state_inputs = 0
    for k in batch.keys():
        if k.startswith("state_in_"):
            num_state_inputs += 1
    state_keys = ["state_in_{}".format(i) for i in range(num_state_inputs)]
    log_likelihoods: TensorType = policy.compute_log_likelihoods(
        actions=batch[SampleBatch.ACTIONS],
        obs_batch=batch[SampleBatch.OBS],
        state_batches=[batch[k] for k in state_keys],
        prev_action_batch=batch.get(SampleBatch.PREV_ACTIONS),
        prev_reward_batch=batch.get(SampleBatch.PREV_REWARDS),
        actions_normalized=actions_normalized,
    )
    log_likelihoods = convert_to_numpy(log_likelihoods)
    return log_likelihoods


def lookup_action_value_fn(
    policy: Policy,
) -> Callable[[Policy, SampleBatch], TensorType]:
    action_value_fn = None
    algo_name = policy.config["algo_class"].__name__

    if algo_name in ["DQN", "ApexDQN"]:
        if policy.config["framework"] == "torch":
            from ray.rllib.algorithms.dqn.dqn_torch_policy import compute_q_values
        elif policy.config["framework"] in ["tf", "tf1", "tf2", "tfe"]:
            from ray.rllib.algorithms.dqn.dqn_tf_policy import compute_q_values

        def dqn_action_value_fn(policy: Policy, batch: SampleBatch) -> TensorType:
            if policy.config["framework"] == "torch":
                batch = convert_to_torch_tensor(batch, policy.device)
            i = 0
            state_batches = []
            while "state_in_{}".format(i) in batch:
                state_batches.append(batch["state_in_{}".format(i)])
                i += 1
            q_values = compute_q_values(
                policy,
                policy.model,
                batch,
                state_batches,
                batch.get(SampleBatch.SEQ_LENS),
            )[0]
            if SampleBatch.ACTIONS in batch:
                q_values = convert_to_numpy(q_values)
                actions = convert_to_numpy(batch[SampleBatch.ACTIONS])
                q_values = np.squeeze(
                    np.take_along_axis(q_values, np.expand_dims(actions, -1), axis=-1),
                    -1,
                )
            return q_values

        action_value_fn = dqn_action_value_fn

    elif algo_name in ["CRR", "CQL", "DDPG", "ApexDDPG", "TD3"]:

        def modelv2_action_value_fn(policy: Policy, batch: SampleBatch) -> TensorType:
            if policy.config["framework"] == "torch":
                batch = convert_to_torch_tensor(batch, policy.device)
            model_out, _ = policy.model(batch)
            q_values = policy.model.get_q_values(model_out, batch[SampleBatch.ACTIONS])
            q_values = convert_to_numpy(q_values).reshape([batch.count])
            return q_values

        action_value_fn = modelv2_action_value_fn

    elif algo_name in ["SAC", "CQL"]:

        def sac_action_value_fn(policy: Policy, batch: SampleBatch) -> TensorType:
            if policy.config["framework"] == "torch":
                batch = convert_to_torch_tensor(batch, policy.device)
            model_out, _ = policy.model(batch)
            if isinstance(policy.action_space, Discrete):
                q_values = policy.model.get_q_values(model_out)[0]
                if SampleBatch.ACTIONS in batch:
                    q_values = convert_to_numpy(q_values)
                    actions = convert_to_numpy(batch[SampleBatch.ACTIONS])
                    q_values = np.squeeze(
                        np.take_along_axis(
                            q_values, np.expand_dims(actions, -1), axis=-1
                        ),
                        -1,
                    )
            elif isinstance(policy.action_space, Box):
                q_values = policy.model.get_q_values(
                    model_out, batch[SampleBatch.ACTIONS]
                )
                q_values = convert_to_numpy(q_values).reshape([batch.count])
            return q_values

        action_value_fn = sac_action_value_fn

    elif algo_name == "SimpleQ":

        def simpleq_action_value_fn(policy: Policy, batch: SampleBatch) -> TensorType:
            q_values = policy._compute_q_values(policy.model, batch["obs"])
            if SampleBatch.ACTIONS in batch:
                q_values = convert_to_numpy(q_values)
                actions = convert_to_numpy(batch[SampleBatch.ACTIONS])
                q_values = np.squeeze(
                    np.take_along_axis(q_values, np.expand_dims(actions, -1), axis=-1),
                    -1,
                )
            return q_values

        action_value_fn = simpleq_action_value_fn

    if not action_value_fn:
        raise ValueError("Could not find action_value_fn for policy:", str(policy))
    return action_value_fn


def _discrete_state_value_fn(policy: Policy, batch: SampleBatch) -> TensorType:
    action_value_fn = lookup_action_value_fn(policy)
    tmp_batch = batch.copy(shallow=True)
    action_probs = []
    q_values = []
    for i in range(policy.action_space.n):
        tmp_batch[SampleBatch.ACTIONS] = np.zeros_like(batch[SampleBatch.ACTIONS]) + i
        tmp_probs = np.exp(action_log_likelihood(policy, tmp_batch))
        action_probs.append(tmp_probs)
        tmp_q = action_value_fn(policy, tmp_batch)
        q_values.append(convert_to_numpy(tmp_q))
    action_probs = np.swapaxes(action_probs, 0, 1)
    q_values = np.swapaxes(q_values, 0, 1)
    v_values = np.sum(q_values * action_probs, axis=-1)
    return v_values


def _sampling_state_value_fn(
    policy: Policy, batch: SampleBatch, n_samples=128
) -> TensorType:
    action_value_fn = lookup_action_value_fn(policy)
    tmp_batch = batch.copy(shallow=True)
    action_probs = []
    q_values = []
    for _ in range(n_samples):
        tmp_batch[SampleBatch.ACTIONS] = np.array(
            [policy.action_space.sample() for _ in range(batch.count)]
        )
        tmp_probs = np.exp(action_log_likelihood(policy, tmp_batch))
        action_probs.append(tmp_probs)
        tmp_q = action_value_fn(policy, tmp_batch)
        q_values.append(convert_to_numpy(tmp_q))
    action_probs = np.swapaxes(action_probs, 0, 1)
    q_values = np.swapaxes(q_values, 0, 1)
    v_values = np.sum(q_values * action_probs, axis=-1)
    return v_values


def lookup_state_value_fn(
    policy: Policy, n_samples=128
) -> Callable[[Policy, SampleBatch], TensorType]:
    state_value_fn = None

    if isinstance(policy.action_space, Discrete):
        # If state_value_fn was not found, but the action space is discrete
        # try to sum over all possible actions using action_value_fn
        logger.log(
            0, "Using action_value_fn to infer state_value_fn for Discrete action space"
        )
        state_value_fn = _discrete_state_value_fn
    elif isinstance(policy.action_space, Box):
        logger.log(
            0, "Using action_value_fn to infer state_value_fn for Box action space"
        )
        state_value_fn = partial(_sampling_state_value_fn, n_samples=n_samples)

    if not state_value_fn:
        raise ValueError("Could not find state_value_fn for policy:", str(policy))
    return state_value_fn
