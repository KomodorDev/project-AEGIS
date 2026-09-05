// Purpose: implement fixed-capacity public, ordinary-private, and reconciliation admission with
// globally ordered command/fence turns, terminal fail-closed timing, and bounded owner progression.

#include "aegis/runtime/serialized_executor.hpp"

#include "aegis/model/domain_error.hpp"

#include <limits>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// Internal validation classification distinguishes a claimed disposition mismatch from a malformed
// retained completion before fixed failure storage is selected.
enum class PrivateCompletionFailure : std::uint8_t {
  None = 0,
  CommittedDisposition = 1,
  RetainedShape = 2,
};

// ########################################################################
// Fixed failure selection delays moving constructor-owned DomainErrors until an actual post-handler
// fault has already selected its exact stable cause.
enum class PrivateReservedFailure : std::uint8_t {
  None = 0,
  CommittedDisposition = 1,
  RetainedCompletion = 2,
  AccountLossCount = 3,
  GlobalLossCount = 4,
  ReconciliationCommittedDisposition = 5,
  ReconciliationRetainedCompletion = 6,
};

// ########################################################################

// --------------------------------------------------------
// Slot/fence commits mechanically depend only on bounded copies and moves that cannot throw.
static_assert(std::is_nothrow_move_constructible_v<model::DomainError>);
static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_copy_assignable_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_copy_constructible_v<oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(std::is_nothrow_copy_assignable_v<oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(std::is_nothrow_copy_constructible_v<CriticalPrivateEventAttempt>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<AccountSafetyReasonOccurrence>>);
static_assert(std::is_nothrow_swappable_v<std::optional<AccountSafetyReasonOccurrence>>);
static_assert(std::is_nothrow_copy_constructible_v<AccountSafetyFenceTurn>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<oms::NormalizedPrivateOrderInput>>);
static_assert(std::is_nothrow_move_assignable_v<std::optional<oms::PrivateEventResolution>>);
static_assert(std::is_nothrow_move_constructible_v<RetainedPrivateTurn>);
static_assert(std::is_nothrow_move_constructible_v<PrivateTurnCompletion>);
static_assert(std::is_nothrow_move_assignable_v<model::Result<void>>);
static_assert(static_cast<std::size_t>(risk::AccountSafetyReason::ProvenanceMismatch) ==
              account_safety_reason_occurrence_capacity);

// --------------------------------------------------------

// --------------------------------------------------------
// Accept only dispositions that the currently executing admission may commit as terminal evidence;
// AppliedFromBuffer belongs to an older buffered ordinal and BufferedGap remains nonterminal.
[[nodiscard]] static bool
is_directly_consumed_private_disposition(oms::PrivateEventDisposition disposition) noexcept {
  switch (disposition) {
  case oms::PrivateEventDisposition::Applied:
  case oms::PrivateEventDisposition::ProjectionOnly:
  case oms::PrivateEventDisposition::ExactEventDuplicate:
  case oms::PrivateEventDisposition::ExactTradeDuplicate:
  case oms::PrivateEventDisposition::SafetyContained:
  case oms::PrivateEventDisposition::ForbiddenRejected:
    return true;
  case oms::PrivateEventDisposition::BufferedGap:
  case oms::PrivateEventDisposition::AppliedFromBuffer:
    return false;
  }
  return false;
}

// --------------------------------------------------------
// Recognize a later AppliedFromBuffer transition as terminal when observing its older admission;
// only BufferedGap remains a successful but non-economic disposition.
[[nodiscard]] static bool
is_terminal_private_disposition(oms::PrivateEventDisposition disposition) noexcept {
  return disposition == oms::PrivateEventDisposition::AppliedFromBuffer ||
         is_directly_consumed_private_disposition(disposition);
}

// --------------------------------------------------------
// Accept only the stable account-safety assignments rather than trusting an enum's underlying byte.
[[nodiscard]] static bool
is_account_safety_reason_assigned(risk::AccountSafetyReason reason) noexcept {
  const auto value = static_cast<std::uint8_t>(reason);
  return value >= static_cast<std::uint8_t>(risk::AccountSafetyReason::SubmissionUnknown) &&
         value <= static_cast<std::uint8_t>(risk::AccountSafetyReason::ProvenanceMismatch);
}

// --------------------------------------------------------
// Validate retained progress against the exact admitted source and receipt. The projection is
// currently allocation-free, while catch-all containment keeps that invariant safe if it changes.
[[nodiscard]] static bool
is_retained_progress_valid(const RetainedPrivateTurn& retained,
                           const oms::PrivateEventIngressSemanticValue& admitted_semantic,
                           const AdmissionReceipt& receipt) noexcept {
  try {
    if (retained.first_admission_resolution().has_value() &&
        !retained.normalized_input().has_value()) {
      return false;
    }
    if (!retained.normalized_input().has_value()) {
      return true;
    }
    return retained.normalized_input()->receive_time() == receipt.received_at &&
           oms::PrivateEventIngressSemanticValue::from_normalized_input(
               retained.normalized_input().value()) == admitted_semantic;
  } catch (...) {
    return false;
  }
}

// --------------------------------------------------------
// Allocate separate fixed terminal and returned errors before private ingress is enabled.
std::optional<SerializedExecutor::PrivateReservedErrors>
SerializedExecutor::create_private_reserved_errors(bool private_enabled) {
  if (!private_enabled) {
    return std::nullopt;
  }
  return PrivateReservedErrors{
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "private_admission.find_committed_private_event_disposition"),
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "private_admission.find_committed_private_event_disposition"),
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "private_admission.find_committed_private_event_disposition"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "private_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "private_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "private_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.global_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.global_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.global_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "private_admission.global_fence_loss_count"),
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "reconciliation_admission.find_committed_reconciliation_event_disposition"),
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "reconciliation_admission.find_committed_reconciliation_event_disposition"),
      model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent,
          "reconciliation_admission.find_committed_reconciliation_event_disposition"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "reconciliation_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "reconciliation_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                          "reconciliation_admission.retained_completion"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "reconciliation_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "reconciliation_admission.account_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "reconciliation_admission.global_fence_loss_count"),
      model::DomainError::create_at_field(model::DomainErrorCode::ExecutorCounterExhausted,
                                          "reconciliation_admission.global_fence_loss_count"),
  };
}

// --------------------------------------------------------
// Construct the unattributable boundary without allocating unused source-fence storage.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, model::ClockProvider& clock,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{
          command_capacity,       0U, clock, nullptr, std::nullopt, nullptr, maximum_drive_turns,
          std::move(counter_seed)} {}

// --------------------------------------------------------
// Preallocate one persistent fence slot per configured canonical source ordinal.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                                       model::ClockProvider& clock,
                                       SourceDiscontinuityHandler& discontinuity_handler,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{command_capacity,       source_capacity,        clock,
                         &discontinuity_handler, std::nullopt,           nullptr,
                         maximum_drive_turns,    std::move(counter_seed)} {}

// --------------------------------------------------------
// Preallocate the sealed private reserve without installing an unrelated public-source table.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, model::ClockProvider& clock,
                                       PrivateAdmissionConfiguration private_configuration,
                                       PrivateAdmissionOwner& private_owner,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{
          command_capacity,
          0U,
          clock,
          nullptr,
          std::optional<PrivateAdmissionConfiguration>{std::move(private_configuration)},
          &private_owner,
          maximum_drive_turns,
          std::move(counter_seed)} {}

// --------------------------------------------------------
// Preallocate public-source and sealed private stores before either ingress becomes observable.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                                       model::ClockProvider& clock,
                                       SourceDiscontinuityHandler& discontinuity_handler,
                                       PrivateAdmissionConfiguration private_configuration,
                                       PrivateAdmissionOwner& private_owner,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{
          command_capacity,
          source_capacity,
          clock,
          &discontinuity_handler,
          std::optional<PrivateAdmissionConfiguration>{std::move(private_configuration)},
          &private_owner,
          maximum_drive_turns,
          std::move(counter_seed)} {}

// --------------------------------------------------------
// Complete every allocation and install bounds and boundary-test counters before ingress becomes
// observable.
SerializedExecutor::SerializedExecutor(
    std::size_t command_capacity, std::size_t source_capacity, model::ClockProvider& clock,
    SourceDiscontinuityHandler* discontinuity_handler,
    std::optional<PrivateAdmissionConfiguration> private_configuration,
    PrivateAdmissionOwner* private_owner, std::size_t maximum_drive_turns,
    ExecutorCounterSeed counter_seed)
    : clock_{clock}, discontinuity_handler_{discontinuity_handler},
      private_configuration_{std::move(private_configuration)}, private_owner_{private_owner},
      private_reserved_errors_{create_private_reserved_errors(private_configuration_.has_value())},
      queue_(command_capacity), fences_(source_capacity),
      private_slots_(
          private_configuration_.has_value()
              ? static_cast<std::size_t>(private_configuration_->private_admission_capacity())
              : 0U),
      reconciliation_slots_(private_configuration_.has_value()
                                ? static_cast<std::size_t>(
                                      private_configuration_->reconciliation_admission_capacity())
                                : 0U),
      maximum_drive_turns_{maximum_drive_turns},
      last_admission_ordinal_{counter_seed.last_admission_ordinal},
      last_receive_sequence_{counter_seed.last_receive_sequence},
      last_turn_ordinal_{counter_seed.last_turn_ordinal},
      private_admission_lease_{
          private_configuration_.has_value()
              ? std::shared_ptr<PrivateAdmissionLease>{new PrivateAdmissionLease{*this}}
              : nullptr} {

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy canonical account bindings into exactly one persistent slot per configured account after
  // the policy ceiling has proved that the complete count is representable.
  if (private_configuration_.has_value()) {
    account_fences_.reserve(private_configuration_->account_bindings().size());
    for (const auto& binding : private_configuration_->account_bindings()) {
      account_fences_.push_back(AccountFenceState{binding, std::nullopt, false});
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Invalidate the lease under its own mutex before any executor storage is destroyed, so a retained
// token either finishes validation against live state or observes a null owner without dereference.
SerializedExecutor::~SerializedExecutor() noexcept {
  if (private_admission_lease_ != nullptr) {
    std::lock_guard lock{private_admission_lease_->mutex_};
    private_admission_lease_->owner_ = nullptr;
  }
}

// --------------------------------------------------------
// Publish immutable token-owned values only while the executor proves the exact active private
// owner call stack, admission generation, and turn identity.
model::Result<AdmittedPrivateOrderSlotView>
AdmittedPrivateOrderSlot::inspect_admitted_private_order_slot() const {
  if (owner_ == nullptr) {
    return model::Result<AdmittedPrivateOrderSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.moved_token"));
  }
  auto lifetime = lifetime_.lock();
  if (lifetime == nullptr) {
    return model::Result<AdmittedPrivateOrderSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.expired_token"));
  }
  std::lock_guard lifetime_lock{lifetime->mutex_};
  if (lifetime->owner_ != owner_) {
    return model::Result<AdmittedPrivateOrderSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.expired_token"));
  }
  auto validation = owner_->validate_admitted_private_order_slot(*this);
  if (!validation) {
    return model::Result<AdmittedPrivateOrderSlotView>::create_failure(validation.error());
  }
  return model::Result<AdmittedPrivateOrderSlotView>::create_success(
      AdmittedPrivateOrderSlotView{attempt_, context_});
}

// --------------------------------------------------------
// Publish immutable reconciliation-token copies only while its lease and exact active owner turn
// remain valid; moved, stale, cross-executor, and post-destruction tokens fail without dereference.
model::Result<AdmittedReconciliationEventSlotView>
AdmittedReconciliationEventSlot::inspect_admitted_reconciliation_event_slot() const {
  if (owner_ == nullptr) {
    return model::Result<AdmittedReconciliationEventSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.moved_token"));
  }
  auto lifetime = lifetime_.lock();
  if (lifetime == nullptr) {
    return model::Result<AdmittedReconciliationEventSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.expired_token"));
  }
  std::lock_guard lifetime_lock{lifetime->mutex_};
  if (lifetime->owner_ != owner_) {
    return model::Result<AdmittedReconciliationEventSlotView>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.expired_token"));
  }
  auto validation = owner_->validate_admitted_reconciliation_event_slot(*this);
  if (!validation) {
    return model::Result<AdmittedReconciliationEventSlotView>::create_failure(validation.error());
  }
  return model::Result<AdmittedReconciliationEventSlotView>::create_success(
      AdmittedReconciliationEventSlotView{attempt_, context_});
}

// --------------------------------------------------------
// Assign every ordinary attempt exactly once, preserving capacity rejection as a successful
// decision and preventing source recovery from crossing a pending or active loss fence.
model::Result<AdmissionDecision>
SerializedExecutor::try_admit(InlineCommandWorkItem work,
                              std::optional<model::MarketSourceOrdinal> source_ordinal) {
  std::unique_lock lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // A terminal fault has precedence over ordinary closure because it can no longer assign replay
  // ordinals safely.
  if (terminal_error_.has_value()) {
    return model::Result<AdmissionDecision>::create_failure(terminal_error_.value());
  }
  auto next_attempt = derive_next_admission_ordinal_locked();
  if (!next_attempt) {
    const auto error = next_attempt.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Ordinary closure still consumes its attempt ordinal, but never reads the clock or creates a
  // source fence.
  if (closed_) {
    last_admission_ordinal_.emplace(next_attempt.value());
    return model::Result<AdmissionDecision>::create_success(
        AdmissionDecision{AdmissionOutcome::Closed, next_attempt.value(), pending_commands_,
                          queue_.size(), std::nullopt, false});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A source with pending or in-flight loss remains gated even when FIFO capacity has reopened.
  // Every such attributable attempt extends the current or successor fixed fence.
  auto* fence = configured_fence_locked(source_ordinal);
  const bool source_is_gated =
      fence != nullptr && (fence->earliest_failed_attempt.has_value() || fence->in_flight);
  if (source_is_gated || pending_commands_ == queue_.size()) {
    if (fence != nullptr) {
      auto loss = record_source_loss_locked(*fence, next_attempt.value());
      if (!loss) {
        const auto error = loss.error();
        fail_closed_locked(error);
        return model::Result<AdmissionDecision>::create_failure(error);
      }
    }
    last_admission_ordinal_.emplace(next_attempt.value());
    const auto observed_pending_depth = pending_commands_;
    const auto observed_pending_capacity = queue_.size();

    lock.unlock();
    if (fence != nullptr) {
      work_available_.notify_one();
    }
    return model::Result<AdmissionDecision>::create_success(AdmissionDecision{
        AdmissionOutcome::CapacityExceeded, next_attempt.value(), observed_pending_depth,
        observed_pending_capacity, std::nullopt, fence != nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive both accepted-only values before committing the attempt. Receive exhaustion and clock
  // regression consequently leave every counter and queue slot unchanged while failing closed.
  auto next_receive = derive_next_receive_sequence_locked();
  if (!next_receive) {
    const auto error = next_receive.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::create_failure(error);
  }
  const auto received_at = clock_.receive_timestamp_now();
  auto clock_observation =
      observe_clock_locked(received_at.nanoseconds(), "executor_admission_clock");
  if (!clock_observation) {
    const auto error = clock_observation.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the copied work and receipt as one mutex-protected ring transition.
  const auto receipt = AdmissionReceipt{next_attempt.value(), next_receive.value(), received_at,
                                        pending_commands_ + 1U, queue_.size()};
  auto& entry = queue_[tail_];
  entry.work.emplace(std::move(work));
  entry.receipt.emplace(receipt);
  tail_ = (tail_ + 1U) % queue_.size();
  ++pending_commands_;
  last_admission_ordinal_.emplace(next_attempt.value());
  last_receive_sequence_.emplace(next_receive.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Wake only after the complete queue entry and counters are visible.
  lock.unlock();
  work_available_.notify_one();
  return model::Result<AdmissionDecision>::create_success(
      AdmissionDecision{AdmissionOutcome::Accepted, next_attempt.value(), receipt.pending_depth,
                        receipt.pending_capacity, receipt, false});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Assign one shared attempt and either copy it into the private reserve or retain source ownership
// behind the exact configured-account or reasonless global loss gate.
model::Result<PrivateAdmissionDecision>
SerializedExecutor::try_admit_private(const oms::PrivateOrderIngressAttempt& attempt) {
  std::unique_lock lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Legacy executors have no private authority and reject before assigning any shared ordinal.
  if (!private_configuration_.has_value() || private_owner_ == nullptr) {
    return model::Result<PrivateAdmissionDecision>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "private_admission.disabled"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Terminal state and ordinal exhaustion precede every private decision, slot, or loss fence.
  if (terminal_error_.has_value()) {
    return model::Result<PrivateAdmissionDecision>::create_failure(terminal_error_.value());
  }
  auto next_attempt = derive_next_admission_ordinal_locked();
  if (!next_attempt) {
    const auto error = next_attempt.error();
    fail_closed_locked(error);
    return model::Result<PrivateAdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit closure consumes an attempt identity but performs no clock read, copy, or fencing.
  if (closed_) {
    last_admission_ordinal_.emplace(next_attempt.value());
    return model::Result<PrivateAdmissionDecision>::create_success(PrivateAdmissionDecision{
        AdmissionOutcome::Closed, next_attempt.value(), pending_private_commands_,
        private_slots_.size(), std::nullopt, false, std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve only an exact sealed root plus account/venue pair. A global gate takes precedence
  // because no ordinary private fact may cross it, even when this source pair is configured.
  auto* account_fence = find_configured_account_fence_locked(attempt.semantic_value());
  const bool global_is_gated = global_fence_.first_attempt.has_value();
  const bool account_is_gated =
      account_fence != nullptr && (account_fence->pending.has_value() || account_fence->in_flight);
  const bool requires_global_fence = global_is_gated || account_fence == nullptr;
  if (requires_global_fence || account_is_gated ||
      pending_private_commands_ == private_slots_.size()) {
    const bool use_account_fence = !requires_global_fence && account_fence != nullptr;
    const auto critical_attempt = CriticalPrivateEventAttempt{attempt};
    auto loss =
        use_account_fence
            ? record_account_loss_locked(*account_fence, critical_attempt, next_attempt.value(),
                                         risk::AccountSafetyReason::CriticalAdmissionLoss)
            : record_global_loss_locked(critical_attempt, next_attempt.value());
    if (loss.has_value()) {
      // The attempted ordinal is valid and therefore becomes the published high-water even though
      // checked loss-count exhaustion cannot create a decision or observation.
      auto& errors = private_reserved_errors_.value();
      auto terminal_error = use_account_fence ? std::move(errors.account_ingress_loss_terminal)
                                              : std::move(errors.global_ingress_loss_terminal);
      auto result_error = use_account_fence ? std::move(errors.account_ingress_loss_result)
                                            : std::move(errors.global_ingress_loss_result);
      last_admission_ordinal_.emplace(next_attempt.value());
      fail_closed_locked(std::move(terminal_error));
      return model::Result<PrivateAdmissionDecision>::create_failure(std::move(result_error));
    }
    last_admission_ordinal_.emplace(next_attempt.value());
    const auto observed_pending_depth = pending_private_commands_;
    const auto observed_pending_capacity = private_slots_.size();

    lock.unlock();
    work_available_.notify_one();
    return model::Result<PrivateAdmissionDecision>::create_success(PrivateAdmissionDecision{
        AdmissionOutcome::CapacityExceeded, next_attempt.value(), observed_pending_depth,
        observed_pending_capacity, std::nullopt, use_account_fence, std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive every accepted-only observation before mutating the slot or shared counter prefix.
  auto next_receive = derive_next_receive_sequence_locked();
  if (!next_receive) {
    const auto error = next_receive.error();
    fail_closed_locked(error);
    return model::Result<PrivateAdmissionDecision>::create_failure(error);
  }
  const auto received_at = clock_.receive_timestamp_now();
  auto clock_observation =
      observe_clock_locked(received_at.nanoseconds(), "private_admission_clock");
  if (!clock_observation) {
    const auto error = clock_observation.error();
    fail_closed_locked(error);
    return model::Result<PrivateAdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copies of the bounded attempt are allocation-free; publish slot, receipt, and both shared
  // counters together only after the complete accepted value exists.
  const auto receipt = AdmissionReceipt{next_attempt.value(), next_receive.value(), received_at,
                                        pending_private_commands_ + 1U, private_slots_.size()};
  auto& slot = private_slots_[private_tail_];
  slot.attempt.emplace(attempt);
  slot.receipt.emplace(receipt);
  private_tail_ = (private_tail_ + 1U) % private_slots_.size();
  ++pending_private_commands_;
  last_admission_ordinal_.emplace(next_attempt.value());
  last_receive_sequence_.emplace(next_receive.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Wake only after the copied source fact, receipt, and lifecycle state are jointly visible.
  lock.unlock();
  work_available_.notify_one();
  return model::Result<PrivateAdmissionDecision>::create_success(PrivateAdmissionDecision{
      AdmissionOutcome::Accepted, next_attempt.value(), receipt.pending_depth,
      receipt.pending_capacity, receipt, false, CriticalPrivateAdmissionState::CopiedAndAdmitted});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Assign one shared attempt and either copy an authoritative row into the reconciliation reserve or
// retain source ownership behind its exact configured-account or reasonless global loss fence.
model::Result<ReconciliationAdmissionDecision> SerializedExecutor::try_admit_reconciliation_event(
    const oms::ReconciliationPrivateEventIngressAttempt& attempt) {
  std::unique_lock lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Legacy executors have no trusted reconciliation authority and reject before assigning an
  // ordinal that they cannot safely own.
  if (!private_configuration_.has_value() || private_owner_ == nullptr) {
    return model::Result<ReconciliationAdmissionDecision>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.disabled"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Terminal state and global ordinal exhaustion precede every reconciliation decision or copy.
  if (terminal_error_.has_value()) {
    return model::Result<ReconciliationAdmissionDecision>::create_failure(terminal_error_.value());
  }
  auto next_attempt = derive_next_admission_ordinal_locked();
  if (!next_attempt) {
    const auto error = next_attempt.error();
    fail_closed_locked(error);
    return model::Result<ReconciliationAdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit closure consumes its shared attempt identity without reading the clock or fencing.
  if (closed_) {
    last_admission_ordinal_.emplace(next_attempt.value());
    return model::Result<ReconciliationAdmissionDecision>::create_success(
        ReconciliationAdmissionDecision{
            AdmissionOutcome::Closed, next_attempt.value(), pending_reconciliation_commands_,
            reconciliation_slots_.size(), std::nullopt, false, std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A pending account interval must become owner-local before later reconciliation for that
  // account. The permanent global-private gate is deliberately absent here: trusted reconciliation
  // is the only lane that may eventually resolve its source.
  auto* account_fence = find_configured_account_fence_locked(attempt.semantic_value());
  const bool account_is_gated =
      account_fence != nullptr && (account_fence->pending.has_value() || account_fence->in_flight);
  const bool requires_global_fence = account_fence == nullptr;
  if (requires_global_fence || account_is_gated ||
      pending_reconciliation_commands_ == reconciliation_slots_.size()) {
    const bool use_account_fence = !requires_global_fence && account_fence != nullptr;
    const auto critical_attempt = CriticalPrivateEventAttempt{attempt};
    auto loss =
        use_account_fence
            ? record_account_loss_locked(*account_fence, critical_attempt, next_attempt.value(),
                                         risk::AccountSafetyReason::CriticalAdmissionLoss)
            : record_global_loss_locked(critical_attempt, next_attempt.value());
    if (loss.has_value()) {
      auto& errors = private_reserved_errors_.value();
      auto terminal_error = use_account_fence
                                ? std::move(errors.reconciliation_account_ingress_loss_terminal)
                                : std::move(errors.reconciliation_global_ingress_loss_terminal);
      auto result_error = use_account_fence
                              ? std::move(errors.reconciliation_account_ingress_loss_result)
                              : std::move(errors.reconciliation_global_ingress_loss_result);
      last_admission_ordinal_.emplace(next_attempt.value());
      fail_closed_locked(std::move(terminal_error));
      return model::Result<ReconciliationAdmissionDecision>::create_failure(
          std::move(result_error));
    }
    last_admission_ordinal_.emplace(next_attempt.value());
    const auto observed_pending_depth = pending_reconciliation_commands_;
    const auto observed_pending_capacity = reconciliation_slots_.size();

    lock.unlock();
    work_available_.notify_one();
    return model::Result<ReconciliationAdmissionDecision>::create_success(
        ReconciliationAdmissionDecision{AdmissionOutcome::CapacityExceeded, next_attempt.value(),
                                        observed_pending_depth, observed_pending_capacity,
                                        std::nullopt, use_account_fence, std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive accepted-only receive identity and time before publishing any slot or shared counter.
  auto next_receive = derive_next_receive_sequence_locked();
  if (!next_receive) {
    const auto error = next_receive.error();
    fail_closed_locked(error);
    return model::Result<ReconciliationAdmissionDecision>::create_failure(error);
  }
  const auto received_at = clock_.receive_timestamp_now();
  auto clock_observation =
      observe_clock_locked(received_at.nanoseconds(), "reconciliation_admission_clock");
  if (!clock_observation) {
    const auto error = clock_observation.error();
    fail_closed_locked(error);
    return model::Result<ReconciliationAdmissionDecision>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the bounded authoritative copy, receipt, and shared counters as one synchronized ring
  // transition; neither ordinary private nor public capacity participates in this depth.
  const auto receipt =
      AdmissionReceipt{next_attempt.value(), next_receive.value(), received_at,
                       pending_reconciliation_commands_ + 1U, reconciliation_slots_.size()};
  auto& slot = reconciliation_slots_[reconciliation_tail_];
  slot.attempt.emplace(attempt);
  slot.receipt.emplace(receipt);
  reconciliation_tail_ = (reconciliation_tail_ + 1U) % reconciliation_slots_.size();
  ++pending_reconciliation_commands_;
  last_admission_ordinal_.emplace(next_attempt.value());
  last_receive_sequence_.emplace(next_receive.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Wake only after the complete trusted event, receipt, and lifecycle state are jointly visible.
  lock.unlock();
  work_available_.notify_one();
  return model::Result<ReconciliationAdmissionDecision>::create_success(
      ReconciliationAdmissionDecision{AdmissionOutcome::Accepted, next_attempt.value(),
                                      receipt.pending_depth, receipt.pending_capacity, receipt,
                                      false, CriticalPrivateAdmissionState::CopiedAndAdmitted});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Closure is idempotent and retains the complete admitted/fenced prefix for owner drainage.
void SerializedExecutor::close() noexcept {
  {
    std::lock_guard lock{mutex_};
    closed_ = true;
  }
  work_available_.notify_all();
}

// --------------------------------------------------------
// Latch a fault raised after owner-local mutation while leaving execute_next_turn responsible for
// publishing the active handler's successful completion before that fault surfaces at the next
// boundary.
model::Result<void> SerializedExecutor::request_owner_fault(model::DomainError error) noexcept {
  std::lock_guard lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject callers without the current owner authority before lifecycle state can change.
  if (!owner_thread_.has_value()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // This seam is post-mutation authority for a running handler, never an out-of-turn close API.
  if (!turn_active_) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorReentryDetected, "executor_owner_fault"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the first cause, close later admission, and wake a dedicated driver. The active
  // handler still owns its stack and may return success so execute_next_turn can publish its
  // TurnReport.
  fail_closed_locked(std::move(error));
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Binding is idempotent for the current thread and rejects an implicit concurrent handoff.
model::Result<void> SerializedExecutor::bind_to_current_thread() {
  std::lock_guard lock{mutex_};
  const auto current_thread = std::this_thread::get_id();
  if (owner_thread_.has_value() && owner_thread_.value() != current_thread) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  owner_thread_ = current_thread;
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Release requires the bound owner and refuses a handoff from inside active owner work.
model::Result<void> SerializedExecutor::release_from_current_thread() {
  std::lock_guard lock{mutex_};
  const auto validation = validate_owner_locked();
  if (!validation) {
    return validation;
  }
  owner_thread_.reset();
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Public deterministic progression has no dedicated-stop gate.
model::Result<std::optional<TurnReport>> SerializedExecutor::execute_next_turn() {
  return execute_next_turn_impl(nullptr);
}

// --------------------------------------------------------
// Select, validate, remove, and execute at most one command or fence in global attempt order.
model::Result<std::optional<TurnReport>>
SerializedExecutor::execute_next_turn_impl(const std::atomic_bool* stop_requested) {
  std::optional<InlineCommandWorkItem> work;
  std::optional<AdmissionReceipt> receipt;
  std::optional<SourceDiscontinuity> discontinuity;
  std::optional<AccountSafetyFenceTurn> account_fence_turn;
  std::optional<GlobalPrivateFenceTurn> global_fence_turn;
  std::optional<RunnableTurnCandidate> selected_candidate;
  std::optional<AcceptedTurnContext> accepted_context;
  std::optional<ControlTurnContext> control_context;
  std::optional<oms::PrivateOrderIngressAttempt> private_attempt;
  std::optional<oms::ReconciliationPrivateEventIngressAttempt> reconciliation_attempt;
  std::size_t pending_commands_at_start = 0U;
  std::size_t pending_fences_at_start = 0U;
  TurnKind turn_kind = TurnKind::Command;

  {
    std::lock_guard lock{mutex_};

    // ++++++++++++++++++++++++++++++++++++++++
    // Ownership and terminal lifecycle checks precede any runnable-state mutation.
    const auto owner_validation = validate_owner_locked();
    if (!owner_validation) {
      return model::Result<std::optional<TurnReport>>::create_failure(owner_validation.error());
    }
    if (terminal_error_.has_value()) {
      return model::Result<std::optional<TurnReport>>::create_failure(terminal_error_.value());
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Dedicated stop wins an open-ingress turn only when it acquires this mutex before turn start.
    // Closed-prefix drainage deliberately retains precedence over the same stop request.
    if (stop_requested != nullptr && stop_requested->load() && !closed_) {
      return model::Result<std::optional<TurnReport>>::create_success(std::nullopt);
    }
    const auto candidate = find_oldest_runnable_turn_locked();
    if (!candidate.has_value()) {
      return model::Result<std::optional<TurnReport>>::create_success(std::nullopt);
    }
    selected_candidate.emplace(candidate.value());

    // ++++++++++++++++++++++++++++++++++++++++
    // Validate the next turn ordinal and sole processing-time observation while the selected head
    // remains intact, so either failure closes without invoking mutable owner work.
    auto next_turn = derive_next_turn_ordinal_locked();
    if (!next_turn) {
      const auto error = next_turn.error();
      fail_closed_locked(error);
      return model::Result<std::optional<TurnReport>>::create_failure(error);
    }
    const auto processing_start = clock_.processing_timestamp_now();
    auto clock_observation =
        observe_clock_locked(processing_start.nanoseconds(), "executor_processing_clock");
    if (!clock_observation) {
      const auto error = clock_observation.error();
      fail_closed_locked(error);
      return model::Result<std::optional<TurnReport>>::create_failure(error);
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Extract exactly the selected physical value and build only its matching stack-scoped
    // authority. Every fence remains explicitly in-flight while producers fold later losses.
    if (candidate->source == RunnableSource::Command) {
      turn_kind = TurnKind::Command;
      auto& entry = queue_[head_];
      const auto processing_delay =
          processing_start.nanoseconds() - entry.receipt->received_at.nanoseconds();
      work.emplace(std::move(entry.work.value()));
      receipt.emplace(entry.receipt.value());
      accepted_context.emplace(AcceptedTurnContext{entry.receipt.value(), next_turn.value(),
                                                   processing_start,
                                                   model::ElapsedNanoseconds{processing_delay}});
      entry.work.reset();
      entry.receipt.reset();
      head_ = (head_ + 1U) % queue_.size();
      --pending_commands_;
    } else if (candidate->source == RunnableSource::SourceFence) {
      turn_kind = TurnKind::SourceDiscontinuity;
      auto& fence = fences_[candidate->index];
      const auto source_ordinal =
          model::MarketSourceOrdinal::from_value(candidate->index + 1U).value();
      discontinuity.emplace(SourceDiscontinuity{
          source_ordinal, fence.earliest_failed_attempt.value(), fence.lost_attempt_count});
      control_context.emplace(ControlTurnContext{next_turn.value(), processing_start});
      fence.earliest_failed_attempt.reset();
      fence.lost_attempt_count = 0U;
      fence.in_flight = true;
      --pending_fences_;
      ++in_flight_fences_;
    } else if (candidate->source == RunnableSource::PrivateCommand) {
      turn_kind = TurnKind::PrivateCommand;
      auto& slot = private_slots_[candidate->index];
      const auto processing_delay =
          processing_start.nanoseconds() - slot.receipt->received_at.nanoseconds();
      receipt.emplace(slot.receipt.value());
      private_attempt.emplace(slot.attempt.value());
      accepted_context.emplace(AcceptedTurnContext{slot.receipt.value(), next_turn.value(),
                                                   processing_start,
                                                   model::ElapsedNanoseconds{processing_delay}});
      active_private_turn_.emplace(
          ActivePrivateTurnState{slot.attempt.value(), slot.receipt.value(), next_turn.value()});
      slot.attempt.reset();
      slot.receipt.reset();
      private_head_ = (private_head_ + 1U) % private_slots_.size();
      --pending_private_commands_;
    } else if (candidate->source == RunnableSource::ReconciliationCommand) {
      turn_kind = TurnKind::ReconciliationCommand;
      auto& slot = reconciliation_slots_[candidate->index];
      const auto processing_delay =
          processing_start.nanoseconds() - slot.receipt->received_at.nanoseconds();
      receipt.emplace(slot.receipt.value());
      reconciliation_attempt.emplace(slot.attempt.value());
      accepted_context.emplace(AcceptedTurnContext{slot.receipt.value(), next_turn.value(),
                                                   processing_start,
                                                   model::ElapsedNanoseconds{processing_delay}});
      active_reconciliation_turn_.emplace(ActiveReconciliationTurnState{
          slot.attempt.value(), slot.receipt.value(), next_turn.value()});
      slot.attempt.reset();
      slot.receipt.reset();
      reconciliation_head_ = (reconciliation_head_ + 1U) % reconciliation_slots_.size();
      --pending_reconciliation_commands_;
    } else if (candidate->source == RunnableSource::AccountFence) {
      turn_kind = TurnKind::AccountSafetyFence;
      auto& fence = account_fences_[candidate->index];
      auto interval = std::move(fence.pending).value();
      fence.pending.reset();
      fence.in_flight = true;
      account_fence_turn.emplace(AccountSafetyFenceTurn{
          fence.binding.logical_account_id, fence.binding.venue_id, interval.lost_attempt_count,
          std::move(interval.ordered_unique_reason_occurrences), interval.reason_occurrence_count});
      control_context.emplace(ControlTurnContext{next_turn.value(), processing_start});
    } else {
      turn_kind = TurnKind::GlobalPrivateFence;
      global_fence_turn.emplace(GlobalPrivateFenceTurn{
          global_fence_.first_attempt.value(), global_fence_.earliest_attempt_ordinal.value(),
          global_fence_.lost_attempt_count});
      control_context.emplace(ControlTurnContext{next_turn.value(), processing_start});
      global_fence_.in_flight = true;
    }
    pending_commands_at_start = pending_commands_;
    pending_fences_at_start = pending_fences_;
    turn_active_ = true;

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Invoke exactly one owner surface outside the executor mutex. Private completion is a closed
  // value; account/global handlers retain Result failure for their no-mutation restoration rule.
  auto handler_result = model::Result<void>::create_success();
  std::optional<PrivateTurnCompletion> private_completion;
  std::optional<oms::PrivateEventDisposition> find_committed_private_event_disposition;
  const model::DomainError* committed_retention_error = nullptr;
  std::optional<model::DomainError> reserved_result_error;
  auto private_completion_failure = PrivateCompletionFailure::None;
  auto reserved_failure = PrivateReservedFailure::None;
  if (turn_kind == TurnKind::Command) {
    handler_result = work->execute_work(accepted_context.value());
  } else if (turn_kind == TurnKind::SourceDiscontinuity) {
    handler_result = discontinuity_handler_->on_source_discontinuity(discontinuity.value(),
                                                                     control_context.value());
  } else if (turn_kind == TurnKind::PrivateCommand) {
    private_completion.emplace(private_owner_->commit_private_order_turn(
        AdmittedPrivateOrderSlot{*this, receipt->attempt_ordinal, private_admission_lease_,
                                 private_attempt.value(), accepted_context.value()}));
    if (const auto* consumed = std::get_if<ConsumedPrivateTurn>(&private_completion.value())) {
      find_committed_private_event_disposition =
          private_owner_->find_committed_private_event_disposition(receipt->attempt_ordinal);
      if (!is_directly_consumed_private_disposition(consumed->disposition) ||
          find_committed_private_event_disposition != consumed->disposition) {
        private_completion_failure = PrivateCompletionFailure::CommittedDisposition;
      }
    } else if (std::holds_alternative<BufferedPrivateTurn>(private_completion.value())) {
      find_committed_private_event_disposition =
          private_owner_->find_committed_private_event_disposition(receipt->attempt_ordinal);
      if (find_committed_private_event_disposition != oms::PrivateEventDisposition::BufferedGap) {
        private_completion_failure = PrivateCompletionFailure::CommittedDisposition;
      }
    } else {
      committed_retention_error =
          private_owner_->find_committed_retained_private_event_error(receipt->attempt_ordinal);
    }
  } else if (turn_kind == TurnKind::ReconciliationCommand) {
    private_completion.emplace(private_owner_->commit_reconciliation_event_turn(
        AdmittedReconciliationEventSlot{*this, receipt->attempt_ordinal, private_admission_lease_,
                                        reconciliation_attempt.value(), accepted_context.value()}));
    if (const auto* consumed = std::get_if<ConsumedPrivateTurn>(&private_completion.value())) {
      find_committed_private_event_disposition =
          private_owner_->find_committed_reconciliation_event_disposition(receipt->attempt_ordinal);
      if (!is_directly_consumed_private_disposition(consumed->disposition) ||
          find_committed_private_event_disposition != consumed->disposition) {
        private_completion_failure = PrivateCompletionFailure::CommittedDisposition;
      }
    } else if (std::holds_alternative<BufferedPrivateTurn>(private_completion.value())) {
      find_committed_private_event_disposition =
          private_owner_->find_committed_reconciliation_event_disposition(receipt->attempt_ordinal);
      if (find_committed_private_event_disposition != oms::PrivateEventDisposition::BufferedGap) {
        private_completion_failure = PrivateCompletionFailure::CommittedDisposition;
      }
    } else {
      committed_retention_error =
          private_owner_->find_committed_retained_reconciliation_event_error(
              receipt->attempt_ordinal);
    }
  } else if (account_fence_turn.has_value()) {
    handler_result = private_owner_->apply_account_safety_fence(account_fence_turn.value(),
                                                                control_context.value());
  } else {
    handler_result = private_owner_->apply_global_private_fence(global_fence_turn.value(),
                                                                control_context.value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reconcile the selected physical store before publishing failure or success. Every branch
  // preserves an admitted or fenced fact until its exact terminal authority is independently held.
  std::lock_guard lock{mutex_};
  if (turn_kind == TurnKind::SourceDiscontinuity) {
    if (!handler_result) {
      restore_failed_fence_locked(selected_candidate->index, discontinuity.value());
    } else {
      fences_[selected_candidate->index].in_flight = false;
      --in_flight_fences_;
    }
  } else if (turn_kind == TurnKind::PrivateCommand ||
             turn_kind == TurnKind::ReconciliationCommand) {
    const bool reconciliation_turn = turn_kind == TurnKind::ReconciliationCommand;
    const auto active_attempt =
        reconciliation_turn ? CriticalPrivateEventAttempt{active_reconciliation_turn_->attempt}
                            : CriticalPrivateEventAttempt{active_private_turn_->attempt};
    const auto& active_receipt =
        reconciliation_turn ? active_reconciliation_turn_->receipt : active_private_turn_->receipt;
    const auto* active_semantic = reconciliation_turn
                                      ? &active_reconciliation_turn_->attempt.semantic_value()
                                      : &active_private_turn_->attempt.semantic_value();
    if (private_completion_failure == PrivateCompletionFailure::None &&
        std::holds_alternative<RetainedPrivateTurn>(private_completion.value())) {
      auto& retained = std::get<RetainedPrivateTurn>(private_completion.value());
      auto* configured = find_configured_account_fence_locked(*active_semantic);
      const bool progress_is_valid =
          is_retained_progress_valid(retained, *active_semantic, active_receipt);
      const bool reason_shape_is_valid =
          configured != nullptr
              ? retained.account_safety_reason().has_value() &&
                    is_account_safety_reason_assigned(retained.account_safety_reason().value())
              : !retained.account_safety_reason().has_value();
      const bool retained_evidence_is_committed =
          committed_retention_error != nullptr &&
          *committed_retention_error == retained.retention_error();
      if (!progress_is_valid || !reason_shape_is_valid || !retained_evidence_is_committed) {
        private_completion_failure = PrivateCompletionFailure::RetainedShape;
      } else {
        const auto account_reason = retained.account_reason_;
        auto containment =
            configured != nullptr
                ? record_account_loss_locked(*configured, active_attempt,
                                             active_receipt.attempt_ordinal, account_reason.value())
                : record_global_loss_locked(active_attempt, active_receipt.attempt_ordinal);
        if (containment.has_value()) {
          reserved_failure = configured != nullptr ? PrivateReservedFailure::AccountLossCount
                                                   : PrivateReservedFailure::GlobalLossCount;
        }
      }
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // An invalid completion is itself a post-copy failure: preserve the complete active source in
    // conservative containment before faulting, never falsely publishing a terminal observation.
    if (private_completion_failure != PrivateCompletionFailure::None) {
      auto& errors = private_reserved_errors_.value();
      auto& invalid_observation = reconciliation_turn
                                      ? invalid_reconciliation_completion_observation_
                                      : invalid_private_completion_observation_;
      if (private_completion_failure == PrivateCompletionFailure::CommittedDisposition) {
        auto& observation_error = reconciliation_turn ? errors.reconciliation_committed_observation
                                                      : errors.committed_observation;
        invalid_observation.emplace(
            PrivateAdmissionObservation{active_receipt.attempt_ordinal,
                                        CriticalPrivateAdmissionState::RetainedForReconciliation,
                                        std::nullopt, std::move(observation_error)});
        reserved_failure = reconciliation_turn
                               ? PrivateReservedFailure::ReconciliationCommittedDisposition
                               : PrivateReservedFailure::CommittedDisposition;
      } else {
        auto& observation_error = reconciliation_turn ? errors.reconciliation_retained_observation
                                                      : errors.retained_observation;
        invalid_observation.emplace(
            PrivateAdmissionObservation{active_receipt.attempt_ordinal,
                                        CriticalPrivateAdmissionState::RetainedForReconciliation,
                                        std::nullopt, std::move(observation_error)});
        reserved_failure = reconciliation_turn
                               ? PrivateReservedFailure::ReconciliationRetainedCompletion
                               : PrivateReservedFailure::RetainedCompletion;
      }
      auto* configured = find_configured_account_fence_locked(*active_semantic);
      auto containment =
          configured != nullptr
              ? record_account_loss_locked(*configured, active_attempt,
                                           active_receipt.attempt_ordinal,
                                           risk::AccountSafetyReason::CriticalAdmissionLoss)
              : record_global_loss_locked(active_attempt, active_receipt.attempt_ordinal);
      if (containment.has_value()) {
        reserved_failure = configured != nullptr ? PrivateReservedFailure::AccountLossCount
                                                 : PrivateReservedFailure::GlobalLossCount;
      }
    }
    if (reconciliation_turn) {
      active_reconciliation_turn_.reset();
    } else {
      active_private_turn_.reset();
    }
  } else if (turn_kind == TurnKind::AccountSafetyFence && account_fence_turn.has_value()) {
    auto& fence = account_fences_[selected_candidate->index];
    if (handler_result) {
      fence.in_flight = false;
    } else {
      auto restoration =
          restore_failed_account_fence_locked(fence, std::move(account_fence_turn).value());
      if (restoration.has_value()) {
        reserved_failure = PrivateReservedFailure::AccountLossCount;
      }
    }
  } else if (turn_kind == TurnKind::GlobalPrivateFence) {
    global_fence_.in_flight = false;
    if (handler_result) {
      global_fence_.owner_applied = true;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Select constructor-owned terminal/result copies only after exact failure classification;
  // ingress races own separate reserved pairs.
  if (reserved_failure != PrivateReservedFailure::None) {
    auto& errors = private_reserved_errors_.value();
    model::DomainError* terminal_error = nullptr;
    model::DomainError* result_error = nullptr;
    switch (reserved_failure) {
    case PrivateReservedFailure::CommittedDisposition:
      terminal_error = &errors.committed_terminal;
      result_error = &errors.committed_result;
      break;
    case PrivateReservedFailure::RetainedCompletion:
      terminal_error = &errors.retained_terminal;
      result_error = &errors.retained_result;
      break;
    case PrivateReservedFailure::AccountLossCount:
      terminal_error = &errors.account_owner_loss_terminal;
      result_error = &errors.account_owner_loss_result;
      break;
    case PrivateReservedFailure::GlobalLossCount:
      terminal_error = &errors.global_owner_loss_terminal;
      result_error = &errors.global_owner_loss_result;
      break;
    case PrivateReservedFailure::ReconciliationCommittedDisposition:
      terminal_error = &errors.reconciliation_committed_terminal;
      result_error = &errors.reconciliation_committed_result;
      break;
    case PrivateReservedFailure::ReconciliationRetainedCompletion:
      terminal_error = &errors.reconciliation_retained_terminal;
      result_error = &errors.reconciliation_retained_result;
      break;
    case PrivateReservedFailure::None:
      break;
    }
    handler_result = model::Result<void>::create_failure(std::move(*terminal_error));
    reserved_result_error.emplace(std::move(*result_error));
  }
  turn_active_ = false;

  // ++++++++++++++++++++++++++++++++++++++++
  // Handler failure consumes accepted work but publishes no completed turn. The first terminal
  // fault remains stable if another producer fault raced the active handler.
  if (!handler_result) {
    const bool terminal_was_already_present = terminal_error_.has_value();
    fail_closed_locked(std::move(handler_result).error());
    if (!terminal_was_already_present && reserved_result_error.has_value()) {
      return model::Result<std::optional<TurnReport>>::create_failure(
          std::move(reserved_result_error).value());
    }
    return model::Result<std::optional<TurnReport>>::create_failure(terminal_error_.value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Successful applied work always advances and reports, even when a producer published a terminal
  // admission fault during the active handler. That stored fault remains closed and is returned by
  // the next progression boundary rather than erasing evidence for this completed turn.
  const bool accepted_turn = turn_kind == TurnKind::Command ||
                             turn_kind == TurnKind::PrivateCommand ||
                             turn_kind == TurnKind::ReconciliationCommand;
  const auto turn_ordinal =
      accepted_turn ? accepted_context->turn_ordinal : control_context->turn_ordinal;
  const auto started_at = accepted_turn ? accepted_context->processing_timestamp
                                        : control_context->processing_timestamp;
  last_turn_ordinal_.emplace(turn_ordinal);
  const auto completed_turns = turn_ordinal.value();
  const auto attempt_ordinal = selected_candidate->attempt_ordinal;
  const auto report = TurnReport{
      turn_kind,
      turn_ordinal,
      attempt_ordinal,
      accepted_turn ? std::optional<model::ReceiveSequence>{receipt->receive_sequence}
                    : std::nullopt,
      accepted_turn ? std::optional<model::ReceiveTimestamp>{receipt->received_at} : std::nullopt,
      started_at,
      accepted_turn ? std::optional<model::ElapsedNanoseconds>{accepted_context->queue_age}
                    : std::nullopt,
      discontinuity,
      pending_commands_at_start,
      pending_commands_,
      pending_fences_at_start,
      pending_fences_,
      queue_.size(),
      completed_turns,
      private_lane_snapshot_locked(),
  };
  return model::Result<std::optional<TurnReport>>::create_success(report);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Bounded progression validates even a zero-turn call and rejects policy-bound violations before
// starting any owner turn.
model::Result<PendingTurnExecutionReport>
SerializedExecutor::execute_pending_turns(std::size_t max_turns) {
  {
    std::lock_guard lock{mutex_};
    const auto owner_validation = validate_owner_locked();
    if (!owner_validation) {
      return model::Result<PendingTurnExecutionReport>::create_failure(owner_validation.error());
    }
    if (terminal_error_.has_value()) {
      return model::Result<PendingTurnExecutionReport>::create_failure(terminal_error_.value());
    }
    if (max_turns > maximum_drive_turns_) {
      return model::Result<PendingTurnExecutionReport>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "maximum_drive_turns"));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Stop at the explicit turn bound or the first observation with neither command nor fence.
  std::size_t turns_executed = 0U;
  while (turns_executed < max_turns) {
    auto turn = execute_next_turn();
    if (!turn) {
      return model::Result<PendingTurnExecutionReport>::create_failure(std::move(turn).error());
    }
    if (!turn.value().has_value()) {
      break;
    }
    ++turns_executed;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Snapshot all remaining bounded work once after progression.
  const auto state = queue_snapshot();
  return model::Result<PendingTurnExecutionReport>::create_success(PendingTurnExecutionReport{
      turns_executed, state.pending_commands, state.pending_fences, state.command_capacity,
      state.maximum_drive_turns, state.completed_turns, state.private_lane});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Read every related queue, fence, owner, bound, and lifecycle field under one synchronization
// boundary.
ExecutorQueueSnapshot SerializedExecutor::queue_snapshot() const noexcept {
  std::lock_guard lock{mutex_};
  return ExecutorQueueSnapshot{pending_commands_,
                               pending_fences_,
                               in_flight_fences_,
                               queue_.size(),
                               fences_.size(),
                               maximum_drive_turns_,
                               last_turn_ordinal_.has_value() ? last_turn_ordinal_->value() : 0U,
                               closed_,
                               terminal_error_.has_value(),
                               owner_thread_.has_value(),
                               turn_active_,
                               private_lane_snapshot_locked()};
}

// --------------------------------------------------------
// Compare the caller with the synchronized owner token without publishing a thread identifier.
bool SerializedExecutor::is_current_thread_owner() const noexcept {
  std::lock_guard lock{mutex_};
  return owner_thread_.has_value() && owner_thread_.value() == std::this_thread::get_id();
}

// --------------------------------------------------------
// Resolve and copy one source's loss state under the same mutex used for folding and restoration.
std::optional<SourceFenceSnapshot> SerializedExecutor::source_fence_snapshot(
    model::MarketSourceOrdinal source_ordinal) const noexcept {
  std::lock_guard lock{mutex_};
  if (source_ordinal.value() > fences_.size()) {
    return std::nullopt;
  }
  const auto& fence = fences_[static_cast<std::size_t>(source_ordinal.value() - 1U)];
  return SourceFenceSnapshot{source_ordinal, fence.earliest_failed_attempt,
                             fence.lost_attempt_count, fence.in_flight};
}

// --------------------------------------------------------
// Prefer pending or active executor state over append-only owner evidence so a concurrent observer
// cannot expose a premature terminal state during the mutex-linearized extraction transition.
std::optional<PrivateAdmissionObservation>
SerializedExecutor::private_admission_observation(model::AdmissionOrdinal attempt_ordinal) const {
  PrivateAdmissionOwner* private_owner = nullptr;
  {
    std::lock_guard lock{mutex_};
    for (const auto& slot : private_slots_) {
      if (!slot.receipt.has_value() || slot.receipt->attempt_ordinal != attempt_ordinal) {
        continue;
      }
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::CopiedAndAdmitted,
                                         std::nullopt, std::nullopt};
    }
    if (active_private_turn_.has_value() &&
        active_private_turn_->receipt.attempt_ordinal == attempt_ordinal) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::CopiedAndAdmitted,
                                         std::nullopt, std::nullopt};
    }
    if (invalid_private_completion_observation_.has_value() &&
        invalid_private_completion_observation_->attempt_ordinal == attempt_ordinal) {
      return invalid_private_completion_observation_;
    }
    private_owner = private_owner_;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A completed turn is absent from executor storage only after the owner's separately synchronized
  // oracle has published its exact committed evidence or retained error.
  if (private_owner != nullptr) {
    auto disposition = private_owner->find_committed_private_event_disposition(attempt_ordinal);
    if (disposition.has_value() && is_terminal_private_disposition(disposition.value())) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::EconomicallyConsumed,
                                         disposition, std::nullopt};
    }
    const auto* retention_error =
        private_owner->find_committed_retained_private_event_error(attempt_ordinal);
    if (retention_error != nullptr) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::RetainedForReconciliation,
                                         std::nullopt, *retention_error};
    }
  }
  return std::nullopt;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Prefer the reconciliation ring or active token over its lane-specific append-only owner evidence,
// preventing cross-lane ordinal matches during concurrent extraction and publication.
std::optional<PrivateAdmissionObservation> SerializedExecutor::reconciliation_admission_observation(
    model::AdmissionOrdinal attempt_ordinal) const {
  PrivateAdmissionOwner* private_owner = nullptr;
  {
    std::lock_guard lock{mutex_};
    for (const auto& slot : reconciliation_slots_) {
      if (!slot.receipt.has_value() || slot.receipt->attempt_ordinal != attempt_ordinal) {
        continue;
      }
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::CopiedAndAdmitted,
                                         std::nullopt, std::nullopt};
    }
    if (active_reconciliation_turn_.has_value() &&
        active_reconciliation_turn_->receipt.attempt_ordinal == attempt_ordinal) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::CopiedAndAdmitted,
                                         std::nullopt, std::nullopt};
    }
    if (invalid_reconciliation_completion_observation_.has_value() &&
        invalid_reconciliation_completion_observation_->attempt_ordinal == attempt_ordinal) {
      return invalid_reconciliation_completion_observation_;
    }
    private_owner = private_owner_;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The reconciliation oracle is consulted only after executor ownership has ended for this
  // ordinal; it cannot borrow a disposition or error from the ordinary-private namespace.
  if (private_owner != nullptr) {
    auto disposition =
        private_owner->find_committed_reconciliation_event_disposition(attempt_ordinal);
    if (disposition.has_value() && is_terminal_private_disposition(disposition.value())) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::EconomicallyConsumed,
                                         disposition, std::nullopt};
    }
    const auto* retention_error =
        private_owner->find_committed_retained_reconciliation_event_error(attempt_ordinal);
    if (retention_error != nullptr) {
      return PrivateAdmissionObservation{attempt_ordinal,
                                         CriticalPrivateAdmissionState::RetainedForReconciliation,
                                         std::nullopt, *retention_error};
    }
  }
  return std::nullopt;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Expose one synchronized bounded diagnostic copy without leaking slots or mutable fence values.
PrivateLaneSnapshot SerializedExecutor::private_lane_snapshot() const noexcept {
  std::lock_guard lock{mutex_};
  return private_lane_snapshot_locked();
}

// --------------------------------------------------------
// Resolve one exact configured binding and copy only its pending reason/ordinal summaries under the
// same mutex used for insertion, extraction, and restoration.
std::optional<AccountSafetyFenceSnapshot>
SerializedExecutor::find_account_safety_fence_snapshot(model::LogicalAccountId logical_account_id,
                                                       model::VenueId venue_id) const noexcept {
  std::lock_guard lock{mutex_};
  for (const auto& fence : account_fences_) {
    if (fence.binding.logical_account_id != logical_account_id ||
        fence.binding.venue_id != venue_id) {
      continue;
    }
    auto snapshot = AccountSafetyFenceSnapshot{
        logical_account_id, venue_id, std::nullopt, 0U, {}, 0U, fence.in_flight};
    if (!fence.pending.has_value()) {
      return snapshot;
    }
    snapshot.earliest_pending_attempt_ordinal.emplace(
        fence.pending->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal);
    snapshot.pending_lost_attempt_count = fence.pending->lost_attempt_count;
    snapshot.reason_occurrence_count = fence.pending->reason_occurrence_count;
    for (std::size_t index = 0U; index < fence.pending->reason_occurrence_count; ++index) {
      const auto& occurrence = fence.pending->ordered_unique_reason_occurrences[index].value();
      snapshot.ordered_unique_reason_occurrences[index].emplace(
          AccountSafetyReasonOccurrenceSnapshot{occurrence.reason,
                                                occurrence.first_attempt_ordinal});
    }
    return snapshot;
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Copy the stable terminal error while holding the same lock that publishes it.
std::optional<model::DomainError> SerializedExecutor::terminal_error() const {
  std::lock_guard lock{mutex_};
  return terminal_error_;
}

// --------------------------------------------------------
// Wait for any state that may let the dedicated driver run or terminate without polling.
bool SerializedExecutor::wait_for_runnable_work(const std::atomic_bool& stop_requested) {
  std::unique_lock lock{mutex_};
  work_available_.wait(lock, [this, &stop_requested] {
    return has_runnable_locked() || closed_ || terminal_error_.has_value() || stop_requested.load();
  });

  // ++++++++++++++++++++++++++++++++++++++++
  // Terminal faults always stop. Explicit close instead drains its immutable prefix before the
  // driver exits; an ordinary synchronized stop on open ingress exits between turns.
  if (terminal_error_.has_value()) {
    return false;
  }
  if (closed_) {
    return has_runnable_locked();
  }
  if (stop_requested.load()) {
    return false;
  }
  return has_runnable_locked();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Publish stop under the predicate mutex so notify cannot race the wait transition.
void SerializedExecutor::publish_driver_stop(std::atomic_bool& stop_requested) noexcept {
  {
    std::lock_guard lock{mutex_};
    stop_requested.store(true);
  }
  work_available_.notify_all();
}

// --------------------------------------------------------
// Couple dedicated owner release to terminal observation so a producer fault cannot hide in the
// gap between two separately synchronized operations.
model::Result<std::optional<model::DomainError>>
SerializedExecutor::release_from_current_thread_for_driver() {
  std::lock_guard lock{mutex_};
  const auto validation = validate_owner_locked();
  if (!validation) {
    return model::Result<std::optional<model::DomainError>>::create_failure(validation.error());
  }
  auto terminal = terminal_error_;
  owner_thread_.reset();
  return model::Result<std::optional<model::DomainError>>::create_success(std::move(terminal));
}

// --------------------------------------------------------
// Apply the same synchronized stop predicate again at the exact shared turn-start boundary.
model::Result<std::optional<TurnReport>>
SerializedExecutor::execute_next_turn_for_driver(const std::atomic_bool& stop_requested) {
  return execute_next_turn_impl(&stop_requested);
}

// --------------------------------------------------------
// Owner validation reports absence, wrong thread, then nested progression in stable precedence.
model::Result<void> SerializedExecutor::validate_owner_locked() const {
  if (!owner_thread_.has_value()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  if (turn_active_) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorReentryDetected, "executor_reentry"));
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Derive the next attempt ordinal while preserving absence as the pre-first-attempt state.
model::Result<model::AdmissionOrdinal>
SerializedExecutor::derive_next_admission_ordinal_locked() const {
  if (!last_admission_ordinal_.has_value()) {
    return model::Result<model::AdmissionOrdinal>::create_success(
        model::AdmissionOrdinal::create_initial());
  }
  return last_admission_ordinal_->derive_next_ordinal();
}

// --------------------------------------------------------
// Derive the next accepted-only receive sequence without advancing it for rejected attempts.
model::Result<model::ReceiveSequence>
SerializedExecutor::derive_next_receive_sequence_locked() const {
  if (!last_receive_sequence_.has_value()) {
    return model::Result<model::ReceiveSequence>::create_success(
        model::ReceiveSequence::create_initial());
  }
  return last_receive_sequence_->derive_next_ordinal();
}

// --------------------------------------------------------
// Derive the next completed-turn ordinal before selected owner work can begin.
model::Result<model::TurnOrdinal> SerializedExecutor::derive_next_turn_ordinal_locked() const {
  if (!last_turn_ordinal_.has_value()) {
    return model::Result<model::TurnOrdinal>::create_success(model::TurnOrdinal::create_initial());
  }
  return last_turn_ordinal_->derive_next_ordinal();
}

// --------------------------------------------------------
// Reject a provider regression against the last admission or owner-start observation.
model::Result<void> SerializedExecutor::observe_clock_locked(std::uint64_t nanoseconds,
                                                             const char* field) {
  if (last_clock_observation_.has_value() && nanoseconds < last_clock_observation_.value()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorClockRegression, field));
  }
  last_clock_observation_.emplace(nanoseconds);
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Preserve the first terminal cause, close ingress, and wake an idle dedicated owner.
void SerializedExecutor::fail_closed_locked(model::DomainError error) {
  if (!terminal_error_.has_value()) {
    terminal_error_.emplace(std::move(error));
  }
  closed_ = true;
  work_available_.notify_all();
}

// --------------------------------------------------------
// Map a canonical one-based ordinal directly to its construction-time table slot.
SerializedExecutor::SourceFenceState* SerializedExecutor::configured_fence_locked(
    std::optional<model::MarketSourceOrdinal> source_ordinal) noexcept {
  if (!source_ordinal.has_value() || source_ordinal->value() > fences_.size()) {
    return nullptr;
  }
  return &fences_[static_cast<std::size_t>(source_ordinal->value() - 1U)];
}

// --------------------------------------------------------
// Extend one current or successor loss interval without erasing its earliest attempt.
model::Result<void>
SerializedExecutor::record_source_loss_locked(SourceFenceState& fence,
                                              model::AdmissionOrdinal attempt_ordinal) {
  if (!fence.earliest_failed_attempt.has_value()) {
    fence.earliest_failed_attempt.emplace(attempt_ordinal);
    fence.lost_attempt_count = 1U;
    ++pending_fences_;
    return model::Result<void>::create_success();
  }
  if (fence.lost_attempt_count == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorCounterExhausted, "executor_discontinuity_loss_count"));
  }
  ++fence.lost_attempt_count;
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Resolve only an exact sealed root plus canonical account/venue pair. Any mismatch is
// intentionally unattributable and must use the reasonless global fence.
SerializedExecutor::AccountFenceState* SerializedExecutor::find_configured_account_fence_locked(
    const oms::PrivateEventIngressSemanticValue& semantic) noexcept {
  if (!private_configuration_.has_value() ||
      semantic.provenance().root() != private_configuration_->root_provenance()) {
    return nullptr;
  }
  for (auto& fence : account_fences_) {
    if (fence.binding.logical_account_id == semantic.logical_account_id() &&
        fence.binding.venue_id == semantic.venue_id()) {
      return &fence;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Preserve const observation of the same exact root/account/venue tuple used by loss recording.
const SerializedExecutor::AccountFenceState*
SerializedExecutor::find_configured_account_fence_locked(
    const oms::PrivateEventIngressSemanticValue& semantic) const noexcept {
  if (!private_configuration_.has_value() ||
      semantic.provenance().root() != private_configuration_->root_provenance()) {
    return nullptr;
  }
  for (const auto& fence : account_fences_) {
    if (fence.binding.logical_account_id == semantic.logical_account_id() &&
        fence.binding.venue_id == semantic.venue_id()) {
      return &fence;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Insert a reason's first occurrence or replace it with an earlier racing owner occurrence. Moving
// later entries preserves ascending ordinal order without allocating.
bool SerializedExecutor::record_earliest_account_reason_occurrence(
    PrivateFenceInterval& interval, AccountSafetyReasonOccurrence occurrence) noexcept {
  std::size_t existing_index = interval.reason_occurrence_count;
  for (std::size_t index = 0U; index < interval.reason_occurrence_count; ++index) {
    if (interval.ordered_unique_reason_occurrences[index]->reason == occurrence.reason) {
      existing_index = index;
      break;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact or later repeats retain their already canonical first provenance.
  if (existing_index < interval.reason_occurrence_count) {
    if (interval.ordered_unique_reason_occurrences[existing_index]->first_attempt_ordinal.value() <=
        occurrence.first_attempt_ordinal.value()) {
      return true;
    }
    interval.ordered_unique_reason_occurrences[existing_index].emplace(std::move(occurrence));
    while (
        existing_index > 0U &&
        interval.ordered_unique_reason_occurrences[existing_index]->first_attempt_ordinal.value() <
            interval.ordered_unique_reason_occurrences[existing_index - 1U]
                ->first_attempt_ordinal.value()) {
      std::swap(interval.ordered_unique_reason_occurrences[existing_index],
                interval.ordered_unique_reason_occurrences[existing_index - 1U]);
      --existing_index;
    }
    return true;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Nineteen assigned enum values make this bound exhaustive, but keep the mutation total if a
  // future vocabulary extension omits the corresponding storage change.
  if (interval.reason_occurrence_count == account_safety_reason_occurrence_capacity) {
    return false;
  }
  auto insertion_index = interval.reason_occurrence_count;
  while (insertion_index > 0U &&
         occurrence.first_attempt_ordinal.value() <
             interval.ordered_unique_reason_occurrences[insertion_index - 1U]
                 ->first_attempt_ordinal.value()) {
    interval.ordered_unique_reason_occurrences[insertion_index] =
        std::move(interval.ordered_unique_reason_occurrences[insertion_index - 1U]);
    --insertion_index;
  }
  interval.ordered_unique_reason_occurrences[insertion_index].emplace(std::move(occurrence));
  ++interval.reason_occurrence_count;
  return true;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Fold one assigned reason into the account's sole fixed interval while retaining the first
// complete occurrence of every distinct cause.
std::optional<SerializedExecutor::PrivateLossFailure>
SerializedExecutor::record_account_loss_locked(AccountFenceState& fence,
                                               const CriticalPrivateEventAttempt& attempt,
                                               model::AdmissionOrdinal attempt_ordinal,
                                               risk::AccountSafetyReason reason) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject malformed input and checked-count exhaustion before changing canonical reason evidence.
  if (!is_account_safety_reason_assigned(reason)) {
    return PrivateLossFailure::CountExhausted;
  }
  if (fence.pending.has_value() &&
      fence.pending->lost_attempt_count == std::numeric_limits<std::uint64_t>::max()) {
    return PrivateLossFailure::CountExhausted;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Install or extend the sole pending interval only after every bounded precondition is known.
  const bool created_interval = !fence.pending.has_value();
  if (created_interval) {
    fence.pending.emplace(PrivateFenceInterval{});
  }
  auto& interval = fence.pending.value();
  if (!record_earliest_account_reason_occurrence(
          interval, AccountSafetyReasonOccurrence{reason, attempt, attempt_ordinal})) {
    if (created_interval) {
      fence.pending.reset();
    }
    return PrivateLossFailure::CountExhausted;
  }
  ++interval.lost_attempt_count;
  return std::nullopt;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the first complete unattributable value permanently while checked folding reports every
// later global admission loss without selecting an account or reason.
std::optional<SerializedExecutor::PrivateLossFailure>
SerializedExecutor::record_global_loss_locked(const CriticalPrivateEventAttempt& attempt,
                                              model::AdmissionOrdinal attempt_ordinal) noexcept {
  if (!global_fence_.first_attempt.has_value()) {
    global_fence_.first_attempt.emplace(attempt);
    global_fence_.earliest_attempt_ordinal.emplace(attempt_ordinal);
    global_fence_.lost_attempt_count = 1U;
    return std::nullopt;
  }
  if (global_fence_.lost_attempt_count == std::numeric_limits<std::uint64_t>::max()) {
    return PrivateLossFailure::CountExhausted;
  }
  if (attempt_ordinal.value() < global_fence_.earliest_attempt_ordinal->value()) {
    global_fence_.first_attempt.emplace(attempt);
    global_fence_.earliest_attempt_ordinal.emplace(attempt_ordinal);
  }
  ++global_fence_.lost_attempt_count;
  return std::nullopt;
}

// --------------------------------------------------------
// Reject any token that is moved-from, cross-executor, off-owner, stale, or detached from the
// currently active private call stack before exposing its immutable local copies.
model::Result<void> SerializedExecutor::validate_admitted_private_order_slot(
    const AdmittedPrivateOrderSlot& admitted) const {
  if (admitted.owner_ != this) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutionNotPermitted, "private_admission.token_owner"));
  }
  std::lock_guard lock{mutex_};
  if (!owner_thread_.has_value()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  if (!turn_active_ || !active_private_turn_.has_value() ||
      admitted.context_.turn_ordinal != active_private_turn_->turn_ordinal) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutionNotPermitted, "private_admission.token_turn"));
  }
  if (active_private_turn_->receipt.attempt_ordinal != admitted.generation_ ||
      active_private_turn_->receipt != admitted.context_.receipt ||
      active_private_turn_->attempt != admitted.attempt_) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutionNotPermitted, "private_admission.token_generation"));
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Reject reconciliation authority that is moved-from, cross-executor, off-owner, stale, or detached
// from the exact active reconciliation call stack before publishing any copied view.
model::Result<void> SerializedExecutor::validate_admitted_reconciliation_event_slot(
    const AdmittedReconciliationEventSlot& admitted) const {
  if (admitted.owner_ != this) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutionNotPermitted, "reconciliation_admission.token_owner"));
  }
  std::lock_guard lock{mutex_};
  if (!owner_thread_.has_value()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  if (!turn_active_ || !active_reconciliation_turn_.has_value() ||
      admitted.context_.turn_ordinal != active_reconciliation_turn_->turn_ordinal) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::ExecutionNotPermitted, "reconciliation_admission.token_turn"));
  }
  if (active_reconciliation_turn_->receipt.attempt_ordinal != admitted.generation_ ||
      active_reconciliation_turn_->receipt != admitted.context_.receipt ||
      active_reconciliation_turn_->attempt != admitted.attempt_) {
    return model::Result<void>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::ExecutionNotPermitted,
                                            "reconciliation_admission.token_generation"));
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Derive diagnostics from the ring count, sole active turn, and one fixed fence slot per account.
PrivateLaneSnapshot SerializedExecutor::private_lane_snapshot_locked() const noexcept {
  PrivateLaneSnapshot private_lane;
  private_lane.private_capacity = private_slots_.size();
  private_lane.account_fence_capacity = account_fences_.size();
  private_lane.queued_slots = pending_private_commands_;
  private_lane.in_flight_slots = active_private_turn_.has_value() ? 1U : 0U;
  private_lane.occupied_slots = private_lane.queued_slots + private_lane.in_flight_slots;
  for (const auto& fence : account_fences_) {
    if (fence.pending.has_value()) {
      ++private_lane.pending_account_fences;
    }
    if (fence.in_flight) {
      ++private_lane.in_flight_account_fences;
    }
  }
  private_lane.global_fence_active = global_fence_.first_attempt.has_value();
  private_lane.global_fence_in_flight = global_fence_.in_flight;
  private_lane.global_fence_owner_applied = global_fence_.owner_applied;
  private_lane.reconciliation_capacity = reconciliation_slots_.size();
  private_lane.reconciliation_queued_slots = pending_reconciliation_commands_;
  private_lane.reconciliation_in_flight_slots = active_reconciliation_turn_.has_value() ? 1U : 0U;
  private_lane.reconciliation_occupied_slots =
      private_lane.reconciliation_queued_slots + private_lane.reconciliation_in_flight_slots;
  return private_lane;
}

// --------------------------------------------------------
// Restore failed control work before its successor interval. Admission ordinals make overflow
// unreachable, but the addition remains checked before preserving a defensive saturated bound.
void SerializedExecutor::restore_failed_fence_locked(
    std::size_t fence_index, const SourceDiscontinuity& discontinuity) noexcept {
  auto& fence = fences_[fence_index];
  if (fence.earliest_failed_attempt.has_value()) {
    if (fence.lost_attempt_count >
        std::numeric_limits<std::uint64_t>::max() - discontinuity.lost_attempt_count) {
      fence.lost_attempt_count = std::numeric_limits<std::uint64_t>::max();
    } else {
      fence.lost_attempt_count += discontinuity.lost_attempt_count;
    }
    fence.earliest_failed_attempt.emplace(discontinuity.earliest_failed_attempt);
  } else {
    fence.earliest_failed_attempt.emplace(discontinuity.earliest_failed_attempt);
    fence.lost_attempt_count = discontinuity.lost_attempt_count;
    ++pending_fences_;
  }
  fence.in_flight = false;
  --in_flight_fences_;
}

// --------------------------------------------------------
// Restore an account-fence handler failure into the sole fixed slot, checked-merging a
// producer-race successor while retaining every unique reason's earliest complete occurrence.
std::optional<SerializedExecutor::PrivateLossFailure>
SerializedExecutor::restore_failed_account_fence_locked(AccountFenceState& fence,
                                                        AccountSafetyFenceTurn failed) noexcept {
  fence.in_flight = false;
  auto restored = PrivateFenceInterval{failed.lost_attempt_count,
                                       std::move(failed.ordered_unique_reason_occurrences),
                                       failed.reason_occurrence_count};
  if (!fence.pending.has_value()) {
    fence.pending.emplace(std::move(restored));
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The extracted interval precedes its producer-race successor, but the ordered merge also handles
  // any future call path that presents the intervals in the opposite callback-arrival order.
  auto successor = std::move(fence.pending).value();
  fence.pending.reset();
  bool storage_exhausted = false;
  for (std::size_t index = 0U; index < successor.reason_occurrence_count; ++index) {
    if (!record_earliest_account_reason_occurrence(
            restored, std::move(successor.ordered_unique_reason_occurrences[index]).value())) {
      storage_exhausted = true;
    }
  }
  if (successor.lost_attempt_count >
      std::numeric_limits<std::uint64_t>::max() - restored.lost_attempt_count) {
    restored.lost_attempt_count = std::numeric_limits<std::uint64_t>::max();
    storage_exhausted = true;
  } else {
    restored.lost_attempt_count += successor.lost_attempt_count;
  }
  fence.pending.emplace(std::move(restored));
  return storage_exhausted ? std::optional{PrivateLossFailure::CountExhausted} : std::nullopt;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Merge all public, ordinary-private, reconciliation, and fence stores by their shared unique
// attempt ordinals while the owner-applied global gate suppresses only ordinary-private work.
std::optional<SerializedExecutor::RunnableTurnCandidate>
SerializedExecutor::find_oldest_runnable_turn_locked() const noexcept {
  std::optional<RunnableTurnCandidate> oldest;

  // ++++++++++++++++++++++++++++++++++++++++
  // Replace the candidate only for a strictly earlier attempt; unique ordinals make equal values
  // impossible among simultaneously runnable original admissions.
  const auto consider = [&oldest](RunnableSource source, std::size_t index,
                                  model::AdmissionOrdinal attempt_ordinal) {
    if (!oldest.has_value() || attempt_ordinal.value() < oldest->attempt_ordinal.value()) {
      oldest.emplace(RunnableTurnCandidate{source, index, attempt_ordinal});
    }
  };

  // ++++++++++++++++++++++++++++++++++++++++
  // Seed selection from the FIFO head because later accepted queue entries cannot be older.
  if (pending_commands_ > 0U) {
    const auto attempt = queue_[head_].receipt->attempt_ordinal;
    consider(RunnableSource::Command, 0U, attempt);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Every active source slot contributes its canonical earliest failed attempt.
  for (std::size_t index = 0U; index < fences_.size(); ++index) {
    const auto& fence = fences_[index];
    if (fence.earliest_failed_attempt.has_value()) {
      consider(RunnableSource::SourceFence, index, fence.earliest_failed_attempt.value());
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The private FIFO contributes only its head. Extraction releases that pending slot before the
  // handler begins, while a globally applied unattributable fence blocks later consumption.
  if (!global_fence_.owner_applied && pending_private_commands_ > 0U) {
    const auto& slot = private_slots_[private_head_];
    consider(RunnableSource::PrivateCommand, private_head_, slot.receipt->attempt_ordinal);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The reconciliation FIFO contributes its head even after the permanent global-private gate is
  // owner-applied, because only trusted reconciliation may eventually resolve that condition.
  if (pending_reconciliation_commands_ > 0U) {
    const auto& slot = reconciliation_slots_[reconciliation_head_];
    consider(RunnableSource::ReconciliationCommand, reconciliation_head_,
             slot.receipt->attempt_ordinal);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Every configured account contributes at most its one pending canonical interval.
  for (std::size_t index = 0U; index < account_fences_.size(); ++index) {
    const auto& fence = account_fences_[index];
    if (fence.pending.has_value()) {
      consider(RunnableSource::AccountFence, index,
               fence.pending->ordered_unique_reason_occurrences[0U]->first_attempt_ordinal);
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The reasonless global interval executes once, remains permanently observable, and thereafter
  // gates private work until a future recovery-only capability explicitly resolves it.
  if (global_fence_.first_attempt.has_value() && !global_fence_.in_flight &&
      !global_fence_.owner_applied) {
    consider(RunnableSource::GlobalFence, 0U, global_fence_.earliest_attempt_ordinal.value());
  }
  return oldest;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reuse the complete attempt-order merge as the sole runnable-state oracle for owner waits.
bool SerializedExecutor::has_runnable_locked() const noexcept {
  return find_oldest_runnable_turn_locked().has_value();
}

// --------------------------------------------------------
// The deterministic driver borrows all state from one non-relocatable executor.
DeterministicExecutorDriver::DeterministicExecutorDriver(SerializedExecutor& executor) noexcept
    : executor_{executor} {}

// --------------------------------------------------------
// Manual binding uses the same owner transition as the dedicated driver.
model::Result<void> DeterministicExecutorDriver::bind_to_current_thread() {
  return executor_.bind_to_current_thread();
}

// --------------------------------------------------------
// Manual release makes a driver handoff explicit.
model::Result<void> DeterministicExecutorDriver::release_from_current_thread() {
  return executor_.release_from_current_thread();
}

// --------------------------------------------------------
// One deterministic step is exactly one shared executor step.
model::Result<std::optional<TurnReport>> DeterministicExecutorDriver::execute_next_turn() {
  return executor_.execute_next_turn();
}

// --------------------------------------------------------
// Bounded deterministic progression delegates to the executor's policy-bound execute_pending_turns
// loop.
model::Result<PendingTurnExecutionReport>
DeterministicExecutorDriver::execute_pending_turns(std::size_t max_turns) {
  return executor_.execute_pending_turns(max_turns);
}

// --------------------------------------------------------

} // namespace aegis::runtime
