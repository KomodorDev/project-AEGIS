// Purpose: prove revisioned metadata validates units and owns tick, step, and multiplier rules.

#include "aegis/model/instrument_metadata.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace {

using namespace aegis::model;

template <typename Value> [[nodiscard]] Value parsed(std::string_view text) {
  auto result = Value::parse_ascii(text);
  REQUIRE(result);
  return result.value();
}

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

TEST_CASE("the reference inverse metadata is a revisioned fixture with declared units",
          "[model][metadata]") {
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

  const auto value =
      metadata.value().contract_value(parsed<Quantity>("3"), 0U, RoundingMode::Exact);
  REQUIRE(value);
  CHECK(value.value() == parsed<Notional>("30"));

  const auto fractional =
      metadata.value().contract_value(parsed<Quantity>("1.5"), 1U, RoundingMode::Exact);
  REQUIRE_FALSE(fractional);
  CHECK(fractional.error().code == DomainErrorCode::MisalignedQuantity);
}

TEST_CASE("metadata validates exact tick and contract-step alignment", "[model][metadata]") {
  const auto metadata = InstrumentMetadata::create(reference_params()).value();

  CHECK(metadata.validate_price_alignment(parsed<Price>("100.5")));
  const auto bad_price = metadata.validate_price_alignment(parsed<Price>("100.25"));
  REQUIRE_FALSE(bad_price);
  CHECK(bad_price.error().code == DomainErrorCode::MisalignedPrice);

  CHECK(metadata.validate_quantity_alignment(parsed<Quantity>("2")));
  const auto bad_quantity = metadata.validate_quantity_alignment(parsed<Quantity>("1.5"));
  REQUIRE_FALSE(bad_quantity);
  CHECK(bad_quantity.error().code == DomainErrorCode::MisalignedQuantity);

  CHECK(metadata.quantize_price(parsed<Price>("100.24"), RoundingMode::Floor).value() ==
        parsed<Price>("100"));
  CHECK(metadata.quantize_price(parsed<Price>("100.24"), RoundingMode::Ceiling).value() ==
        parsed<Price>("100.5"));
  CHECK(metadata.quantize_quantity(parsed<Quantity>("1.5"), RoundingMode::Floor).value() ==
        parsed<Quantity>("1"));
}

TEST_CASE("metadata rejects corrupt fields in canonical order", "[model][metadata]") {
  auto params = reference_params();
  params.base_currency = "btc";
  params.tick_size = parsed<Price>("-0.5");
  const auto first_failure = InstrumentMetadata::create(params);
  REQUIRE_FALSE(first_failure);
  CHECK(first_failure.error().code == DomainErrorCode::InvalidMetadata);
  CHECK(first_failure.error().context.field == "instrument.base_currency");

  params = reference_params();
  params.contract_style = static_cast<ContractStyle>(99U);
  const auto bad_style = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_style);
  CHECK(bad_style.error().context.field == "instrument.contract_style");

  params = reference_params();
  params.contract_multiplier_unit = ContractMultiplierUnit::BaseCurrencyPerContract;
  const auto bad_inverse_unit = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_inverse_unit);
  CHECK(bad_inverse_unit.error().context.field == "instrument.contract_multiplier_unit");

  params = reference_params();
  params.price_scale = 0U;
  const auto bad_tick_scale = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_tick_scale);
  CHECK(bad_tick_scale.error().context.field == "instrument.tick_size");

  params = reference_params();
  params.quantity_scale = 1U;
  params.minimum_quantity = parsed<Quantity>("1.5");
  const auto bad_minimum = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_minimum);
  CHECK(bad_minimum.error().context.field == "instrument.minimum_quantity");

  params = reference_params();
  params.settlement_currency = "USD";
  const auto bad_settlement = InstrumentMetadata::create(params);
  REQUIRE_FALSE(bad_settlement);
  CHECK(bad_settlement.error().context.field == "instrument.settlement_currency");
}

TEST_CASE("linear and inverse multiplier units cannot be crossed", "[model][metadata]") {
  auto params = reference_params();
  params.contract_style = ContractStyle::Linear;
  params.contract_multiplier_unit = ContractMultiplierUnit::BaseCurrencyPerContract;
  params.settlement_currency = "USD";
  const auto linear = InstrumentMetadata::create(params);
  REQUIRE(linear);
  CHECK(linear.value().contract_value_currency() == "BTC");

  params.contract_multiplier_unit = ContractMultiplierUnit::QuoteCurrencyPerContract;
  const auto wrong_linear_unit = InstrumentMetadata::create(params);
  REQUIRE_FALSE(wrong_linear_unit);
  CHECK(wrong_linear_unit.error().code == DomainErrorCode::InvalidMetadata);
}

} // namespace
