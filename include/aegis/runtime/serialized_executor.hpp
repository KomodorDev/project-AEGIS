// Purpose: define bounded admission, attempt-ordered loss fencing, and the one shared serialized
// owner-turn processor used by deterministic and dedicated runtime drivers.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Ordinary admission outcomes remain successful Result values so overload and shutdown cannot be
// confused with terminal executor faults.
enum class AdmissionOutcome : std::uint8_t {
  Accepted = 1,
  CapacityExceeded = 2,
  Closed = 3,
};

// ########################################################################

// ########################################################################
// An accepted receipt binds one attempt to its distinct receive sequence, timestamp, and atomic
// pending-queue observation.
struct AdmissionReceipt {
  model::AdmissionOrdinal attempt_ordinal;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp received_at;
  std::size_t pending_depth;
  std::size_t pending_capacity;

  // --------------------------------------------------------
  // Structural equality pins the complete accepted-admission replay input.
  friend bool operator==(const AdmissionReceipt&, const AdmissionReceipt&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Every non-terminal attempt returns an ordinal and mutex-linearized queue depth/capacity; only
// Accepted carries a receipt, and only attributable capacity loss may record a discontinuity fence.
struct AdmissionDecision {
  AdmissionOutcome outcome;
  model::AdmissionOrdinal attempt_ordinal;
  std::size_t pending_depth;
  std::size_t pending_capacity;
  std::optional<AdmissionReceipt> receipt;
  bool discontinuity_recorded;

  // --------------------------------------------------------
  // Structural equality makes accepted, overloaded, and closed replay decisions exact.
  friend bool operator==(const AdmissionDecision&, const AdmissionDecision&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// AcceptedTurnContext is stack-scoped owner authority for one accepted command. Handlers must use
// stable runtime handles in their copied command and must not retain this context reference.
struct AcceptedTurnContext {
  AdmissionReceipt receipt;
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;
  model::ElapsedNanoseconds queue_age;

  // --------------------------------------------------------
  // Structural equality pins every value available while one accepted command runs.
  friend bool operator==(const AcceptedTurnContext&, const AcceptedTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// ControlTurnContext gives a source-discontinuity handler only its owner-turn identity and
// processing time; a fence has no receive sequence, timestamp, or queue age.
struct ControlTurnContext {
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins the complete control-turn authority.
  friend bool operator==(const ControlTurnContext&, const ControlTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One bounded fence summarizes all currently pending losses for a configured source without
// erasing the earliest failed attempt that determines global owner-turn order.
struct SourceDiscontinuity {
  model::MarketSourceOrdinal source_ordinal;
  model::AdmissionOrdinal earliest_failed_attempt;
  std::uint64_t lost_attempt_count;

  // --------------------------------------------------------
  // Structural equality pins the complete owner-visible loss summary.
  friend bool operator==(const SourceDiscontinuity&, const SourceDiscontinuity&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Runtime-owned implementations apply source loss on the serialized owner before later attempts
// can execute; their lifetime must enclose the executor that borrows them.
class SourceDiscontinuityHandler {
public:

  // --------------------------------------------------------
  // Polymorphic cleanup supports concrete runtime-owned handlers without exposing their state.
  virtual ~SourceDiscontinuityHandler() = default;

  // --------------------------------------------------------
  // Validate all fallible work before data-plane mutation, then consume one merged fence. A
  // failure certifies that the handler made no data-plane change and lets the executor restore it.
  [[nodiscard]] virtual model::Result<void>
  on_source_discontinuity(const SourceDiscontinuity& discontinuity,
                          const ControlTurnContext& context) noexcept = 0;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A turn kind distinguishes accepted command work from a preallocated control fence.
enum class TurnKind : std::uint8_t {
  Command = 1,
  SourceDiscontinuity = 2,
};

// ########################################################################

// ########################################################################
// A successful report covers one completed owner turn. Fence turns deliberately omit receive
// identity and queue age because they were never admitted into a pending command slot. Execution
// duration is measured externally or at callback dispatch, not by an additional executor clock
// read.
struct TurnReport {
  TurnKind kind;
  model::TurnOrdinal turn_ordinal;
  model::AdmissionOrdinal attempt_ordinal;
  std::optional<model::ReceiveSequence> receive_sequence;
  std::optional<model::ReceiveTimestamp> received_at;
  model::ProcessingTimestamp started_at;
  std::optional<model::ElapsedNanoseconds> queue_age;
  std::optional<SourceDiscontinuity> discontinuity;
  std::size_t pending_commands_at_start;
  std::size_t pending_commands_at_completion;
  std::size_t pending_fences_at_start;
  std::size_t pending_fences_at_completion;
  std::size_t command_capacity;
  std::uint64_t completed_turns;

  // --------------------------------------------------------
  // Structural equality pins ordering, timing, and bounded-state observations together.
  friend bool operator==(const TurnReport&, const TurnReport&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Bounded drive reports distinguish turns completed by this call from lifetime completion and
// expose both forms of remaining runnable work.
struct PendingTurnExecutionReport {
  std::size_t turns_executed;
  std::size_t pending_commands;
  std::size_t pending_fences;
  std::size_t command_capacity;
  std::size_t maximum_drive_turns;
  std::uint64_t completed_turns;

  // --------------------------------------------------------
  // Structural equality supports exact bounded-stop assertions.
  friend bool operator==(const PendingTurnExecutionReport&,
                         const PendingTurnExecutionReport&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A synchronized snapshot exposes bounded ingress and terminal lifecycle state without publishing
// queue slots, fence storage, or owner-local commands.
struct ExecutorQueueSnapshot {
  std::size_t pending_commands;
  std::size_t pending_fences;
  std::size_t in_flight_fences;
  std::size_t command_capacity;
  std::size_t source_capacity;
  std::size_t maximum_drive_turns;
  std::uint64_t completed_turns;
  bool closed;
  bool faulted;
  bool owner_bound;
  bool turn_active;

  // --------------------------------------------------------
  // Structural equality keeps diagnostic observations deterministic.
  friend bool operator==(const ExecutorQueueSnapshot&, const ExecutorQueueSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A source-fence snapshot exposes loss folding and in-flight gating without publishing mutable
// fence storage or permitting callers to clear an integrity interval.
struct SourceFenceSnapshot {
  model::MarketSourceOrdinal source_ordinal;
  std::optional<model::AdmissionOrdinal> earliest_pending_attempt;
  std::uint64_t pending_loss_count;
  bool in_flight;

  // --------------------------------------------------------
  // Structural equality pins one configured source's complete coordination state.
  friend bool operator==(const SourceFenceSnapshot&, const SourceFenceSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Construction-only counter seeds support deterministic boundary qualification without exposing
// mutable production counters after the executor begins accepting work.
struct ExecutorCounterSeed {
  std::optional<model::AdmissionOrdinal> last_admission_ordinal;
  std::optional<model::ReceiveSequence> last_receive_sequence;
  std::optional<model::TurnOrdinal> last_turn_ordinal;
};

// ########################################################################

// ########################################################################
// InlineCommandWorkItem owns one immutable, trivially copyable command in fixed inline storage.
// Pointer-like members may only be stable runtime handles whose lifetime encloses the executor;
// temporary caller payload addresses are forbidden. A handler failure certifies no data-plane
// mutation occurred.
class InlineCommandWorkItem final {
public:
  static constexpr std::size_t inline_capacity_bytes = 192U;

  // --------------------------------------------------------
  // Interesting syntax: a non-type template argument binds the exact Result-returning noexcept
  // handler while every queue slot retains only the copied command object representation.
  template <auto Handler, typename Command>
    requires(
        std::is_trivially_copyable_v<Command> && sizeof(Command) <= inline_capacity_bytes &&
        std::same_as<decltype(Handler),
                     model::Result<void> (*)(const Command&, const AcceptedTurnContext&) noexcept>)
  [[nodiscard]] static InlineCommandWorkItem
  create_inline_command_work_item(Command command) noexcept {

    // ++++++++++++++++++++++++++++++++++++++++
    // Copy the complete immutable command into zero-initialized bounded storage.
    InlineCommandWorkItem work;
    std::memcpy(work.storage_.data(), &command, sizeof(Command));

    // ++++++++++++++++++++++++++++++++++++++++
    // Bind invocation to the command type without retaining an untyped caller payload pointer.
    work.invoke_ = &invoke_typed<Handler, Command>;
    return work;

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Value semantics move or copy only bounded inline bytes and one function pointer.
  InlineCommandWorkItem(const InlineCommandWorkItem&) noexcept = default;
  InlineCommandWorkItem& operator=(const InlineCommandWorkItem&) noexcept = default;
  InlineCommandWorkItem(InlineCommandWorkItem&&) noexcept = default;
  InlineCommandWorkItem& operator=(InlineCommandWorkItem&&) noexcept = default;
  ~InlineCommandWorkItem() = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Shared aliases keep every queue slot's storage and invocation shape identical.
  using InlineCommandStorage = std::array<std::byte, inline_capacity_bytes>;
  using WorkInvocationFunction = model::Result<void> (*)(const InlineCommandStorage&,
                                                         const AcceptedTurnContext&) noexcept;

  // ########################################################################

  // --------------------------------------------------------
  // Only the typed factory may create a publishable work item.
  InlineCommandWorkItem() noexcept = default;

  // --------------------------------------------------------
  // Reconstitute a local command value and forward stack-scoped turn authority without retaining
  // either reference.
  template <auto Handler, typename Command>
  [[nodiscard]] static model::Result<void>
  invoke_typed(const InlineCommandStorage& storage, const AcceptedTurnContext& context) noexcept {
    std::array<std::byte, sizeof(Command)> command_bytes{};
    std::memcpy(command_bytes.data(), storage.data(), sizeof(Command));
    const auto command = std::bit_cast<Command>(command_bytes);
    return Handler(command, context);
  }

  // --------------------------------------------------------
  // Invocation is restricted to the executor after it establishes the owner-turn boundary.
  [[nodiscard]] model::Result<void>
  execute_work(const AcceptedTurnContext& context) const noexcept {
    return invoke_(storage_, context);
  }

  // --------------------------------------------------------
  InlineCommandStorage storage_{};
  WorkInvocationFunction invoke_{nullptr};

  friend class SerializedExecutor;
};

// ########################################################################

// ########################################################################
// SerializedExecutor owns one preallocated command ring and one preallocated fence per configured
// source. Exactly one bound thread drains their merged global attempt order.
class SerializedExecutor final {
public:

  // --------------------------------------------------------
  // Construct an executor for unattributable work with no source-fence table and one hard drive
  // bound; raw tests default to the widest representable bound.
  explicit SerializedExecutor(
      std::size_t command_capacity, model::ClockProvider& clock,
      std::size_t maximum_drive_turns = std::numeric_limits<std::size_t>::max(),
      ExecutorCounterSeed counter_seed = {});

  // --------------------------------------------------------
  // Construct the configured-source table once and borrow its owner-side fence handler.
  SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                     model::ClockProvider& clock, SourceDiscontinuityHandler& discontinuity_handler,
                     std::size_t maximum_drive_turns = std::numeric_limits<std::size_t>::max(),
                     ExecutorCounterSeed counter_seed = {});

  // --------------------------------------------------------
  // The synchronization and ownership boundary cannot be copied or relocated.
  SerializedExecutor(const SerializedExecutor&) = delete;
  SerializedExecutor& operator=(const SerializedExecutor&) = delete;
  SerializedExecutor(SerializedExecutor&&) = delete;
  SerializedExecutor& operator=(SerializedExecutor&&) = delete;
  ~SerializedExecutor() = default;

  // --------------------------------------------------------
  // Decide one non-blocking attempt. Every configured attempt gated by pending/in-flight loss
  // returns CapacityExceeded and records or extends that source's bounded fence.
  [[nodiscard]] model::Result<AdmissionDecision>
  try_admit(InlineCommandWorkItem work,
            std::optional<model::MarketSourceOrdinal> source_ordinal = std::nullopt);

  // --------------------------------------------------------
  // Permanently reject later work while preserving the already established command/fence prefix.
  void close() noexcept;

  // --------------------------------------------------------
  // Let the current owner latch a post-mutation terminal fault without erasing the active turn's
  // successful completion report. This authority exists only on an active owner call stack.
  [[nodiscard]] model::Result<void> request_owner_fault(model::DomainError error) noexcept;

  // --------------------------------------------------------
  // Bind owner progression to the calling thread; only an explicit release permits handoff.
  [[nodiscard]] model::Result<void> bind_to_current_thread();

  // --------------------------------------------------------
  // Release from the bound thread outside a turn.
  [[nodiscard]] model::Result<void> release_from_current_thread();

  // --------------------------------------------------------
  // Execute zero or one oldest command/fence to completion without waiting for future work.
  [[nodiscard]] model::Result<std::optional<TurnReport>> execute_next_turn();

  // --------------------------------------------------------
  // Execute no more than max_turns through execute_next_turn, stopping early when both stores are
  // empty.
  [[nodiscard]] model::Result<PendingTurnExecutionReport>
  execute_pending_turns(std::size_t max_turns);

  // --------------------------------------------------------
  // Return one synchronized ingress, ownership, and terminal-state observation.
  [[nodiscard]] ExecutorQueueSnapshot queue_snapshot() const noexcept;

  // --------------------------------------------------------
  // Report whether the calling thread is the currently bound owner without exposing its identity.
  [[nodiscard]] bool is_current_thread_owner() const noexcept;

  // --------------------------------------------------------
  // Return one configured source's synchronized loss state, or nullopt when it is unconfigured.
  [[nodiscard]] std::optional<SourceFenceSnapshot>
  source_fence_snapshot(model::MarketSourceOrdinal source_ordinal) const noexcept;

  // --------------------------------------------------------
  // Copy the first stable terminal failure, if one has failed admission and progression closed.
  [[nodiscard]] std::optional<model::DomainError> terminal_error() const;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Queue entries pair immutable work with a complete accepted receipt in construction-time
  // storage; optionals mark ring occupancy without allocating.
  struct QueuedWorkEntry {
    std::optional<InlineCommandWorkItem> work;
    std::optional<AdmissionReceipt> receipt;
  };

  // ########################################################################

  // ########################################################################
  // Each configured one-based source ordinal indexes one persistent merge slot.
  struct SourceFenceState {
    std::optional<model::AdmissionOrdinal> earliest_failed_attempt;
    std::uint64_t lost_attempt_count{0U};
    bool in_flight{false};
  };

  // ########################################################################

  // ########################################################################
  // Candidate selection names either the FIFO head or one source-fence slot.
  struct RunnableTurnCandidate {
    TurnKind kind;
    std::size_t fence_index;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Build both public constructors through one complete allocation phase.
  SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                     model::ClockProvider& clock, SourceDiscontinuityHandler* discontinuity_handler,
                     std::size_t maximum_drive_turns, ExecutorCounterSeed counter_seed);

  // --------------------------------------------------------
  // Block a dedicated owner until work, closure, a fault, or an explicit stop becomes visible.
  [[nodiscard]] bool wait_for_runnable_work(const std::atomic_bool& stop_requested);

  // --------------------------------------------------------
  // Publish a dedicated-driver stop under the wait mutex before waking the sleeping owner.
  void publish_driver_stop(std::atomic_bool& stop_requested) noexcept;

  // --------------------------------------------------------
  // Atomically release dedicated ownership and copy any terminal fault that preceded handoff.
  [[nodiscard]] model::Result<std::optional<model::DomainError>>
  release_from_current_thread_for_driver();

  // --------------------------------------------------------
  // Begin a dedicated turn only if synchronized stop has not won the same owner-start mutex.
  [[nodiscard]] model::Result<std::optional<TurnReport>>
  execute_next_turn_for_driver(const std::atomic_bool& stop_requested);

  // --------------------------------------------------------
  // Share selection and execution while optionally applying the dedicated synchronized stop gate.
  [[nodiscard]] model::Result<std::optional<TurnReport>>
  execute_next_turn_impl(const std::atomic_bool* stop_requested);

  // --------------------------------------------------------
  // Validate owner identity and non-reentry with deterministic error precedence.
  [[nodiscard]] model::Result<void> validate_owner_locked() const;

  // --------------------------------------------------------
  // Return the next representable ordinal without committing it to executor state.
  [[nodiscard]] model::Result<model::AdmissionOrdinal> derive_next_admission_ordinal_locked() const;
  [[nodiscard]] model::Result<model::ReceiveSequence> derive_next_receive_sequence_locked() const;
  [[nodiscard]] model::Result<model::TurnOrdinal> derive_next_turn_ordinal_locked() const;

  // --------------------------------------------------------
  // Compare one injected clock observation with every earlier observation before publishing it.
  [[nodiscard]] model::Result<void> observe_clock_locked(std::uint64_t nanoseconds,
                                                         const char* field);

  // --------------------------------------------------------
  // Preserve the first terminal failure and close ingress exactly once.
  void fail_closed_locked(model::DomainError error);

  // --------------------------------------------------------
  // Resolve an attributable source only when its canonical one-based ordinal is configured.
  [[nodiscard]] SourceFenceState*
  configured_fence_locked(std::optional<model::MarketSourceOrdinal> source_ordinal) noexcept;

  // --------------------------------------------------------
  // Record one attributable lost attempt in the current or successor preallocated fence.
  [[nodiscard]] model::Result<void>
  record_source_loss_locked(SourceFenceState& fence, model::AdmissionOrdinal attempt_ordinal);

  // --------------------------------------------------------
  // Restore a failed in-flight fence ahead of any successor losses without unchecked addition.
  void restore_failed_fence_locked(std::size_t fence_index,
                                   const SourceDiscontinuity& discontinuity) noexcept;

  // --------------------------------------------------------
  // Find the globally oldest runnable queue head or source fence.
  [[nodiscard]] std::optional<RunnableTurnCandidate> oldest_runnable_locked() const noexcept;

  // --------------------------------------------------------
  // Return whether either construction-time store currently contains an owner turn.
  [[nodiscard]] bool has_runnable_locked() const noexcept;

  // --------------------------------------------------------
  model::ClockProvider& clock_;
  SourceDiscontinuityHandler* discontinuity_handler_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::vector<QueuedWorkEntry> queue_;
  std::vector<SourceFenceState> fences_;
  std::size_t head_{0U};
  std::size_t tail_{0U};
  std::size_t pending_commands_{0U};
  std::size_t pending_fences_{0U};
  std::size_t in_flight_fences_{0U};
  std::size_t maximum_drive_turns_;
  std::optional<model::AdmissionOrdinal> last_admission_ordinal_;
  std::optional<model::ReceiveSequence> last_receive_sequence_;
  std::optional<model::TurnOrdinal> last_turn_ordinal_;
  std::optional<std::uint64_t> last_clock_observation_;
  std::optional<std::thread::id> owner_thread_;
  std::optional<model::DomainError> terminal_error_;
  bool closed_{false};
  bool turn_active_{false};

  friend class DedicatedExecutorDriver;
};

// ########################################################################

// ########################################################################
// DeterministicExecutorDriver gives one caller explicit owner binding and bounded manual
// progression through exactly the same execute_next_turn implementation as the dedicated thread.
class DeterministicExecutorDriver final {
public:

  // --------------------------------------------------------
  // Borrow one executor without implicitly binding it.
  explicit DeterministicExecutorDriver(SerializedExecutor& executor) noexcept;

  // --------------------------------------------------------
  // Bind deterministic progression to the calling thread.
  [[nodiscard]] model::Result<void> bind_to_current_thread();

  // --------------------------------------------------------
  // Release deterministic ownership for a deliberate driver handoff.
  [[nodiscard]] model::Result<void> release_from_current_thread();

  // --------------------------------------------------------
  // Delegate one optional owner turn to the shared processor.
  [[nodiscard]] model::Result<std::optional<TurnReport>> execute_next_turn();

  // --------------------------------------------------------
  // Delegate bounded owner progression to the shared processor.
  [[nodiscard]] model::Result<PendingTurnExecutionReport>
  execute_pending_turns(std::size_t max_turns);

  // --------------------------------------------------------
private:
  SerializedExecutor& executor_;
};

// ########################################################################

} // namespace aegis::runtime
