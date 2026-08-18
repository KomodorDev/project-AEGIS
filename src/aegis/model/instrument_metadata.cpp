// Purpose: enforce deterministic instrument metadata validation and explicit unit conversion.

#include "aegis/model/instrument_metadata.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace aegis::model {
namespace {

// Currency identifiers are deliberately narrower than general IDs: 3-12 uppercase ASCII letters
// or digits, beginning with a letter, keep unit comparisons deterministic and locale-free.
[[nodiscard]] bool valid_currency(std::string_view value) noexcept {
  if (value.size() < 3U || value.size() > 12U || value.front() < 'A' || value.front() > 'Z') {
    return false;
  }
  for (const auto character : value) {
    const bool uppercase = character >= 'A' && character <= 'Z';
    const bool digit = character >= '0' && character <= '9';
    if (!uppercase && !digit) {
      return false;
    }
  }
  return true;
}

// Metadata construction failures share one stable code and identify the first invalid field.
[[nodiscard]] Result<InstrumentMetadata> invalid_metadata(std::string field) {
  return Result<InstrumentMetadata>::failure(
      DomainError::at_field(DomainErrorCode::InvalidMetadata, std::move(field)));
}

// Preserve arithmetic failure semantics while translating the generic numeric field to the public
// metadata operation that the caller invoked.
template <typename T>
[[nodiscard]] Result<T> remap_numeric_error(Result<T> result, std::string field) {
  if (result) {
    return result;
  }
  auto error = result.error();
  error.context.field = std::move(field);
  return Result<T>::failure(std::move(error));
}

} // namespace

Result<InstrumentMetadata> InstrumentMetadata::create(InstrumentMetadataParams params) {
  // This order is a compatibility contract: callers always receive the first canonical failure.
  // Phase 1: validate currency grammar and the fundamental base/quote distinction.
  if (!valid_currency(params.base_currency)) {
    return invalid_metadata("instrument.base_currency");
  }
  if (!valid_currency(params.quote_currency)) {
    return invalid_metadata("instrument.quote_currency");
  }
  if (!valid_currency(params.settlement_currency)) {
    return invalid_metadata("instrument.settlement_currency");
  }
  if (params.base_currency == params.quote_currency) {
    return invalid_metadata("instrument.quote_currency");
  }

  // Phase 2: validate enum assignments and the style-specific multiplier and settlement units.
  if (params.contract_style != ContractStyle::Linear &&
      params.contract_style != ContractStyle::Inverse) {
    return invalid_metadata("instrument.contract_style");
  }
  if (params.quantity_unit != QuantityUnit::Contracts) {
    return invalid_metadata("instrument.quantity_unit");
  }
  if (params.contract_multiplier_unit != ContractMultiplierUnit::BaseCurrencyPerContract &&
      params.contract_multiplier_unit != ContractMultiplierUnit::QuoteCurrencyPerContract) {
    return invalid_metadata("instrument.contract_multiplier_unit");
  }
  if (params.contract_style == ContractStyle::Inverse &&
      params.contract_multiplier_unit != ContractMultiplierUnit::QuoteCurrencyPerContract) {
    return invalid_metadata("instrument.contract_multiplier_unit");
  }
  if (params.contract_style == ContractStyle::Linear &&
      params.contract_multiplier_unit != ContractMultiplierUnit::BaseCurrencyPerContract) {
    return invalid_metadata("instrument.contract_multiplier_unit");
  }
  if (params.contract_style == ContractStyle::Inverse &&
      params.settlement_currency != params.base_currency) {
    return invalid_metadata("instrument.settlement_currency");
  }

  // Phase 3: prove scales and positive increments are representable in the decimal kernel.
  if (params.price_scale > FixedPoint::maximum_scale) {
    return invalid_metadata("instrument.price_scale");
  }
  if (params.quantity_scale > FixedPoint::maximum_scale) {
    return invalid_metadata("instrument.quantity_scale");
  }
  if (params.tick_size.coefficient() <= 0 || params.tick_size.scale() > params.price_scale) {
    return invalid_metadata("instrument.tick_size");
  }
  if (params.quantity_step.coefficient() <= 0 ||
      params.quantity_step.scale() > params.quantity_scale) {
    return invalid_metadata("instrument.quantity_step");
  }
  if (params.minimum_quantity.coefficient() <= 0 ||
      params.minimum_quantity.scale() > params.quantity_scale) {
    return invalid_metadata("instrument.minimum_quantity");
  }

  // Phase 4: require the minimum to lie on the quantity step and the contract multiplier to be
  // positive before accepting the snapshot.
  const auto minimum_aligned = params.minimum_quantity.is_multiple_of(params.quantity_step);
  if (!minimum_aligned || !minimum_aligned.value()) {
    return invalid_metadata("instrument.minimum_quantity");
  }
  if (params.contract_multiplier.coefficient() <= 0) {
    return invalid_metadata("instrument.contract_multiplier");
  }

  // Ownership transfers only after every field has passed in canonical order.
  return Result<InstrumentMetadata>::success(InstrumentMetadata{std::move(params)});
}

// Distinguish a numeric-kernel failure from a valid price that simply misses the declared tick.
Result<void> InstrumentMetadata::validate_price_alignment(Price price) const {
  const auto aligned = price.is_multiple_of(params_.tick_size);
  if (!aligned) {
    auto error = aligned.error();
    error.context.field = "price";
    return Result<void>::failure(std::move(error));
  }
  if (!aligned.value()) {
    return Result<void>::failure(DomainError::at_field(DomainErrorCode::MisalignedPrice, "price"));
  }
  return Result<void>::success();
}

// Quantity alignment mirrors price alignment but reports the quantity-specific decision error.
Result<void> InstrumentMetadata::validate_quantity_alignment(Quantity quantity) const {
  const auto aligned = quantity.is_multiple_of(params_.quantity_step);
  if (!aligned) {
    auto error = aligned.error();
    error.context.field = "quantity";
    return Result<void>::failure(std::move(error));
  }
  if (!aligned.value()) {
    return Result<void>::failure(
        DomainError::at_field(DomainErrorCode::MisalignedQuantity, "quantity"));
  }
  return Result<void>::success();
}

// Quantization delegates exact arithmetic to the metadata-owned increment and remaps error context.
Result<Price> InstrumentMetadata::quantize_price(Price price, RoundingMode rounding) const {
  return remap_numeric_error(price.quantize(params_.tick_size, rounding), "price");
}

Result<Quantity> InstrumentMetadata::quantize_quantity(Quantity quantity,
                                                       RoundingMode rounding) const {
  return remap_numeric_error(quantity.quantize(params_.quantity_step, rounding), "quantity");
}

Result<Notional> InstrumentMetadata::contract_value_validated(Quantity contracts,
                                                              std::uint64_t target_scale,
                                                              RoundingMode rounding) const {
  // Reject off-step contract counts before privileged access combines nominal Quantity and Notional
  // kernels; callers cannot reach this cross-domain multiplication directly.
  const auto aligned = validate_quantity_alignment(contracts);
  if (!aligned) {
    return Result<Notional>::failure(aligned.error());
  }
  auto converted = contracts.fixed_point().multiply(params_.contract_multiplier.fixed_point(),
                                                    target_scale, rounding);
  if (!converted) {
    auto error = converted.error();
    error.context.field = "contract_value";
    return Result<Notional>::failure(std::move(error));
  }
  return Result<Notional>::success(Notional{converted.value()});
}

// The multiplier declaration, not market price or settlement currency, determines face-value
// currency.
std::string_view InstrumentMetadata::contract_value_currency() const noexcept {
  if (params_.contract_multiplier_unit == ContractMultiplierUnit::BaseCurrencyPerContract) {
    return params_.base_currency;
  }
  return params_.quote_currency;
}

} // namespace aegis::model
