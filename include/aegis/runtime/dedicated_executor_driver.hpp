// Purpose: define the dedicated owner-thread driver that waits for commands or loss fences without
// introducing a second turn-processing implementation.

#pragma once

#include "aegis/model/domain_error.hpp"
#include "aegis/runtime/serialized_executor.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace aegis::runtime {

// ########################################################################
// DedicatedExecutorDriver owns the production-thread seam, publishes startup/terminal state, and
// stops only between run-to-completion turns.
class DedicatedExecutorDriver final {
public:

  // --------------------------------------------------------
  // Start one owner thread and wait until binding success or failure has been published.
  explicit DedicatedExecutorDriver(SerializedExecutor& executor);

  // --------------------------------------------------------
  // Stop and join before any status synchronization members are destroyed.
  ~DedicatedExecutorDriver();

  // --------------------------------------------------------
  // Capturing this in the worker prevents safe relocation, so the driver is neither copyable nor
  // movable.
  DedicatedExecutorDriver(const DedicatedExecutorDriver&) = delete;
  DedicatedExecutorDriver& operator=(const DedicatedExecutorDriver&) = delete;
  DedicatedExecutorDriver(DedicatedExecutorDriver&&) = delete;
  DedicatedExecutorDriver& operator=(DedicatedExecutorDriver&&) = delete;

  // --------------------------------------------------------
  // Report whether initial owner binding succeeded; false remains stable after a terminal failure.
  [[nodiscard]] bool started_successfully() const noexcept;

  // --------------------------------------------------------
  // Report whether the worker is currently waiting or executing after successful startup.
  [[nodiscard]] bool running() const noexcept;

  // --------------------------------------------------------
  // Return the stable binding, processing, or release error that stopped this driver, if any.
  [[nodiscard]] std::optional<model::DomainError> terminal_error() const;

  // --------------------------------------------------------
  // Copy the most recent completed turn report, including queue age for accepted commands.
  [[nodiscard]] std::optional<TurnReport> last_turn_report() const;

  // --------------------------------------------------------
  // Publish synchronized shutdown after the active turn and wake an otherwise idle worker.
  void request_stop() noexcept;

  // --------------------------------------------------------
  // Wait until the owner thread has released the executor and published its final status.
  void wait_until_stopped();

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Bind, process command and fence turns through SerializedExecutor::run_one, then release.
  void worker_loop() noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // Interesting syntax: AppleClang 16's supported libc++ disables C++20 stop_token/jthread, while
  // libstdc++ provides it. Use jthread when advertised and retain an owned-stop std::thread seam on
  // the supported fallback so runtime behavior and shutdown ordering remain identical.
#if defined(__cpp_lib_jthread)
  using OwnerThread = std::jthread;
#else
  using OwnerThread = std::thread;
#endif

  // ########################################################################

  // --------------------------------------------------------
  SerializedExecutor& executor_;
  mutable std::mutex status_mutex_;
  std::condition_variable status_changed_;
  bool startup_complete_{false};
  bool startup_succeeded_{false};
  bool running_{false};
  bool stopped_{false};
  std::optional<model::DomainError> terminal_error_;
  std::optional<TurnReport> last_turn_report_;
  std::atomic_bool stop_requested_{false};
  OwnerThread worker_;
};

// ########################################################################

} // namespace aegis::runtime
