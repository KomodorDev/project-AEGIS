// Purpose: validate every normalized private-event dependent shape and derive its source provenance
// from sealed authority before publishing an immutable correlation-independent input.

#include "private_order_event_factory.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Return the stable source-shape failure without exposing an open caller detail channel.
[[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> invalid(std::string_view field) {
  return model::Result<oms::NormalizedPrivateOrderInput>::failure(model::DomainError::at_field(
      model::DomainErrorCode::InvalidPrivateEvent, std::string{field}));
}

// --------------------------------------------------------
// Accept only the two assigned order sides at raw enum boundaries.
[[nodiscard]] bool assigned(execution::OrderSide side) noexcept {
  return side == execution::OrderSide::Buy || side == execution::OrderSide::Sell;
}

// --------------------------------------------------------
// Accept only the six assigned venue-neutral rejection categories.
[[nodiscard]] bool assigned(oms::ExchangeRejectionCategory category) noexcept {
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
[[nodiscard]] bool assigned(oms::CancellationResult result) noexcept {
  return result == oms::CancellationResult::Cancelled ||
         result == oms::CancellationResult::CancelRejected;
}

// --------------------------------------------------------
// Validate the common positive execution interval and exact fixed-point subtraction boundary.
[[nodiscard]] bool valid_execution(model::Quantity incremental_quantity,
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
// Attach maximal independently proved source provenance to one authoritative payload.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::authoritative(
    oms::PrivateEventOriginValue origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, const std::optional<model::InstrumentId>& instrument_id,
    oms::PrivateOrderEventPayload payload) const {
  auto provenance = resolver_.authoritative_source(logical_account_id, venue_id, instrument_id);
  return model::Result<oms::NormalizedPrivateOrderInput>::success(oms::NormalizedPrivateOrderInput{
      std::move(origin), oms::PrivateEventSubjectScope::Order, std::move(logical_account_id),
      std::move(venue_id), std::move(provenance), std::move(payload)});
}

// --------------------------------------------------------
// Normalize one ordinary venue acknowledgement without inferring local ownership.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::venue_acknowledgement(
    oms::VenuePrivateEventOrigin origin, oms::ExchangeOrderId exchange_order_id,
    std::optional<model::OrderId> local_order_locator) const {
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return authoritative(std::move(origin), account, venue, std::nullopt,
                       oms::ExchangeAcknowledgedPayload{std::move(exchange_order_id),
                                                        std::move(local_order_locator)});
}

// --------------------------------------------------------
// Normalize one reconciliation acknowledgement without inferring local ownership.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::reconciliation_acknowledgement(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::ExchangeOrderId exchange_order_id,
    std::optional<model::OrderId> local_order_locator,
    std::optional<model::InstrumentId> source_instrument_id) const {
  return authoritative(std::move(origin), std::move(logical_account_id), std::move(venue_id),
                       source_instrument_id,
                       oms::ExchangeAcknowledgedPayload{std::move(exchange_order_id),
                                                        std::move(local_order_locator)});
}

// --------------------------------------------------------
// Normalize a venue rejection after assigned-category and bounded-detail validation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::venue_rejection(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
    oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const {
  if (!assigned(category)) {
    return invalid("private_event.rejection_category");
  }
  auto retained_detail = oms::PrivateRejectionDetail::create(detail);
  if (!retained_detail) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(retained_detail.error());
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return authoritative(std::move(origin), account, venue, std::nullopt,
                       oms::ExchangeRejectedPayload{std::move(locator), category,
                                                    std::move(retained_detail).value()});
}

// --------------------------------------------------------
// Normalize a reconciliation rejection under the same closed shape.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::reconciliation_rejection(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator,
    oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const {
  if (!assigned(category)) {
    return invalid("private_event.rejection_category");
  }
  auto retained_detail = oms::PrivateRejectionDetail::create(detail);
  if (!retained_detail) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(retained_detail.error());
  }
  return authoritative(std::move(origin), std::move(logical_account_id), std::move(venue_id),
                       std::nullopt,
                       oms::ExchangeRejectedPayload{std::move(locator), category,
                                                    std::move(retained_detail).value()});
}

// --------------------------------------------------------
// Normalize a venue execution while retaining optional source side before correlation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::venue_execution(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
    model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price, std::optional<execution::OrderSide> source_side) const {
  if (!valid_execution(incremental_quantity, cumulative_quantity, execution_price)) {
    return invalid("private_event.execution");
  }
  if (source_side.has_value() && !assigned(*source_side)) {
    return invalid("private_event.source_side");
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  const auto source_instrument = instrument_id;
  return authoritative(std::move(origin), account, venue, source_instrument,
                       oms::ExecutionPayload{std::move(locator), std::move(trade_id),
                                             std::move(instrument_id), metadata_revision,
                                             incremental_quantity, cumulative_quantity,
                                             execution_price, source_side});
}

// --------------------------------------------------------
// Normalize a reconciliation execution whose row-side evidence is mandatory.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::reconciliation_execution(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
    model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price, execution::OrderSide source_side) const {
  if (!valid_execution(incremental_quantity, cumulative_quantity, execution_price)) {
    return invalid("private_event.execution");
  }
  if (!assigned(source_side)) {
    return invalid("private_event.source_side");
  }
  const auto source_instrument = instrument_id;
  return authoritative(
      std::move(origin), std::move(logical_account_id), std::move(venue_id), source_instrument,
      oms::ExecutionPayload{std::move(locator), std::move(trade_id), std::move(instrument_id),
                            metadata_revision, incremental_quantity, cumulative_quantity,
                            execution_price, source_side});
}

// --------------------------------------------------------
// Normalize a venue cancellation result and enforce its exact terminal-cumulative shape.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::venue_cancellation_result(
    oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
    oms::CancellationResult result,
    std::optional<model::Quantity> terminal_cumulative_quantity) const {
  if (!assigned(result) ||
      (result == oms::CancellationResult::Cancelled) != terminal_cumulative_quantity.has_value() ||
      (terminal_cumulative_quantity.has_value() &&
       terminal_cumulative_quantity->coefficient() < 0)) {
    return invalid("private_event.cancellation_result");
  }
  const auto account = origin.event_key.logical_account_id;
  const auto venue = origin.event_key.venue_id;
  return authoritative(
      std::move(origin), account, venue, std::nullopt,
      oms::CancellationResultPayload{std::move(locator), result, terminal_cumulative_quantity});
}

// --------------------------------------------------------
// Normalize a reconciliation cancellation result under the same result-dependent shape.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::reconciliation_cancellation_result(
    oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::CancellationResult result,
    std::optional<model::Quantity> terminal_cumulative_quantity) const {
  if (!assigned(result) ||
      (result == oms::CancellationResult::Cancelled) != terminal_cumulative_quantity.has_value() ||
      (terminal_cumulative_quantity.has_value() &&
       terminal_cumulative_quantity->coefficient() < 0)) {
    return invalid("private_event.cancellation_result");
  }
  return authoritative(
      std::move(origin), std::move(logical_account_id), std::move(venue_id), std::nullopt,
      oms::CancellationResultPayload{std::move(locator), result, terminal_cumulative_quantity});
}

// --------------------------------------------------------
// Mint an account timeout only after exact configured account/venue validation.
model::Result<oms::NormalizedPrivateOrderInput>
PrivateOrderEventFactory::account_timeout(oms::LocalPrivateEventOrigin origin,
                                          model::LogicalAccountId logical_account_id,
                                          model::VenueId venue_id) const {
  auto provenance = resolver_.configured_account(logical_account_id, venue_id);
  if (!provenance) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(provenance.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(oms::NormalizedPrivateOrderInput{
      std::move(origin), oms::PrivateEventSubjectScope::Account, std::move(logical_account_id),
      std::move(venue_id), std::move(provenance).value(), oms::AccountTimeoutObservedPayload{}});
}

// --------------------------------------------------------
// Mint a source disconnect only after exact configured account/venue validation.
model::Result<oms::NormalizedPrivateOrderInput> PrivateOrderEventFactory::disconnect(
    oms::LocalPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
    model::VenueId venue_id, oms::PrivateSourceEpochId affected_source_epoch_id) const {
  auto provenance = resolver_.configured_account(logical_account_id, venue_id);
  if (!provenance) {
    return model::Result<oms::NormalizedPrivateOrderInput>::failure(provenance.error());
  }
  return model::Result<oms::NormalizedPrivateOrderInput>::success(oms::NormalizedPrivateOrderInput{
      std::move(origin), oms::PrivateEventSubjectScope::PrivateSource,
      std::move(logical_account_id), std::move(venue_id), std::move(provenance).value(),
      oms::DisconnectObservedPayload{std::move(affected_source_epoch_id)}});
}

// --------------------------------------------------------

} // namespace aegis::runtime
