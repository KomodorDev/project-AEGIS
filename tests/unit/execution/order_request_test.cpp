// Purpose: prove the M3 request/result vocabulary, exact canonical validation order, and absence of
// caller-forgeable attribution or acknowledgement semantics.

#include "aegis/execution/order_validation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid decimal fixture text is a test bug rather than a production branch under examination.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view text) {
  auto parsed = Decimal::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid decimal test fixture"};
  }
  return parsed.value();
}

// --------------------------------------------------------
// Invalid identifier fixture text is likewise a test construction defect.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier test fixture"};
  }
  return parsed.value();
}

// --------------------------------------------------------
// Build accepted inverse metadata whose scale, tick, minimum, and step expose every M3 filter.
[[nodiscard]] model::InstrumentMetadata create_reference_metadata_or_throw() {
  auto created =
      model::InstrumentMetadata::create_instrument_metadata(model::InstrumentMetadataParams{
          parse_identifier_or_throw<model::VenueId>("deribit"),
          parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
          parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
          model::InstrumentMetadataRevision::create_initial(), "BTC", "USD", "BTC",
          model::ContractStyle::Inverse, model::QuantityUnit::Contracts,
          model::ContractMultiplierUnit::QuoteCurrencyPerContract, 1U, 0U,
          parse_decimal_or_throw<model::Price>("0.5"),
          parse_decimal_or_throw<model::Quantity>("10"),
          parse_decimal_or_throw<model::Quantity>("10"),
          parse_decimal_or_throw<model::Notional>("10")});
  if (!created) {
    throw std::logic_error{"invalid metadata test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build one canonical request that later cases mutate one field at a time.
[[nodiscard]] execution::OrderRequest create_reference_request_or_throw() {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-primary"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      parse_decimal_or_throw<model::Price>("60000"),
      parse_decimal_or_throw<model::Quantity>("20")};
}

// --------------------------------------------------------
// Assigned numeric values and aggregate shape are compatibility contracts, not implementation
// conveniences.
TEST_CASE("M3 order and result vocabularies retain their assigned local-only values",
          "[execution][submission]") {
  static_assert(static_cast<std::uint8_t>(execution::OrderSide::Buy) == 1U);
  static_assert(static_cast<std::uint8_t>(execution::OrderSide::Sell) == 2U);
  static_assert(static_cast<std::uint8_t>(execution::OrderType::Limit) == 1U);
  static_assert(static_cast<std::uint8_t>(execution::TimeInForce::GoodTilCancelled) == 1U);
  static_assert(static_cast<std::uint8_t>(execution::SubmitDisposition::LocallyRejected) == 1U);
  static_assert(static_cast<std::uint8_t>(execution::SubmitDisposition::WriteInitiated) == 2U);
  static_assert(static_cast<std::uint8_t>(execution::SubmitDisposition::SubmissionUnknown) == 3U);
  static_assert(std::is_aggregate_v<execution::OrderRequest>);
  static_assert(!std::is_default_constructible_v<execution::SubmitResult>);

  const auto rejected = execution::SubmitResult::create_locally_rejected_result(
      execution::SubmissionStage::Route, execution::SubmissionReason::RouteNotFound);
  CHECK(rejected.disposition() == execution::SubmitDisposition::LocallyRejected);
  CHECK_FALSE(rejected.attempt_id());
  CHECK_FALSE(rejected.order_id());
}

// --------------------------------------------------------
// The first invalid field in the accepted ADR order must win without rounding later fields.
TEST_CASE("canonical limit validation has stable first-failure precedence",
          "[execution][submission]") {
  const auto instrument = create_reference_metadata_or_throw();
  auto candidate = create_reference_request_or_throw();

  candidate.side = static_cast<execution::OrderSide>(0U);
  candidate.price = parse_decimal_or_throw<model::Price>("-1");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::UnsupportedSide);

  candidate = create_reference_request_or_throw();
  candidate.price = parse_decimal_or_throw<model::Price>("-1");
  candidate.quantity = parse_decimal_or_throw<model::Quantity>("1");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::PriceNotPositive);

  candidate = create_reference_request_or_throw();
  candidate.price = parse_decimal_or_throw<model::Price>("60000.25");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::PriceScaleExceeded);

  candidate = create_reference_request_or_throw();
  candidate.price = parse_decimal_or_throw<model::Price>("60000.1");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::PriceTickMismatch);

  candidate = create_reference_request_or_throw();
  candidate.quantity = parse_decimal_or_throw<model::Quantity>("1");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::QuantityBelowMinimum);

  candidate = create_reference_request_or_throw();
  candidate.quantity = parse_decimal_or_throw<model::Quantity>("15");
  CHECK(execution::validate_canonical_order(candidate, instrument).reason ==
        execution::SubmissionReason::QuantityStepMismatch);
}

// --------------------------------------------------------
// Accepted validation returns the exact caller-authored economics, including canonicalized scale.
TEST_CASE("canonical validation preserves exact approved economics", "[execution][submission]") {
  const auto candidate = create_reference_request_or_throw();
  const auto decision =
      execution::validate_canonical_order(candidate, create_reference_metadata_or_throw());

  REQUIRE(decision.is_accepted());
  REQUIRE(decision.economics);
  CHECK(decision.reason == execution::SubmissionReason::None);
  CHECK(decision.economics->price == candidate.price);
  CHECK(decision.economics->quantity == candidate.quantity);
  CHECK(decision.economics->price.scale() == 0U);
}

// --------------------------------------------------------

} // namespace
