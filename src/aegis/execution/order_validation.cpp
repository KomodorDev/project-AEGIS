// Purpose: implement the exact M3 canonical order validation precedence before identity generation
// and risk, preserving the strategy-authored price and quantity bit-for-bit.

#include "aegis/execution/order_validation.hpp"

namespace aegis::execution {
namespace {

// --------------------------------------------------------
// Return the first ordinary validation failure without constructing partially approved economics.
[[nodiscard]] CanonicalValidationDecision
create_rejected_validation_decision(SubmissionReason reason) noexcept {
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
    return create_rejected_validation_decision(SubmissionReason::UnsupportedSide);
  }
  if (request.type != OrderType::Limit) {
    return create_rejected_validation_decision(SubmissionReason::UnsupportedOrderType);
  }
  if (request.time_in_force != TimeInForce::GoodTilCancelled) {
    return create_rejected_validation_decision(SubmissionReason::UnsupportedTimeInForce);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate exact price precision and tick alignment without quantizing the caller's value.
  if (request.price.coefficient() <= 0) {
    return create_rejected_validation_decision(SubmissionReason::PriceNotPositive);
  }
  if (request.price.scale() > metadata.price_scale()) {
    return create_rejected_validation_decision(SubmissionReason::PriceScaleExceeded);
  }
  if (!metadata.validate_price_alignment(request.price)) {
    return create_rejected_validation_decision(SubmissionReason::PriceTickMismatch);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate positive quantity, maximum precision, minimum, and exact lot-step alignment.
  if (request.quantity.coefficient() <= 0) {
    return create_rejected_validation_decision(SubmissionReason::QuantityNotPositive);
  }
  if (request.quantity.scale() > metadata.quantity_scale()) {
    return create_rejected_validation_decision(SubmissionReason::QuantityScaleExceeded);
  }
  if (request.quantity < metadata.minimum_quantity()) {
    return create_rejected_validation_decision(SubmissionReason::QuantityBelowMinimum);
  }
  if (!metadata.validate_quantity_alignment(request.quantity)) {
    return create_rejected_validation_decision(SubmissionReason::QuantityStepMismatch);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // M3 supports only inverse contracts whose multiplier is quote currency per contract.
  if (metadata.contract_style() != model::ContractStyle::Inverse ||
      metadata.quantity_unit() != model::QuantityUnit::Contracts ||
      metadata.contract_multiplier_unit() !=
          model::ContractMultiplierUnit::QuoteCurrencyPerContract) {
    return create_rejected_validation_decision(SubmissionReason::UnsupportedContractEconomics);
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
