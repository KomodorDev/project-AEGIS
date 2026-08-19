// Purpose: implement dedicated serialized-owner lifecycle, blocking wakeup, and terminal error
// publication around the executor's shared run_one processor.

#include "aegis/runtime/dedicated_executor_driver.hpp"

#include <mutex>
#include <optional>
#include <utility>

namespace aegis::runtime {

// --------------------------------------------------------
// Construction waits for the worker's binding decision so callers never race an unknown startup
// state when admitting or diagnosing dedicated execution.
DedicatedExecutorDriver::DedicatedExecutorDriver(SerializedExecutor& executor)
    : executor_{executor}, worker_{[this] { worker_loop(); }} {
  std::unique_lock lock{status_mutex_};
  status_changed_.wait(lock, [this] { return startup_complete_; });
}

// --------------------------------------------------------
// Requesting stop before jthread destruction wakes an idle executor wait; jthread then performs the
// required join while all referenced status members still exist.
DedicatedExecutorDriver::~DedicatedExecutorDriver() {
  request_stop();
  wait_until_stopped();
  if (worker_.joinable()) {
    worker_.join();
  }
}

// --------------------------------------------------------
// Startup success records only the initial binding outcome and remains stable after later shutdown.
bool DedicatedExecutorDriver::started_successfully() const noexcept {
  std::lock_guard lock{status_mutex_};
  return startup_succeeded_;
}

// --------------------------------------------------------
// Running state is synchronized with terminal publication and owner release.
bool DedicatedExecutorDriver::running() const noexcept {
  std::lock_guard lock{status_mutex_};
  return running_;
}

// --------------------------------------------------------
// Copy the optional stable error while holding its publication lock.
std::optional<model::DomainError> DedicatedExecutorDriver::terminal_error() const {
  std::lock_guard lock{status_mutex_};
  return terminal_error_;
}

// --------------------------------------------------------
// Copy the latest completed report under the same lock used for terminal and stopped publication.
std::optional<TurnReport> DedicatedExecutorDriver::last_turn_report() const {
  std::lock_guard lock{status_mutex_};
  return last_turn_report_;
}

// --------------------------------------------------------
// Stop is idempotent and publishes under the executor predicate mutex before waking any waiter.
void DedicatedExecutorDriver::request_stop() noexcept {
  executor_.publish_driver_stop(stop_requested_);
#if defined(__cpp_lib_jthread)
  worker_.request_stop();
#endif
}

// --------------------------------------------------------
// Wait for owner release, not merely a stop request, so a caller can safely bind a later driver.
void DedicatedExecutorDriver::wait_until_stopped() {
  std::unique_lock lock{status_mutex_};
  status_changed_.wait(lock, [this] { return stopped_; });
}

// --------------------------------------------------------
// The worker performs no data-plane operation of its own: it binds and repeatedly invokes the
// shared serialized turn processor until closure, stop, or a stable executor error.
void DedicatedExecutorDriver::worker_loop() noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the binding decision before processing any already accepted work.
  const auto binding = executor_.bind_to_current_thread();
  {
    std::lock_guard lock{status_mutex_};
    startup_complete_ = true;
    startup_succeeded_ = binding.has_value();
    running_ = binding.has_value();
    if (!binding) {
      terminal_error_.emplace(binding.error());
      stopped_ = true;
    }
  }
  status_changed_.notify_all();
  if (!binding) {
    return;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Process exactly one shared turn per wake-loop iteration; close drains the merged command/fence
  // prefix before the wait predicate terminates, while an open stop exits between turns.
  while (executor_.wait_for_runnable_work(stop_requested_)) {
    const auto turn = executor_.run_one_for_driver(stop_requested_);
    if (!turn) {
      std::lock_guard lock{status_mutex_};
      terminal_error_.emplace(turn.error());
      break;
    }
    if (!turn.value().has_value()) {
      break;
    }
    {
      std::lock_guard lock{status_mutex_};
      last_turn_report_.emplace(turn.value().value());
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A terminal fault may wake wait_for_runnable_work without entering run_one. Release and copy it
  // under one executor lock so a producer cannot publish a fault in an unobservable handoff gap.
  const auto release = executor_.release_from_current_thread_for_driver();
  {
    std::lock_guard lock{status_mutex_};
    if (release && release.value().has_value() && !terminal_error_.has_value()) {
      terminal_error_.emplace(release.value().value());
    }
    if (!release && !terminal_error_.has_value()) {
      terminal_error_.emplace(release.error());
    }
    running_ = false;
    stopped_ = true;
  }
  status_changed_.notify_all();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
