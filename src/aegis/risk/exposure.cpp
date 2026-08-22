// Purpose: calculate one conservative M3 inverse-contract order exposure through the metadata-owned
// dimensional conversion and fixed AwayFromZero rounding rule.

#include "aegis/risk/exposure.hpp"

#include "aegis/model/domain_error.hpp"

#include <cstdint>
#include <utility>

namespace aegis::risk {

// --------------------------------------------------------
// Reject unsupported economics before invoking the sole privileged quantity-to-notional conversion.
model::Result<OrderExposure>
calculate_order_exposure(const execution::CanonicalOrderEconomics& economics,
                         const model::InstrumentMetadata& metadata, std::uint8_t notional_scale) {

  // ++++++++++++++++++++++++++++++++++++++++
  // M3 accepts only positive contract quantities on inverse quote-multiplier metadata.
  if (economics.quantity.coefficient() <= 0 ||
      metadata.contract_style() != model::ContractStyle::Inverse ||
      metadata.quantity_unit() != model::QuantityUnit::Contracts ||
      metadata.contract_multiplier_unit() !=
          model::ContractMultiplierUnit::QuoteCurrencyPerContract ||
      metadata.contract_value_currency() != metadata.quote_currency() ||
      notional_scale > model::FixedPoint::maximum_scale) {
    return model::Result<OrderExposure>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidRiskPolicy, "risk_exposure.economics"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Convert and conservatively round exactly once before any scope accumulator sees the value.
  auto notional = metadata.contract_value(economics.quantity, notional_scale,
                                          model::RoundingMode::AwayFromZero);
  if (!notional) {
    return model::Result<OrderExposure>::failure(std::move(notional).error());
  }
  if (notional.value().coefficient() <= 0) {
    return model::Result<OrderExposure>::failure(model::DomainError::at_field(
        model::DomainErrorCode::ArithmeticOverflow, "risk_exposure.quote_notional"));
  }
  return model::Result<OrderExposure>::success(OrderExposure{economics.quantity, notional.value()});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::risk
