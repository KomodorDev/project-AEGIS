// Purpose: author sealed source events and detached OMS planning inputs for independent lifecycle
// tests; hypothetical projections never modify the genuine retained M3 order.

#pragma once

#include "aegis/execution/submission_route.hpp"
#include "aegis/oms/private_oms_transition.hpp"
#include "m4_private_event_fixture.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace aegis::test_support {

// --------------------------------------------------------
// Stop at the first malformed fixture value, retaining the production field in the setup error.
template <typename Value>
[[nodiscard]] Value extract_private_oms_value_or_throw(model::Result<Value> value) {
  if (!value) {
    throw std::logic_error{"invalid private OMS fixture: " + value.error().context.field};
  }
  return std::move(value).value();
}

// --------------------------------------------------------

// ########################################################################
// Own genuine sealed metadata and an immutable admitted order while authoring hypothetical
// snapshots separately. Member order keeps every borrowed route and row alive until destruction.
class PrivateOmsTransitionFixture final {
public:

  // --------------------------------------------------------
  // Admit the chosen small integer order; setup failure throws without exposing a partial fixture.
  explicit PrivateOmsTransitionFixture(std::int64_t quantity = 6,
                                       execution::OrderSide side = execution::OrderSide::Buy)
      : routes{create_route_catalog_or_throw(source.test_authority().configuration)},
        orders{extract_private_oms_value_or_throw(oms::OutboundOms::create_outbound_oms(4U))} {
    if (quantity <= 0 || quantity > 1000) {
      throw std::logic_error{"private OMS fixture quantity must be one through 1000"};
    }
    auto admission = source.outbound_order_record().admission();
    admission.economics.side = side;
    admission.economics.price = create_m4_decimal_or_throw<model::Price>(100);
    admission.economics.quantity = create_m4_decimal_or_throw<model::Quantity>(quantity);
    admission.exposure = risk::OrderExposure{
        admission.economics.quantity, create_m4_decimal_or_throw<model::Notional>(quantity * 10)};
    auto admitted = extract_private_oms_value_or_throw(orders.admit_outbound_order(admission));
    if (!admitted.is_admitted() || admitted.record() == nullptr) {
      throw std::logic_error{"private OMS fixture order was not admitted"};
    }
    record_ = admitted.record();
    route_ = routes.find_route(record_->provenance().route_id);
    if (route_ == nullptr) {
      throw std::logic_error{"missing private OMS fixture route"};
    }
  }

  // --------------------------------------------------------
  // Preserve stable borrowed row and route addresses for every detached test input.
  PrivateOmsTransitionFixture(const PrivateOmsTransitionFixture&) = delete;
  PrivateOmsTransitionFixture& operator=(const PrivateOmsTransitionFixture&) = delete;
  PrivateOmsTransitionFixture(PrivateOmsTransitionFixture&&) = delete;
  PrivateOmsTransitionFixture& operator=(PrivateOmsTransitionFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the genuine retained row; its live projection remains PendingEncoding in these tests.
  [[nodiscard]] const oms::OutboundOrderRecord& order() const noexcept { return *record_; }

  // --------------------------------------------------------
  // Borrow the matching canonical installed route already validated by fixture construction.
  [[nodiscard]] const execution::InstalledSubmissionRoute& route() const noexcept {
    return *route_;
  }

  // --------------------------------------------------------
  // Borrow the exact policy fingerprint used by normalized fixture inputs.
  [[nodiscard]] const runtime::M4Policy& policy() const noexcept {
    return source.test_authority().m4_policy;
  }

  // --------------------------------------------------------
  // Return the current hypothetical runtime epoch used by locally authored cancel identities.
  [[nodiscard]] recovery::RuntimeEpochId create_runtime_epoch_or_throw() const {
    return create_m4_runtime_epoch_or_throw();
  }

  // --------------------------------------------------------
  // Borrow the hypothetical values only for this synchronous pure calculation; omitted global
  // history count means exactly this order's history, while explicit values test shared headroom.
  [[nodiscard]] model::Result<oms::PrivateOmsTransitionPlan>
  derive_transition(const oms::PrivateOrderProjection& before,
                    const oms::NormalizedPrivateOrderInput& input,
                    std::span<const oms::PendingPrivateExecution> pending = {},
                    std::span<const oms::RetainedCancelAttempt> cancels = {},
                    std::optional<std::uint32_t> global_cancel_count = std::nullopt) const {
    const auto epoch = create_runtime_epoch_or_throw();
    return oms::derive_private_oms_transition(oms::PrivateOmsTransitionInputs{
        order().admission(), before, route(), policy(), epoch, input, pending, cancels,
        global_cancel_count.value_or(static_cast<std::uint32_t>(cancels.size()))});
  }

  // --------------------------------------------------------
  // Return one deterministic exchange key; equality is opaque rather than numeric correlation.
  [[nodiscard]] oms::ExchangeOrderId create_exchange_order_id_or_throw() const {
    return create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  }

  // --------------------------------------------------------
  // Author a coherent detached primary state, defaulting partial fill to one and full fill to the
  // original quantity. Tests explicitly author pending counts and cancellation history afterwards.
  [[nodiscard]] oms::PrivateOrderProjection create_detached_projection_or_throw(
      oms::OutboundOrderState state = oms::OutboundOrderState::WriteInitiated,
      std::optional<std::int64_t> cumulative = std::nullopt) const {
    auto result = order().private_projection();
    result.state = state;
    const auto filled = cumulative.value_or(
        state == oms::OutboundOrderState::Filled ? order().economics().quantity.coefficient()
        : state == oms::OutboundOrderState::PartiallyFilled ? 1
                                                            : 0);
    result.cumulative_filled_quantity = create_m4_decimal_or_throw<model::Quantity>(filled);
    result.execution_evidence_observed = filled > 0;
    result.reconciliation_required = state == oms::OutboundOrderState::SubmissionUnknown;
    if (state == oms::OutboundOrderState::Working ||
        state == oms::OutboundOrderState::PartiallyFilled ||
        state == oms::OutboundOrderState::Filled || state == oms::OutboundOrderState::Cancelled) {
      result.exchange_acknowledged = true;
      result.exchange_order_id = create_exchange_order_id_or_throw();
    }
    if (state == oms::OutboundOrderState::Cancelled) {
      result.authoritative_terminal_cumulative_quantity = result.cumulative_filled_quantity;
      result.cancellation_state = oms::CancellationState::Confirmed;
    }
    return result;
  }

  // --------------------------------------------------------
  // Build an exact cancel identity, with optional foreign order or epoch for negative tests.
  [[nodiscard]] oms::CancelAttemptId create_cancel_attempt_id_or_throw(
      std::uint64_t ordinal = 1U, std::uint64_t runtime_counter = 1U,
      std::optional<model::OrderId> alternate_order = std::nullopt) const {
    return extract_private_oms_value_or_throw(
        oms::CancelAttemptId::cancel_attempt_id_from_components(
            create_m4_runtime_epoch_or_throw(runtime_counter),
            alternate_order.value_or(order().order_id()), ordinal));
  }

  // --------------------------------------------------------
  // Preserve local and exchange locator bytes without granting either ownership authority.
  [[nodiscard]] oms::PrivateOrderLocator create_local_exchange_locator_or_throw() const {
    return extract_private_oms_value_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(
            order().order_id(), create_exchange_order_id_or_throw()));
  }

  // --------------------------------------------------------
  // Normalize a consistent venue or reconciliation acknowledgement through the real factory.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_acknowledgement_or_throw(std::uint8_t event = 1U, bool reconciliation = false) const {
    const auto& factory = source.private_event_factory();
    return extract_private_oms_value_or_throw(
        reconciliation
            ? factory.normalize_reconciliation_acknowledgement(
                  source.create_reconciliation_private_event_origin_or_throw(event),
                  source.account_id(), source.venue_id(), create_exchange_order_id_or_throw(),
                  order().order_id(), source.instrument_id())
            : factory.normalize_venue_acknowledgement(
                  source.create_venue_private_event_origin_or_throw(event),
                  create_exchange_order_id_or_throw(), order().order_id()));
  }

  // --------------------------------------------------------
  // Normalize an authoritative rejection with independently authored category and empty detail.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_rejection_or_throw(std::uint8_t event = 1U, bool reconciliation = false) const {
    const auto& factory = source.private_event_factory();
    return extract_private_oms_value_or_throw(
        reconciliation
            ? factory.normalize_reconciliation_rejection(
                  source.create_reconciliation_private_event_origin_or_throw(event),
                  source.account_id(), source.venue_id(), create_local_exchange_locator_or_throw(),
                  oms::ExchangeRejectionCategory::InvalidOrder, {})
            : factory.normalize_venue_rejection(
                  source.create_venue_private_event_origin_or_throw(event),
                  create_local_exchange_locator_or_throw(),
                  oms::ExchangeRejectionCategory::InvalidOrder, {}));
  }

  // --------------------------------------------------------
  // Author an integer fill independently from production interval calculations. Reconciliation
  // carries mandatory source side; ordinary venue input may intentionally omit that evidence.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_execution_or_throw(std::int64_t increment, std::int64_t cumulative,
                               std::uint8_t event = 1U, bool reconciliation = false,
                               std::optional<execution::OrderSide> source_side = std::nullopt,
                               std::int64_t price = 100) const {
    const auto& factory = source.private_event_factory();
    const auto trade = create_m4_opaque_identity_or_throw<oms::TradeId>(event);
    const auto quantity = create_m4_decimal_or_throw<model::Quantity>(increment);
    const auto cumulative_quantity = create_m4_decimal_or_throw<model::Quantity>(cumulative);
    const auto execution_price = create_m4_decimal_or_throw<model::Price>(price);
    return extract_private_oms_value_or_throw(
        reconciliation
            ? factory.normalize_reconciliation_execution(
                  source.create_reconciliation_private_event_origin_or_throw(event),
                  source.account_id(), source.venue_id(), create_local_exchange_locator_or_throw(),
                  trade, source.instrument_id(), route().metadata().revision(), quantity,
                  cumulative_quantity, execution_price,
                  source_side.value_or(order().economics().side))
            : factory.normalize_venue_execution(
                  source.create_venue_private_event_origin_or_throw(event),
                  create_local_exchange_locator_or_throw(), trade, source.instrument_id(),
                  route().metadata().revision(), quantity, cumulative_quantity, execution_price,
                  source_side));
  }

  // --------------------------------------------------------
  // Normalize a cancellation target without manufacturing a causal cancel-attempt identity.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_cancelled_or_throw(std::int64_t cumulative, std::uint8_t event = 1U,
                               bool reconciliation = false) const {
    const auto& factory = source.private_event_factory();
    const auto target = create_m4_decimal_or_throw<model::Quantity>(cumulative);
    return extract_private_oms_value_or_throw(
        reconciliation
            ? factory.normalize_reconciliation_cancellation_result(
                  source.create_reconciliation_private_event_origin_or_throw(event),
                  source.account_id(), source.venue_id(), create_local_exchange_locator_or_throw(),
                  oms::CancellationResult::Cancelled, target)
            : factory.normalize_venue_cancellation_result(
                  source.create_venue_private_event_origin_or_throw(event),
                  create_local_exchange_locator_or_throw(), oms::CancellationResult::Cancelled,
                  target));
  }

  // --------------------------------------------------------
  // Preserve optional exact causal evidence, never inferring an attempt from order timing.
  [[nodiscard]] oms::NormalizedPrivateOrderInput normalize_cancel_rejection_or_throw(
      std::uint8_t event = 1U, std::optional<oms::CancelAttemptId> causal = std::nullopt) const {
    const auto& factory = source.private_event_factory();
    const auto origin = source.create_venue_private_event_origin_or_throw(event);
    return extract_private_oms_value_or_throw(
        causal ? factory.normalize_venue_cancel_rejection_with_causal_id(
                     origin, create_local_exchange_locator_or_throw(), *causal)
               : factory.normalize_venue_cancellation_result(
                     origin, create_local_exchange_locator_or_throw(),
                     oms::CancellationResult::CancelRejected, std::nullopt));
  }

  // --------------------------------------------------------
  // Normalize a local cancel request using the genuine retained row solely as attribution proof.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_cancel_request_or_throw(oms::CancelAttemptId attempt, std::uint64_t event = 1U) const {
    return extract_private_oms_value_or_throw(
        source.private_event_factory().normalize_order_cancel_request(
            source.create_local_private_event_origin_or_throw(event), orders, order(), attempt));
  }

  // --------------------------------------------------------
  // Normalize one first local write observation without modifying any hypothetical attempt state.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_cancel_write_outcome_or_throw(oms::CancelAttemptId attempt,
                                          oms::CancelWriteOutcome outcome,
                                          std::uint64_t event = 1U) const {
    return extract_private_oms_value_or_throw(
        source.private_event_factory().normalize_order_cancel_write_outcome(
            source.create_local_private_event_origin_or_throw(event), orders, order(), attempt,
            outcome));
  }

  // --------------------------------------------------------
  // Normalize a local certainty observation against exact retained submission attribution.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_local_failure_or_throw(oms::LocalFailureCertainty certainty,
                                   std::uint64_t event = 1U) const {
    return extract_private_oms_value_or_throw(
        source.private_event_factory().normalize_order_local_failure(
            source.create_local_private_event_origin_or_throw(event), orders, order(), certainty));
  }

  // --------------------------------------------------------
  // Normalize an order-scoped timeout through the closed genuine-row factory boundary.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_order_timeout_or_throw(std::uint64_t event = 1U) const {
    return extract_private_oms_value_or_throw(
        source.private_event_factory().normalize_order_timeout(
            source.create_local_private_event_origin_or_throw(event), orders, order()));
  }

  // --------------------------------------------------------
  // Source/configuration, catalog, and OMS are separate owned values; no fixture publishes a
  // transition plan into these genuine rows or into private admission.
  M4PrivateEventFixture source;
  execution::OwnerLocalRouteCatalog routes;
  oms::OutboundOms orders;

private:

  // --------------------------------------------------------
  // Install every sealed configuration route using the same production catalog validation.
  [[nodiscard]] static execution::OwnerLocalRouteCatalog
  create_route_catalog_or_throw(const configuration::StartupConfiguration& configuration) {
    std::vector<execution::SubmissionRouteInput> inputs;
    for (const auto& route : configuration.routes().routes()) {
      const auto* attribution = configuration.organization().find_bot(route.bot_id);
      const auto* metadata =
          configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
      if (attribution == nullptr || metadata == nullptr) {
        throw std::logic_error{"missing private OMS route attribution"};
      }
      inputs.push_back(execution::SubmissionRouteInput{route, *attribution, *metadata});
    }
    return extract_private_oms_value_or_throw(
        execution::OwnerLocalRouteCatalog::create_owner_local_route_catalog(
            configuration.fingerprint(), configuration.revision(),
            configuration.organization().revision(), configuration.routes().revision(),
            std::move(inputs)));
  }

  // --------------------------------------------------------
  const oms::OutboundOrderRecord* record_{nullptr};
  const execution::InstalledSubmissionRoute* route_{nullptr};
};

// ########################################################################

} // namespace aegis::test_support
