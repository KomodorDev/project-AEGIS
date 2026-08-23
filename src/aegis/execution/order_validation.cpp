// Purpose: implement the exact M3 canonical order validation precedence before identity generation
// and risk, preserving the strategy-authored price and quantity bit-for-bit.

#include "aegis/execution/order_validation.hpp"

namespace aegis::execution {
namespace {

// --------------------------------------------------------
// Return the first ordinary validation failure without constructing partially approved economics.
[[nodiscard]] CanonicalValidationDecision rejected(SubmissionReason reason) noexcept {
  return CanonicalValidationDecision{std::nullopt, reason};
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Apply assigned vocabulary, price filters, quantity filters, then supported inverse economics.
CanonicalValidationDecision
validate_canonical_order(const OrderRequest& request,
                         const model::InstrumentMetadata& metadata) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject unassigned enum storage before interpreting any economic value.
  if (request.side != OrderSide::Buy && request.side != OrderSide::Sell) {
    return rejected(SubmissionReason::UnsupportedSide);
  }
  if (request.type != OrderType::Limit) {
    return rejected(SubmissionReason::UnsupportedOrderType);
  }
  if (request.time_in_force != TimeInForce::GoodTilCancelled) {
    return rejected(SubmissionReason::UnsupportedTimeInForce);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate exact price precision and tick alignment without quantizing the caller's value.
  if (request.price.coefficient() <= 0) {
    return rejected(SubmissionReason::PriceNotPositive);
  }
  if (request.price.scale() > metadata.price_scale()) {
    return rejected(SubmissionReason::PriceScaleExceeded);
  }
  if (!metadata.validate_price_alignment(request.price)) {
    return rejected(SubmissionReason::PriceTickMismatch);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate positive quantity, maximum precision, minimum, and exact lot-step alignment.
  if (request.quantity.coefficient() <= 0) {
    return rejected(SubmissionReason::QuantityNotPositive);
  }
  if (request.quantity.scale() > metadata.quantity_scale()) {
    return rejected(SubmissionReason::QuantityScaleExceeded);
  }
  if (request.quantity < metadata.minimum_quantity()) {
    return rejected(SubmissionReason::QuantityBelowMinimum);
  }
  if (!metadata.validate_quantity_alignment(request.quantity)) {
    return rejected(SubmissionReason::QuantityStepMismatch);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // M3 supports only inverse contracts whose multiplier is quote currency per contract.
  if (metadata.contract_style() != model::ContractStyle::Inverse ||
      metadata.quantity_unit() != model::QuantityUnit::Contracts ||
      metadata.contract_multiplier_unit() !=
          model::ContractMultiplierUnit::QuoteCurrencyPerContract) {
    return rejected(SubmissionReason::UnsupportedContractEconomics);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish exact authored economics only after every earlier branch has succeeded.
  return CanonicalValidationDecision{CanonicalOrderEconomics{request.side, request.type,
                                                             request.time_in_force, request.price,
                                                             request.quantity},
                                     SubmissionReason::None};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::execution
