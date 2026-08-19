// Purpose: validate revisioned instrument units, increments, and contract conversion boundaries.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/integer_input.hpp"
#include "aegis/model/time.hpp"

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegis::model {

// ########################################################################
// Contract style and unit enums make economic meaning explicit; create() validates their permitted
// combinations rather than relying on conventions at conversion call sites.
enum class ContractStyle : std::uint8_t {
  Linear = 1,
  Inverse = 2,
};

// ########################################################################
// Quantity units identify what a quantity coefficient counts.
enum class QuantityUnit : std::uint8_t {
  Contracts = 1,
};

// ########################################################################
// Multiplier units identify the currency dimension of one contract's face value.
enum class ContractMultiplierUnit : std::uint8_t {
  BaseCurrencyPerContract = 1,
  QuoteCurrencyPerContract = 2,
};

// ########################################################################
// Carry the complete untrusted configuration snapshot into a single validation boundary. Scales
// stay wide so out-of-range inputs cannot wrap before validation; getters narrow only accepted
// data.
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
  std::uint64_t price_scale;
  std::uint64_t quantity_scale;
  Price tick_size;
  Quantity quantity_step;
  Quantity minimum_quantity;
  Notional contract_multiplier;
};

// ########################################################################
// A successfully created instance owns the revisioned identifiers, currencies, scales, increments,
// and multiplier contract used by every downstream alignment and conversion decision.
class InstrumentMetadata {
public:

  // --------------------------------------------------------
  // Validate one complete authored snapshot before publishing immutable metadata.
  [[nodiscard]] static Result<InstrumentMetadata> create(InstrumentMetadataParams params);

  // --------------------------------------------------------
  // Borrow the validated venue identity.
  [[nodiscard]] const VenueId& venue_id() const noexcept { return params_.venue_id; }

  // --------------------------------------------------------
  // Borrow the validated normalized instrument identity.
  [[nodiscard]] const InstrumentId& instrument_id() const noexcept { return params_.instrument_id; }

  // --------------------------------------------------------
  // Borrow the validated venue-native instrument identity.
  [[nodiscard]] const VenueInstrumentId& venue_instrument_id() const noexcept {
    return params_.venue_instrument_id;
  }

  // --------------------------------------------------------
  // Return the accepted metadata revision.
  [[nodiscard]] InstrumentMetadataRevision revision() const noexcept { return params_.revision; }

  // --------------------------------------------------------
  // Borrow the validated base-currency code.
  [[nodiscard]] std::string_view base_currency() const noexcept { return params_.base_currency; }

  // --------------------------------------------------------
  // Borrow the validated quote-currency code.
  [[nodiscard]] std::string_view quote_currency() const noexcept { return params_.quote_currency; }

  // --------------------------------------------------------
  // Borrow the validated settlement-currency code.
  [[nodiscard]] std::string_view settlement_currency() const noexcept {
    return params_.settlement_currency;
  }

  // --------------------------------------------------------
  // Return the validated linear or inverse contract style.
  [[nodiscard]] ContractStyle contract_style() const noexcept { return params_.contract_style; }

  // --------------------------------------------------------
  // Return the validated quantity unit.
  [[nodiscard]] QuantityUnit quantity_unit() const noexcept { return params_.quantity_unit; }

  // --------------------------------------------------------
  // Return the validated contract-multiplier unit.
  [[nodiscard]] ContractMultiplierUnit contract_multiplier_unit() const noexcept {
    return params_.contract_multiplier_unit;
  }

  // --------------------------------------------------------
  // Return the validated price scale after safe narrowing.
  [[nodiscard]] std::uint8_t price_scale() const noexcept {
    return static_cast<std::uint8_t>(params_.price_scale);
  }

  // --------------------------------------------------------
  // Return the validated quantity scale after safe narrowing.
  [[nodiscard]] std::uint8_t quantity_scale() const noexcept {
    return static_cast<std::uint8_t>(params_.quantity_scale);
  }

  // --------------------------------------------------------
  // Return the exact validated price increment.
  [[nodiscard]] Price tick_size() const noexcept { return params_.tick_size; }

  // --------------------------------------------------------
  // Return the exact validated quantity increment.
  [[nodiscard]] Quantity quantity_step() const noexcept { return params_.quantity_step; }

  // --------------------------------------------------------
  // Return the exact validated minimum quantity.
  [[nodiscard]] Quantity minimum_quantity() const noexcept { return params_.minimum_quantity; }

  // --------------------------------------------------------
  // Return the exact validated per-contract face value.
  [[nodiscard]] Notional contract_multiplier() const noexcept {
    return params_.contract_multiplier;
  }

  // --------------------------------------------------------
  // Validate a price against the metadata-owned tick size.
  [[nodiscard]] Result<void> validate_price_alignment(Price price) const;

  // --------------------------------------------------------
  // Validate a quantity against the metadata-owned step and minimum.
  [[nodiscard]] Result<void> validate_quantity_alignment(Quantity quantity) const;

  // --------------------------------------------------------
  // Quantize a price to the metadata-owned tick under an explicit rounding policy.
  [[nodiscard]] Result<Price> quantize_price(Price price, RoundingMode rounding) const;

  // --------------------------------------------------------
  // Quantize a quantity to the metadata-owned step under an explicit rounding policy.
  [[nodiscard]] Result<Quantity> quantize_quantity(Quantity quantity, RoundingMode rounding) const;

  // --------------------------------------------------------
  // Convert declared contracts to the multiplier's declared currency. The API deliberately takes no
  // price: inverse face value must not be computed through a generic price-times-quantity shortcut.
  // Interesting syntax: CheckedIntegerInput rejects bool, enum, and plain/wide/Unicode character
  // scales, while the deleted overload gives floating-point scales the same compile-time firewall.
  // Signed and unsigned char remain supported numeric sources and wide integers remain available
  // for checked validation.
  // The deduced scale retains its sign through in_range, so negative values become InvalidScale
  // instead of wrapping into a large unsigned target.
  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<Notional> contract_value(Quantity contracts, Scale target_scale,
                                                RoundingMode rounding) const {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject signed-negative or unrepresentable scales before any narrowing conversion.
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<Notional>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "contract_value"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width scale to the stable conversion implementation.
    return contract_value_validated(contracts, static_cast<std::uint64_t>(target_scale), rounding);

    // ++++++++++++++++++++++++++++++++++++++++
  }
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<Notional> contract_value(Quantity, Scale, RoundingMode) const = delete;

  // --------------------------------------------------------
  // Return the currency dimension promised by contract_value.
  [[nodiscard]] std::string_view contract_value_currency() const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // The constrained public gate normalizes scale width before this stable implementation boundary.
  [[nodiscard]] Result<Notional> contract_value_validated(Quantity contracts,
                                                          std::uint64_t target_scale,
                                                          RoundingMode rounding) const;

  // --------------------------------------------------------
  // Construction is private so callers cannot bypass the aggregate validation performed by
  // create().
  explicit InstrumentMetadata(InstrumentMetadataParams params) : params_{std::move(params)} {}

  // --------------------------------------------------------
  InstrumentMetadataParams params_;
};

// ########################################################################
} // namespace aegis::model
