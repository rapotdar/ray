// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/core_worker/transport/direct_actor_task_submitter.h"

#include <thread>

#include "ray/common/task/task.h"
#include "ray/gcs/pb_util.h"

using ray::rpc::ActorTableData;
using namespace ray::gcs;

namespace ray {
namespace core {

void CoreWorkerDirectActorTaskSubmitter::AddActorQueueIfNotExists(
    const ActorID &actor_id,
    int32_t max_pending_calls,
    bool execute_out_of_order,
    bool fail_if_actor_unreachable) {
  absl::MutexLock lock(&mu_);
  // No need to check whether the insert was successful, since it is possible
  // for this worker to have multiple references to the same actor.
  RAY_LOG(INFO) << "Set max pending calls to " << max_pending_calls << " for actor "
                << actor_id;
  client_queues_.emplace(
      actor_id,
      ClientQueue(
          actor_id, execute_out_of_order, max_pending_calls, fail_if_actor_unreachable));
}

void CoreWorkerDirectActorTaskSubmitter::KillActor(const ActorID &actor_id,
                                                   bool force_kill,
                                                   bool no_restart) {
  absl::MutexLock lock(&mu_);
  rpc::KillActorRequest request;
  request.set_intended_actor_id(actor_id.Binary());
  request.set_force_kill(force_kill);
  request.set_no_restart(no_restart);

  auto it = client_queues_.find(actor_id);
  // The language frontend can only kill actors that it has a reference to.
  RAY_CHECK(it != client_queues_.end());

  if (!it->second.pending_force_kill) {
    it->second.pending_force_kill = request;
  } else if (force_kill) {
    // Overwrite the previous request to kill the actor if the new request is a
    // force kill.
    it->second.pending_force_kill->set_force_kill(true);
    if (no_restart) {
      // Overwrite the previous request to disable restart if the new request's
      // no_restart flag is set to true.
      it->second.pending_force_kill->set_no_restart(true);
    }
  }

  SendPendingTasks(actor_id);
}

Status CoreWorkerDirectActorTaskSubmitter::SubmitTask(TaskSpecification task_spec) {
  auto task_id = task_spec.TaskId();
  auto actor_id = task_spec.ActorId();
  RAY_LOG(DEBUG) << "Submitting task " << task_id;
  RAY_CHECK(task_spec.IsActorTask());

  bool task_queued = false;
  uint64_t send_pos = 0;
  {
    absl::MutexLock lock(&mu_);
    auto queue = client_queues_.find(actor_id);
    RAY_CHECK(queue != client_queues_.end());
    if (queue->second.state != rpc::ActorTableData::DEAD) {
      // We must fix the send order prior to resolving dependencies, which may
      // complete out of order. This ensures that we will not deadlock due to
      // backpressure. The receiving actor will execute the tasks according to
      // this sequence number.
      send_pos = task_spec.ActorCounter();
      RAY_CHECK(queue->second.actor_submit_queue->Emplace(send_pos, task_spec));
      queue->second.cur_pending_calls++;
      task_queued = true;
    }
  }

  if (task_queued) {
    io_service_.post(
        [task_spec, send_pos, this]() mutable {
          // We must release the lock before resolving the task dependencies since
          // the callback may get called in the same call stack.
          auto actor_id = task_spec.ActorId();
          auto task_id = task_spec.TaskId();
          resolver_.ResolveDependencies(
              task_spec, [this, send_pos, actor_id, task_id](Status status) {
                task_finisher_.MarkDependenciesResolved(task_id);
                auto fail_or_retry_task = TaskID::Nil();
                {
                  absl::MutexLock lock(&mu_);
                  auto queue = client_queues_.find(actor_id);
                  RAY_CHECK(queue != client_queues_.end());
                  auto &actor_submit_queue = queue->second.actor_submit_queue;
                  // Only dispatch tasks if the submitted task is still queued. The task
                  // may have been dequeued if the actor has since failed.
                  if (actor_submit_queue->Contains(send_pos)) {
                    if (status.ok()) {
                      actor_submit_queue->MarkDependencyResolved(send_pos);
                      SendPendingTasks(actor_id);
                    } else {
                      fail_or_retry_task =
                          actor_submit_queue->Get(send_pos).first.TaskId();
                      actor_submit_queue->MarkDependencyFailed(send_pos);
                    }
                  }
                }

                if (!fail_or_retry_task.IsNil()) {
                  GetTaskFinisherWithoutMu().FailOrRetryPendingTask(
                      task_id, rpc::ErrorType::DEPENDENCY_RESOLUTION_FAILED, &status);
                }
              });
        },
        "CoreWorkerDirectActorTaskSubmitter::SubmitTask");
  } else {
    // Do not hold the lock while calling into task_finisher_.
    task_finisher_.MarkTaskCanceled(task_id);
    rpc::ErrorType error_type;
    rpc::RayErrorInfo error_info;
    {
      absl::MutexLock lock(&mu_);
      const auto queue_it = client_queues_.find(task_spec.ActorId());
      const auto &death_cause = queue_it->second.death_cause;
      error_info = GetErrorInfoFromActorDeathCause(death_cause);
      error_type = error_info.error_type();
    }
    auto status = Status::IOError("cancelling task of dead actor");
    // No need to increment the number of completed tasks since the actor is
    // dead.
    bool fail_immediatedly =
        error_info.has_actor_died_error() &&
        error_info.actor_died_error().has_oom_context() &&
        error_info.actor_died_error().oom_context().fail_immediately();
    GetTaskFinisherWithoutMu().FailOrRetryPendingTask(task_id,
                                                      error_type,
                                                      &status,
                                                      &error_info,
                                                      /*mark_task_object_failed*/ true,
                                                      fail_immediatedly);
  }

  // If the task submission subsequently fails, then the client will receive
  // the error in a callback.
  return Status::OK();
}

void CoreWorkerDirectActorTaskSubmitter::DisconnectRpcClient(ClientQueue &queue) {
  queue.rpc_client = nullptr;
  core_worker_client_pool_.Disconnect(WorkerID::FromBinary(queue.worker_id));
  queue.worker_id.clear();
  queue.pending_force_kill.reset();
}

void CoreWorkerDirectActorTaskSubmitter::FailInflightTasks(
    const absl::flat_hash_map<TaskID, rpc::ClientCallback<rpc::PushTaskReply>>
        &inflight_task_callbacks) {
  // NOTE(kfstorm): We invoke the callbacks with a bad status to act like there's a
  // network issue. We don't call `task_finisher_.FailOrRetryPendingTask` directly because
  // there's much more work to do in the callback.
  auto status = Status::IOError("Fail all inflight tasks due to actor state change.");
  rpc::PushTaskReply reply;
  for (const auto &[_, callback] : inflight_task_callbacks) {
    callback(status, reply);
  }
}

void CoreWorkerDirectActorTaskSubmitter::ConnectActor(const ActorID &actor_id,
                                                      const rpc::Address &address,
                                                      int64_t num_restarts) {
  RAY_LOG(DEBUG) << "Connecting to actor " << actor_id << " at worker "
                 << WorkerID::FromBinary(address.worker_id());

  absl::flat_hash_map<TaskID, rpc::ClientCallback<rpc::PushTaskReply>>
      inflight_task_callbacks;

  {
    absl::MutexLock lock(&mu_);

    auto queue = client_queues_.find(actor_id);
    RAY_CHECK(queue != client_queues_.end());
    if (num_restarts < queue->second.num_restarts) {
      // This message is about an old version of the actor and the actor has
      // already restarted since then. Skip the connection.
      RAY_LOG(INFO) << "Skip actor connection that has already been restarted, actor_id="
                    << actor_id;
      return;
    }

    if (queue->second.rpc_client &&
        queue->second.rpc_client->Addr().ip_address() == address.ip_address() &&
        queue->second.rpc_client->Addr().port() == address.port()) {
      RAY_LOG(DEBUG) << "Skip actor that has already been connected, actor_id="
                     << actor_id;
      return;
    }

    if (queue->second.state == rpc::ActorTableData::DEAD) {
      // This message is about an old version of the actor and the actor has
      // already died since then. Skip the connection.
      return;
    }

    queue->second.num_restarts = num_restarts;
    if (queue->second.rpc_client) {
      // Clear the client to the old version of the actor.
      DisconnectRpcClient(queue->second);
      inflight_task_callbacks = std::move(queue->second.inflight_task_callbacks);
      queue->second.inflight_task_callbacks.clear();
    }

    queue->second.state = rpc::ActorTableData::ALIVE;
    // Update the mapping so new RPCs go out with the right intended worker id.
    queue->second.worker_id = address.worker_id();
    // Create a new connection to the actor.
    queue->second.rpc_client = core_worker_client_pool_.GetOrConnect(address);
    queue->second.actor_submit_queue->OnClientConnected();

    RAY_LOG(INFO) << "Connecting to actor " << actor_id << " at worker "
                  << WorkerID::FromBinary(address.worker_id());
    ResendOutOfOrderTasks(actor_id);
    SendPendingTasks(actor_id);
  }

  // NOTE(kfstorm): We need to make sure the lock is released before invoking callbacks.
  FailInflightTasks(inflight_task_callbacks);
}

void CoreWorkerDirectActorTaskSubmitter::DisconnectActor(
    const ActorID &actor_id,
    int64_t num_restarts,
    bool dead,
    const rpc::ActorDeathCause &death_cause) {
  RAY_LOG(DEBUG) << "Disconnecting from actor " << actor_id
                 << ", death context type=" << GetActorDeathCauseString(death_cause);

  absl::flat_hash_map<TaskID, rpc::ClientCallback<rpc::PushTaskReply>>
      inflight_task_callbacks;
  std::deque<std::pair<int64_t, std::pair<TaskSpecification, Status>>>
      wait_for_death_info_tasks;
  std::vector<TaskID> task_ids_to_fail;
  {
    absl::MutexLock lock(&mu_);
    auto queue = client_queues_.find(actor_id);
    RAY_CHECK(queue != client_queues_.end());
    if (!dead) {
      RAY_CHECK(num_restarts > 0);
    }
    if (num_restarts <= queue->second.num_restarts && !dead) {
      // This message is about an old version of the actor that has already been
      // restarted successfully. Skip the message handling.
      RAY_LOG(INFO)
          << "Skip actor disconnection that has already been restarted, actor_id="
          << actor_id;
      return;
    }

    // The actor failed, so erase the client for now. Either the actor is
    // permanently dead or the new client will be inserted once the actor is
    // restarted.
    DisconnectRpcClient(queue->second);
    inflight_task_callbacks = std::move(queue->second.inflight_task_callbacks);
    queue->second.inflight_task_callbacks.clear();

    if (dead) {
      queue->second.state = rpc::ActorTableData::DEAD;
      queue->second.death_cause = death_cause;
      // If there are pending requests, treat the pending tasks as failed.
      RAY_LOG(INFO) << "Failing pending tasks for actor " << actor_id
                    << " because the actor is already dead.";

      task_ids_to_fail = queue->second.actor_submit_queue->ClearAllTasks();
      // We need to execute this outside of the lock to prevent deadlock.
      wait_for_death_info_tasks = std::move(queue->second.wait_for_death_info_tasks);
      // Reset the queue
      queue->second.wait_for_death_info_tasks =
          std::deque<std::pair<int64_t, std::pair<TaskSpecification, Status>>>();
    } else if (queue->second.state != rpc::ActorTableData::DEAD) {
      // Only update the actor's state if it is not permanently dead. The actor
      // will eventually get restarted or marked as permanently dead.
      queue->second.state = rpc::ActorTableData::RESTARTING;
      queue->second.num_restarts = num_restarts;
    }
  }

  if (task_ids_to_fail.size() + wait_for_death_info_tasks.size() != 0) {
    // Failing tasks has to be done without mu_ hold because the callback
    // might require holding mu_ which will lead to a deadlock.
    auto status = Status::IOError("cancelling all pending tasks of dead actor");
    const auto error_info = GetErrorInfoFromActorDeathCause(death_cause);
    const auto error_type = error_info.error_type();

    for (auto &task_id : task_ids_to_fail) {
      // No need to increment the number of completed tasks since the actor is
      // dead.
      task_finisher_.MarkTaskCanceled(task_id);
      // This task may have been waiting for dependency resolution, so cancel
      // this first.
      resolver_.CancelDependencyResolution(task_id);
      bool fail_immediatedly =
          error_info.has_actor_died_error() &&
          error_info.actor_died_error().has_oom_context() &&
          error_info.actor_died_error().oom_context().fail_immediately();
      GetTaskFinisherWithoutMu().FailOrRetryPendingTask(task_id,
                                                        error_type,
                                                        &status,
                                                        &error_info,
                                                        /*mark_task_object_failed*/ true,
                                                        fail_immediatedly);
    }
    if (!wait_for_death_info_tasks.empty()) {
      RAY_LOG(DEBUG) << "Failing tasks waiting for death info, size="
                     << wait_for_death_info_tasks.size() << ", actor_id=" << actor_id;
      for (auto &net_err_task_pair : wait_for_death_info_tasks) {
        RAY_UNUSED(GetTaskFinisherWithoutMu().FailPendingTask(
            net_err_task_pair.second.first.TaskId(),
            error_type,
            /* status */ &net_err_task_pair.second.second,
            &error_info));
      }
    }
  }
  // NOTE(kfstorm): We need to make sure the lock is released before invoking callbacks.
  FailInflightTasks(inflight_task_callbacks);
}

void CoreWorkerDirectActorTaskSubmitter::FailTaskWithError(const TaskInfo &task_info) {
  rpc::ActorDeathCause actor_death_cause;
  actor_death_cause.mutable_actor_died_error_context()->set_actor_id(
      task_info.actor_id.Binary());
  actor_death_cause.mutable_actor_died_error_context()->set_preempted(
      task_info.preempted);
  rpc::RayErrorInfo error_info;
  error_info.mutable_actor_died_error()->CopyFrom(actor_death_cause);
  error_info.set_error_type(rpc::ErrorType::ACTOR_DIED);
  error_info.set_error_message("Actor died.");

  GetTaskFinisherWithoutMu().FailPendingTask(task_info.specification.TaskId(),
                                             rpc::ErrorType::ACTOR_DIED,
                                             &task_info.status,
                                             &error_info);
}

void CoreWorkerDirectActorTaskSubmitter::CheckTimeoutTasks() {
  auto task_info_list = std::make_shared<std::vector<TaskInfo>>();
  {
    absl::MutexLock lock(&mu_);
    for (auto &queue_pair : client_queues_) {
      auto &queue = queue_pair.second;
      auto deque_itr = queue.wait_for_death_info_tasks.begin();
      while (deque_itr != queue.wait_for_death_info_tasks.end() &&
             /*timeout timestamp*/ deque_itr->first < current_time_ms()) {
        auto &task_spec_status_pair = deque_itr->second;
        task_info_list->push_back(TaskInfo{
            task_spec_status_pair.first,
            task_spec_status_pair.second,
            queue_pair.first,
            queue.preempted,
        });
        deque_itr = queue.wait_for_death_info_tasks.erase(deque_itr);
      }
    }
  }

  if (task_info_list->empty()) {
    return;
  }

  // Do not hold mu_, because FailPendingTask may call python from cpp,
  // and may cause deadlock with SubmitActorTask thread when aquire GIL.
  for (auto &task_info : *task_info_list) {
    FailTaskWithError(task_info);
  }
}

void CoreWorkerDirectActorTaskSubmitter::SendPendingTasks(const ActorID &actor_id) {
  auto it = client_queues_.find(actor_id);
  RAY_CHECK(it != client_queues_.end());
  auto &client_queue = it->second;
  auto &actor_submit_queue = client_queue.actor_submit_queue;
  if (!client_queue.rpc_client) {
    if (client_queue.state == rpc::ActorTableData::RESTARTING &&
        client_queue.fail_if_actor_unreachable) {
      // When `fail_if_actor_unreachable` is true, tasks submitted while the actor is in
      // `RESTARTING` state fail immediately.
      while (true) {
        auto task = actor_submit_queue->PopNextTaskToSend();
        if (!task.has_value()) {
          break;
        }

        io_service_.post(
            [this, task_spec = std::move(task.value().first)] {
              rpc::PushTaskReply reply;
              rpc::Address addr;
              HandlePushTaskReply(
                  Status::IOError("The actor is temporarily unavailable."),
                  reply,
                  addr,
                  task_spec);
            },
            "CoreWorkerDirectActorTaskSubmitter::SendPendingTasks_ForceFail");
      }
    }
    return;
  }

  // Check if there is a pending force kill. If there is, send it and disconnect the
  // client.
  if (client_queue.pending_force_kill) {
    RAY_LOG(INFO) << "Sending KillActor request to actor " << actor_id;
    // It's okay if this fails because this means the worker is already dead.
    client_queue.rpc_client->KillActor(*client_queue.pending_force_kill, nullptr);
    client_queue.pending_force_kill.reset();
  }

  // Submit all pending actor_submit_queue->
  while (true) {
    auto task = actor_submit_queue->PopNextTaskToSend();
    if (!task.has_value()) {
      break;
    }
    RAY_CHECK(!client_queue.worker_id.empty());
    PushActorTask(client_queue, task.value().first, task.value().second);
  }
}

void CoreWorkerDirectActorTaskSubmitter::ResendOutOfOrderTasks(const ActorID &actor_id) {
  auto it = client_queues_.find(actor_id);
  RAY_CHECK(it != client_queues_.end());
  if (!it->second.rpc_client) {
    return;
  }
  auto &client_queue = it->second;
  RAY_CHECK(!client_queue.worker_id.empty());
  auto out_of_order_completed_tasks =
      client_queue.actor_submit_queue->PopAllOutOfOrderCompletedTasks();
  for (const auto &completed_task : out_of_order_completed_tasks) {
    // Making a copy here because we are flipping a flag and the original value is
    // const.
    auto task_spec = completed_task.second;
    task_spec.GetMutableMessage().set_skip_execution(true);
    PushActorTask(client_queue, task_spec, /*skip_queue=*/true);
  }
}

void CoreWorkerDirectActorTaskSubmitter::PushActorTask(ClientQueue &queue,
                                                       const TaskSpecification &task_spec,
                                                       bool skip_queue) {
  const auto task_id = task_spec.TaskId();

  auto request = std::make_unique<rpc::PushTaskRequest>();
  // NOTE(swang): CopyFrom is needed because if we use Swap here and the task
  // fails, then the task data will be gone when the TaskManager attempts to
  // access the task.
  request->mutable_task_spec()->CopyFrom(task_spec.GetMessage());

  request->set_intended_worker_id(queue.worker_id);
  request->set_sequence_number(queue.actor_submit_queue->GetSequenceNumber(task_spec));

  const auto actor_id = task_spec.ActorId();
  const auto actor_counter = task_spec.ActorCounter();
  const auto num_queued = queue.inflight_task_callbacks.size();
  RAY_LOG(DEBUG) << "Pushing task " << task_id << " to actor " << actor_id
                 << " actor counter " << actor_counter << " seq no "
                 << request->sequence_number() << " num queued " << num_queued;
  if (num_queued >= next_queueing_warn_threshold_) {
    // TODO(ekl) add more debug info about the actor name, etc.
    warn_excess_queueing_(actor_id, num_queued);
    next_queueing_warn_threshold_ *= 2;
  }

  rpc::Address addr(queue.rpc_client->Addr());
  rpc::ClientCallback<rpc::PushTaskReply> reply_callback =
      [this, addr, task_spec](const Status &status, const rpc::PushTaskReply &reply) {
        HandlePushTaskReply(status, reply, addr, task_spec);
      };

  queue.inflight_task_callbacks.emplace(task_id, std::move(reply_callback));
  rpc::ClientCallback<rpc::PushTaskReply> wrapped_callback =
      [this, task_id, actor_id](const Status &status, const rpc::PushTaskReply &reply) {
        rpc::ClientCallback<rpc::PushTaskReply> reply_callback;
        {
          absl::MutexLock lock(&mu_);
          auto it = client_queues_.find(actor_id);
          RAY_CHECK(it != client_queues_.end());
          auto &queue = it->second;
          auto callback_it = queue.inflight_task_callbacks.find(task_id);
          if (callback_it == queue.inflight_task_callbacks.end()) {
            RAY_LOG(DEBUG) << "The task " << task_id
                           << " has already been marked as failed. Ignore the reply.";
            return;
          }
          reply_callback = std::move(callback_it->second);
          queue.inflight_task_callbacks.erase(callback_it);
        }
        reply_callback(status, reply);
      };

  task_finisher_.MarkTaskWaitingForExecution(task_id,
                                             NodeID::FromBinary(addr.raylet_id()),
                                             WorkerID::FromBinary(addr.worker_id()));
  queue.rpc_client->PushActorTask(std::move(request), skip_queue, wrapped_callback);
}

void CoreWorkerDirectActorTaskSubmitter::HandlePushTaskReply(
    const Status &status,
    const rpc::PushTaskReply &reply,
    const rpc::Address &addr,
    const TaskSpecification &task_spec) {
  const auto task_id = task_spec.TaskId();
  const auto actor_id = task_spec.ActorId();
  const auto actor_counter = task_spec.ActorCounter();
  const auto task_skipped = task_spec.GetMessage().skip_execution();
  /// Whether or not we will retry this actor task.
  auto will_retry = false;

  if (task_skipped) {
    // NOTE(simon):Increment the task counter regardless of the status because the
    // reply for a previously completed task. We are not calling CompletePendingTask
    // because the tasks are pushed directly to the actor, not placed on any queues
    // in task_finisher_.
  } else if (status.ok()) {
    task_finisher_.CompletePendingTask(
        task_id, reply, addr, reply.is_application_error());
  } else if (status.IsSchedulingCancelled()) {
    std::ostringstream stream;
    stream << "The task " << task_id << " is canceled from an actor " << actor_id
           << " before it executes.";
    const auto &msg = stream.str();
    RAY_LOG(DEBUG) << msg;
    rpc::RayErrorInfo error_info;
    error_info.set_error_message(msg);
    error_info.set_error_type(rpc::ErrorType::TASK_CANCELLED);
    GetTaskFinisherWithoutMu().FailPendingTask(task_spec.TaskId(),
                                               rpc::ErrorType::TASK_CANCELLED,
                                               /*status*/ nullptr,
                                               &error_info);
  } else {
    bool is_actor_dead = false;
    bool fail_immediatedly = false;
    rpc::ErrorType error_type;
    rpc::RayErrorInfo error_info;
    {
      // push task failed due to network error. For example, actor is dead
      // and no process response for the push task.
      absl::MutexLock lock(&mu_);
      auto queue_pair = client_queues_.find(actor_id);
      RAY_CHECK(queue_pair != client_queues_.end());
      auto &queue = queue_pair->second;

      // If the actor is already dead, immediately mark the task object as failed.
      // Otherwise, start the grace period before marking the object as dead.
      is_actor_dead = queue.state == rpc::ActorTableData::DEAD;
      const auto &death_cause = queue.death_cause;
      error_info = GetErrorInfoFromActorDeathCause(death_cause);
      error_type = error_info.error_type();
      fail_immediatedly = error_info.has_actor_died_error() &&
                          error_info.actor_died_error().has_oom_context() &&
                          error_info.actor_died_error().oom_context().fail_immediately();
    }

    // This task may have been waiting for dependency resolution, so cancel
    // this first.
    resolver_.CancelDependencyResolution(task_id);

    will_retry = GetTaskFinisherWithoutMu().FailOrRetryPendingTask(
        task_id,
        error_type,
        &status,
        &error_info,
        /*mark_task_object_failed*/ is_actor_dead,
        fail_immediatedly);

    if (!is_actor_dead && !will_retry) {
      // No retry == actor is dead.
      // If actor is not dead yet, wait for the grace period until we mark the
      // return object as failed.
      if (RayConfig::instance().timeout_ms_task_wait_for_death_info() != 0) {
        int64_t death_info_grace_period_ms =
            current_time_ms() +
            RayConfig::instance().timeout_ms_task_wait_for_death_info();
        absl::MutexLock lock(&mu_);
        auto queue_pair = client_queues_.find(actor_id);
        RAY_CHECK(queue_pair != client_queues_.end());
        auto &queue = queue_pair->second;
        queue.wait_for_death_info_tasks.emplace_back(death_info_grace_period_ms,
                                                     std::make_pair(task_spec, status));
        RAY_LOG(INFO)
            << "PushActorTask failed because of network error, this task "
               "will be stashed away and waiting for Death info from GCS, task_id="
            << task_spec.TaskId()
            << ", wait_queue_size=" << queue.wait_for_death_info_tasks.size();
      } else {
        // TODO(vitsai): if we don't need death info, just fail the request.
        {
          absl::MutexLock lock(&mu_);
          auto queue_pair = client_queues_.find(actor_id);
          RAY_CHECK(queue_pair != client_queues_.end());
        }
        GetTaskFinisherWithoutMu().FailPendingTask(
            task_spec.TaskId(), rpc::ErrorType::ACTOR_DIED, &status);
      }
    }
  }
  {
    absl::MutexLock lock(&mu_);
    auto queue_pair = client_queues_.find(actor_id);
    RAY_CHECK(queue_pair != client_queues_.end());
    auto &queue = queue_pair->second;
    if (!will_retry) {
      queue.actor_submit_queue->MarkTaskCompleted(actor_counter, task_spec);
    }
    queue.cur_pending_calls--;
  }
}

bool CoreWorkerDirectActorTaskSubmitter::IsActorAlive(const ActorID &actor_id) const {
  absl::MutexLock lock(&mu_);

  auto iter = client_queues_.find(actor_id);
  return (iter != client_queues_.end() && iter->second.rpc_client);
}

bool CoreWorkerDirectActorTaskSubmitter::PendingTasksFull(const ActorID &actor_id) const {
  absl::MutexLock lock(&mu_);
  auto it = client_queues_.find(actor_id);
  RAY_CHECK(it != client_queues_.end());
  return it->second.max_pending_calls > 0 &&
         it->second.cur_pending_calls >= it->second.max_pending_calls;
}

size_t CoreWorkerDirectActorTaskSubmitter::NumPendingTasks(
    const ActorID &actor_id) const {
  absl::MutexLock lock(&mu_);
  auto it = client_queues_.find(actor_id);
  RAY_CHECK(it != client_queues_.end());
  return it->second.cur_pending_calls;
}

bool CoreWorkerDirectActorTaskSubmitter::CheckActorExists(const ActorID &actor_id) const {
  absl::MutexLock lock(&mu_);
  return client_queues_.find(actor_id) != client_queues_.end();
}

std::string CoreWorkerDirectActorTaskSubmitter::DebugString(
    const ActorID &actor_id) const {
  absl::MutexLock lock(&mu_);
  auto it = client_queues_.find(actor_id);
  RAY_CHECK(it != client_queues_.end());
  std::ostringstream stream;
  stream << "Submitter debug string for actor " << actor_id << " "
         << it->second.DebugString();
  return stream.str();
}

void CoreWorkerDirectActorTaskSubmitter::RetryCancelTask(TaskSpecification task_spec,
                                                         bool recursive,
                                                         int64_t milliseconds) {
  RAY_LOG(DEBUG) << "Task " << task_spec.TaskId() << " cancelation will be retried in "
                 << milliseconds << " ms";
  execute_after(
      io_service_,
      [this, task_spec = std::move(task_spec), recursive] {
        RAY_UNUSED(CancelTask(task_spec, recursive));
      },
      std::chrono::milliseconds(milliseconds));
}

Status CoreWorkerDirectActorTaskSubmitter::CancelTask(TaskSpecification task_spec,
                                                      bool recursive) {
  // We don't support force_kill = true for actor tasks.
  bool force_kill = false;
  RAY_LOG(INFO) << "Cancelling a task: " << task_spec.TaskId()
                << " for an actor: " << task_spec.ActorId()
                << " force_kill: " << force_kill << " recursive: " << recursive;

  // Tasks are in one of the following states.
  // - dependencies not resolved
  // - queued
  // - sent
  // - finished.

  const auto actor_id = task_spec.ActorId();
  const auto &task_id = task_spec.TaskId();
  auto send_pos = task_spec.ActorCounter();

  // Shouldn't hold a lock while accessing task_finisher_.
  // Task is already canceled or finished.
  if (!GetTaskFinisherWithoutMu().MarkTaskCanceled(task_id)) {
    RAY_LOG(DEBUG) << "a task " << task_id << " is already finished or canceled";
    return Status::OK();
  }

  auto task_queued = false;
  {
    absl::MutexLock lock(&mu_);

    auto queue = client_queues_.find(actor_id);
    RAY_CHECK(queue != client_queues_.end());
    if (queue->second.state == rpc::ActorTableData::DEAD) {
      // No need to decrement cur_pending_calls because it doesn't matter.
      RAY_LOG(DEBUG) << "a task " << task_id
                     << "'s actor is already dead. Ignoring the cancel request.";
      return Status::OK();
    }

    task_queued = queue->second.actor_submit_queue->Contains(send_pos);
    if (task_queued) {
      auto dep_resolved = queue->second.actor_submit_queue->Get(send_pos).second;
      if (!dep_resolved) {
        RAY_LOG(DEBUG)
            << "a task " << task_id
            << " has been resolving dependencies. Cancel to resolve dependencies";
        resolver_.CancelDependencyResolution(task_id);
      }
      RAY_LOG(DEBUG) << "a task " << task_id
                     << " was queued. Mark a task is canceled from a queue.";
      queue->second.actor_submit_queue->MarkTaskCanceled(send_pos);
    }
  }

  // Fail a request immediately if it is still queued.
  // The task won't be sent to an actor in this case.
  // We cannot hold a lock when calling `FailOrRetryPendingTask`.
  if (task_queued) {
    rpc::RayErrorInfo error_info;
    std::ostringstream stream;
    stream << "The task " << task_id << " is canceled from an actor " << actor_id
           << " before it executes.";
    error_info.set_error_message(stream.str());
    error_info.set_error_type(rpc::ErrorType::TASK_CANCELLED);
    GetTaskFinisherWithoutMu().FailOrRetryPendingTask(
        task_id, rpc::ErrorType::TASK_CANCELLED, /*status*/ nullptr, &error_info);
    return Status::OK();
  }

  // At this point, the task is in "sent" state and not finished yet.
  // We cannot guarantee a cancel request is received "after" a task
  // is submitted because gRPC is not ordered. To get around it,
  // we keep retrying cancel RPCs until task is finished or
  // an executor tells us to stop retrying.

  // If there's no client, it means actor is not created yet.
  // Retry in 1 second.
  {
    absl::MutexLock lock(&mu_);
    RAY_LOG(DEBUG) << "a task " << task_id << " was sent to an actor. Send a cancel RPC.";
    auto queue = client_queues_.find(actor_id);
    RAY_CHECK(queue != client_queues_.end());
    if (!queue->second.rpc_client) {
      RetryCancelTask(task_spec, recursive, 1000);
      return Status::OK();
    }

    const auto &client = queue->second.rpc_client;
    auto request = rpc::CancelTaskRequest();
    request.set_intended_task_id(task_spec.TaskId().Binary());
    request.set_force_kill(force_kill);
    request.set_recursive(recursive);
    request.set_caller_worker_id(task_spec.CallerWorkerId().Binary());
    client->CancelTask(request,
                       [this, task_spec, recursive, task_id](
                           const Status &status, const rpc::CancelTaskReply &reply) {
                         RAY_LOG(DEBUG) << "CancelTask RPC response received for "
                                        << task_spec.TaskId() << " with status "
                                        << status.ToString();

                         // Keep retrying every 2 seconds until a task is officially
                         // finished.
                         if (!GetTaskFinisherWithoutMu().GetTaskSpec(task_id)) {
                           // Task is already finished.
                           RAY_LOG(DEBUG) << "Task " << task_spec.TaskId()
                                          << " is finished. Stop a cancel request.";
                           return;
                         }

                         if (!reply.attempt_succeeded()) {
                           RetryCancelTask(task_spec, recursive, 2000);
                         }
                       });
  }

  // NOTE: Currently, ray.cancel is asynchronous.
  // If we want to have a better guarantee in the cancelation result
  // we should make it synchronos, but that can regress the performance.
  return Status::OK();
}

}  // namespace core
}  // namespace ray
