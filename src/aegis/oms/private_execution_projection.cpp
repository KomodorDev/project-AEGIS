// Purpose: check and expand one genuine OMS fill plan into exact per-execution audit projections
// while preserving the live projection and bounded pending interval list.

#include "private_execution_projection.hpp"

#include <variant>

namespace aegis::oms {
namespace {

// --------------------------------------------------------
// Report which exact part of the execution projection chain disagrees with the accepted plan.
[[nodiscard]] model::Result<PrivateExecutionProjection>
reject_private_execution_projection_at_field(const char* field) {
  return model::Result<PrivateExecutionProjection>::create_failure(
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState, field));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Verify the frame's position and contiguous interval before deriving its immutable projection.
model::Result<PrivateExecutionProjection> derive_private_execution_projection(
    const PrivateOmsTransitionPlan& transition, const NormalizedPrivateOrderInput& execution,
    const PrivateOrderProjection& preceding_projection, std::uint32_t effect_index) {

  // ++++++++++++++++++++++++++++++++++++++++
  // A valid execution-only proposal names the incoming primary plus its exact drained prefix.
  const auto* fill = std::get_if<ExecutionPayload>(&execution.payload());
  const auto count = transition.transition_effect_count;
  const auto drained_count = transition.drained_pending_prefix_count;
  const auto final_state = transition.after.state;
  const bool closes_by_fill = final_state == OutboundOrderState::Filled;
  if (fill == nullptr || execution.origin() == PrivateEventOrigin::Local ||
      transition.classification != ProposedPrivateOmsClassification::Applied ||
      transition.economics_action != ProposedPrivateEconomicsAction::ApplyExecutions ||
      transition.safety_reason || transition.rejection_code || transition.gap_insertion ||
      transition.cancel_history_change || effect_index >= count || count - 1U != drained_count ||
      transition.order_callback_count != count ||
      drained_count > transition.before.pending_fill_count ||
      (final_state != OutboundOrderState::PartiallyFilled && !closes_by_fill) ||
      (closes_by_fill
           ? transition.reservation_closure_cause != risk::ReservationClosureCause::FullFill
           : transition.reservation_closure_cause.has_value())) {
    return reject_private_execution_projection_at_field("private_oms.execution_projection_plan");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Later frames must start from the preceding partial fill, with one pending row removed per
  // previously drained execution. The exact interval start ties this frame to that cumulative.
  auto start = fill->cumulative_quantity.checked_subtract(fill->incremental_quantity);
  if (!start || fill->incremental_quantity.coefficient() <= 0 ||
      start.value() != preceding_projection.cumulative_filled_quantity) {
    return reject_private_execution_projection_at_field(
        "private_oms.execution_projection_interval");
  }
  auto expected_before = transition.before;
  if (effect_index != 0U) {
    if (start.value() <= transition.before.cumulative_filled_quantity) {
      return reject_private_execution_projection_at_field(
          "private_oms.execution_projection_prefix");
    }
    expected_before.state = OutboundOrderState::PartiallyFilled;
    expected_before.cumulative_filled_quantity = start.value();
    expected_before.execution_evidence_observed = true;
    expected_before.exchange_order_id = transition.after.exchange_order_id;
    expected_before.exchange_mapping_established_by_execution =
        transition.after.exchange_mapping_established_by_execution;
    expected_before.pending_fill_count -= effect_index - 1U;
  }
  if (preceding_projection != expected_before) {
    return reject_private_execution_projection_at_field("private_oms.execution_projection_prefix");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only the incoming execution can establish the plan's mapping. Drained facts retain the
  // established binding and each remove one pending interval from the proposed projection.
  auto after = expected_before;
  const auto& exchange = fill->locator.exchange_order_id();
  if (exchange) {
    if ((after.exchange_order_id && after.exchange_order_id != exchange) ||
        (!after.exchange_order_id && effect_index != 0U)) {
      return reject_private_execution_projection_at_field(
          "private_oms.execution_projection_mapping");
    }
    if (!after.exchange_order_id) {
      after.exchange_order_id = exchange;
      after.exchange_mapping_established_by_execution = true;
    }
  }
  if (after.exchange_order_id != transition.after.exchange_order_id ||
      after.exchange_mapping_established_by_execution !=
          transition.after.exchange_mapping_established_by_execution) {
    return reject_private_execution_projection_at_field("private_oms.execution_projection_mapping");
  }
  after.execution_evidence_observed = true;
  after.cumulative_filled_quantity = fill->cumulative_quantity;
  if (effect_index != 0U) {
    --after.pending_fill_count;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A partial prefix preserves the uncertainty flag. Only a full final fill clears it, and the
  // complete terminal frame must exactly reproduce the detached plan's final projection.
  const bool is_final = effect_index == count - 1U;
  after.state = is_final ? final_state : OutboundOrderState::PartiallyFilled;
  if (is_final && closes_by_fill) {
    after.reconciliation_required = false;
  }
  if ((is_final && after != transition.after) ||
      (!is_final &&
       after.cumulative_filled_quantity >= transition.after.cumulative_filled_quantity)) {
    return reject_private_execution_projection_at_field("private_oms.execution_projection_result");
  }
  return model::Result<PrivateExecutionProjection>::create_success(
      PrivateExecutionProjection{expected_before, after});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::oms
