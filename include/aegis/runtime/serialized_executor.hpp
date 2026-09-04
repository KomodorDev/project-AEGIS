// Purpose: define bounded admission, attempt-ordered loss fencing, and the one shared serialized
// owner-turn processor used by deterministic and dedicated runtime drivers.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/runtime/private_order_admission.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aegis::runtime {

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
// Stable turn kinds distinguish public, ordinary-private, and reconciliation work plus each
// attributable or reasonless fence.
enum class TurnKind : std::uint8_t {
  Command = 1,
  SourceDiscontinuity = 2,
  PrivateCommand = 3,
  AccountSafetyFence = 4,
  GlobalPrivateFence = 5,
  ReconciliationCommand = 6,
};

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
  PrivateLaneSnapshot private_lane{};

  // --------------------------------------------------------
  // Structural equality pins ordering, timing, and bounded-state observations together.
  friend bool operator==(const TurnReport&, const TurnReport&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A bounded execution report distinguishes turns completed by one call from lifetime completion
// and appends the complete private-lane diagnostic alongside existing public/source counters.
struct PendingTurnExecutionReport {
  std::size_t turns_executed;
  std::size_t pending_commands;
  std::size_t pending_fences;
  std::size_t command_capacity;
  std::size_t maximum_drive_turns;
  std::uint64_t completed_turns;
  PrivateLaneSnapshot private_lane{};

  // --------------------------------------------------------
  // Structural equality supports exact bounded-stop assertions.
  friend bool operator==(const PendingTurnExecutionReport&,
                         const PendingTurnExecutionReport&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A synchronized queue snapshot exposes bounded ingress and terminal lifecycle state without
// publishing public, ordinary-private, or reconciliation slots, fence values, or owner-local
// commands.
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
  PrivateLaneSnapshot private_lane{};

  // --------------------------------------------------------
  // Structural equality keeps diagnostic observations deterministic.
  friend bool operator==(const ExecutorQueueSnapshot&, const ExecutorQueueSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A source-fence snapshot exposes loss folding and in-flight gating without publishing
// mutable fence storage or permitting callers to clear an integrity interval.
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
// Construction-only counter seeds support deterministic boundary qualification without exposing
// mutable production counters after the executor begins accepting work.
struct ExecutorCounterSeed {
  std::optional<model::AdmissionOrdinal> last_admission_ordinal;
  std::optional<model::ReceiveSequence> last_receive_sequence;
  std::optional<model::TurnOrdinal> last_turn_ordinal;
};

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
// The construction-time lease serializes token inspection with executor invalidation. It grants no
// admission authority and is never allocated or replaced on an owner turn.
class PrivateAdmissionLease final {
private:

  // --------------------------------------------------------
  // Only an executor may bind a lease to its non-relocatable address.
  explicit PrivateAdmissionLease(SerializedExecutor& owner) noexcept : owner_{&owner} {}

  // --------------------------------------------------------
  // The lease mutex protects the nullable raw address through validation or destruction.
  mutable std::mutex mutex_;
  SerializedExecutor* owner_;

  friend class SerializedExecutor;
  friend class AdmittedPrivateOrderSlot;
  friend class AdmittedReconciliationEventSlot;
};

// ########################################################################
// SerializedExecutor owns separate preallocated public, ordinary-private, and reconciliation
// reserves plus source, account, and global fence stores. Exactly one bound thread drains their
// merged global attempt order.
class SerializedExecutor final {
public:

  // --------------------------------------------------------
  // Construct an executor for unattributable work with no source-fence table and one hard turn
  // execution bound; raw tests default to the widest representable bound.
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
  // Install sealed M4 private storage and borrow its sole owner while leaving public-source
  // fencing disabled.
  SerializedExecutor(std::size_t command_capacity, model::ClockProvider& clock,
                     PrivateAdmissionConfiguration private_configuration,
                     PrivateAdmissionOwner& private_owner,
                     std::size_t maximum_drive_turns = std::numeric_limits<std::size_t>::max(),
                     ExecutorCounterSeed counter_seed = {});

  // --------------------------------------------------------
  // Install public-source and sealed M4 private stores behind the same counters and owner turn.
  SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                     model::ClockProvider& clock, SourceDiscontinuityHandler& discontinuity_handler,
                     PrivateAdmissionConfiguration private_configuration,
                     PrivateAdmissionOwner& private_owner,
                     std::size_t maximum_drive_turns = std::numeric_limits<std::size_t>::max(),
                     ExecutorCounterSeed counter_seed = {});

  // --------------------------------------------------------
  // The synchronization and ownership boundary cannot be copied or relocated.
  SerializedExecutor(const SerializedExecutor&) = delete;
  SerializedExecutor& operator=(const SerializedExecutor&) = delete;
  SerializedExecutor(SerializedExecutor&&) = delete;
  SerializedExecutor& operator=(SerializedExecutor&&) = delete;
  ~SerializedExecutor() noexcept;

  // --------------------------------------------------------
  // Decide one non-blocking attempt. Every configured attempt gated by pending/in-flight loss
  // returns CapacityExceeded and records or extends that source's bounded fence.
  [[nodiscard]] model::Result<AdmissionDecision>
  try_admit(InlineCommandWorkItem work,
            std::optional<model::MarketSourceOrdinal> source_ordinal = std::nullopt);

  // --------------------------------------------------------
  // Copy one trusted receive-time-free source fact into the private critical reserve, or retain
  // producer ownership while atomically activating its account/global loss fence.
  [[nodiscard]] model::Result<PrivateAdmissionDecision>
  try_admit_private(const oms::PrivateOrderIngressAttempt& attempt);

  // --------------------------------------------------------
  // Interesting syntax: deleting the rvalue overload prevents a temporary source fact from being
  // destroyed after a non-acceptance that deliberately leaves ownership with its producer.
  [[nodiscard]] model::Result<PrivateAdmissionDecision>
  try_admit_private(oms::PrivateOrderIngressAttempt&& attempt) = delete;

  // --------------------------------------------------------
  // Interesting syntax: const temporaries also cannot bind through the accepted const-reference
  // overload and disappear after an ordinary non-acceptance.
  [[nodiscard]] model::Result<PrivateAdmissionDecision>
  try_admit_private(const oms::PrivateOrderIngressAttempt&& attempt) = delete;

  // --------------------------------------------------------
  // Copy one trusted authoritative reconciliation fact into its isolated reserve, or retain source
  // ownership while atomically recording the exact configured-account or global loss fence.
  [[nodiscard]] model::Result<ReconciliationAdmissionDecision>
  try_admit_reconciliation_event(const oms::ReconciliationPrivateEventIngressAttempt& attempt);

  // --------------------------------------------------------
  // Interesting syntax: deleting the rvalue overload preserves a temporary authoritative fact when
  // non-acceptance requires its trusted caller to retry or retain it.
  [[nodiscard]] model::Result<ReconciliationAdmissionDecision>
  try_admit_reconciliation_event(oms::ReconciliationPrivateEventIngressAttempt&& attempt) = delete;

  // --------------------------------------------------------
  // Interesting syntax: const reconciliation temporaries cannot disappear after non-acceptance by
  // binding through the accepted const-reference overload.
  [[nodiscard]] model::Result<ReconciliationAdmissionDecision> try_admit_reconciliation_event(
      const oms::ReconciliationPrivateEventIngressAttempt&& attempt) = delete;

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
  // Execute no more than max_turns through execute_next_turn, stopping early when no store is
  // runnable.
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
  // Resolve copied, retained, or append-only consumed state by the original shared attempt ordinal.
  // The retained observation copies its string-owned DomainError and is intentionally not noexcept.
  [[nodiscard]] std::optional<PrivateAdmissionObservation>
  private_admission_observation(model::AdmissionOrdinal attempt_ordinal) const;

  // --------------------------------------------------------
  // Resolve reconciliation copied, retained, or append-only consumed state only through its
  // lane-specific owner oracle; retained errors are copied and this query is not noexcept.
  [[nodiscard]] std::optional<PrivateAdmissionObservation>
  reconciliation_admission_observation(model::AdmissionOrdinal attempt_ordinal) const;

  // --------------------------------------------------------
  // Copy bounded ordinary-private and reconciliation counts plus their shared safety gates under
  // the executor synchronization boundary.
  [[nodiscard]] PrivateLaneSnapshot private_lane_snapshot() const noexcept;

  // --------------------------------------------------------
  // Find one configured account/venue fence and copy only its bounded diagnostic summary.
  [[nodiscard]] std::optional<AccountSafetyFenceSnapshot>
  find_account_safety_fence_snapshot(model::LogicalAccountId logical_account_id,
                                     model::VenueId venue_id) const noexcept;

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
  // Each configured one-based source ordinal indexes one persistent merge slot.
  struct SourceFenceState {
    std::optional<model::AdmissionOrdinal> earliest_failed_attempt;
    std::uint64_t lost_attempt_count{0U};
    bool in_flight{false};
  };

  // ########################################################################
  // Loss folding reports only fixed internal failure classes so post-handler containment performs
  // no string-owning DomainError construction or copy before the executor becomes terminal.
  enum class PrivateLossFailure : std::uint8_t {
    CountExhausted = 1,
  };

  // ########################################################################
  // One fixed pending private slot owns the immutable attempt/receipt pair until owner extraction.
  struct PrivateSlot {
    std::optional<oms::PrivateOrderIngressAttempt> attempt;
    std::optional<AdmissionReceipt> receipt;
  };

  // ########################################################################
  // One fixed reconciliation slot owns an authoritative attempt/receipt pair until owner
  // extraction; this distinct type prevents accidental reserve sharing with ordinary private work.
  struct ReconciliationSlot {
    std::optional<oms::ReconciliationPrivateEventIngressAttempt> attempt;
    std::optional<AdmissionReceipt> receipt;
  };

  // ########################################################################
  // The sole active owner turn is detached from pending capacity while retaining exact token
  // validation authority until its terminal completion is reconciled.
  struct ActivePrivateTurnState {
    oms::PrivateOrderIngressAttempt attempt;
    AdmissionReceipt receipt;
    model::TurnOrdinal turn_ordinal;
  };

  // ########################################################################
  // The sole active reconciliation turn is detached from its pending reserve while retaining exact
  // token-validation authority until owner completion is reconciled.
  struct ActiveReconciliationTurnState {
    oms::ReconciliationPrivateEventIngressAttempt attempt;
    AdmissionReceipt receipt;
    model::TurnOrdinal turn_ordinal;
  };

  // ########################################################################
  // One typed interval preserves every unique reason's first complete occurrence in ordinal order;
  // its invariant is reason_occurrence_count in [1, 19] whenever the interval occupies a fence
  // slot.
  struct PrivateFenceInterval {
    std::uint64_t lost_attempt_count{0U};
    std::array<std::optional<AccountSafetyReasonOccurrence>,
               account_safety_reason_occurrence_capacity>
        ordered_unique_reason_occurrences{};
    std::size_t reason_occurrence_count{0U};
  };

  // ########################################################################
  // A configured account owns exactly one fixed pending interval. The owner extracts it onto its
  // stack while a Boolean gate lets producer races refill the same sole slot with a successor.
  struct AccountFenceState {
    PrivateAdmissionAccountBinding binding;
    std::optional<PrivateFenceInterval> pending;
    bool in_flight{false};
  };

  // ########################################################################
  // Constructor-owned fixed errors let every executor-authored post-handler failure move separate
  // retained observation, terminal, and returned values without allocating on an owner turn.
  struct PrivateReservedErrors {
    model::DomainError committed_observation;
    model::DomainError committed_terminal;
    model::DomainError committed_result;
    model::DomainError retained_observation;
    model::DomainError retained_terminal;
    model::DomainError retained_result;
    model::DomainError account_ingress_loss_terminal;
    model::DomainError account_ingress_loss_result;
    model::DomainError account_owner_loss_terminal;
    model::DomainError account_owner_loss_result;
    model::DomainError global_ingress_loss_terminal;
    model::DomainError global_ingress_loss_result;
    model::DomainError global_owner_loss_terminal;
    model::DomainError global_owner_loss_result;
    model::DomainError reconciliation_committed_observation;
    model::DomainError reconciliation_committed_terminal;
    model::DomainError reconciliation_committed_result;
    model::DomainError reconciliation_retained_observation;
    model::DomainError reconciliation_retained_terminal;
    model::DomainError reconciliation_retained_result;
    model::DomainError reconciliation_account_ingress_loss_terminal;
    model::DomainError reconciliation_account_ingress_loss_result;
    model::DomainError reconciliation_global_ingress_loss_terminal;
    model::DomainError reconciliation_global_ingress_loss_result;
  };

  // ########################################################################
  // The reasonless global fence permanently keeps its first unattributable fact and becomes an
  // owner-applied gate without inventing an account identity.
  struct GlobalFenceState {
    std::optional<CriticalPrivateEventAttempt> first_attempt;
    std::optional<model::AdmissionOrdinal> earliest_attempt_ordinal;
    std::uint64_t lost_attempt_count{0U};
    bool in_flight{false};
    bool owner_applied{false};
  };

  // ########################################################################
  // Internal candidate sources distinguish physical stores while reports retain the stable turn
  // vocabulary.
  enum class RunnableSource : std::uint8_t {
    Command = 1,
    SourceFence = 2,
    PrivateCommand = 3,
    AccountFence = 4,
    GlobalFence = 5,
    ReconciliationCommand = 6,
  };

  // ########################################################################
  // Candidate selection names one physical store and carries its already validated global attempt.
  struct RunnableTurnCandidate {
    RunnableSource source;
    std::size_t index;
    model::AdmissionOrdinal attempt_ordinal;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Build every legacy and M4 constructor through one complete allocation phase.
  SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                     model::ClockProvider& clock, SourceDiscontinuityHandler* discontinuity_handler,
                     std::optional<PrivateAdmissionConfiguration> private_configuration,
                     PrivateAdmissionOwner* private_owner, std::size_t maximum_drive_turns,
                     ExecutorCounterSeed counter_seed);

  // --------------------------------------------------------
  // Allocate every fixed private error during construction, or preserve legacy zero-overhead state.
  [[nodiscard]] static std::optional<PrivateReservedErrors>
  create_private_reserved_errors(bool private_enabled);

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
  // Resolve only an exact sealed root plus configured account/venue pair without a partial match.
  [[nodiscard]] AccountFenceState* find_configured_account_fence_locked(
      const oms::PrivateEventIngressSemanticValue& semantic) noexcept;
  [[nodiscard]] const AccountFenceState* find_configured_account_fence_locked(
      const oms::PrivateEventIngressSemanticValue& semantic) const noexcept;

  // --------------------------------------------------------
  // Fold a failed configured-account attempt into its one fixed canonical interval.
  [[nodiscard]] std::optional<PrivateLossFailure>
  record_account_loss_locked(AccountFenceState& fence, const CriticalPrivateEventAttempt& attempt,
                             model::AdmissionOrdinal attempt_ordinal,
                             risk::AccountSafetyReason reason) noexcept;

  // --------------------------------------------------------
  // Insert or replace one reason's earliest occurrence while preserving ascending ordinal order.
  [[nodiscard]] static bool
  record_earliest_account_reason_occurrence(PrivateFenceInterval& interval,
                                            AccountSafetyReasonOccurrence occurrence) noexcept;

  // --------------------------------------------------------
  // Fold an unattributable attempt into the sole permanent reasonless global fence.
  [[nodiscard]] std::optional<PrivateLossFailure>
  record_global_loss_locked(const CriticalPrivateEventAttempt& attempt,
                            model::AdmissionOrdinal attempt_ordinal) noexcept;

  // --------------------------------------------------------
  // Validate a move-only capability against the exact active owner, admission generation, and turn.
  [[nodiscard]] model::Result<void>
  validate_admitted_private_order_slot(const AdmittedPrivateOrderSlot& admitted) const;

  // --------------------------------------------------------
  // Validate reconciliation capability identity, lifetime generation, and exact active owner turn.
  [[nodiscard]] model::Result<void> validate_admitted_reconciliation_event_slot(
      const AdmittedReconciliationEventSlot& admitted) const;

  // --------------------------------------------------------
  // Copy bounded private diagnostics while the caller already owns the synchronization mutex.
  [[nodiscard]] PrivateLaneSnapshot private_lane_snapshot_locked() const noexcept;

  // --------------------------------------------------------
  // Restore a failed in-flight fence ahead of any successor losses without unchecked addition.
  void restore_failed_fence_locked(std::size_t fence_index,
                                   const SourceDiscontinuity& discontinuity) noexcept;

  // --------------------------------------------------------
  // Restore a failed account-fence turn ahead of any successor interval in its sole fixed slot.
  [[nodiscard]] std::optional<PrivateLossFailure>
  restore_failed_account_fence_locked(AccountFenceState& fence,
                                      AccountSafetyFenceTurn failed) noexcept;

  // --------------------------------------------------------
  // Find the globally oldest runnable public, ordinary-private, or reconciliation value or the
  // oldest source, account, or global fence.
  [[nodiscard]] std::optional<RunnableTurnCandidate>
  find_oldest_runnable_turn_locked() const noexcept;

  // --------------------------------------------------------
  // Return whether any construction-time store currently contains an owner turn.
  [[nodiscard]] bool has_runnable_locked() const noexcept;

  // --------------------------------------------------------
  model::ClockProvider& clock_;
  SourceDiscontinuityHandler* discontinuity_handler_;
  std::optional<PrivateAdmissionConfiguration> private_configuration_;
  PrivateAdmissionOwner* private_owner_;
  std::optional<PrivateReservedErrors> private_reserved_errors_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::vector<QueuedWorkEntry> queue_;
  std::vector<SourceFenceState> fences_;
  std::vector<PrivateSlot> private_slots_;
  std::vector<ReconciliationSlot> reconciliation_slots_;
  std::vector<AccountFenceState> account_fences_;
  GlobalFenceState global_fence_;
  std::size_t head_{0U};
  std::size_t tail_{0U};
  std::size_t pending_commands_{0U};
  std::size_t pending_fences_{0U};
  std::size_t in_flight_fences_{0U};
  std::size_t private_head_{0U};
  std::size_t private_tail_{0U};
  std::size_t pending_private_commands_{0U};
  std::size_t reconciliation_head_{0U};
  std::size_t reconciliation_tail_{0U};
  std::size_t pending_reconciliation_commands_{0U};
  std::size_t maximum_drive_turns_;
  std::optional<model::AdmissionOrdinal> last_admission_ordinal_;
  std::optional<model::ReceiveSequence> last_receive_sequence_;
  std::optional<model::TurnOrdinal> last_turn_ordinal_;
  std::optional<std::uint64_t> last_clock_observation_;
  std::optional<std::thread::id> owner_thread_;
  std::optional<model::DomainError> terminal_error_;
  bool closed_{false};
  bool turn_active_{false};
  std::optional<ActivePrivateTurnState> active_private_turn_;
  std::optional<ActiveReconciliationTurnState> active_reconciliation_turn_;
  std::optional<PrivateAdmissionObservation> invalid_private_completion_observation_;
  std::optional<PrivateAdmissionObservation> invalid_reconciliation_completion_observation_;
  std::shared_ptr<PrivateAdmissionLease> private_admission_lease_;

  friend class DedicatedExecutorDriver;
  friend class AdmittedPrivateOrderSlot;
  friend class AdmittedReconciliationEventSlot;
};

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
