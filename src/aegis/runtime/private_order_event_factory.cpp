// Purpose: validate receive-time-free private-order attempts, attach trusted source provenance,
// privately add receipt observations, and mediate read-only retained-order provenance checks.

#include "private_order_event_factory.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Return the stable attempt-shape failure without exposing an open caller detail channel.
[[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
private_order_ingress_attempt_failure_from_field(std::string_view field) {
  return model::Result<oms::PrivateOrderIngressAttempt>::failure(model::DomainError::at_field(
      model::DomainErrorCode::InvalidPrivateEvent, std::string{field}));
}

// --------------------------------------------------------
// Return the equivalent normalized-input failure for reconciliation compatibility paths.
[[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
normalized_private_order_input_failure_from_field(std::string_view field) {
  return model::Result<oms::NormalizedPrivateOrderInput>::failure(model::DomainError::at_field(
      model::DomainErrorCode::InvalidPrivateEvent, std::string{field}));
}

// --------------------------------------------------------
// Accept only the two assigned order sides at raw enum boundaries.
[[nodiscard]] bool is_order_side_assigned(execution::OrderSide side) noexcept {
  return side == execution::OrderSide::Buy || side == execution::OrderSide::Sell;
}

// --------------------------------------------------------
// Accept only the six assigned venue-neutral rejection categories.
[[nodiscard]] bool
is_exchange_rejection_category_assigned(oms::ExchangeRejectionCategory category) noexcept {
  switch (category) {
  case oms::ExchangeRejectionCategory::Unspecified:
  case oms::ExchangeRejectionCategory::InvalidOrder:
  case oms::ExchangeRejectionCategory::InsufficientAuthority:
  case oms::ExchangeRejectionCategory::InsufficientFunds:
  case oms::ExchangeRejectionCategory::PostOnlyWouldCross:
  case oms::ExchangeRejectionCategory::VenueRiskRejected:
    return true;
  }
  return false;
}

// --------------------------------------------------------
// Accept only definitive cancellation or cancel-rejection authority.
[[nodiscard]] bool is_cancellation_result_assigned(oms::CancellationResult result) noexcept {
  return result == oms::CancellationResult::Cancelled ||
         result == oms::CancellationResult::CancelRejected;
}

// --------------------------------------------------------
// Validate the common positive execution interval and exact fixed-point subtraction boundary.
[[nodiscard]] bool is_execution_interval_valid(model::Quantity incremental_quantity,
                                               model::Quantity cumulative_quantity,
                                               model::Price execution_price) {
  if (incremental_quantity.coefficient() <= 0 || cumulative_quantity.coefficient() <= 0 ||
      execution_price.coefficient() <= 0) {
    return false;
  }
  const auto preceding = cumulative_quantity.checked_subtract(incremental_quantity);
  return preceding && preceding.value().coefficient() >= 0;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Return whether sealed configuration proves the exact logical-account and venue binding.
bool PrivateOrderEventFactory::has_configured_account_venue_binding(
    const model::LogicalAccountId& logical_account_id,
    const model::VenueId& venue_id) const noexcept {
  return resolver_.has_configured_account_venue_binding(logical_account_id, venue_id);
}

// --------------------------------------------------------
// Derive maximal correlation-independent source provenance from sealed configuration only.
model::M4Provenance PrivateOrderEventFactory::derive_authoritative_source_provenance(
    const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id,
    const std::optional<model::InstrumentId>& instrument_id) const noexcept {
  return resolver_.derive_authoritative_source_provenance(logical_account_id, venue_id,
                                                          instrument_id);
}

// --------------------------------------------------------
// Validate one genuine immutable OMS row and derive its complete known-order provenance.
model::Result<model::M4Provenance> PrivateOrderEventFactory::derive_retained_order_provenance(
    const oms::OutboundOrderRecord& retained_order) const {
  return resolver_.derive_retained_order_provenance(retained_order);
}

// --------------------------------------------------------
// Attach maximal independently proved source provenance to one receive-time-free attempt.
oms::PrivateOrderIngressAttempt
PrivateOrderEventFactory::create_authoritative_private_order_ingress_attempt(
    oms::PrivateIngressOriginValue origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, const std::optional<model::InstrumentId>& instrument_id,
    oms::PrivateOrderEventPayload payload) const noexcept {
  auto provenance =
      resolver_.derive_authoritative_source_provenance(logical_account_id, venue_id, instrument_id);
  return oms::PrivateOrderIngressAttempt{oms::PrivateEventIngressSemanticValue{
      std::move(origin), oms::PrivateEventSubjectScope::Order, std::move(logical_account_id),
      std::move(venue_id), std::move(provenance), std::move(payload)}};
}

// --------------------------------------------------------
// Attach one supplied observation time while retaining every receive-time-free field exactly.
oms::NormalizedPrivateOrderInput PrivateOrderEventFactory::normalize_private_order_ingress_attempt(
    const oms::PrivateOrderIngressAttempt& attempt,
    model::ReceiveTimestamp received_at) const noexcept {
  const auto& semantic = attempt.semantic_value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Convert exactly the active source domain and attach only the supplied observation.
  auto normalized_origin = std::visit(
      [received_at](const auto& source_origin) -> oms::PrivateEventOriginValue {
        using Origin = std::decay_t<decltype(source_origin)>;
        if constexpr (std::is_same_v<Origin, oms::LocalPrivateIngressOrigin>) {
          return oms::LocalPrivateEventOrigin{source_origin.event_id, source_origin.source_time,
                                              received_at};
        } else if constexpr (std::is_same_v<Origin, oms::VenuePrivateIngressOrigin>) {
          return oms::VenuePrivateEventOrigin{source_origin.event_key, source_origin.source_time,
                                              received_at};
        } else {
          static_assert(std::is_same_v<Origin, oms::ReconciliationPrivateIngressOrigin>);
          return oms::ReconciliationPrivateEventOrigin{
              source_origin.reconciliation_epoch_id, source_origin.authoritative_cut_id,
              source_origin.row_ordinal, source_origin.cut_time, received_at};
        }
      },
      semantic.origin());

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the retained source value so normalization cannot mutate the attempt.
  return oms::NormalizedPrivateOrderInput{std::move(normalized_origin),  semantic.subject_scope(),
                                          semantic.logical_account_id(), semantic.venue_id(),
                                          semantic.provenance(),         semantic.payload()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate one ordinary venue acknowledgement without inferring local ownership.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_venue_acknowledgement_attempt(
    oms::VenuePrivateIngressOrigin origin, oms::ExchangeOrderId exchange_order_id,
    std::optional<model::OrderId> local_order_locator) const {
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      create_authoritative_private_order_ingress_attempt(
          std::move(origin), account, venue, std::nullopt,
          oms::ExchangeAcknowledgedPayload{std::move(exchange_order_id),
                                           std::move(local_order_locator)}));
}

// --------------------------------------------------------
// Normalize one venue acknowledgement by attaching the caller-supplied receipt observation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_venue_acknowledgement(
    oms::VenuePrivateEventOrigin origin, oms::ExchangeOrderId exchange_order_id,
    std::optional<model::OrderId> local_order_locator) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_venue_acknowledgement_attempt(
      oms::VenuePrivateIngressOrigin{std::move(origin.event_key), origin.source_time},
      std::move(exchange_order_id), std::move(local_order_locator));
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Normalize one reconciliation acknowledgement without exposing a producer-facing attempt API.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_reconciliation_acknowledgement(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::ExchangeOrderId exchange_order_id,
    std::optional<model::OrderId> local_order_locator,
    std::optional<model::InstrumentId> source_instrument_id) const {
  const auto received_at = origin.receive_time;
  const auto attempt = create_authoritative_private_order_ingress_attempt(
      oms::ReconciliationPrivateIngressOrigin{std::move(origin.reconciliation_epoch_id),
                                              std::move(origin.authoritative_cut_id),
                                              origin.row_ordinal, origin.cut_time},
      std::move(logical_account_id), std::move(venue_id), source_instrument_id,
      oms::ExchangeAcknowledgedPayload{std::move(exchange_order_id),
                                       std::move(local_order_locator)});
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt, received_at));
}

// --------------------------------------------------------
// Validate a venue rejection after assigned-category and bounded-detail checks.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_venue_rejection_attempt(oms::VenuePrivateIngressOrigin origin,
                                                         oms::PrivateOrderLocator locator,
                                                         oms::ExchangeRejectionCategory category,
                                                         std::span<const std::byte> detail) const {
  if (!is_exchange_rejection_category_assigned(category)) {
    return private_order_ingress_attempt_failure_from_field("private_event.rejection_category");
  }
  auto retained_detail = oms::PrivateRejectionDetail::create(detail);
  if (!retained_detail) {
    return model::Result<oms::PrivateOrderIngressAttempt>::failure(retained_detail.error());
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      create_authoritative_private_order_ingress_attempt(
          std::move(origin), account, venue, std::nullopt,
          oms::ExchangeRejectedPayload{std::move(locator), category,
                                       std::move(retained_detail).value()}));
}

// --------------------------------------------------------
// Normalize one venue rejection by attaching only the supplied receipt observation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::normalize_venue_rejection(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
    oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_venue_rejection_attempt(
      oms::VenuePrivateIngressOrigin{std::move(origin.event_key), origin.source_time},
      std::move(locator), category, detail);
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Normalize a reconciliation rejection under the same bounded semantic validation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_reconciliation_rejection(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator,
    oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const {
  if (!is_exchange_rejection_category_assigned(category)) {
    return normalized_private_order_input_failure_from_field("private_event.rejection_category");
  }
  auto retained_detail = oms::PrivateRejectionDetail::create(detail);
  if (!retained_detail) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(retained_detail.error());
  }
  const auto received_at = origin.receive_time;
  const auto attempt = create_authoritative_private_order_ingress_attempt(
      oms::ReconciliationPrivateIngressOrigin{std::move(origin.reconciliation_epoch_id),
                                              std::move(origin.authoritative_cut_id),
                                              origin.row_ordinal, origin.cut_time},
      std::move(logical_account_id), std::move(venue_id), std::nullopt,
      oms::ExchangeRejectedPayload{std::move(locator), category,
                                   std::move(retained_detail).value()});
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt, received_at));
}

// --------------------------------------------------------
// Validate a venue execution attempt while retaining optional side before owner correlation.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_venue_execution_attempt(
    oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
    model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price, std::optional<execution::OrderSide> source_side) const {
  if (!is_execution_interval_valid(incremental_quantity, cumulative_quantity, execution_price)) {
    return private_order_ingress_attempt_failure_from_field("private_event.execution");
  }
  if (source_side.has_value() && !is_order_side_assigned(*source_side)) {
    return private_order_ingress_attempt_failure_from_field("private_event.source_side");
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  const auto source_instrument = instrument_id;
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      create_authoritative_private_order_ingress_attempt(
          std::move(origin), account, venue, source_instrument,
          oms::ExecutionPayload{std::move(locator), std::move(trade_id), std::move(instrument_id),
                                metadata_revision, incremental_quantity, cumulative_quantity,
                                execution_price, source_side}));
}

// --------------------------------------------------------
// Normalize one venue execution by attaching only the supplied receipt observation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::normalize_venue_execution(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
    model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price, std::optional<execution::OrderSide> source_side) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_venue_execution_attempt(
      oms::VenuePrivateIngressOrigin{std::move(origin.event_key), origin.source_time},
      std::move(locator), std::move(trade_id), std::move(instrument_id), metadata_revision,
      incremental_quantity, cumulative_quantity, execution_price, source_side);
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Normalize a reconciliation execution whose source row always supplies side evidence.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_reconciliation_execution(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
    model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price, execution::OrderSide source_side) const {
  if (!is_execution_interval_valid(incremental_quantity, cumulative_quantity, execution_price)) {
    return normalized_private_order_input_failure_from_field("private_event.execution");
  }
  if (!is_order_side_assigned(source_side)) {
    return normalized_private_order_input_failure_from_field("private_event.source_side");
  }
  const auto received_at = origin.receive_time;
  const auto source_instrument = instrument_id;
  const auto attempt = create_authoritative_private_order_ingress_attempt(
      oms::ReconciliationPrivateIngressOrigin{std::move(origin.reconciliation_epoch_id),
                                              std::move(origin.authoritative_cut_id),
                                              origin.row_ordinal, origin.cut_time},
      std::move(logical_account_id), std::move(venue_id), source_instrument,
      oms::ExecutionPayload{std::move(locator), std::move(trade_id), std::move(instrument_id),
                            metadata_revision, incremental_quantity, cumulative_quantity,
                            execution_price, source_side});
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt, received_at));
}

// --------------------------------------------------------
// Validate a venue cancellation result and its exact terminal-cumulative shape.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_venue_cancellation_result_attempt(
    oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
    oms::CancellationResult result,
    std::optional<model::Quantity> terminal_cumulative_quantity) const {
  if (!is_cancellation_result_assigned(result) ||
      (result == oms::CancellationResult::Cancelled) != terminal_cumulative_quantity.has_value() ||
      (terminal_cumulative_quantity.has_value() &&
       terminal_cumulative_quantity->coefficient() < 0)) {
    return private_order_ingress_attempt_failure_from_field("private_event.cancellation_result");
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      create_authoritative_private_order_ingress_attempt(
          std::move(origin), account, venue, std::nullopt,
          oms::CancellationResultPayload{std::move(locator), result, std::nullopt,
                                         terminal_cumulative_quantity}));
}

// --------------------------------------------------------
// Retain an exact venue request/response causal identity without treating it as order authority.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_venue_cancel_rejection_attempt_with_causal_id(
    oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
    oms::CancelAttemptId causal_cancel_attempt_id) const {
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      create_authoritative_private_order_ingress_attempt(
          std::move(origin), account, venue, std::nullopt,
          oms::CancellationResultPayload{std::move(locator),
                                         oms::CancellationResult::CancelRejected,
                                         std::move(causal_cancel_attempt_id), std::nullopt}));
}

// --------------------------------------------------------
// Normalize one venue cancellation result by attaching only its supplied receipt observation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_venue_cancellation_result(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
    oms::CancellationResult result,
    std::optional<model::Quantity> terminal_cumulative_quantity) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_venue_cancellation_result_attempt(
      oms::VenuePrivateIngressOrigin{std::move(origin.event_key), origin.source_time},
      std::move(locator), result, terminal_cumulative_quantity);
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Attach one supplied receive observation to a causally bound venue cancel rejection.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_venue_cancel_rejection_with_causal_id(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
    oms::CancelAttemptId causal_cancel_attempt_id) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_venue_cancel_rejection_attempt_with_causal_id(
      oms::VenuePrivateIngressOrigin{std::move(origin.event_key), origin.source_time},
      std::move(locator), std::move(causal_cancel_attempt_id));
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Normalize a reconciliation cancellation result under the same source-shape validation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_reconciliation_cancellation_result(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::CancellationResult result,
    std::optional<model::Quantity> terminal_cumulative_quantity) const {
  if (!is_cancellation_result_assigned(result) ||
      (result == oms::CancellationResult::Cancelled) != terminal_cumulative_quantity.has_value() ||
      (terminal_cumulative_quantity.has_value() &&
       terminal_cumulative_quantity->coefficient() < 0)) {
    return normalized_private_order_input_failure_from_field("private_event.cancellation_result");
  }
  const auto received_at = origin.receive_time;
  const auto attempt = create_authoritative_private_order_ingress_attempt(
      oms::ReconciliationPrivateIngressOrigin{std::move(origin.reconciliation_epoch_id),
                                              std::move(origin.authoritative_cut_id),
                                              origin.row_ordinal, origin.cut_time},
      std::move(logical_account_id), std::move(venue_id), std::nullopt,
      oms::CancellationResultPayload{std::move(locator), result, std::nullopt,
                                     terminal_cumulative_quantity});
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt, received_at));
}

// --------------------------------------------------------
// Validate an account timeout under exact configured account and venue authority.
model::Result<oms::PrivateOrderIngressAttempt>
PrivateOrderEventFactory::create_account_timeout_attempt(oms::LocalPrivateIngressOrigin origin,
                                                         model::LogicalAccountId logical_account_id,
                                                         model::VenueId venue_id) const {
  auto provenance = resolver_.create_configured_account_provenance(logical_account_id, venue_id);
  if (!provenance) {
    return model::Result<oms::PrivateOrderIngressAttempt>::failure(provenance.error());
  }
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      oms::PrivateOrderIngressAttempt{oms::PrivateEventIngressSemanticValue{
          std::move(origin), oms::PrivateEventSubjectScope::Account, std::move(logical_account_id),
          std::move(venue_id), std::move(provenance).value(),
          oms::AccountTimeoutObservedPayload{}}});
}

// --------------------------------------------------------
// Normalize an account timeout by attaching only the supplied local receipt observation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::normalize_account_timeout(oms::LocalPrivateEventOrigin origin,
                                                    model::LogicalAccountId logical_account_id,
                                                    model::VenueId venue_id) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{std::move(origin.event_id), origin.source_time},
      std::move(logical_account_id), std::move(venue_id));
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------
// Validate a private-source disconnect under exact configured account and venue authority.
model::Result<oms::PrivateOrderIngressAttempt> PrivateOrderEventFactory::create_disconnect_attempt(
    oms::LocalPrivateIngressOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateSourceEpochId affected_source_epoch_id) const {
  auto provenance = resolver_.create_configured_account_provenance(logical_account_id, venue_id);
  if (!provenance) {
    return model::Result<oms::PrivateOrderIngressAttempt>::failure(provenance.error());
  }
  return model::Result<oms::PrivateOrderIngressAttempt>::success(
      oms::PrivateOrderIngressAttempt{oms::PrivateEventIngressSemanticValue{
          std::move(origin), oms::PrivateEventSubjectScope::PrivateSource,
          std::move(logical_account_id), std::move(venue_id), std::move(provenance).value(),
          oms::DisconnectObservedPayload{std::move(affected_source_epoch_id)}}});
}

// --------------------------------------------------------
// Normalize a private-source disconnect by attaching only the supplied receipt observation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::normalize_disconnect(
    oms::LocalPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateSourceEpochId affected_source_epoch_id) const {
  const auto received_at = origin.receive_time;
  auto attempt = create_disconnect_attempt(
      oms::LocalPrivateIngressOrigin{std::move(origin.event_id), origin.source_time},
      std::move(logical_account_id), std::move(venue_id), std::move(affected_source_epoch_id));
  if (!attempt) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(attempt.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(
      normalize_private_order_ingress_attempt(attempt.value(), received_at));
}

// --------------------------------------------------------

} // namespace aegis::runtime
