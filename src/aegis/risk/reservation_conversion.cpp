// Purpose: preserve exact approved exposure while conservatively rounding residual contracts;
// calculate all cumulative and incremental allocations without changing any owner state.

#include "aegis/risk/reservation_conversion.hpp"

#include <initializer_list>
#include <utility>

namespace aegis::risk {
namespace {

// --------------------------------------------------------
// Calculate only positive residual exposure, preserving the original rounding at zero fill and
// reaching exact zero at full fill. Endpoint validation belongs to the enclosing calculation.
[[nodiscard]] model::Result<OrderExposure>
calculate_remaining_exposure(const OrderExposure& original, model::Quantity cumulative_quantity,
                             const model::InstrumentMetadata& metadata,
                             std::uint64_t notional_scale) {
  if (cumulative_quantity.coefficient() == 0) {
    return model::Result<OrderExposure>::create_success(original);
  }
  auto remaining_quantity = original.quantity.checked_subtract(cumulative_quantity);
  if (!remaining_quantity) {
    return model::Result<OrderExposure>::create_failure(std::move(remaining_quantity).error());
  }
  auto remaining_notional = metadata.calculate_contract_value(
      remaining_quantity.value(), notional_scale, model::RoundingMode::AwayFromZero);
  if (!remaining_notional) {
    return model::Result<OrderExposure>::create_failure(std::move(remaining_notional).error());
  }
  return model::Result<OrderExposure>::create_success(
      OrderExposure{remaining_quantity.value(), remaining_notional.value()});
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Recompute the previous allocation before deriving any transfer, so inconsistent retained state
// cannot turn a cumulative observation into understated residual exposure.
model::Result<CumulativeReservationConversion> calculate_cumulative_reservation_conversion(
    const OrderExposure& original, const OrderExposure& previous_cumulative_confirmed,
    model::Quantity next_cumulative_quantity, const model::InstrumentMetadata& metadata,
    std::uint64_t notional_scale) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the metadata dimensions and complete authored scale before narrowing or arithmetic.
  if (metadata.contract_style() != model::ContractStyle::Inverse ||
      metadata.quantity_unit() != model::QuantityUnit::Contracts ||
      metadata.contract_multiplier_unit() !=
          model::ContractMultiplierUnit::QuoteCurrencyPerContract ||
      metadata.contract_value_currency() != metadata.quote_currency()) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "reservation_conversion.metadata"));
  }
  if (notional_scale > model::FixedPoint::maximum_scale ||
      original.quantity.scale() > metadata.quantity_scale() ||
      previous_cumulative_confirmed.quantity.scale() > metadata.quantity_scale() ||
      next_cumulative_quantity.scale() > metadata.quantity_scale()) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidScale,
                                            "reservation_conversion.scale"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A cumulative endpoint may remain fixed but cannot decrease or exceed its approved order.
  if (original.quantity.coefficient() <= 0 || original.quote_notional.coefficient() <= 0 ||
      previous_cumulative_confirmed.quantity.coefficient() < 0 ||
      previous_cumulative_confirmed.quote_notional.coefficient() < 0 ||
      next_cumulative_quantity < previous_cumulative_confirmed.quantity ||
      next_cumulative_quantity > original.quantity) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "reservation_conversion.cumulative"));
  }
  for (const auto quantity :
       {original.quantity, previous_cumulative_confirmed.quantity, next_cumulative_quantity}) {
    auto alignment = metadata.validate_quantity_alignment(quantity);
    if (!alignment) {
      return model::Result<CumulativeReservationConversion>::create_failure(
          std::move(alignment).error());
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Recheck both original and previous cumulative economics instead of trusting numeric copies.
  auto approved_notional = metadata.calculate_contract_value(original.quantity, notional_scale,
                                                             model::RoundingMode::AwayFromZero);
  if (!approved_notional) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(approved_notional).error());
  }
  if (approved_notional.value() != original.quote_notional) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "reservation_conversion.original"));
  }
  auto previous_remaining = calculate_remaining_exposure(
      original, previous_cumulative_confirmed.quantity, metadata, notional_scale);
  if (!previous_remaining) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(previous_remaining).error());
  }
  auto expected_previous_notional =
      original.quote_notional.checked_subtract(previous_remaining.value().quote_notional);
  if (!expected_previous_notional) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(expected_previous_notional).error());
  }
  if (expected_previous_notional.value() != previous_cumulative_confirmed.quote_notional) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                            "reservation_conversion.previous"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate the difference between cumulative endpoints; never round an incremental fill.
  auto remaining =
      calculate_remaining_exposure(original, next_cumulative_quantity, metadata, notional_scale);
  if (!remaining) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(remaining).error());
  }
  auto cumulative_notional =
      original.quote_notional.checked_subtract(remaining.value().quote_notional);
  if (!cumulative_notional) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(cumulative_notional).error());
  }
  auto quantity_delta =
      next_cumulative_quantity.checked_subtract(previous_cumulative_confirmed.quantity);
  if (!quantity_delta) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(quantity_delta).error());
  }
  auto notional_delta =
      cumulative_notional.value().checked_subtract(previous_cumulative_confirmed.quote_notional);
  if (!notional_delta) {
    return model::Result<CumulativeReservationConversion>::create_failure(
        std::move(notional_delta).error());
  }
  return model::Result<CumulativeReservationConversion>::create_success(
      CumulativeReservationConversion{
          remaining.value(), OrderExposure{next_cumulative_quantity, cumulative_notional.value()},
          OrderExposure{quantity_delta.value(), notional_delta.value()}});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::risk
