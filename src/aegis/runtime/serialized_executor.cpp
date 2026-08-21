// Purpose: implement fixed-capacity admission, globally ordered command/fence turns, terminal
// fail-closed timing and handlers, and bounded deterministic owner progression.

#include "aegis/runtime/serialized_executor.hpp"

#include "aegis/model/domain_error.hpp"

#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace aegis::runtime {

// --------------------------------------------------------
// Construct the unattributable boundary without allocating unused source-fence storage.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, model::ClockProvider& clock,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{command_capacity,       0U, clock, nullptr, maximum_drive_turns,
                         std::move(counter_seed)} {}

// --------------------------------------------------------
// Preallocate one persistent fence slot per configured canonical source ordinal.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                                       model::ClockProvider& clock,
                                       SourceDiscontinuityHandler& discontinuity_handler,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : SerializedExecutor{command_capacity,       source_capacity,     clock,
                         &discontinuity_handler, maximum_drive_turns, std::move(counter_seed)} {}

// --------------------------------------------------------
// Complete every allocation and install bounds and boundary-test counters before ingress becomes
// observable.
SerializedExecutor::SerializedExecutor(std::size_t command_capacity, std::size_t source_capacity,
                                       model::ClockProvider& clock,
                                       SourceDiscontinuityHandler* discontinuity_handler,
                                       std::size_t maximum_drive_turns,
                                       ExecutorCounterSeed counter_seed)
    : clock_{clock}, discontinuity_handler_{discontinuity_handler}, queue_(command_capacity),
      fences_(source_capacity), maximum_drive_turns_{maximum_drive_turns},
      last_admission_ordinal_{counter_seed.last_admission_ordinal},
      last_receive_sequence_{counter_seed.last_receive_sequence},
      last_turn_ordinal_{counter_seed.last_turn_ordinal} {}

// --------------------------------------------------------
// Assign every ordinary attempt exactly once, preserving capacity rejection as a successful
// decision and preventing source recovery from crossing a pending or active loss fence.
model::Result<AdmissionDecision>
SerializedExecutor::try_admit(WorkItem work,
                              std::optional<model::MarketSourceOrdinal> source_ordinal) {
  std::unique_lock lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // A terminal fault has precedence over ordinary closure because it can no longer assign replay
  // ordinals safely.
  if (terminal_error_.has_value()) {
    return model::Result<AdmissionDecision>::failure(terminal_error_.value());
  }
  auto next_attempt = next_admission_ordinal_locked();
  if (!next_attempt) {
    const auto error = next_attempt.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Ordinary closure still consumes its attempt ordinal, but never reads the clock or creates a
  // source fence.
  if (closed_) {
    last_admission_ordinal_.emplace(next_attempt.value());
    return model::Result<AdmissionDecision>::success(
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
        return model::Result<AdmissionDecision>::failure(error);
      }
    }
    last_admission_ordinal_.emplace(next_attempt.value());
    const auto observed_pending_depth = pending_commands_;
    const auto observed_pending_capacity = queue_.size();

    lock.unlock();
    if (fence != nullptr) {
      work_available_.notify_one();
    }
    return model::Result<AdmissionDecision>::success(AdmissionDecision{
        AdmissionOutcome::CapacityExceeded, next_attempt.value(), observed_pending_depth,
        observed_pending_capacity, std::nullopt, fence != nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive both accepted-only values before committing the attempt. Receive exhaustion and clock
  // regression consequently leave every counter and queue slot unchanged while failing closed.
  auto next_receive = next_receive_sequence_locked();
  if (!next_receive) {
    const auto error = next_receive.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::failure(error);
  }
  const auto received_at = clock_.receive_now();
  auto clock_observation =
      observe_clock_locked(received_at.nanoseconds(), "executor_admission_clock");
  if (!clock_observation) {
    const auto error = clock_observation.error();
    fail_closed_locked(error);
    return model::Result<AdmissionDecision>::failure(error);
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
  return model::Result<AdmissionDecision>::success(
      AdmissionDecision{AdmissionOutcome::Accepted, next_attempt.value(), receipt.pending_depth,
                        receipt.pending_capacity, receipt, false});

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
// Latch a fault raised after owner-local mutation while leaving run_one responsible for publishing
// the active handler's successful completion before that fault surfaces at the next boundary.
model::Result<void> SerializedExecutor::request_owner_fault(model::DomainError error) noexcept {
  std::lock_guard lock{mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject callers without the current owner authority before lifecycle state can change.
  if (!owner_thread_.has_value()) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // This seam is post-mutation authority for a running handler, never an out-of-turn close API.
  if (!turn_active_) {
    return model::Result<void>::failure(model::DomainError::at_field(
        model::DomainErrorCode::ExecutorReentryDetected, "executor_owner_fault"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the first cause, close later admission, and wake a dedicated driver. The active
  // handler still owns its stack and may return success so run_one can publish its TurnReport.
  fail_closed_locked(std::move(error));
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Binding is idempotent for the current thread and rejects an implicit concurrent handoff.
model::Result<void> SerializedExecutor::bind_to_current_thread() {
  std::lock_guard lock{mutex_};
  const auto current_thread = std::this_thread::get_id();
  if (owner_thread_.has_value() && owner_thread_.value() != current_thread) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  owner_thread_ = current_thread;
  return model::Result<void>::success();
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
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Public deterministic progression has no dedicated-stop gate.
model::Result<std::optional<TurnReport>> SerializedExecutor::run_one() {
  return run_one_impl(nullptr);
}

// --------------------------------------------------------
// Select, validate, remove, and execute at most one command or fence in global attempt order.
model::Result<std::optional<TurnReport>>
SerializedExecutor::run_one_impl(const std::atomic_bool* stop_requested) {
  std::optional<WorkItem> work;
  std::optional<AdmissionReceipt> receipt;
  std::optional<SourceDiscontinuity> discontinuity;
  std::optional<std::size_t> fence_index;
  std::optional<AcceptedTurnContext> accepted_context;
  std::optional<ControlTurnContext> control_context;
  std::size_t pending_commands_at_start = 0U;
  std::size_t pending_fences_at_start = 0U;
  TurnKind turn_kind = TurnKind::Command;

  {
    std::lock_guard lock{mutex_};

    // ++++++++++++++++++++++++++++++++++++++++
    // Ownership and terminal lifecycle checks precede any runnable-state mutation.
    const auto owner_validation = validate_owner_locked();
    if (!owner_validation) {
      return model::Result<std::optional<TurnReport>>::failure(owner_validation.error());
    }
    if (terminal_error_.has_value()) {
      return model::Result<std::optional<TurnReport>>::failure(terminal_error_.value());
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Dedicated stop wins an open-ingress turn only when it acquires this mutex before turn start.
    // Closed-prefix drainage deliberately retains precedence over the same stop request.
    if (stop_requested != nullptr && stop_requested->load() && !closed_) {
      return model::Result<std::optional<TurnReport>>::success(std::nullopt);
    }
    const auto candidate = oldest_runnable_locked();
    if (!candidate.has_value()) {
      return model::Result<std::optional<TurnReport>>::success(std::nullopt);
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Validate the next turn ordinal and sole processing-time observation while the selected head
    // remains intact, so either failure closes without invoking mutable owner work.
    auto next_turn = next_turn_ordinal_locked();
    if (!next_turn) {
      const auto error = next_turn.error();
      fail_closed_locked(error);
      return model::Result<std::optional<TurnReport>>::failure(error);
    }
    const auto processing_start = clock_.processing_now();
    auto clock_observation =
        observe_clock_locked(processing_start.nanoseconds(), "executor_processing_clock");
    if (!clock_observation) {
      const auto error = clock_observation.error();
      fail_closed_locked(error);
      return model::Result<std::optional<TurnReport>>::failure(error);
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Extract exactly the selected candidate and build its stack-scoped authority. An extracted
    // fence remains marked in-flight so later source attempts extend a successor fence.
    turn_kind = candidate->kind;
    if (candidate->kind == TurnKind::Command) {
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
    } else {
      fence_index.emplace(candidate->fence_index);
      auto& fence = fences_[candidate->fence_index];
      const auto source_ordinal =
          model::MarketSourceOrdinal::from_value(candidate->fence_index + 1U).value();
      discontinuity.emplace(SourceDiscontinuity{
          source_ordinal, fence.earliest_failed_attempt.value(), fence.lost_attempt_count});
      control_context.emplace(ControlTurnContext{next_turn.value(), processing_start});
      fence.earliest_failed_attempt.reset();
      fence.lost_attempt_count = 0U;
      fence.in_flight = true;
      --pending_fences_;
      ++in_flight_fences_;
    }
    pending_commands_at_start = pending_commands_;
    pending_fences_at_start = pending_fences_;
    turn_active_ = true;

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Handlers receive only stack-scoped turn authority. Their failure contract guarantees all
  // fallible validation happened before any data-plane mutation.
  auto handler_result = turn_kind == TurnKind::Command
                            ? work->execute(accepted_context.value())
                            : discontinuity_handler_->on_source_discontinuity(
                                  discontinuity.value(), control_context.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Reconcile the in-flight fence before publishing failure or success. Failed fence work is
  // restored ahead of any successor losses recorded concurrently by producers.
  std::lock_guard lock{mutex_};
  if (turn_kind == TurnKind::SourceDiscontinuity) {
    if (!handler_result) {
      restore_failed_fence_locked(fence_index.value(), discontinuity.value());
    } else {
      fences_[fence_index.value()].in_flight = false;
      --in_flight_fences_;
    }
  }
  turn_active_ = false;

  // ++++++++++++++++++++++++++++++++++++++++
  // Handler failure consumes accepted work but publishes no completed turn. The first terminal
  // fault remains stable if another producer fault raced the active handler.
  if (!handler_result) {
    fail_closed_locked(handler_result.error());
    return model::Result<std::optional<TurnReport>>::failure(terminal_error_.value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Successful applied work always advances and reports, even when a producer published a terminal
  // admission fault during the active handler. That stored fault remains closed and is returned by
  // the next progression boundary rather than erasing evidence for this completed turn.
  const auto turn_ordinal = turn_kind == TurnKind::Command ? accepted_context->turn_ordinal
                                                           : control_context->turn_ordinal;
  const auto started_at = turn_kind == TurnKind::Command ? accepted_context->processing_timestamp
                                                         : control_context->processing_timestamp;
  last_turn_ordinal_.emplace(turn_ordinal);
  const auto completed_turns = turn_ordinal.value();
  const auto attempt_ordinal = turn_kind == TurnKind::Command
                                   ? receipt->attempt_ordinal
                                   : discontinuity->earliest_failed_attempt;
  const auto report = TurnReport{
      turn_kind,
      turn_ordinal,
      attempt_ordinal,
      turn_kind == TurnKind::Command
          ? std::optional<model::ReceiveSequence>{receipt->receive_sequence}
          : std::nullopt,
      turn_kind == TurnKind::Command ? std::optional<model::ReceiveTimestamp>{receipt->received_at}
                                     : std::nullopt,
      started_at,
      turn_kind == TurnKind::Command
          ? std::optional<model::ElapsedNanoseconds>{accepted_context->queue_age}
          : std::nullopt,
      discontinuity,
      pending_commands_at_start,
      pending_commands_,
      pending_fences_at_start,
      pending_fences_,
      queue_.size(),
      completed_turns,
  };
  return model::Result<std::optional<TurnReport>>::success(report);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Bounded progression validates even a zero-turn call and rejects policy-bound violations before
// starting any owner turn.
model::Result<DriveReport> SerializedExecutor::drive(std::size_t max_turns) {
  {
    std::lock_guard lock{mutex_};
    const auto owner_validation = validate_owner_locked();
    if (!owner_validation) {
      return model::Result<DriveReport>::failure(owner_validation.error());
    }
    if (terminal_error_.has_value()) {
      return model::Result<DriveReport>::failure(terminal_error_.value());
    }
    if (max_turns > maximum_drive_turns_) {
      return model::Result<DriveReport>::failure(model::DomainError::at_field(
          model::DomainErrorCode::InvalidRuntimePolicy, "maximum_drive_turns"));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Stop at the explicit turn bound or the first observation with neither command nor fence.
  std::size_t turns_executed = 0U;
  while (turns_executed < max_turns) {
    auto turn = run_one();
    if (!turn) {
      return model::Result<DriveReport>::failure(std::move(turn).error());
    }
    if (!turn.value().has_value()) {
      break;
    }
    ++turns_executed;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Snapshot all remaining bounded work once after progression.
  const auto state = snapshot();
  return model::Result<DriveReport>::success(
      DriveReport{turns_executed, state.pending_commands, state.pending_fences,
                  state.command_capacity, state.maximum_drive_turns, state.completed_turns});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Read every related queue, fence, owner, bound, and lifecycle field under one synchronization
// boundary.
QueueSnapshot SerializedExecutor::snapshot() const noexcept {
  std::lock_guard lock{mutex_};
  return QueueSnapshot{pending_commands_,
                       pending_fences_,
                       in_flight_fences_,
                       queue_.size(),
                       fences_.size(),
                       maximum_drive_turns_,
                       last_turn_ordinal_.has_value() ? last_turn_ordinal_->value() : 0U,
                       closed_,
                       terminal_error_.has_value(),
                       owner_thread_.has_value(),
                       turn_active_};
}

// --------------------------------------------------------
// Compare the caller with the synchronized owner token without publishing a thread identifier.
bool SerializedExecutor::current_thread_is_owner() const noexcept {
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
    return model::Result<std::optional<model::DomainError>>::failure(validation.error());
  }
  auto terminal = terminal_error_;
  owner_thread_.reset();
  return model::Result<std::optional<model::DomainError>>::success(std::move(terminal));
}

// --------------------------------------------------------
// Apply the same synchronized stop predicate again at the exact shared turn-start boundary.
model::Result<std::optional<TurnReport>>
SerializedExecutor::run_one_for_driver(const std::atomic_bool& stop_requested) {
  return run_one_impl(&stop_requested);
}

// --------------------------------------------------------
// Owner validation reports absence, wrong thread, then nested progression in stable precedence.
model::Result<void> SerializedExecutor::validate_owner_locked() const {
  if (!owner_thread_.has_value()) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorNotBound, "executor_owner"));
  }
  if (owner_thread_.value() != std::this_thread::get_id()) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorWrongOwner, "executor_owner"));
  }
  if (turn_active_) {
    return model::Result<void>::failure(model::DomainError::at_field(
        model::DomainErrorCode::ExecutorReentryDetected, "executor_reentry"));
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Derive the next attempt ordinal while preserving absence as the pre-first-attempt state.
model::Result<model::AdmissionOrdinal> SerializedExecutor::next_admission_ordinal_locked() const {
  if (!last_admission_ordinal_.has_value()) {
    return model::Result<model::AdmissionOrdinal>::success(model::AdmissionOrdinal::initial());
  }
  return last_admission_ordinal_->next();
}

// --------------------------------------------------------
// Derive the next accepted-only receive sequence without advancing it for rejected attempts.
model::Result<model::ReceiveSequence> SerializedExecutor::next_receive_sequence_locked() const {
  if (!last_receive_sequence_.has_value()) {
    return model::Result<model::ReceiveSequence>::success(model::ReceiveSequence::initial());
  }
  return last_receive_sequence_->next();
}

// --------------------------------------------------------
// Derive the next completed-turn ordinal before selected owner work can begin.
model::Result<model::TurnOrdinal> SerializedExecutor::next_turn_ordinal_locked() const {
  if (!last_turn_ordinal_.has_value()) {
    return model::Result<model::TurnOrdinal>::success(model::TurnOrdinal::initial());
  }
  return last_turn_ordinal_->next();
}

// --------------------------------------------------------
// Reject a provider regression against the last admission or owner-start observation.
model::Result<void> SerializedExecutor::observe_clock_locked(std::uint64_t nanoseconds,
                                                             const char* field) {
  if (last_clock_observation_.has_value() && nanoseconds < last_clock_observation_.value()) {
    return model::Result<void>::failure(
        model::DomainError::at_field(model::DomainErrorCode::ExecutorClockRegression, field));
  }
  last_clock_observation_.emplace(nanoseconds);
  return model::Result<void>::success();
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
SerializedExecutor::FenceState* SerializedExecutor::configured_fence_locked(
    std::optional<model::MarketSourceOrdinal> source_ordinal) noexcept {
  if (!source_ordinal.has_value() || source_ordinal->value() > fences_.size()) {
    return nullptr;
  }
  return &fences_[static_cast<std::size_t>(source_ordinal->value() - 1U)];
}

// --------------------------------------------------------
// Extend one current or successor loss interval without erasing its earliest attempt.
model::Result<void>
SerializedExecutor::record_source_loss_locked(FenceState& fence,
                                              model::AdmissionOrdinal attempt_ordinal) {
  if (!fence.earliest_failed_attempt.has_value()) {
    fence.earliest_failed_attempt.emplace(attempt_ordinal);
    fence.lost_attempt_count = 1U;
    ++pending_fences_;
    return model::Result<void>::success();
  }
  if (fence.lost_attempt_count == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<void>::failure(model::DomainError::at_field(
        model::DomainErrorCode::ExecutorCounterExhausted, "executor_discontinuity_loss_count"));
  }
  ++fence.lost_attempt_count;
  return model::Result<void>::success();
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
// Merge the FIFO head with all active source slots by their unique attempt ordinals.
std::optional<SerializedExecutor::RunnableCandidate>
SerializedExecutor::oldest_runnable_locked() const noexcept {
  std::optional<RunnableCandidate> oldest;
  std::optional<std::uint64_t> oldest_attempt;

  // ++++++++++++++++++++++++++++++++++++++++
  // Seed selection from the FIFO head because later accepted queue entries cannot be older.
  if (pending_commands_ > 0U) {
    oldest.emplace(RunnableCandidate{TurnKind::Command, 0U});
    oldest_attempt.emplace(queue_[head_].receipt->attempt_ordinal.value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Scan the fixed source table and replace only with a strictly earlier pending fence.
  for (std::size_t index = 0U; index < fences_.size(); ++index) {
    const auto& fence = fences_[index];
    if (fence.earliest_failed_attempt.has_value() &&
        (!oldest_attempt.has_value() ||
         fence.earliest_failed_attempt->value() < oldest_attempt.value())) {
      oldest.emplace(RunnableCandidate{TurnKind::SourceDiscontinuity, index});
      oldest_attempt.emplace(fence.earliest_failed_attempt->value());
    }
  }
  return oldest;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Summarize the two preallocated stores for owner and wait predicates.
bool SerializedExecutor::has_runnable_locked() const noexcept {
  return pending_commands_ > 0U || pending_fences_ > 0U;
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
model::Result<std::optional<TurnReport>> DeterministicExecutorDriver::run_one() {
  return executor_.run_one();
}

// --------------------------------------------------------
// Bounded deterministic progression delegates to the executor's policy-bound drive loop.
model::Result<DriveReport> DeterministicExecutorDriver::drive(std::size_t max_turns) {
  return executor_.drive(max_turns);
}

// --------------------------------------------------------

} // namespace aegis::runtime
