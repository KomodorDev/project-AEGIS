// Purpose: prove inverse M3 order exposure uses exact quantity-times-quote-multiplier economics,
// rounds away from zero without understatement, ignores limit price, and rejects overflow.

#include "aegis/risk/exposure.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid fixture identifiers are test-authoring defects rather than exposure outcomes.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid exposure-test identifier"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Invalid fixture decimals fail immediately so tests exercise only risk conversion behavior.
template <typename Decimal> [[nodiscard]] Decimal decimal(std::string_view value) {
  auto parsed = Decimal::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid exposure-test decimal"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build accepted inverse metadata with caller-selected exact multiplier and quantity increment.
[[nodiscard]] model::InstrumentMetadata inverse_metadata(model::Notional multiplier,
                                                         model::Quantity quantity_step) {
  auto created = model::InstrumentMetadata::create(model::InstrumentMetadataParams{
      id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::initial(),
      "BTC",
      "USD",
      "BTC",
      model::ContractStyle::Inverse,
      model::QuantityUnit::Contracts,
      model::ContractMultiplierUnit::QuoteCurrencyPerContract,
      1U,
      quantity_step.scale(),
      decimal<model::Price>("0.5"),
      quantity_step,
      quantity_step,
      multiplier,
  });
  if (!created) {
    throw std::logic_error{"invalid inverse exposure metadata"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Construct canonical limit/GTC economics while allowing price-independence comparisons.
[[nodiscard]] execution::CanonicalOrderEconomics economics(model::Price price,
                                                           model::Quantity quantity) {
  return execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                            execution::TimeInForce::GoodTilCancelled, price,
                                            quantity};
}

// --------------------------------------------------------

// --------------------------------------------------------
// An independent integer ceiling grid proves AwayFromZero never understates positive exact value.
TEST_CASE("inverse quote face exposure is never understated on an independent property grid",
          "[risk][exposure][property][m3]") {
  const auto metadata =
      inverse_metadata(decimal<model::Notional>("0.333"), decimal<model::Quantity>("0.001"));
  const auto price = decimal<model::Price>("12345.5");

  for (std::int64_t quantity_coefficient = 1; quantity_coefficient <= 500; ++quantity_coefficient) {
    const auto quantity = model::Quantity::from_scaled(quantity_coefficient, 3U).value();
    const auto result = risk::calculate_order_exposure(economics(price, quantity), metadata, 2U);
    REQUIRE(result);

    // ++++++++++++++++++++++++++++++++++++++++
    // q*333 has scale six; scale two AwayFromZero is the independent positive integer ceiling.
    const auto exact_product_coefficient = quantity_coefficient * 333;
    const auto expected_coefficient = (exact_product_coefficient + 9'999) / 10'000;
    const auto expected = model::Notional::from_scaled(expected_coefficient, 2U).value();
    CHECK(result.value().quantity == quantity);
    CHECK(result.value().quote_notional == expected);
    CHECK(result.value().quote_notional.coefficient() > 0);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Inverse face value depends on quantity and multiplier only; changing limit price cannot change
// it.
TEST_CASE("inverse exposure preserves approved quantity and never multiplies by limit price",
          "[risk][exposure][m3]") {
  const auto metadata =
      inverse_metadata(decimal<model::Notional>("10"), decimal<model::Quantity>("1"));
  const auto quantity = decimal<model::Quantity>("7");
  const auto low =
      risk::calculate_order_exposure(economics(decimal<model::Price>("1"), quantity), metadata, 2U);
  const auto high = risk::calculate_order_exposure(
      economics(decimal<model::Price>("999999.5"), quantity), metadata, 2U);

  REQUIRE(low);
  REQUIRE(high);
  CHECK(low.value() == high.value());
  CHECK(low.value().order_quantity() == quantity);
  CHECK(low.value().quote_face_notional() == decimal<model::Notional>("70"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Unrepresentable multiplication fails instead of wrapping, saturating, or authorizing less risk.
TEST_CASE("inverse exposure rejects arithmetic overflow without a partial value",
          "[risk][exposure][overflow][m3]") {
  const auto metadata = inverse_metadata(
      model::Notional::from_scaled(std::numeric_limits<std::int64_t>::max(), 0U).value(),
      decimal<model::Quantity>("1"));
  const auto quantity =
      model::Quantity::from_scaled(std::numeric_limits<std::int64_t>::max(), 0U).value();
  const auto result = risk::calculate_order_exposure(
      economics(decimal<model::Price>("1"), quantity), metadata, 18U);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::ArithmeticOverflow);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
