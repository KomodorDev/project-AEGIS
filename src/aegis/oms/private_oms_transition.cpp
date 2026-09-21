// Purpose: validate detached known-order snapshots and calculate allocation-free M4 lifecycle,
// pending-fill, and cancel-history proposals without consuming facts or mutating any owner.

#include "private_oms_transition.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::oms {
namespace {

// --------------------------------------------------------
// Return the literal ADR-0010 open venue-risk state set.
bool is_open_venue_risk(OutboundOrderState state) noexcept {
  return state == OutboundOrderState::WriteInitiated ||
         state == OutboundOrderState::SubmissionUnknown || state == OutboundOrderState::Working ||
         state == OutboundOrderState::PartiallyFilled;
}

// --------------------------------------------------------
// Return the literal ADR-0010 venue-terminal state set, excluding local failure.
bool is_terminal_venue(OutboundOrderState state) noexcept {
  return state == OutboundOrderState::Filled || state == OutboundOrderState::ExchangeRejected ||
         state == OutboundOrderState::Cancelled || state == OutboundOrderState::ReconciledAbsent;
}

// --------------------------------------------------------
// Identify cancellation states that still prevent issuing a new explicit request.
bool is_unresolved_cancel(CancellationState state) noexcept {
  return state == CancellationState::Requested || state == CancellationState::WriteInitiated ||
         state == CancellationState::OutcomeUnknown;
}

// --------------------------------------------------------
// Report invalid detached authority or state without manufacturing a partial lifecycle proposal.
model::Result<void> create_snapshot_failure(std::string_view field) {
  return model::Result<void>::create_failure(model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidPrivateOmsState, std::string{field}));
}

// --------------------------------------------------------
// Compare the complete admission against the sealed route and every inherited M4 root authority.
bool has_matching_admission_authority(const PrivateOmsTransitionInputs& inputs) noexcept {
  const auto& admission = inputs.admission;
  const auto& provenance = admission.provenance;
  const auto& route = inputs.route;
  const auto& root = inputs.policy.root_provenance();
  return provenance.route_id == route.route().id && provenance.venue_id == route.route().venue_id &&
         provenance.logical_account_id == route.route().logical_account_id &&
         provenance.instrument_id == route.metadata().instrument_id() &&
         provenance.venue_instrument_id == route.metadata().venue_instrument_id() &&
         provenance.firm_id == route.attribution().firm_id &&
         provenance.desk_id == route.attribution().desk_id &&
         provenance.bot_id == route.attribution().bot_id &&
         provenance.strategy_id == route.attribution().strategy_id &&
         provenance.configuration_fingerprint == root.configuration_fingerprint() &&
         provenance.configuration_fingerprint == route.configuration_fingerprint().bytes() &&
         provenance.configuration_revision == route.configuration_revision() &&
         provenance.organization_revision == root.organization_revision() &&
         provenance.organization_revision == route.organization_revision() &&
         provenance.route_revision == route.route_revision() &&
         provenance.metadata_revision == route.metadata().revision() &&
         provenance.runtime_policy_fingerprint == root.runtime_policy_fingerprint() &&
         provenance.risk_policy_fingerprint == root.risk_policy_fingerprint() &&
         provenance.risk_policy_revision == root.risk_policy_revision() &&
         provenance.submission_policy_fingerprint == root.submission_policy_fingerprint() &&
         admission.exposure.quantity == admission.economics.quantity &&
         admission.reservation_id.value() == admission.attempt_id.value();
}

// --------------------------------------------------------
// Require one source fact to belong to the admitted account, venue, and installed policy root.
bool has_matching_event_authority(const PrivateOmsTransitionInputs& inputs,
                                  const NormalizedPrivateOrderInput& input) noexcept {
  return input.subject_scope() == PrivateEventSubjectScope::Order &&
         input.logical_account_id() == inputs.admission.provenance.logical_account_id &&
         input.venue_id() == inputs.admission.provenance.venue_id &&
         input.provenance().root() == inputs.policy.root_provenance();
}

// --------------------------------------------------------
// Check local snapshot correlation without pretending to own the global exchange-key registry.
bool has_consistent_locator(const PrivateOmsTransitionInputs& inputs,
                            const PrivateOrderLocator& locator) noexcept {
  if (locator.local_order_id() && *locator.local_order_id() != inputs.admission.order_id) {
    return false;
  }
  if (inputs.before.exchange_order_id && locator.exchange_order_id() &&
      *inputs.before.exchange_order_id != *locator.exchange_order_id()) {
    return false;
  }
  return locator.local_order_id().has_value() ||
         (inputs.before.exchange_order_id && locator.exchange_order_id() &&
          *inputs.before.exchange_order_id == *locator.exchange_order_id());
}

// --------------------------------------------------------
// Validate a source interval against immutable order economics before deriving its exact start.
// Sealed metadata guarantees positive tick/step divisors, so their divisibility queries cannot
// fail.
std::optional<model::Quantity>
calculate_validated_execution_start(const PrivateOmsTransitionInputs& inputs,
                                    const NormalizedPrivateOrderInput& input) {
  const auto* execution = std::get_if<ExecutionPayload>(&input.payload());
  if (execution == nullptr || input.origin() == PrivateEventOrigin::Local ||
      !has_matching_event_authority(inputs, input) ||
      !has_consistent_locator(inputs, execution->locator) ||
      execution->instrument_id != inputs.admission.provenance.instrument_id ||
      execution->metadata_revision != inputs.admission.provenance.metadata_revision ||
      (execution->source_side && *execution->source_side != inputs.admission.economics.side) ||
      (input.origin() == PrivateEventOrigin::Reconciliation && !execution->source_side) ||
      execution->incremental_quantity.coefficient() <= 0 ||
      execution->cumulative_quantity.coefficient() <= 0 ||
      execution->cumulative_quantity > inputs.admission.economics.quantity ||
      execution->execution_price.coefficient() <= 0 ||
      execution->execution_price.scale() > inputs.route.metadata().price_scale() ||
      execution->incremental_quantity.scale() > inputs.route.metadata().quantity_scale() ||
      execution->cumulative_quantity.scale() > inputs.route.metadata().quantity_scale() ||
      (inputs.admission.economics.side == execution::OrderSide::Buy &&
       execution->execution_price > inputs.admission.economics.price) ||
      (inputs.admission.economics.side == execution::OrderSide::Sell &&
       execution->execution_price < inputs.admission.economics.price) ||
      !execution->execution_price.is_multiple_of(inputs.route.metadata().tick_size()).value() ||
      !execution->incremental_quantity.is_multiple_of(inputs.route.metadata().quantity_step())
           .value() ||
      !execution->cumulative_quantity.is_multiple_of(inputs.route.metadata().quantity_step())
           .value()) {
    return std::nullopt;
  }
  auto start = execution->cumulative_quantity.checked_subtract(execution->incremental_quantity);
  if (!start || start.value().coefficient() < 0 ||
      (inputs.before.authoritative_terminal_cumulative_quantity &&
       execution->cumulative_quantity >
           *inputs.before.authoritative_terminal_cumulative_quantity)) {
    return std::nullopt;
  }
  return start.value();
}

// --------------------------------------------------------
// Validate scalar projection consistency without inferring a terminal cancel-attempt close that
// the literal late-outcome partition deliberately permits to remain Requested.
model::Result<void> validate_projection_snapshot(const PrivateOmsTransitionInputs& inputs) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject incompatible admission authority, nominal economics, and scalar/count representations.
  const auto& before = inputs.before;
  const auto& quantity = inputs.admission.economics.quantity;
  if (!has_matching_admission_authority(inputs) ||
      !has_matching_event_authority(inputs, inputs.input) || quantity.coefficient() <= 0 ||
      inputs.admission.exposure.quote_notional.coefficient() <= 0 ||
      (inputs.admission.economics.side != execution::OrderSide::Buy &&
       inputs.admission.economics.side != execution::OrderSide::Sell) ||
      inputs.admission.economics.type != execution::OrderType::Limit ||
      inputs.admission.economics.time_in_force != execution::TimeInForce::GoodTilCancelled ||
      inputs.admission.economics.price.coefficient() <= 0 ||
      quantity.scale() > inputs.route.metadata().quantity_scale() ||
      inputs.admission.economics.price.scale() > inputs.route.metadata().price_scale() ||
      !inputs.route.metadata().validate_quantity_alignment(quantity) ||
      !inputs.route.metadata().validate_price_alignment(inputs.admission.economics.price) ||
      before.state < OutboundOrderState::PendingEncoding ||
      before.state > OutboundOrderState::ReconciledAbsent ||
      before.cancellation_state < CancellationState::None ||
      before.cancellation_state > CancellationState::Confirmed ||
      before.cumulative_filled_quantity.coefficient() < 0 ||
      before.cumulative_filled_quantity > quantity ||
      before.cumulative_filled_quantity.scale() > inputs.route.metadata().quantity_scale() ||
      !inputs.route.metadata().validate_quantity_alignment(before.cumulative_filled_quantity) ||
      (before.exchange_acknowledged && !before.exchange_order_id) ||
      (before.exchange_mapping_established_by_execution &&
       (!before.exchange_order_id || !before.execution_evidence_observed)) ||
      (before.cumulative_filled_quantity.coefficient() > 0 &&
       !before.execution_evidence_observed) ||
      (before.state == OutboundOrderState::Filled &&
       before.cumulative_filled_quantity != quantity) ||
      (is_open_venue_risk(before.state) && before.cumulative_filled_quantity >= quantity) ||
      (before.state == OutboundOrderState::PartiallyFilled &&
       before.cumulative_filled_quantity.coefficient() == 0) ||
      (before.state == OutboundOrderState::SubmissionUnknown && !before.reconciliation_required) ||
      before.pending_fill_count != inputs.pending.size() ||
      before.cancel_attempt_count != inputs.cancels.size() ||
      inputs.global_cancel_attempt_count < inputs.cancels.size() ||
      inputs.pending.size() > inputs.policy.capacities().max_pending_fill_intervals_per_order ||
      inputs.global_cancel_attempt_count > inputs.policy.capacities().max_cancel_attempts) {
    return create_snapshot_failure("private_oms.snapshot");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate lifecycle invariants separately from the monotonic execution-evidence cache: even a
  // terminal or pre-initiation row may observe a later contradictory execution without a fill.
  const bool pre_initiation = before.state == OutboundOrderState::PendingEncoding ||
                              before.state == OutboundOrderState::PendingInitiation;
  const bool locally_failed = before.state == OutboundOrderState::LocallyFailed;
  const bool zero_cumulative_state = pre_initiation || locally_failed ||
                                     before.state == OutboundOrderState::WriteInitiated ||
                                     before.state == OutboundOrderState::SubmissionUnknown ||
                                     before.state == OutboundOrderState::Working ||
                                     before.state == OutboundOrderState::ExchangeRejected;
  if ((zero_cumulative_state && before.cumulative_filled_quantity.coefficient() != 0) ||
      ((is_terminal_venue(before.state) || pre_initiation || locally_failed) &&
       before.reconciliation_required) ||
      (before.state == OutboundOrderState::Working && !before.exchange_acknowledged) ||
      ((before.state == OutboundOrderState::WriteInitiated ||
        before.state == OutboundOrderState::SubmissionUnknown ||
        before.state == OutboundOrderState::ExchangeRejected) &&
       before.exchange_acknowledged) ||
      ((pre_initiation || locally_failed) &&
       (before.exchange_acknowledged || before.exchange_order_id ||
        before.cancellation_state != CancellationState::None || !inputs.cancels.empty())) ||
      ((pre_initiation || locally_failed || before.state == OutboundOrderState::ExchangeRejected ||
        before.state == OutboundOrderState::ReconciledAbsent) &&
       before.authoritative_terminal_cumulative_quantity) ||
      (before.state == OutboundOrderState::ReconciledAbsent &&
       before.cumulative_filled_quantity >= quantity)) {
    return create_snapshot_failure("private_oms.lifecycle_snapshot");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Confirmation requires its monotonic terminal target; an absent target must never let a later
  // rejection restore cancel retry eligibility. Open orders must remain below the aligned bound.
  if (before.cancellation_state == CancellationState::Confirmed &&
      !before.authoritative_terminal_cumulative_quantity) {
    return create_snapshot_failure("private_oms.terminal_target");
  }
  if (before.authoritative_terminal_cumulative_quantity) {
    const auto& target = *before.authoritative_terminal_cumulative_quantity;
    if (target < before.cumulative_filled_quantity || target > quantity ||
        target.scale() > inputs.route.metadata().quantity_scale() ||
        !inputs.route.metadata().validate_quantity_alignment(target) ||
        (is_open_venue_risk(before.state) &&
         (target == before.cumulative_filled_quantity || !before.reconciliation_required ||
          before.cancellation_state != CancellationState::Confirmed))) {
      return create_snapshot_failure("private_oms.terminal_target");
    }
  }
  if (before.state == OutboundOrderState::Cancelled &&
      (!before.authoritative_terminal_cumulative_quantity ||
       *before.authoritative_terminal_cumulative_quantity != before.cumulative_filled_quantity ||
       before.cumulative_filled_quantity >= quantity)) {
    return create_snapshot_failure("private_oms.cancelled_snapshot");
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Check retained immutable intervals in canonical endpoint order before any incoming proposal.
model::Result<void> validate_pending_snapshot(const PrivateOmsTransitionInputs& inputs) {
  auto previous_endpoint = inputs.before.cumulative_filled_quantity;
  for (const auto& pending : inputs.pending) {
    const auto start = calculate_validated_execution_start(inputs, pending.execution);
    const auto* execution = std::get_if<ExecutionPayload>(&pending.execution.payload());
    if (!start || execution == nullptr || !is_open_venue_risk(inputs.before.state) ||
        !inputs.before.execution_evidence_observed ||
        start.value() <= inputs.before.cumulative_filled_quantity ||
        start.value() < previous_endpoint) {
      return create_snapshot_failure("private_oms.pending_snapshot");
    }
    previous_endpoint = execution->cumulative_quantity;
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Map the three accepted local certainty values without assigning venue-terminal meaning.
CancellationState cancellation_state_from_write_outcome(CancelWriteOutcome outcome) noexcept {
  switch (outcome) {
  case CancelWriteOutcome::DefiniteFailureBeforeAcceptance:
    return CancellationState::DefinitelyFailed;
  case CancelWriteOutcome::AcceptedAndInitiated:
    return CancellationState::WriteInitiated;
  case CancelWriteOutcome::AcceptedThenOutcomeLost:
    return CancellationState::OutcomeUnknown;
  }
  return CancellationState::Unassigned;
}

// --------------------------------------------------------
// Require complete retained cancel identities and source evidence, preserving old runtime epochs
// while allowing only the latest append to remain unresolved.
model::Result<void> validate_cancel_snapshot(const PrivateOmsTransitionInputs& inputs) {
  std::size_t unresolved_count = 0U;
  for (std::size_t index = 0U; index < inputs.cancels.size(); ++index) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Prove each source request names this exact order and advances its own epoch's ordinal.
    const auto& attempt = inputs.cancels[index];
    const auto* request = std::get_if<CancelRequestedPayload>(&attempt.request.payload());
    if (!has_matching_event_authority(inputs, attempt.request) ||
        attempt.request.origin() != PrivateEventOrigin::Local || request == nullptr ||
        request->order_id != inputs.admission.order_id ||
        request->cancel_attempt_id != attempt.id ||
        attempt.id.order_id() != inputs.admission.order_id ||
        attempt.state < CancellationState::Requested ||
        attempt.state > CancellationState::Confirmed ||
        (attempt.state == CancellationState::Confirmed &&
         !inputs.before.authoritative_terminal_cumulative_quantity)) {
      return create_snapshot_failure("private_oms.cancel_history");
    }
    for (std::size_t earlier = 0U; earlier < index; ++earlier) {
      const auto& prior_id = inputs.cancels[earlier].id;
      if (prior_id.runtime_epoch_id() == attempt.id.runtime_epoch_id() &&
          prior_id.ordinal() >= attempt.id.ordinal()) {
        return create_snapshot_failure("private_oms.cancel_ordinal_order");
      }
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Only the latest append may stay unresolved and control the order-level cancellation state.
    if (is_unresolved_cancel(attempt.state)) {
      ++unresolved_count;
      if (index + 1U != inputs.cancels.size() ||
          inputs.before.cancellation_state != attempt.state) {
        return create_snapshot_failure("private_oms.current_cancel");
      }
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // First write certainty must agree with the attempt's state or its later authoritative close.
    if (attempt.write_outcome) {
      const auto* outcome =
          std::get_if<CancelWriteOutcomePayload>(&attempt.write_outcome->payload());
      if (!has_matching_event_authority(inputs, *attempt.write_outcome) ||
          attempt.write_outcome->origin() != PrivateEventOrigin::Local || outcome == nullptr ||
          outcome->order_id != inputs.admission.order_id ||
          outcome->cancel_attempt_id != attempt.id ||
          attempt.state == CancellationState::Requested ||
          (attempt.state != CancellationState::Rejected &&
           attempt.state != CancellationState::Confirmed &&
           attempt.state != cancellation_state_from_write_outcome(outcome->outcome)) ||
          (attempt.state == CancellationState::Rejected &&
           outcome->outcome == CancelWriteOutcome::DefiniteFailureBeforeAcceptance)) {
        return create_snapshot_failure("private_oms.cancel_write_history");
      }
    } else if (attempt.state == CancellationState::WriteInitiated ||
               attempt.state == CancellationState::OutcomeUnknown ||
               attempt.state == CancellationState::DefinitelyFailed) {
      return create_snapshot_failure("private_oms.missing_cancel_write");
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // A causal close retains exact source proof; confirmation can retain later causal history.
    if (attempt.causal_rejection) {
      const auto* rejection =
          std::get_if<CancellationResultPayload>(&attempt.causal_rejection->payload());
      if (!has_matching_event_authority(inputs, *attempt.causal_rejection) ||
          attempt.causal_rejection->origin() != PrivateEventOrigin::Venue || rejection == nullptr ||
          rejection->result != CancellationResult::CancelRejected ||
          rejection->causal_cancel_attempt_id != attempt.id ||
          !has_consistent_locator(inputs, rejection->locator) ||
          (attempt.state != CancellationState::Rejected &&
           attempt.state != CancellationState::Confirmed)) {
        return create_snapshot_failure("private_oms.cancel_rejection_history");
      }
    } else if (attempt.state == CancellationState::Rejected) {
      return create_snapshot_failure("private_oms.missing_causal_rejection");
    }

    // ++++++++++++++++++++++++++++++++++++++++
  }
  if (unresolved_count > 1U ||
      (is_unresolved_cancel(inputs.before.cancellation_state) && unresolved_count != 1U)) {
    return create_snapshot_failure("private_oms.unresolved_cancel_count");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Start a value-only accepted proposal with one effect and one known-order notification.
PrivateOmsTransitionPlan create_initial_transition_plan(const PrivateOmsTransitionInputs& inputs) {
  return PrivateOmsTransitionPlan{inputs.before,
                                  inputs.before,
                                  ProposedPrivateOmsClassification::Applied,
                                  ProposedPrivateEconomicsAction::None,
                                  std::nullopt,
                                  std::nullopt,
                                  std::nullopt,
                                  std::nullopt,
                                  0U,
                                  std::nullopt,
                                  1U,
                                  1U};
}

// --------------------------------------------------------
// Classify the exhaustive remainder while preserving all projection fields except the exact
// monotonic known-execution cache required even for a safety-contained source-side contradiction.
PrivateOmsTransitionPlan create_remainder_plan(const PrivateOmsTransitionInputs& inputs) {
  auto plan = create_initial_transition_plan(inputs);
  if (inputs.input.origin() == PrivateEventOrigin::Local) {
    plan.classification = ProposedPrivateOmsClassification::ForbiddenRejected;
    plan.rejection_code = model::DomainErrorCode::InvalidPrivateOmsState;
    plan.order_callback_count = 0U;
  } else {
    plan.classification = ProposedPrivateOmsClassification::SafetyContained;
    plan.safety_reason = risk::AccountSafetyReason::AuthoritativeContradiction;
    if (inputs.input.kind() == PrivateOrderEventKind::Execution) {
      plan.after.execution_evidence_observed = true;
    }
  }
  return plan;
}

// --------------------------------------------------------
// Add a missing candidate exchange identity only after an authoritative transition was accepted.
void propose_exchange_mapping(PrivateOmsTransitionPlan& plan,
                              const std::optional<ExchangeOrderId>& exchange_id,
                              bool established_by_execution) {
  if (!plan.after.exchange_order_id && exchange_id) {
    plan.after.exchange_order_id = exchange_id;
    plan.after.exchange_mapping_established_by_execution = established_by_execution;
  }
}

// --------------------------------------------------------
// Resolve an exact epoch-qualified attempt without letting a late fact select a newer request.
std::optional<std::size_t> find_cancel_attempt_index(const PrivateOmsTransitionInputs& inputs,
                                                     const CancelAttemptId& id) noexcept {
  for (std::size_t index = 0U; index < inputs.cancels.size(); ++index) {
    if (inputs.cancels[index].id == id) {
      return index;
    }
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Find the sole validated unresolved attempt; terminal primary state does not imply its closure.
std::optional<std::size_t>
find_unresolved_cancel_index(const PrivateOmsTransitionInputs& inputs) noexcept {
  if (!inputs.cancels.empty() && is_unresolved_cancel(inputs.cancels.back().state)) {
    return inputs.cancels.size() - 1U;
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Close only the sole unresolved attempt after authoritative terminal cancellation was accepted.
void propose_cancel_confirmation(const PrivateOmsTransitionInputs& inputs,
                                 PrivateOmsTransitionPlan& plan) {
  plan.after.cancellation_state = CancellationState::Confirmed;
  if (const auto unresolved = find_unresolved_cancel_index(inputs)) {
    auto replacement = inputs.cancels[*unresolved];
    replacement.state = CancellationState::Confirmed;
    plan.cancel_history_change = ProposedCancelHistoryChange{*unresolved, std::move(replacement)};
  }
}

// --------------------------------------------------------
// Evaluate acknowledgement against the complete accepted state set and immutable local mapping.
PrivateOmsTransitionPlan
derive_acknowledgement_transition(const PrivateOmsTransitionInputs& inputs,
                                  const ExchangeAcknowledgedPayload& acknowledgement) {
  const auto state = inputs.before.state;
  if ((acknowledgement.local_order_locator &&
       *acknowledgement.local_order_locator != inputs.admission.order_id) ||
      (inputs.before.exchange_order_id &&
       *inputs.before.exchange_order_id != acknowledgement.exchange_order_id) ||
      (!acknowledgement.local_order_locator && !inputs.before.exchange_order_id) ||
      (!is_open_venue_risk(state) && state != OutboundOrderState::Filled &&
       state != OutboundOrderState::Cancelled)) {
    return create_remainder_plan(inputs);
  }
  auto plan = create_initial_transition_plan(inputs);
  plan.after.exchange_acknowledged = true;
  propose_exchange_mapping(plan, acknowledgement.exchange_order_id, false);
  if (state == OutboundOrderState::WriteInitiated ||
      state == OutboundOrderState::SubmissionUnknown) {
    plan.after.state = OutboundOrderState::Working;
  } else {
    plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  }
  return plan;
}

// --------------------------------------------------------
// Accept a pre-fill rejection exactly once, preserving all later rejection evidence as projection.
PrivateOmsTransitionPlan
derive_exchange_rejection_transition(const PrivateOmsTransitionInputs& inputs,
                                     const ExchangeRejectedPayload& rejection) {
  if (!has_consistent_locator(inputs, rejection.locator)) {
    return create_remainder_plan(inputs);
  }
  auto plan = create_initial_transition_plan(inputs);
  if (inputs.before.state == OutboundOrderState::ExchangeRejected) {
    plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  } else if ((inputs.before.state == OutboundOrderState::WriteInitiated ||
              inputs.before.state == OutboundOrderState::SubmissionUnknown) &&
             !inputs.before.exchange_acknowledged && !inputs.before.execution_evidence_observed &&
             !inputs.before.authoritative_terminal_cumulative_quantity) {
    plan.after.state = OutboundOrderState::ExchangeRejected;
    plan.after.reconciliation_required = false;
    plan.economics_action = ProposedPrivateEconomicsAction::ReleaseResidual;
    plan.reservation_closure_cause = risk::ReservationClosureCause::ExchangeRejected;
  } else {
    return create_remainder_plan(inputs);
  }
  propose_exchange_mapping(plan, rejection.locator.exchange_order_id(), false);
  return plan;
}

// --------------------------------------------------------
// Derive one execution followed by every newly contiguous gap; no independent prefix becomes an
// economic proposal before the final endpoint and every overlap have been checked.
PrivateOmsTransitionPlan derive_execution_transition(const PrivateOmsTransitionInputs& inputs,
                                                     const ExecutionPayload& execution) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject contradictory economics and any overlap with the complete retained pending sequence.
  const auto start = calculate_validated_execution_start(inputs, inputs.input);
  if (!start || !is_open_venue_risk(inputs.before.state) ||
      start.value() < inputs.before.cumulative_filled_quantity) {
    return create_remainder_plan(inputs);
  }
  for (const auto& pending : inputs.pending) {
    const auto& retained = std::get<ExecutionPayload>(pending.execution.payload());
    const auto retained_start =
        retained.cumulative_quantity.checked_subtract(retained.incremental_quantity);
    if (start.value() < retained.cumulative_quantity &&
        retained_start.value() < execution.cumulative_quantity) {
      return create_remainder_plan(inputs);
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A gap changes only retained interval metadata, execution evidence, and candidate correlation.
  auto plan = create_initial_transition_plan(inputs);
  plan.after.execution_evidence_observed = true;
  propose_exchange_mapping(plan, execution.locator.exchange_order_id(), true);
  if (start.value() > inputs.before.cumulative_filled_quantity) {
    plan.classification = ProposedPrivateOmsClassification::BufferedGap;
    plan.gap_insertion = PendingPrivateExecution{inputs.input};
    plan.after.pending_fill_count += 1U;
    plan.order_callback_count = 0U;
    return plan;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The immutable sorted prefix names the drain order without allocating result-sized storage.
  auto final_quantity = execution.cumulative_quantity;
  for (const auto& pending : inputs.pending) {
    const auto& retained = std::get<ExecutionPayload>(pending.execution.payload());
    const auto retained_start =
        retained.cumulative_quantity.checked_subtract(retained.incremental_quantity);
    if (retained_start.value() != final_quantity) {
      break;
    }
    final_quantity = retained.cumulative_quantity;
    ++plan.drained_pending_prefix_count;
  }
  plan.after.cumulative_filled_quantity = final_quantity;
  plan.after.pending_fill_count -= plan.drained_pending_prefix_count;
  plan.transition_effect_count += plan.drained_pending_prefix_count;
  plan.order_callback_count += plan.drained_pending_prefix_count;
  plan.economics_action = ProposedPrivateEconomicsAction::ApplyExecutions;
  if (final_quantity == inputs.admission.economics.quantity) {
    plan.after.state = OutboundOrderState::Filled;
    plan.after.reconciliation_required = false;
    plan.reservation_closure_cause = risk::ReservationClosureCause::FullFill;
  } else if (inputs.before.authoritative_terminal_cumulative_quantity &&
             final_quantity == *inputs.before.authoritative_terminal_cumulative_quantity) {
    plan.after.state = OutboundOrderState::Cancelled;
    plan.after.reconciliation_required = false;
    plan.economics_action = ProposedPrivateEconomicsAction::ApplyExecutionsThenReleaseResidual;
    plan.reservation_closure_cause = risk::ReservationClosureCause::DefinitiveCancellation;
  } else {
    plan.after.state = OutboundOrderState::PartiallyFilled;
  }
  return plan;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Authorize one new ordinal from the installed runtime after every prior unresolved request closed.
PrivateOmsTransitionPlan derive_cancel_request_transition(const PrivateOmsTransitionInputs& inputs,
                                                          const CancelRequestedPayload& request) {
  const auto state = inputs.before.cancellation_state;
  if (!is_open_venue_risk(inputs.before.state) ||
      (state != CancellationState::None && state != CancellationState::DefinitelyFailed &&
       state != CancellationState::Rejected) ||
      find_unresolved_cancel_index(inputs) ||
      inputs.before.authoritative_terminal_cumulative_quantity ||
      request.order_id != inputs.admission.order_id ||
      request.cancel_attempt_id.order_id() != inputs.admission.order_id ||
      request.cancel_attempt_id.runtime_epoch_id() != inputs.runtime_epoch) {
    return create_remainder_plan(inputs);
  }
  std::uint64_t previous_ordinal = 0U;
  for (const auto& retained : inputs.cancels) {
    if (retained.id.runtime_epoch_id() == inputs.runtime_epoch) {
      previous_ordinal = retained.id.ordinal();
    }
  }
  if (previous_ordinal == std::numeric_limits<std::uint64_t>::max() ||
      request.cancel_attempt_id.ordinal() != previous_ordinal + 1U) {
    return create_remainder_plan(inputs);
  }
  auto plan = create_initial_transition_plan(inputs);
  plan.after.cancellation_state = CancellationState::Requested;
  plan.after.cancel_attempt_count += 1U;
  plan.cancel_history_change = ProposedCancelHistoryChange{
      inputs.cancels.size(),
      RetainedCancelAttempt{request.cancel_attempt_id, inputs.input, CancellationState::Requested,
                            std::nullopt, std::nullopt}};
  return plan;
}

// --------------------------------------------------------
// Preserve the first exact write outcome on current or already authoritatively closed attempts;
// late evidence can never change a newer request or downgrade an order-level confirmation.
PrivateOmsTransitionPlan derive_cancel_write_transition(const PrivateOmsTransitionInputs& inputs,
                                                        const CancelWriteOutcomePayload& outcome) {
  const auto index = find_cancel_attempt_index(inputs, outcome.cancel_attempt_id);
  if ((!is_open_venue_risk(inputs.before.state) && !is_terminal_venue(inputs.before.state)) ||
      outcome.order_id != inputs.admission.order_id || !index ||
      inputs.cancels[*index].write_outcome ||
      cancellation_state_from_write_outcome(outcome.outcome) == CancellationState::Unassigned) {
    return create_remainder_plan(inputs);
  }
  auto replacement = inputs.cancels[*index];
  const bool current_requested =
      replacement.state == CancellationState::Requested && *index + 1U == inputs.cancels.size();
  const bool late_rejected = replacement.state == CancellationState::Rejected &&
                             outcome.outcome != CancelWriteOutcome::DefiniteFailureBeforeAcceptance;
  const bool late_confirmed = replacement.state == CancellationState::Confirmed &&
                              (is_open_venue_risk(inputs.before.state) ||
                               inputs.before.state == OutboundOrderState::Filled ||
                               inputs.before.state == OutboundOrderState::Cancelled);
  if (!current_requested && !late_rejected && !late_confirmed) {
    return create_remainder_plan(inputs);
  }
  auto plan = create_initial_transition_plan(inputs);
  replacement.write_outcome = inputs.input;
  if (current_requested) {
    replacement.state = cancellation_state_from_write_outcome(outcome.outcome);
    plan.after.cancellation_state = replacement.state;
  } else {
    plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  }
  plan.cancel_history_change = ProposedCancelHistoryChange{*index, std::move(replacement)};
  return plan;
}

// --------------------------------------------------------
// Apply causal rejection only to the named retained attempt, with terminal confirmation dominant.
PrivateOmsTransitionPlan
derive_cancel_rejection_transition(const PrivateOmsTransitionInputs& inputs,
                                   const CancellationResultPayload& result) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject unavailable or definitely unaccepted causal identities before selecting any state row.
  if ((!is_open_venue_risk(inputs.before.state) && !is_terminal_venue(inputs.before.state)) ||
      !has_consistent_locator(inputs, result.locator)) {
    return create_remainder_plan(inputs);
  }
  const auto index = result.causal_cancel_attempt_id
                         ? find_cancel_attempt_index(inputs, *result.causal_cancel_attempt_id)
                         : std::nullopt;
  if (result.causal_cancel_attempt_id &&
      (result.causal_cancel_attempt_id->order_id() != inputs.admission.order_id || !index ||
       inputs.cancels[*index].state == CancellationState::DefinitelyFailed)) {
    return create_remainder_plan(inputs);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A retained cancellation target admits only history for an already closed named attempt.
  const auto unresolved = find_unresolved_cancel_index(inputs);
  const bool target_dominates =
      inputs.before.authoritative_terminal_cumulative_quantity.has_value() &&
      (is_open_venue_risk(inputs.before.state) ||
       inputs.before.state == OutboundOrderState::Cancelled);
  const bool named_rejected = index && inputs.cancels[*index].state == CancellationState::Rejected;
  const bool named_confirmed =
      index && inputs.cancels[*index].state == CancellationState::Confirmed;
  const bool named_unresolved = index && unresolved && *index == *unresolved;
  if ((target_dominates && index && !named_rejected && !named_confirmed) ||
      (inputs.before.state == OutboundOrderState::Cancelled && index && !named_rejected &&
       !named_confirmed) ||
      ((inputs.before.state == OutboundOrderState::ExchangeRejected ||
        inputs.before.state == OutboundOrderState::ReconciledAbsent) &&
       named_confirmed) ||
      (!target_dominates && is_open_venue_risk(inputs.before.state) && named_confirmed) ||
      (index && !named_unresolved && !named_rejected && !named_confirmed)) {
    return create_remainder_plan(inputs);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Close only a causal unresolved attempt; otherwise retain exact history without touching it.
  auto plan = create_initial_transition_plan(inputs);
  plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  if (index) {
    auto replacement = inputs.cancels[*index];
    const bool has_first_causal_rejection = replacement.causal_rejection.has_value();
    if (!has_first_causal_rejection) {
      replacement.causal_rejection = inputs.input;
    }
    if (named_unresolved && !target_dominates) {
      replacement.state = CancellationState::Rejected;
      plan.after.cancellation_state = CancellationState::Rejected;
      plan.classification = ProposedPrivateOmsClassification::Applied;
    }
    if (!has_first_causal_rejection) {
      plan.cancel_history_change = ProposedCancelHistoryChange{*index, std::move(replacement)};
    }
  } else if (is_open_venue_risk(inputs.before.state) && !target_dominates && !unresolved) {
    plan.after.cancellation_state = CancellationState::Rejected;
    plan.classification = ProposedPrivateOmsClassification::Applied;
  }
  propose_exchange_mapping(plan, result.locator.exchange_order_id(), false);
  return plan;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Establish or repeat an authoritative cumulative cancellation target after bounding every gap.
PrivateOmsTransitionPlan derive_cancelled_transition(const PrivateOmsTransitionInputs& inputs,
                                                     const CancellationResultPayload& result) {
  if (!has_consistent_locator(inputs, result.locator) || !result.terminal_cumulative_quantity ||
      result.causal_cancel_attempt_id) {
    return create_remainder_plan(inputs);
  }
  const auto target = *result.terminal_cumulative_quantity;
  if (target < inputs.before.cumulative_filled_quantity ||
      target > inputs.admission.economics.quantity ||
      target.scale() > inputs.route.metadata().quantity_scale() ||
      !target.is_multiple_of(inputs.route.metadata().quantity_step()).value() ||
      (inputs.before.authoritative_terminal_cumulative_quantity &&
       *inputs.before.authoritative_terminal_cumulative_quantity != target)) {
    return create_remainder_plan(inputs);
  }
  for (const auto& pending : inputs.pending) {
    if (std::get<ExecutionPayload>(pending.execution.payload()).cumulative_quantity > target) {
      return create_remainder_plan(inputs);
    }
  }
  auto plan = create_initial_transition_plan(inputs);
  if (inputs.before.state == OutboundOrderState::Filled &&
      target == inputs.admission.economics.quantity) {
    plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  } else if (inputs.before.state == OutboundOrderState::Cancelled &&
             inputs.before.authoritative_terminal_cumulative_quantity == target) {
    plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
  } else if (is_open_venue_risk(inputs.before.state)) {
    if (target == inputs.before.cumulative_filled_quantity &&
        !inputs.before.authoritative_terminal_cumulative_quantity &&
        target < inputs.admission.economics.quantity) {
      plan.after.state = OutboundOrderState::Cancelled;
      plan.after.reconciliation_required = false;
      plan.economics_action = ProposedPrivateEconomicsAction::ReleaseResidual;
      plan.reservation_closure_cause = risk::ReservationClosureCause::DefinitiveCancellation;
    } else if (target > inputs.before.cumulative_filled_quantity) {
      plan.after.reconciliation_required = true;
    } else {
      return create_remainder_plan(inputs);
    }
  } else {
    return create_remainder_plan(inputs);
  }
  plan.after.authoritative_terminal_cumulative_quantity = target;
  propose_cancel_confirmation(inputs, plan);
  propose_exchange_mapping(plan, result.locator.exchange_order_id(), false);
  return plan;
}

// --------------------------------------------------------
// Dispatch the closed normalized vocabulary after the complete detached context is validated.
PrivateOmsTransitionPlan derive_validated_transition(const PrivateOmsTransitionInputs& inputs) {
  const auto& payload = inputs.input.payload();
  if (const auto* acknowledgement = std::get_if<ExchangeAcknowledgedPayload>(&payload)) {
    return derive_acknowledgement_transition(inputs, *acknowledgement);
  }
  if (const auto* rejection = std::get_if<ExchangeRejectedPayload>(&payload)) {
    return derive_exchange_rejection_transition(inputs, *rejection);
  }
  if (const auto* execution = std::get_if<ExecutionPayload>(&payload)) {
    return derive_execution_transition(inputs, *execution);
  }
  if (const auto* request = std::get_if<CancelRequestedPayload>(&payload)) {
    return derive_cancel_request_transition(inputs, *request);
  }
  if (const auto* outcome = std::get_if<CancelWriteOutcomePayload>(&payload)) {
    return derive_cancel_write_transition(inputs, *outcome);
  }
  if (const auto* result = std::get_if<CancellationResultPayload>(&payload)) {
    return result->result == CancellationResult::Cancelled
               ? derive_cancelled_transition(inputs, *result)
               : derive_cancel_rejection_transition(inputs, *result);
  }
  if (const auto* failure = std::get_if<LocalFailurePayload>(&payload)) {
    if (failure->order_id == inputs.admission.order_id &&
        failure->submission_attempt_id == inputs.admission.attempt_id &&
        ((inputs.before.state == OutboundOrderState::LocallyFailed &&
          failure->certainty == LocalFailureCertainty::ProvenBeforeAcceptance) ||
         (inputs.before.state == OutboundOrderState::SubmissionUnknown &&
          failure->certainty == LocalFailureCertainty::AcceptanceCouldHaveOccurred))) {
      auto plan = create_initial_transition_plan(inputs);
      plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
      return plan;
    }
  }
  if (const auto* timeout = std::get_if<OrderTimeoutObservedPayload>(&payload)) {
    if (timeout->order_id == inputs.admission.order_id &&
        (is_open_venue_risk(inputs.before.state) || is_terminal_venue(inputs.before.state))) {
      auto plan = create_initial_transition_plan(inputs);
      if (is_open_venue_risk(inputs.before.state)) {
        plan.after.reconciliation_required = true;
        plan.safety_reason = risk::AccountSafetyReason::TimeoutObserved;
      } else {
        plan.classification = ProposedPrivateOmsClassification::ProjectionOnly;
      }
      return plan;
    }
  }
  return create_remainder_plan(inputs);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate owner-independent snapshots first, then capacity-check the complete detached proposal;
// neither successful nor failed derivation alters any source object or canonical event registry.
model::Result<PrivateOmsTransitionPlan>
derive_private_oms_transition(const PrivateOmsTransitionInputs& inputs) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject inconsistent borrowed snapshots before deriving an incoming business classification.
  auto projection_validation = validate_projection_snapshot(inputs);
  if (!projection_validation) {
    return model::Result<PrivateOmsTransitionPlan>::create_failure(
        std::move(projection_validation).error());
  }
  auto pending_validation = validate_pending_snapshot(inputs);
  if (!pending_validation) {
    return model::Result<PrivateOmsTransitionPlan>::create_failure(
        std::move(pending_validation).error());
  }
  auto cancel_validation = validate_cancel_snapshot(inputs);
  if (!cancel_validation) {
    return model::Result<PrivateOmsTransitionPlan>::create_failure(
        std::move(cancel_validation).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare every bounded replacement against policy before returning any usable complete proposal.
  auto plan = derive_validated_transition(inputs);
  const auto& capacities = inputs.policy.capacities();
  if ((plan.gap_insertion &&
       inputs.pending.size() >= capacities.max_pending_fill_intervals_per_order) ||
      (plan.cancel_history_change && plan.cancel_history_change->index == inputs.cancels.size() &&
       inputs.global_cancel_attempt_count >= capacities.max_cancel_attempts) ||
      plan.transition_effect_count > capacities.max_transition_effects_per_turn ||
      plan.order_callback_count > capacities.max_order_callbacks_per_turn) {
    return model::Result<PrivateOmsTransitionPlan>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEventCapacityExceeded,
                                            "private_oms.transition_capacity"));
  }
  return model::Result<PrivateOmsTransitionPlan>::create_success(std::move(plan));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::oms
