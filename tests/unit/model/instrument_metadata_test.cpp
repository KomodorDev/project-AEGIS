// Purpose: prove revisioned metadata validates units and owns tick, step, and multiplier rules.

#include "aegis/model/instrument_metadata.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace {

using namespace aegis::model;

// ########################################################################
// Interesting syntax: a requires-expression proves float, bool, enum, and plain/wide/Unicode
// character scales are removed at overload resolution without adding uncompilable test calls.
template <typename Scale>
concept HasContractValue = requires(InstrumentMetadata metadata, Quantity quantity, Scale scale) {
  metadata.contract_value(quantity, scale, RoundingMode::Exact);
};

// ########################################################################
// Model a legacy unscoped enum that would otherwise convert implicitly to an integer scale.
enum LegacyScale { LegacyScaleZero = 0 };

// ########################################################################
// Contract conversion has the same compile-time floating-point precision firewall as FixedPoint.
static_assert(!HasContractValue<double>);
static_assert(!HasContractValue<long double>);

// The exclusions match std::in_range's portable source set, while signed integers remain callable;
// the negative-scale assertion below instantiates the body and proves checked runtime rejection.
static_assert(!HasContractValue<bool>);
static_assert(!HasContractValue<LegacyScale>);
static_assert(!HasContractValue<char>);
static_assert(!HasContractValue<char16_t>);
static_assert(HasContractValue<int>);

// ########################################################################

// --------------------------------------------------------
// Route fixture literals through nominal production parsers so invalid test data fails at its
// source.
template <typename Value> [[nodiscard]] Value parsed(std::string_view text) {
  auto result = Value::parse_ascii(text);
  REQUIRE(result);
  return result.value();
}

// --------------------------------------------------------
// Model the reference inverse BTC contract once; rejection tests mutate one field of this
// known-valid snapshot so the reported failure cannot be attributed to unrelated fixture drift.
[[nodiscard]] InstrumentMetadataParams reference_params() {
  return InstrumentMetadataParams{
      VenueId::parse("deribit").value(),
      InstrumentId::parse("BTC-USD-PERPETUAL").value(),
      VenueInstrumentId::parse("BTC-PERPETUAL").value(),
      InstrumentMetadataRevision::initial(),
      "BTC",
      "USD",
      "BTC",
      ContractStyle::Inverse,
      QuantityUnit::Contracts,
      ContractMultiplierUnit::QuoteCurrencyPerContract,
      1U,
      0U,
      parsed<Price>("0.5"),
      parsed<Quantity>("1"),
      parsed<Quantity>("1"),
      parsed<Notional>("10"),
  };
}

// --------------------------------------------------------
// The reference fixture proves revision ownership, declared units, price-free inverse face value,
// and quantity-step enforcement through one validated metadata object.
TEST_CASE("the reference inverse metadata is a revisioned fixture with declared units",
          "[model][metadata]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the reference authoring parameters and inspect every owned economic field.
  const auto metadata = InstrumentMetadata::create(reference_params());
  REQUIRE(metadata);

  CHECK(metadata.value().venue_id().value() == "deribit");
  CHECK(metadata.value().instrument_id().value() == "BTC-USD-PERPETUAL");
  CHECK(metadata.value().venue_instrument_id().value() == "BTC-PERPETUAL");
  CHECK(metadata.value().revision().value() == 1U);
  CHECK(metadata.value().contract_style() == ContractStyle::Inverse);
  CHECK(metadata.value().quantity_unit() == QuantityUnit::Contracts);
  CHECK(metadata.value().contract_multiplier_unit() ==
        ContractMultiplierUnit::QuoteCurrencyPerContract);
  CHECK(metadata.value().quantity_step() == parsed<Quantity>("1"));
  CHECK(metadata.value().minimum_quantity() == parsed<Quantity>("1"));
  CHECK(metadata.value().contract_multiplier() == parsed<Notional>("10"));
  CHECK(metadata.value().contract_value_currency() == "USD");

  // ++++++++++++++++++++++++++++++++++++++++
  // Convert aligned whole contracts into exact inverse face value without consulting price.
  const auto value =
      metadata.value().contract_value(parsed<Quantity>("3"), 0U, RoundingMode::Exact);
  REQUIRE(value);
  CHECK(value.value() == parsed<Notional>("30"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject fractional contracts that violate the metadata-owned quantity step.
  const auto fractional =
      metadata.value().contract_value(parsed<Quantity>("1.5"), 1U, RoundingMode::Exact);
  REQUIRE_FALSE(fractional);
  CHECK(fractional.error().code == DomainErrorCode::MisalignedQuantity);

  // ++++++++++++++++++++++++++++++++++++++++
  // Retaining the signed input through the public gate prevents unsigned wrap before validation.
  const auto negative_scale =
      metadata.value().contract_value(parsed<Quantity>("1"), -1, RoundingMode::Exact);
  REQUIRE_FALSE(negative_scale);
  CHECK(negative_scale.error() ==
        DomainError::at_field(DomainErrorCode::InvalidScale, "contract_value"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Alignment and quantization must consistently use the metadata-owned tick and contract step.
TEST_CASE("metadata validates exact tick and contract-step alignment", "[model][metadata]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one validated metadata snapshot for all alignment and quantization checks.
  const auto metadata = InstrumentMetadata::create(reference_params()).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Distinguish aligned and misaligned prices using the owned tick size.
  CHECK(metadata.validate_price_alignment(parsed<Price>("100.5")));
  const auto bad_price = metadata.validate_price_alignment(parsed<Price>("100.25"));
  REQUIRE_FALSE(bad_price);
  CHECK(bad_price.error().code == DomainErrorCode::MisalignedPrice);

  // ++++++++++++++++++++++++++++++++++++++++
  // Distinguish aligned and misaligned quantities using the owned contract step.
  CHECK(metadata.validate_quantity_alignment(parsed<Quantity>("2")));
  const auto bad_quantity = metadata.validate_quantity_alignment(parsed<Quantity>("1.5"));
  REQUIRE_FALSE(bad_quantity);
  CHECK(bad_quantity.error().code == DomainErrorCode::MisalignedQuantity);

  // ++++++++++++++++++++++++++++++++++++++++
  // Quantize prices and quantities through explicit floor and ceiling policies.
  CHECK(metadata.quantize_price(parsed<Price>("100.24"), RoundingMode::Floor).value() ==
        parsed<Price>("100"));
  CHECK(metadata.quantize_price(parsed<Price>("100.24"), RoundingMode::Ceiling).value() ==
        parsed<Price>("100.5"));
  CHECK(metadata.quantize_quantity(parsed<Quantity>("1.5"), RoundingMode::Floor).value() ==
        parsed<Quantity>("1"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Multiple corrupt fields prove that create() preserves its canonical first-failure compatibility
// contract rather than returning whichever validation happens to run first after a refactor.
TEST_CASE("metadata rejects corrupt fields in canonical order", "[model][metadata]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Currency syntax precedes numeric validation even when both fields are corrupt.
  auto params = reference_params();
  params.base_currency = "btc";
  params.tick_size = parsed<Price>("-0.5");
  const auto first_failure = InstrumentMetadata::create(params);
  REQUIRE_FALSE(first_failure);
  CHECK(first_failure.error().code == DomainErrorCode::InvalidMetadata);
  CHECK(first_failure.error().context.field == "instrument.base_currency");

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject an unassigned contract-style representation at its owning field.
  params = reference_params();
  params.contract_style = static_cast<ContractStyle>(99U);
  const auto bad_style = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_style);
  CHECK(bad_style.error().context.field == "instrument.contract_style");

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject multiplier units that contradict inverse contract semantics.
  params = reference_params();
  params.contract_multiplier_unit = ContractMultiplierUnit::BaseCurrencyPerContract;
  const auto bad_inverse_unit = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_inverse_unit);
  CHECK(bad_inverse_unit.error().context.field == "instrument.contract_multiplier_unit");

  // ++++++++++++++++++++++++++++++++++++++++
  // A scale wider than uint8_t proves create() validates before its narrow public accessor can
  // wrap.
  params = reference_params();
  params.price_scale = std::uint64_t{256U};
  const auto wrapped_price_scale = InstrumentMetadata::create(params);
  REQUIRE_FALSE(wrapped_price_scale);
  CHECK(wrapped_price_scale.error().context.field == "instrument.price_scale");

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate tick storage against the declared price scale.
  params = reference_params();
  params.price_scale = 0U;
  const auto bad_tick_scale = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_tick_scale);
  CHECK(bad_tick_scale.error().context.field == "instrument.tick_size");

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject a minimum quantity not aligned to the declared contract step.
  params = reference_params();
  params.quantity_scale = 1U;
  params.minimum_quantity = parsed<Quantity>("1.5");
  const auto bad_minimum = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_minimum);
  CHECK(bad_minimum.error().context.field == "instrument.minimum_quantity");

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject inverse settlement semantics that duplicate the quote currency.
  params = reference_params();
  params.settlement_currency = "USD";
  const auto bad_settlement = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_settlement);
  CHECK(bad_settlement.error().context.field == "instrument.settlement_currency");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Linear and inverse styles bind different multiplier currencies and cannot borrow each other's
// unit combinations.
TEST_CASE("linear and inverse multiplier units cannot be crossed", "[model][metadata]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A linear contract binds its multiplier to base currency under the accepted unit combination.
  auto params = reference_params();
  params.contract_style = ContractStyle::Linear;
  params.contract_multiplier_unit = ContractMultiplierUnit::BaseCurrencyPerContract;
  params.settlement_currency = "USD";
  const auto linear = InstrumentMetadata::create(params);
  REQUIRE(linear);
  CHECK(linear.value().contract_value_currency() == "BTC");

  // ++++++++++++++++++++++++++++++++++++++++
  // The inverse quote-per-contract unit cannot be reused after switching to linear style.
  params.contract_multiplier_unit = ContractMultiplierUnit::QuoteCurrencyPerContract;
  const auto wrong_linear_unit = InstrumentMetadata::create(params);
  REQUIRE_FALSE(wrong_linear_unit);
  CHECK(wrong_linear_unit.error().code == DomainErrorCode::InvalidMetadata);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
