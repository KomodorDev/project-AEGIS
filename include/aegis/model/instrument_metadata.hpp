// Purpose: validate revisioned instrument units, increments, and contract conversion boundaries.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/time.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::model {

enum class ContractStyle : std::uint8_t {
  Linear = 1,
  Inverse = 2,
};

enum class QuantityUnit : std::uint8_t {
  Contracts = 1,
};

enum class ContractMultiplierUnit : std::uint8_t {
  BaseCurrencyPerContract = 1,
  QuoteCurrencyPerContract = 2,
};

struct InstrumentMetadataParams {
  VenueId venue_id;
  InstrumentId instrument_id;
  VenueInstrumentId venue_instrument_id;
  InstrumentMetadataRevision revision;
  std::string base_currency;
  std::string quote_currency;
  std::string settlement_currency;
  ContractStyle contract_style;
  QuantityUnit quantity_unit;
  ContractMultiplierUnit contract_multiplier_unit;
  std::uint8_t price_scale;
  std::uint8_t quantity_scale;
  Price tick_size;
  Quantity quantity_step;
  Quantity minimum_quantity;
  Notional contract_multiplier;
};

class InstrumentMetadata {
public:
  [[nodiscard]] static Result<InstrumentMetadata> create(InstrumentMetadataParams params);

  [[nodiscard]] const VenueId& venue_id() const noexcept { return params_.venue_id; }
  [[nodiscard]] const InstrumentId& instrument_id() const noexcept { return params_.instrument_id; }
  [[nodiscard]] const VenueInstrumentId& venue_instrument_id() const noexcept {
    return params_.venue_instrument_id;
  }
  [[nodiscard]] InstrumentMetadataRevision revision() const noexcept { return params_.revision; }
  [[nodiscard]] std::string_view base_currency() const noexcept { return params_.base_currency; }
  [[nodiscard]] std::string_view quote_currency() const noexcept { return params_.quote_currency; }
  [[nodiscard]] std::string_view settlement_currency() const noexcept {
    return params_.settlement_currency;
  }
  [[nodiscard]] ContractStyle contract_style() const noexcept { return params_.contract_style; }
  [[nodiscard]] QuantityUnit quantity_unit() const noexcept { return params_.quantity_unit; }
  [[nodiscard]] ContractMultiplierUnit contract_multiplier_unit() const noexcept {
    return params_.contract_multiplier_unit;
  }
  [[nodiscard]] std::uint8_t price_scale() const noexcept { return params_.price_scale; }
  [[nodiscard]] std::uint8_t quantity_scale() const noexcept { return params_.quantity_scale; }
  [[nodiscard]] Price tick_size() const noexcept { return params_.tick_size; }
  [[nodiscard]] Quantity quantity_step() const noexcept { return params_.quantity_step; }
  [[nodiscard]] Quantity minimum_quantity() const noexcept { return params_.minimum_quantity; }
  [[nodiscard]] Notional contract_multiplier() const noexcept {
    return params_.contract_multiplier;
  }

  [[nodiscard]] Result<void> validate_price_alignment(Price price) const;
  [[nodiscard]] Result<void> validate_quantity_alignment(Quantity quantity) const;
  [[nodiscard]] Result<Price> quantize_price(Price price, RoundingMode rounding) const;
  [[nodiscard]] Result<Quantity> quantize_quantity(Quantity quantity, RoundingMode rounding) const;

  // Converts declared contracts to the multiplier's declared currency. It deliberately takes no
  // price: inverse face value must not be computed through a generic price-times-quantity shortcut.
  [[nodiscard]] Result<Notional> contract_value(Quantity contracts, std::uint8_t target_scale,
                                                RoundingMode rounding) const;
  [[nodiscard]] std::string_view contract_value_currency() const noexcept;

private:
  explicit InstrumentMetadata(InstrumentMetadataParams params) : params_{std::move(params)} {}

  InstrumentMetadataParams params_;
};

} // namespace aegis::model
