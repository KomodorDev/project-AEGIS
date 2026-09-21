// Purpose: join a genuine initial private-order lifecycle proposal with coherent bounded economics
// and prospective audit requirements without consuming any private event or publishing evidence.

#include "private_business_proposal.hpp"

#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <array>
#include <utility>
#include <variant>

namespace aegis::runtime {

// --------------------------------------------------------
// Derive all initial business consequences from one unchanged owner. This deliberately rejects a
// component-only economic advance: applying such a component plan cannot advance the live OMS or
// create a valid new initial business baseline. Later multi-event consumption needs the joint
// journal/audit reducer, not repeated calls that advance a detached shadow order.
model::Result<InitialKnownPrivateBusinessProposal>
PrivateOrderReconciler::derive_initial_known_authoritative_business_proposal(
    const oms::NormalizedPrivateOrderInput& input,
    recovery::AuditOrdinal prospective_first_audit_ordinal) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve first-seen correlation precedence before consulting genuine business components.
  using ProposalResult = model::Result<InitialKnownPrivateBusinessProposal>;
  auto identity = derive_first_seen_authoritative_identity_plan(
      oms::PrivateEventIngressSemanticValue::from_normalized_input(input));
  if (!identity) {
    return ProposalResult::create_failure(std::move(identity).error());
  }
  const auto* known =
      std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&identity.value().correlation_plan());
  if (known == nullptr || known->resolution.known_resolution() == nullptr) {
    return ProposalResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateCorrelationFailed,
                                            "private_business_proposal.known_correlation"));
  }
  const auto* order =
      owner_->outbound_oms().find_order(known->resolution.known_resolution()->order_id);
  if (order == nullptr || !owner_->outbound_oms().has_retained_order_record(*order)) {
    return ProposalResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::PrivateCorrelationFailed, "private_business_proposal.owner"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require a coherent genuine initial M3 baseline before even a projection-only proposal. No
  // inventory source, reservation conversion, retained gap, or cancel history may be hidden here.
  const auto before = order->private_projection();
  const auto* route = owner_->routes().find_route(order->provenance().route_id);
  const auto* inventory = risk::InventoryLedger::installed_inventory(owner_->reservations());
  const auto* reservation =
      owner_->reservations().find_reservation(order->admission().reservation_id);
  if (route == nullptr || inventory == nullptr || reservation == nullptr ||
      inventory->root_provenance() != m4_policy_.root_provenance() ||
      (before.state != oms::OutboundOrderState::WriteInitiated &&
       before.state != oms::OutboundOrderState::SubmissionUnknown) ||
      before.pending_fill_count != 0U || before.cancel_attempt_count != 0U ||
      before.cumulative_filled_quantity.coefficient() != 0 ||
      reservation->state != risk::ReservationState::Held ||
      reservation->closure_cause != risk::ReservationClosureCause::Unassigned ||
      reservation->side != order->admission().economics.side ||
      reservation->exposure != order->admission().exposure ||
      reservation->remaining_exposure != reservation->exposure ||
      reservation->cumulative_confirmed_exposure.quantity.coefficient() != 0 ||
      reservation->cumulative_confirmed_exposure.quote_notional.coefficient() != 0 ||
      inventory->find_source(order->order_id()) != nullptr) {
    return ProposalResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "private_business_proposal.initial_components"));
  }
  auto transition = oms::derive_private_oms_transition(oms::PrivateOmsTransitionInputs{
      order->admission(), before, *route, m4_policy_, runtime_epoch_id_, input, {}, {}, 0U});
  if (!transition) {
    return ProposalResult::create_failure(std::move(transition).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Count the complete prospective span before leasing economic scratch. Initial proposals have
  // exactly one primary effect; callback-bearing turns additionally need Planned and terminal rows.
  const auto& lifecycle = transition.value();
  if (lifecycle.transition_effect_count != 1U || lifecycle.order_callback_count > 1U ||
      lifecycle.drained_pending_prefix_count != 0U) {
    return ProposalResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                            "private_business_proposal.initial_effects"));
  }
  const PrivateBusinessEvidenceRequirements requirements{
      lifecycle.transition_effect_count, lifecycle.order_callback_count,
      lifecycle.transition_effect_count + (lifecycle.order_callback_count == 0U ? 0U : 2U)};
  auto span = recovery::AuditSpan::create_audit_span(prospective_first_audit_ordinal,
                                                     requirements.audit_record_count);
  if (!span) {
    return ProposalResult::create_failure(std::move(span).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive one complete economic candidate while every live component still matches the original
  // row. Empty initial side tables cannot produce a drained execution or deferred cancellation.
  std::optional<risk::ReservationInventoryPlan> economics;
  switch (lifecycle.economics_action) {
  case oms::ProposedPrivateEconomicsAction::None:
    break;
  case oms::ProposedPrivateEconomicsAction::ApplyExecutions: {
    const std::array inputs{
        risk::ReservationInventoryExecutionInput{input, prospective_first_audit_ordinal}};
    auto planned = inventory->plan_cumulative_fill_batch(*order, inputs);
    if (!planned) {
      return ProposalResult::create_failure(std::move(planned).error());
    }
    economics.emplace(std::move(planned).value());
    break;
  }
  case oms::ProposedPrivateEconomicsAction::ReleaseResidual: {
    if (!lifecycle.reservation_closure_cause) {
      return ProposalResult::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateOmsState, "private_business_proposal.closure"));
    }
    auto planned = inventory->plan_terminal_release(*order, *lifecycle.reservation_closure_cause);
    if (!planned) {
      return ProposalResult::create_failure(std::move(planned).error());
    }
    economics.emplace(std::move(planned).value());
    break;
  }
  case oms::ProposedPrivateEconomicsAction::ApplyExecutionsThenReleaseResidual:
    return ProposalResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateOmsState, "private_business_proposal.initial_drain"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Check the shared cumulative and closure postconditions before sealing read-only evidence.
  if (economics && (economics->reservation_after().cumulative_confirmed_exposure.quantity !=
                        lifecycle.after.cumulative_filled_quantity ||
                    economics->reservation_after().closure_cause !=
                        lifecycle.reservation_closure_cause.value_or(
                            risk::ReservationClosureCause::Unassigned))) {
    return ProposalResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "private_business_proposal.joint_postconditions"));
  }
  return ProposalResult::create_success(InitialKnownPrivateBusinessProposal{
      input, known->resolution, std::move(transition).value(), *reservation, std::move(economics),
      requirements, std::move(span).value()});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
