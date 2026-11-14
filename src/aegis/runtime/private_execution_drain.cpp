// Purpose: prepare and validate a contiguous execution and its genuine buffered prefix against
// the live OMS, reservation, inventory, audit, and fake journal owners.

#include "private_execution_drain.hpp"

#include "submission_coordinator.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Report a precise rejection while retaining the originating domain error code.
template <typename Value>
[[nodiscard]] model::Result<Value> reject_private_execution_drain(
    const char* field,
    model::DomainErrorCode code = model::DomainErrorCode::InvalidPrivateOmsState) {
  return model::Result<Value>::create_failure(model::DomainError::create_at_field(code, field));
}

// --------------------------------------------------------
// Borrow the exact configured account for the known order without changing its gate.
[[nodiscard]] const PrivateAccountContainment*
find_execution_drain_account(const PrivateIdentityRetentionState& retention,
                             const model::M4SubjectProvenance& subject) noexcept {
  const auto found =
      std::find_if(retention.accounts.begin(), retention.accounts.end(),
                   [&subject](const PrivateAccountContainment& account) {
                     return account.binding.logical_account_id == subject.logical_account_id() &&
                            account.binding.venue_id == subject.venue_id();
                   });
  return found == retention.accounts.end() ? nullptr : &*found;
}

// --------------------------------------------------------
// Both globally assigned receipt identities must advance past an earlier admitted fact.
[[nodiscard]] bool
is_later_execution_drain_receipt(recovery::JournalReplayProvenance candidate,
                                 recovery::JournalReplayProvenance previous) noexcept {
  return candidate.admission_ordinal > previous.admission_ordinal &&
         candidate.receive_sequence > previous.receive_sequence;
}

// --------------------------------------------------------
// Match the original gap audit by journal linkage. An event's index need not equal its audit
// index once another accepted execution can contribute several audit rows.
[[nodiscard]] bool
has_original_buffered_execution_audit(const PrivateBusinessEvidenceStore& evidence,
                                      const trace::PrivateEventEvidence& event) noexcept {
  const auto count = evidence.accepted_private_audit_record_count();
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto* audit = evidence.accepted_private_audit_record_at(index);
    if (audit->journal_sequence() == event.journal_sequence() &&
        audit->disposition() == oms::PrivateEventDisposition::BufferedGap &&
        audit->source_input() == std::optional{event.input()}) {
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------

// ########################################################################
// Discard borrowing scratch at every exit, before any input or registry dependency can die.
struct ExecutionDrainInputScratchReset {
  std::vector<risk::ReservationInventoryExecutionInput>& inputs;
  std::vector<PrivateExecutionDrainIdentity>& identities;

  // --------------------------------------------------------
  // End the scratch borrow without reallocating its cold backing.
  ~ExecutionDrainInputScratchReset() {
    inputs.clear();
    identities.clear();
  }

  // --------------------------------------------------------
};

// ########################################################################

} // namespace

// --------------------------------------------------------
// Transfer immutable inspection together with every component lease.
PreparedKnownPrivateExecutionDrain::PreparedKnownPrivateExecutionDrain(
    PreparedKnownPrivateExecutionDrain&& other) noexcept
    : state_{std::move(other.state_)} {
  other.state_.reset();
}

// --------------------------------------------------------
// Release this value's old leases before revoking inspection through the source value.
PreparedKnownPrivateExecutionDrain&
PreparedKnownPrivateExecutionDrain::operator=(PreparedKnownPrivateExecutionDrain&& other) noexcept {
  if (this != &other) {
    state_ = std::move(other.state_);
    other.state_.reset();
  }
  return *this;
}

// --------------------------------------------------------
// Accept only the complete state assembled by the genuine owner.
PreparedKnownPrivateExecutionDrain::PreparedKnownPrivateExecutionDrain(State state) noexcept
    : state_{std::move(state)} {
  static_assert(std::is_nothrow_move_constructible_v<State>);
  static_assert(std::is_nothrow_move_assignable_v<State>);
}

// --------------------------------------------------------
// Borrow the sealed first-seen identity while this value owns the preparation.
const FirstSeenAuthoritativePrivateIdentityPlan*
PreparedKnownPrivateExecutionDrain::identity_plan() const noexcept {
  return state_ ? &state_->identity : nullptr;
}

// --------------------------------------------------------
// Borrow the verified complete OMS transition while this value owns the preparation.
const oms::PrivateOmsTransitionPlan*
PreparedKnownPrivateExecutionDrain::oms_transition() const noexcept {
  return state_ ? &state_->transition : nullptr;
}

// --------------------------------------------------------
// Borrow the reserved economic plan while this value owns its lease.
const risk::ReservationInventoryPlan*
PreparedKnownPrivateExecutionDrain::economics_plan() const noexcept {
  return state_ ? &state_->economics : nullptr;
}

// --------------------------------------------------------
// Borrow the complete prospective audit span while this value owns its lease.
const PreparedPrivateBusinessEvidence*
PreparedKnownPrivateExecutionDrain::business_evidence() const noexcept {
  return state_ ? &state_->evidence : nullptr;
}

// --------------------------------------------------------
// Borrow the actual reserved record without transferring its journal authority.
const recovery::JournalRecord* PreparedKnownPrivateExecutionDrain::journal_record() const noexcept {
  return state_ ? state_->journal.record() : nullptr;
}

// --------------------------------------------------------
// The incoming event/trade must be first-seen. Previously retained validated facts can expose
// an overlap or contradictory exchange mapping even if they could not be published.
model::Result<void> PrivateOrderReconciler::validate_first_seen_private_execution_drain_identity(
    const FirstSeenAuthoritativePrivateIdentityPlan& identity,
    const KnownFirstSeenPrivateCorrelationPlan& correlation,
    const FirstSeenPrivateTradeIdentityPlan& trade, const oms::ExecutionPayload& execution) const {
  using Code = model::DomainErrorCode;
  if (find_committed_trade_identity(trade.key) != nullptr) {
    return reject_private_execution_drain<void>("private_execution_drain.compared_identity",
                                                Code::PrivateEventConflict);
  }
  for (std::uint32_t index = 0U; index < retention_.preparations.trade_record_count(); ++index) {
    if (retention_.preparations.trade_record_at(index)->key == trade.key) {
      return reject_private_execution_drain<void>("private_execution_drain.compared_trade",
                                                  Code::PrivateEventConflict);
    }
  }
  const auto start = execution.cumulative_quantity.checked_subtract(execution.incremental_quantity);
  if (!start || start.value().coefficient() < 0) {
    return reject_private_execution_drain<void>("private_execution_drain.interval");
  }
  const auto retained_count = retention_.published_turn_count.load(std::memory_order_relaxed);
  const auto order_id = correlation.resolution.known_resolution()->order_id;
  for (std::size_t index = 0U; index < retained_count; ++index) {
    const auto& retained = retention_.turns[index];
    const auto retained_key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(
        oms::PrivateEventIngressSemanticValue::from_normalized_input(*retained.normalized));
    const auto& progress = retained.execution_progress;
    if (retained_key == identity.event_key() ||
        (progress.trade && progress.trade->key == trade.key)) {
      return reject_private_execution_drain<void>("private_execution_drain.retained_identity",
                                                  Code::PrivateEventConflict);
    }
    if (progress.mapping && correlation.candidate_mapping &&
        ((progress.mapping->exchange_order_key ==
          correlation.candidate_mapping->exchange_order_key) !=
         (progress.mapping->order_id == order_id))) {
      return reject_private_execution_drain<void>("private_execution_drain.retained_mapping",
                                                  Code::PrivateCorrelationFailed);
    }
    const auto& interval = progress.interval;
    if (interval && interval->order_id == order_id && start.value() < interval->end &&
        interval->start < execution.cumulative_quantity) {
      return reject_private_execution_drain<void>("private_execution_drain.retained_overlap",
                                                  Code::PrivateEventConflict);
    }
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Verify both identity rows and the original accepted gap evidence. A surviving pending input
// uses its own source time, origin, and resolution even when the incoming execution has another.
model::Result<PrivateExecutionDrainIdentity>
PrivateOrderReconciler::verify_buffered_private_execution_identity(
    const oms::NormalizedPrivateOrderInput& input,
    const oms::KnownPrivateEventResolution& owner) const {
  using Disposition = oms::PrivateEventDisposition;
  const auto* execution = std::get_if<oms::ExecutionPayload>(&input.payload());
  const auto semantic = oms::PrivateEventIngressSemanticValue::from_normalized_input(input);
  const auto* event = find_committed_event_identity(
      oms::PrivateEventRegistryKey::from_ingress_semantic_value(semantic));
  if (execution == nullptr || event == nullptr || event->ingress_semantic_value != semantic ||
      event->original_disposition != Disposition::BufferedGap ||
      event->resolution.known_resolution() == nullptr ||
      *event->resolution.known_resolution() != owner) {
    return reject_private_execution_drain<PrivateExecutionDrainIdentity>(
        "private_execution_drain.buffered_event");
  }
  const auto* trade = find_committed_trade_identity(
      oms::TradeKey{input.venue_id(), input.logical_account_id(), execution->trade_id});
  const auto* order = owner_->outbound_oms().find_order(owner.order_id);
  if (order == nullptr || trade == nullptr ||
      trade->original_disposition != Disposition::BufferedGap ||
      trade->current_disposition != Disposition::BufferedGap ||
      trade->semantic_value != oms::PrivateTradeSemanticValue::create_known_trade_semantic_value(
                                   *execution, owner.order_id, order->economics().side)) {
    return reject_private_execution_drain<PrivateExecutionDrainIdentity>(
        "private_execution_drain.buffered_trade");
  }
  const auto count = business_evidence_store_->accepted_private_event_record_count();
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto* original = business_evidence_store_->accepted_private_event_record_at(index);
    if (original->disposition() == Disposition::BufferedGap && original->input() == input &&
        original->first_admission_resolution() == event->resolution &&
        original->trade_semantic_value() == std::optional{trade->semantic_value} &&
        original->journal_sequence() &&
        has_original_buffered_execution_audit(*business_evidence_store_, *original)) {
      return model::Result<PrivateExecutionDrainIdentity>::create_success(
          PrivateExecutionDrainIdentity{event->resolution, trade->semantic_value});
    }
  }
  return reject_private_execution_drain<PrivateExecutionDrainIdentity>(
      "private_execution_drain.buffered_evidence");
}

// --------------------------------------------------------
// Preflight actual pending identities, OMS projection, and economic replacements. Reserve the
// next real journal slot and the entire callback-bearing evidence span before sealing the plan.
model::Result<PreparedKnownPrivateExecutionDrain>
PrivateOrderReconciler::prepare_known_private_execution_drain(
    const oms::NormalizedPrivateOrderInput& input,
    recovery::JournalReplayProvenance replay_provenance,
    model::CallbackOrdinal first_callback_ordinal) const {
  using Transaction = PreparedKnownPrivateExecutionDrain;
  using Result = model::Result<Transaction>;
  using Code = model::DomainErrorCode;

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject inaccessible owners and poorly qualified first-seen inputs before acquiring leases.
  if (retention_.globally_blocked || owner_->is_runtime_faulted() ||
      retention_.emergency_turn_published.load(std::memory_order_acquire)) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.owner_blocked",
                                                       Code::ExecutionNotPermitted);
  }
  if (business_evidence_store_->has_outstanding_preparation()) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.evidence_busy",
                                                       Code::PrivateEvidenceExhausted);
  }
  const auto* execution = std::get_if<oms::ExecutionPayload>(&input.payload());
  const auto* reconciliation =
      std::get_if<oms::ReconciliationPrivateEventOrigin>(&input.origin_value());
  if (execution == nullptr ||
      (input.origin() != oms::PrivateEventOrigin::Venue && reconciliation == nullptr)) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.source",
                                                       Code::InvalidPrivateEvent);
  }
  const auto semantic = oms::PrivateEventIngressSemanticValue::from_normalized_input(input);
  const auto key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(semantic);
  if (find_committed_event_identity(key) || retention_.preparations.find_prepared_event(key)) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.compared_identity",
                                                       Code::PrivateEventConflict);
  }
  if (reconciliation != nullptr &&
      reconciliation->reconciliation_epoch_id.runtime_epoch_id() != runtime_epoch_id_) {
    return reject_private_execution_drain<Transaction>(
        "private_execution_drain.reconciliation_epoch", Code::RecoveryProvenanceMismatch);
  }
  const auto accepted_count = business_evidence_store_->accepted_private_event_record_count();
  const auto retained_count = retention_.published_turn_count.load(std::memory_order_relaxed);
  if ((accepted_count != 0U &&
       !is_later_execution_drain_receipt(
           replay_provenance,
           business_evidence_store_->accepted_private_event_record_at(accepted_count - 1U)
               ->replay_provenance())) ||
      (retained_count != 0U &&
       !is_later_execution_drain_receipt(
           replay_provenance,
           recovery::JournalReplayProvenance{
               retention_.turns[retained_count - 1U].receipt->attempt_ordinal,
               retention_.turns[retained_count - 1U].receipt->receive_sequence}))) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.receipt_order",
                                                       Code::InvalidJournalState);
  }
  auto identity = derive_first_seen_authoritative_identity_plan(semantic);
  if (!identity) {
    return Result::create_failure(std::move(identity).error());
  }
  const auto* correlation =
      std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&identity.value().correlation_plan());
  const auto* trade =
      std::get_if<FirstSeenPrivateTradeIdentityPlan>(&identity.value().trade_plan());
  const auto* known = correlation == nullptr ? nullptr : correlation->resolution.known_resolution();
  if (known == nullptr || !known->provenance.subject() || trade == nullptr ||
      identity.value().preliminary_safety_reason()) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.known_execution",
                                                       Code::PrivateCorrelationFailed);
  }
  auto identity_valid = validate_first_seen_private_execution_drain_identity(
      identity.value(), *correlation, *trade, *execution);
  if (!identity_valid) {
    return Result::create_failure(std::move(identity_valid).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Plan the actual live OMS prefix. An isolated gap, any contradiction, or a transition that
  // requires a residual cancellation release cannot produce this execution-only transaction.
  const auto* order = owner_->outbound_oms().find_order(known->order_id);
  const auto* route =
      order == nullptr ? nullptr : owner_->routes().find_route(order->provenance().route_id);
  const auto* inventory = risk::InventoryLedger::installed_inventory(owner_->reservations());
  const auto* account = find_execution_drain_account(retention_, *known->provenance.subject());
  if (order == nullptr || route == nullptr || inventory == nullptr || account == nullptr) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.owner_baseline");
  }
  const auto before = order->private_projection();
  const auto pending = pending_private_executions(known->order_id);
  auto transition = oms::derive_private_oms_transition(oms::PrivateOmsTransitionInputs{
      order->admission(), before, *route, m4_policy_, runtime_epoch_id_, input, pending, {}, 0U});
  if (!transition) {
    return Result::create_failure(std::move(transition).error());
  }
  const auto& lifecycle = transition.value();
  const auto width = lifecycle.transition_effect_count;
  if (lifecycle.classification != oms::ProposedPrivateOmsClassification::Applied ||
      lifecycle.economics_action != oms::ProposedPrivateEconomicsAction::ApplyExecutions ||
      lifecycle.order_callback_count != width ||
      width != lifecycle.drained_pending_prefix_count + 1U ||
      lifecycle.drained_pending_prefix_count > pending.size() ||
      width > execution_drain_inputs_.capacity() ||
      width > execution_drain_identities_.capacity()) {
    return reject_private_execution_drain<Transaction>(
        "private_execution_drain.contiguous_execution");
  }
  if (event_identity_record_count_ >= event_identity_records_.size() ||
      trade_identity_record_count_ >= trade_identity_records_.size() ||
      (correlation->candidate_mapping &&
       exchange_order_mapping_count_ >= exchange_order_mappings_.size())) {
    return reject_private_execution_drain<Transaction>("private_execution_drain.identity_capacity",
                                                       Code::PrivateEventCapacityExceeded);
  }
  auto span = recovery::AuditSpan::create_audit_span(business_evidence_store_->next_audit_ordinal(),
                                                     width + 2U);
  if (!span) {
    return Result::create_failure(std::move(span).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build consecutive economic inputs from the current pending set. Preserve each buffered
  // event's own identity and normalization instead of reconstructing it from the new source.
  const ExecutionDrainInputScratchReset reset{execution_drain_inputs_, execution_drain_identities_};
  execution_drain_inputs_.push_back({input, span.value().first_audit_ordinal()});
  execution_drain_identities_.push_back({correlation->resolution, trade->semantic_value});
  for (std::uint32_t index = 1U; index < width; ++index) {
    const auto& buffered = pending[index - 1U].execution;
    auto verified = verify_buffered_private_execution_identity(buffered, *known);
    if (!verified) {
      return Result::create_failure(std::move(verified).error());
    }
    auto audit_ordinal =
        recovery::AuditOrdinal::from_value(span.value().first_audit_ordinal().value() + index);
    if (!audit_ordinal) {
      return Result::create_failure(std::move(audit_ordinal).error());
    }
    execution_drain_inputs_.push_back({buffered, audit_ordinal.value()});
    execution_drain_identities_.push_back(verified.value());
  }
  auto economics = inventory->plan_cumulative_fill_batch(*order, execution_drain_inputs_);
  if (!economics) {
    return Result::create_failure(std::move(economics).error());
  }
  auto economics_valid = inventory->validate_reservation_inventory_plan(economics.value());
  if (!economics_valid) {
    return Result::create_failure(std::move(economics_valid).error());
  }
  const auto& final_hold = economics.value().reservation_after();
  if (final_hold.cumulative_confirmed_exposure.quantity !=
          lifecycle.after.cumulative_filled_quantity ||
      final_hold.closure_cause !=
          lifecycle.reservation_closure_cause.value_or(risk::ReservationClosureCause::Unassigned)) {
    return reject_private_execution_drain<Transaction>(
        "private_execution_drain.economic_postconditions", Code::InvalidReservationConversion);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind all prospective rows to the real reserved journal sequence. Later failure releases the
  // economic batch and journal leases without consuming any input, record, or counter.
  auto journal = recovery::PreparedPrivateEventJournalInput::reserve_private_event_journal_input(
      recovery_lease_, runtime_epoch_id_, input, correlation->resolution, replay_provenance,
      span.value());
  if (!journal) {
    return Result::create_failure(std::move(journal).error());
  }
  auto evidence = execution_drain_evidence_scratch_.prepare_private_execution_drain_evidence(
      *business_evidence_store_, owner_->reservations(), *route, lifecycle, economics.value(),
      execution_drain_identities_, replay_provenance, first_callback_ordinal,
      journal.value().record()->sequence());
  if (!evidence) {
    return Result::create_failure(std::move(evidence).error());
  }
  const Transaction::OwnerBaseline baseline{
      business_incarnation_,
      event_identity_record_count_,
      trade_identity_record_count_,
      exchange_order_mapping_count_,
      retention_.published_turn_count.load(std::memory_order_relaxed),
      account->cause_count,
      account->fenced_attempt_count,
      account->state};
  return Result::create_success(Transaction{Transaction::State{
      std::move(identity).value(), std::move(transition).value(), std::move(economics).value(),
      std::move(evidence).value(), std::move(journal).value(), baseline}});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reject a stale owner before following any owner-dependent identity or order. Validate the exact
// linked reservations and buffered prefix without publishing or renewing the preparation.
model::Result<void> PrivateOrderReconciler::validate_prepared_private_execution_drain(
    const PreparedKnownPrivateExecutionDrain& preparation) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // A foreign, moved, or faulted owner cannot validate another incarnation's plan.
  if (!preparation.state_ || preparation.state_->baseline.incarnation != business_incarnation_ ||
      retention_.globally_blocked || owner_->is_runtime_faulted() ||
      retention_.emergency_turn_published.load(std::memory_order_acquire)) {
    return reject_private_execution_drain<void>("private_execution_drain.owner");
  }
  const auto& state = *preparation.state_;
  const auto& baseline = state.baseline;
  const auto& correlation =
      std::get<KnownFirstSeenPrivateCorrelationPlan>(state.identity.correlation_plan());
  const auto& known = *correlation.resolution.known_resolution();
  const auto* account = find_execution_drain_account(retention_, *known.provenance.subject());
  if (account == nullptr || account->cause_count != baseline.account_cause_count ||
      account->fenced_attempt_count != baseline.account_fenced_attempt_count ||
      account->state != baseline.account_state ||
      event_identity_record_count_ != baseline.event_count ||
      trade_identity_record_count_ != baseline.trade_count ||
      exchange_order_mapping_count_ != baseline.mapping_count ||
      retention_.published_turn_count.load(std::memory_order_relaxed) !=
          baseline.retained_turn_count) {
    return reject_private_execution_drain<void>(
        "private_execution_drain.identity_or_containment_baseline");
  }
  const auto* inventory = risk::InventoryLedger::installed_inventory(owner_->reservations());
  if (inventory == nullptr) {
    return reject_private_execution_drain<void>("private_execution_drain.economic_owner");
  }
  auto economics = inventory->validate_reservation_inventory_plan(state.economics);
  if (!economics) {
    return economics;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require exact journal/evidence linkage and verify every drained execution against its live
  // slot. An immutable original BufferedGap row remains separate from its prospective application.
  auto journal = state.journal.validate_reservation();
  if (!journal) {
    return journal;
  }
  const auto* record = state.journal.record();
  const auto* event = state.evidence.private_event_evidence();
  const auto* span = state.evidence.audit_span();
  const auto* planned = state.evidence.planned_callback_record();
  const auto* payload = std::get_if<recovery::PrivateEventInputJournalPayload>(&record->payload());
  const auto width = state.transition.transition_effect_count;
  if (event == nullptr || span == nullptr || planned == nullptr || payload == nullptr ||
      !business_evidence_store_->has_outstanding_preparation() ||
      state.evidence.primary_audit_record_count() != width || planned->callback_count() != width ||
      state.evidence.reserved_terminal_audit_ordinal() !=
          std::optional{span->last_audit_ordinal()} ||
      record->audit_span() != std::optional{*span} ||
      record->replay_provenance() != std::optional{event->replay_provenance()} ||
      event->journal_sequence() != std::optional{record->sequence()} ||
      planned->journal_sequence() != event->journal_sequence() ||
      event->ingress_semantic_value() != state.identity.ingress_semantic_value() ||
      event->first_admission_resolution() != correlation.resolution ||
      payload->input() != event->input() ||
      payload->first_admission_resolution() != correlation.resolution) {
    return reject_private_execution_drain<void>("private_execution_drain.evidence_linkage");
  }
  const auto pending = pending_private_executions(known.order_id);
  if (state.transition.drained_pending_prefix_count > pending.size()) {
    return reject_private_execution_drain<void>("private_execution_drain.pending_baseline");
  }
  for (std::uint32_t index = 0U; index < width; ++index) {
    const auto& expected_input = index == 0U ? event->input() : pending[index - 1U].execution;
    const auto* effect = state.economics.execution_effect_at(index);
    const auto* row = state.evidence.primary_audit_record_at(index);
    if (effect == nullptr || row == nullptr || effect->execution != expected_input ||
        row->source_input() != std::optional{expected_input} ||
        row->audit_ordinal() != effect->audit_ordinal ||
        row->journal_sequence() != event->journal_sequence()) {
      return reject_private_execution_drain<void>("private_execution_drain.execution_linkage");
    }
    if (index != 0U) {
      auto verified = verify_buffered_private_execution_identity(expected_input, known);
      if (!verified) {
        return model::Result<void>::create_failure(std::move(verified).error());
      }
    }
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
