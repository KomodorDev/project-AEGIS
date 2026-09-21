// Purpose: bind detached known-order lifecycle proposals to the runtime's genuine retained OMS
// admission and sealed policy without applying live projections, identities, or economics.

#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <utility>
#include <variant>

namespace aegis::runtime {

// --------------------------------------------------------
// Reuse the closed first-seen correlation boundary before reading a genuine retained row. The
// current canonical registries and side tables remain empty, so all proposals describe one input
// against the live M3 baseline and never advance a shadow lifecycle between calls.
model::Result<oms::PrivateOmsTransitionPlan>
PrivateOrderReconciler::derive_initial_known_authoritative_oms_transition(
    const oms::NormalizedPrivateOrderInput& input) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the existing pristine-state, source-shape, provenance, and correlation precedence.
  const auto semantic = oms::PrivateEventIngressSemanticValue::from_normalized_input(input);
  auto identity = derive_first_seen_authoritative_identity_plan(semantic);
  if (!identity) {
    return model::Result<oms::PrivateOmsTransitionPlan>::create_failure(
        std::move(identity).error());
  }
  const auto* known =
      std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&identity.value().correlation_plan());
  if (known == nullptr || known->resolution.known_resolution() == nullptr) {
    return model::Result<oms::PrivateOmsTransitionPlan>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateCorrelationFailed,
                                            "private_order_reconciler.known_transition"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve only through the exact owning coordinator; no detached row is accepted as authority.
  const auto* order =
      owner_->outbound_oms().find_order(known->resolution.known_resolution()->order_id);
  if (order == nullptr || !owner_->outbound_oms().has_retained_order_record(*order)) {
    return model::Result<oms::PrivateOmsTransitionPlan>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateCorrelationFailed,
                                            "private_order_reconciler.transition_owner"));
  }
  const auto* route = owner_->routes().find_route(order->provenance().route_id);
  const auto projection = order->private_projection();
  if (route == nullptr || projection.pending_fill_count != 0U ||
      projection.cancel_attempt_count != 0U) {
    return model::Result<oms::PrivateOmsTransitionPlan>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                            "private_order_reconciler.transition_side_tables"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Return a value-only proposal. A future joint reducer must independently preflight business
  // journal, economics, safety, and audit/callback storage before it can publish any such effect.
  return oms::derive_private_oms_transition(oms::PrivateOmsTransitionInputs{
      order->admission(), projection, *route, m4_policy_, runtime_epoch_id_, input, {}, {}, 0U});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
