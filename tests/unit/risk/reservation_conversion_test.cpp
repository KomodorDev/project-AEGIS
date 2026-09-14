// Purpose: independently qualify cumulative reservation conversion, including conservative
// residual rounding, partition independence, malformed retained allocations, and checked failures.

#include "aegis/risk/reservation_conversion.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Reject invalid test identifiers at construction so arithmetic failures cannot hide fixture drift.
template <typename Identifier>
[[nodiscard]] Identifier parse_conversion_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse_identifier(text);
  if (!result) {
    throw std::logic_error{"invalid reservation-conversion identifier"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Author exact signed decimal coefficients without using the production exposure calculation.
template <typename Decimal>
[[nodiscard]] Decimal create_conversion_decimal_or_throw(std::int64_t coefficient,
                                                         std::uint64_t scale = 0U) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid reservation-conversion decimal"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Keep quantity and notional authoring independent; deliberately inconsistent pairs remain usable
// as untrusted calculator inputs in rejection cases.
[[nodiscard]] risk::OrderExposure
create_conversion_exposure_or_throw(std::int64_t quantity, std::int64_t notional,
                                    std::uint64_t quantity_scale = 0U,
                                    std::uint64_t notional_scale = 0U) {
  return risk::OrderExposure{
      create_conversion_decimal_or_throw<model::Quantity>(quantity, quantity_scale),
      create_conversion_decimal_or_throw<model::Notional>(notional, notional_scale)};
}

// --------------------------------------------------------
// Build valid revisioned metadata with independent step and order-entry minimum parameters;
// the optional linear form tests the calculator's narrower supported economic dimension.
[[nodiscard]] model::InstrumentMetadata create_conversion_metadata_or_throw(
    std::int64_t multiplier = 10, std::uint64_t multiplier_scale = 0U,
    std::uint64_t quantity_scale = 0U, std::int64_t step = 1, std::int64_t minimum = 1,
    model::ContractStyle style = model::ContractStyle::Inverse) {
  const bool is_inverse = style == model::ContractStyle::Inverse;
  auto result =
      model::InstrumentMetadata::create_instrument_metadata(model::InstrumentMetadataParams{
          parse_conversion_identifier_or_throw<model::VenueId>("deribit"),
          parse_conversion_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
          parse_conversion_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
          model::InstrumentMetadataRevision::create_initial(),
          "BTC",
          "USD",
          is_inverse ? "BTC" : "USD",
          style,
          model::QuantityUnit::Contracts,
          is_inverse ? model::ContractMultiplierUnit::QuoteCurrencyPerContract
                     : model::ContractMultiplierUnit::BaseCurrencyPerContract,
          1U,
          quantity_scale,
          create_conversion_decimal_or_throw<model::Price>(5, 1U),
          create_conversion_decimal_or_throw<model::Quantity>(step, quantity_scale),
          create_conversion_decimal_or_throw<model::Quantity>(minimum, quantity_scale),
          create_conversion_decimal_or_throw<model::Notional>(multiplier, multiplier_scale),
      });
  if (!result) {
    throw std::logic_error{"invalid reservation-conversion metadata"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Compute a small rational ceiling with native integers; the property grid bounds the product
// below 12000, so this oracle needs neither the fixed-point kernel nor production rounding code.
[[nodiscard]] std::int64_t calculate_small_rational_ceiling(std::int64_t quantity_units,
                                                            std::int64_t multiplier_units,
                                                            std::int64_t target_factor,
                                                            std::int64_t denominator) {
  const auto numerator = quantity_units * multiplier_units * target_factor;
  return numerator / denominator + (numerator % denominator == 0 ? 0 : 1);
}

// --------------------------------------------------------
// Every partition of each small order must reproduce the same independently computed cumulative
// allocation, including rounded zero-notional increments and the final rounding remainder.
TEST_CASE("cumulative reservation conversion is invariant across every small fill partition",
          "[risk][reservation-conversion]") {
  constexpr std::array<std::int64_t, 3U> decimal_factors{1, 10, 100};
  constexpr std::array<std::int64_t, 5U> multipliers{1, 3, 7, 10, 17};
  for (std::uint64_t quantity_scale = 0U; quantity_scale <= 1U; ++quantity_scale) {
    for (const auto multiplier : multipliers) {
      const auto metadata = create_conversion_metadata_or_throw(multiplier, 2U, quantity_scale);
      for (std::uint64_t notional_scale = 0U; notional_scale <= 2U; ++notional_scale) {
        const auto denominator = decimal_factors[quantity_scale] * 100;
        const auto target_factor = decimal_factors[notional_scale];
        for (std::uint32_t total = 1U; total <= 7U; ++total) {
          const auto approved_notional =
              calculate_small_rational_ceiling(total, multiplier, target_factor, denominator);
          const auto original = create_conversion_exposure_or_throw(total, approved_notional,
                                                                    quantity_scale, notional_scale);
          const auto partition_count = std::uint32_t{1U} << (total - 1U);
          for (std::uint32_t partition = 0U; partition < partition_count; ++partition) {
            auto previous = create_conversion_exposure_or_throw(0, 0);
            std::uint32_t previous_quantity = 0U;
            std::int64_t previous_notional = 0;
            for (std::uint32_t cumulative = 1U; cumulative <= total; ++cumulative) {
              if (cumulative < total &&
                  (partition & (std::uint32_t{1U} << (cumulative - 1U))) == 0U) {
                continue;
              }
              CAPTURE(quantity_scale, multiplier, notional_scale, total, partition, cumulative);
              const auto residual_notional = calculate_small_rational_ceiling(
                  total - cumulative, multiplier, target_factor, denominator);
              const auto confirmed_notional = approved_notional - residual_notional;
              const auto result = risk::calculate_cumulative_reservation_conversion(
                  original, previous,
                  create_conversion_decimal_or_throw<model::Quantity>(cumulative, quantity_scale),
                  metadata, notional_scale);
              REQUIRE(result);
              CHECK(result.value() ==
                    risk::CumulativeReservationConversion{
                        create_conversion_exposure_or_throw(total - cumulative, residual_notional,
                                                            quantity_scale, notional_scale),
                        create_conversion_exposure_or_throw(cumulative, confirmed_notional,
                                                            quantity_scale, notional_scale),
                        create_conversion_exposure_or_throw(cumulative - previous_quantity,
                                                            confirmed_notional - previous_notional,
                                                            quantity_scale, notional_scale)});
              previous = result.value().cumulative_confirmed;
              previous_quantity = cumulative;
              previous_notional = confirmed_notional;
            }
            CHECK(previous == original);
          }
        }
      }
    }
  }
}

// --------------------------------------------------------
// Retaining a ceiling-rounded residual can assign zero notional to an actual positive fill;
// rounding each fill separately would over-allocate the original approved quote amount.
TEST_CASE("reservation conversion conserves sub-quantum and fractional face value",
          "[risk][reservation-conversion]") {
  const auto zero = create_conversion_exposure_or_throw(0, 0);

  SECTION("a fractional multiplier transfers only changes in the conservative residual") {
    const auto metadata = create_conversion_metadata_or_throw(4, 1U);
    const auto original = create_conversion_exposure_or_throw(3, 2);
    const auto first = risk::calculate_cumulative_reservation_conversion(
        original, zero, create_conversion_decimal_or_throw<model::Quantity>(1), metadata, 0U);
    REQUIRE(first);
    CHECK(first.value().remaining == create_conversion_exposure_or_throw(2, 1));
    CHECK(first.value().cumulative_confirmed == create_conversion_exposure_or_throw(1, 1));
    CHECK(first.value().newly_confirmed == create_conversion_exposure_or_throw(1, 1));

    const auto second = risk::calculate_cumulative_reservation_conversion(
        original, first.value().cumulative_confirmed,
        create_conversion_decimal_or_throw<model::Quantity>(2), metadata, 0U);
    REQUIRE(second);
    CHECK(second.value().remaining == create_conversion_exposure_or_throw(1, 1));
    CHECK(second.value().cumulative_confirmed == create_conversion_exposure_or_throw(2, 1));
    CHECK(second.value().newly_confirmed == create_conversion_exposure_or_throw(1, 0));

    const auto final = risk::calculate_cumulative_reservation_conversion(
        original, second.value().cumulative_confirmed,
        create_conversion_decimal_or_throw<model::Quantity>(3), metadata, 0U);
    REQUIRE(final);
    CHECK(final.value().remaining == zero);
    CHECK(final.value().cumulative_confirmed == original);
    CHECK(final.value().newly_confirmed == create_conversion_exposure_or_throw(1, 1));
  }

  SECTION("the smallest approved notional remains reserved until the final contract") {
    const auto metadata = create_conversion_metadata_or_throw(1, 2U);
    const auto original = create_conversion_exposure_or_throw(3, 1);
    const auto partial = risk::calculate_cumulative_reservation_conversion(
        original, zero, create_conversion_decimal_or_throw<model::Quantity>(2), metadata, 0U);
    REQUIRE(partial);
    CHECK(partial.value().remaining == create_conversion_exposure_or_throw(1, 1));
    CHECK(partial.value().newly_confirmed == create_conversion_exposure_or_throw(2, 0));

    const auto final = risk::calculate_cumulative_reservation_conversion(
        original, partial.value().cumulative_confirmed,
        create_conversion_decimal_or_throw<model::Quantity>(3), metadata, 0U);
    REQUIRE(final);
    CHECK(final.value().remaining == zero);
    CHECK(final.value().cumulative_confirmed == original);
    CHECK(final.value().newly_confirmed == create_conversion_exposure_or_throw(1, 1));
  }
}

// --------------------------------------------------------
// Order-entry minimums cannot reject aligned fills or residuals below that minimum after an order
// has been approved; both remain meaningful portions of the original reservation.
TEST_CASE("reservation conversion accepts fills and residuals below the order-entry minimum",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw(4, 1U, 0U, 1, 10);
  const auto original = create_conversion_exposure_or_throw(10, 4);
  const auto result = risk::calculate_cumulative_reservation_conversion(
      original, create_conversion_exposure_or_throw(0, 0),
      create_conversion_decimal_or_throw<model::Quantity>(1), metadata, 0U);
  REQUIRE(result);
  CHECK(result.value().remaining == create_conversion_exposure_or_throw(9, 4));
  CHECK(result.value().cumulative_confirmed == create_conversion_exposure_or_throw(1, 0));
  CHECK(result.value().newly_confirmed == create_conversion_exposure_or_throw(1, 0));
  CHECK(result.value().remaining.quantity < metadata.minimum_quantity());
  CHECK(result.value().newly_confirmed.quantity < metadata.minimum_quantity());
}

// --------------------------------------------------------
// Zero, repeated, and full cumulative endpoints are arithmetic identities; source execution
// identity and admissibility remain decisions for the private-event owner.
TEST_CASE("reservation conversion handles zero equal and fully filled cumulative endpoints",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw();
  const auto original = create_conversion_exposure_or_throw(5, 50);
  const auto zero = create_conversion_exposure_or_throw(0, 0);
  auto previous = zero;
  constexpr std::array<std::int64_t, 5U> endpoints{0, 2, 2, 5, 5};
  constexpr std::array<std::int64_t, 5U> increments{0, 2, 0, 3, 0};
  for (std::size_t index = 0U; index < endpoints.size(); ++index) {
    const auto cumulative = endpoints[index];
    CAPTURE(index, cumulative);
    const auto result = risk::calculate_cumulative_reservation_conversion(
        original, previous, create_conversion_decimal_or_throw<model::Quantity>(cumulative),
        metadata, 0U);
    REQUIRE(result);
    CHECK(result.value().remaining ==
          create_conversion_exposure_or_throw(5 - cumulative, (5 - cumulative) * 10));
    CHECK(result.value().cumulative_confirmed ==
          create_conversion_exposure_or_throw(cumulative, cumulative * 10));
    CHECK(result.value().newly_confirmed ==
          create_conversion_exposure_or_throw(increments[index], increments[index] * 10));
    previous = result.value().cumulative_confirmed;
  }
  CHECK(previous == original);
}

// --------------------------------------------------------
// The calculator cannot trust a retained allocation merely because its quantity is plausible;
// too little, too much, and impossible endpoint notional must all fail before returning a plan.
TEST_CASE("reservation conversion rejects inconsistent prior cumulative allocations",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw();
  const auto original = create_conversion_exposure_or_throw(5, 50);
  const std::array invalid_previous{
      create_conversion_exposure_or_throw(1, 9),
      create_conversion_exposure_or_throw(1, 11),
      create_conversion_exposure_or_throw(0, 1),
      create_conversion_exposure_or_throw(5, 49),
      create_conversion_exposure_or_throw(1, 101, 0U, 1U),
  };
  for (const auto& previous : invalid_previous) {
    const auto saved_original = original;
    const auto saved_previous = previous;
    const auto result = risk::calculate_cumulative_reservation_conversion(
        original, previous, create_conversion_decimal_or_throw<model::Quantity>(5), metadata, 0U);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidReservationConversion);
    CHECK(result.error().context.field == "reservation_conversion.previous");
    CHECK(original == saved_original);
    CHECK(previous == saved_previous);
  }
}

// --------------------------------------------------------
// Original approval economics must match the once-rounded metadata value before any residual
// allocation is derived, even when an altered notional is still positive.
TEST_CASE("reservation conversion rejects incorrect original approval notional",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw();
  for (const std::int64_t notional : {49, 51}) {
    const auto result = risk::calculate_cumulative_reservation_conversion(
        create_conversion_exposure_or_throw(5, notional), create_conversion_exposure_or_throw(0, 0),
        create_conversion_decimal_or_throw<model::Quantity>(1), metadata, 0U);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidReservationConversion);
    CHECK(result.error().context.field == "reservation_conversion.original");
  }
}

// --------------------------------------------------------
// A positive reservation and monotonic nonnegative cumulative quantities are preconditions;
// neither a decreasing observation nor an overfill may manufacture a negative transfer.
TEST_CASE("reservation conversion rejects negative decreasing and overfilled cumulative state",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw();
  auto original = create_conversion_exposure_or_throw(5, 50);
  auto previous = create_conversion_exposure_or_throw(0, 0);
  auto next = create_conversion_decimal_or_throw<model::Quantity>(1);

  SECTION("zero original quantity") { original.quantity = previous.quantity; }
  SECTION("negative original quantity") {
    original.quantity = create_conversion_decimal_or_throw<model::Quantity>(-5);
  }
  SECTION("zero original notional") { original.quote_notional = previous.quote_notional; }
  SECTION("negative original notional") {
    original.quote_notional = create_conversion_decimal_or_throw<model::Notional>(-50);
  }
  SECTION("negative previous quantity") {
    previous.quantity = create_conversion_decimal_or_throw<model::Quantity>(-1);
  }
  SECTION("negative previous notional") {
    previous.quote_notional = create_conversion_decimal_or_throw<model::Notional>(-1);
  }
  SECTION("negative next quantity") {
    next = create_conversion_decimal_or_throw<model::Quantity>(-1);
  }
  SECTION("decreasing cumulative quantity") {
    previous = create_conversion_exposure_or_throw(2, 20);
  }
  SECTION("next quantity exceeds original") {
    next = create_conversion_decimal_or_throw<model::Quantity>(6);
  }
  SECTION("previous and next quantity exceed original") {
    previous = create_conversion_exposure_or_throw(6, 60);
    next = previous.quantity;
  }

  const auto result =
      risk::calculate_cumulative_reservation_conversion(original, previous, next, metadata, 0U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidReservationConversion);
  CHECK(result.error().context.field == "reservation_conversion.cumulative");
}

// --------------------------------------------------------
// Valid linear metadata remains unsupported by the inverse quote-face calculator; metadata
// validity alone does not authorize a conversion across different economic dimensions.
TEST_CASE("reservation conversion rejects unsupported metadata dimensions",
          "[risk][reservation-conversion]") {
  const auto metadata =
      create_conversion_metadata_or_throw(10, 0U, 0U, 1, 1, model::ContractStyle::Linear);
  const auto result = risk::calculate_cumulative_reservation_conversion(
      create_conversion_exposure_or_throw(5, 50), create_conversion_exposure_or_throw(0, 0),
      create_conversion_decimal_or_throw<model::Quantity>(1), metadata, 0U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidReservationConversion);
  CHECK(result.error().context.field == "reservation_conversion.metadata");
}

// --------------------------------------------------------
// Authored output scales stay wide until checked, and each quantity's precision must fit the
// metadata declaration even when another quantity would otherwise trigger an earlier bound error.
TEST_CASE("reservation conversion rejects invalid output and quantity scales",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw();
  auto original = create_conversion_exposure_or_throw(5, 50);
  auto previous = create_conversion_exposure_or_throw(0, 0);
  auto next = create_conversion_decimal_or_throw<model::Quantity>(1);
  std::uint64_t notional_scale = 0U;

  SECTION("scale just beyond decimal support") {
    notional_scale = static_cast<std::uint64_t>(model::FixedPoint::maximum_scale) + 1U;
  }
  SECTION("scale must not narrow to zero") { notional_scale = 256U; }
  SECTION("maximum authored scale") { notional_scale = std::numeric_limits<std::uint64_t>::max(); }
  SECTION("original quantity precision") {
    original.quantity = create_conversion_decimal_or_throw<model::Quantity>(51, 1U);
  }
  SECTION("previous quantity precision") {
    previous.quantity = create_conversion_decimal_or_throw<model::Quantity>(1, 1U);
  }
  SECTION("next quantity precision") {
    next = create_conversion_decimal_or_throw<model::Quantity>(1, 1U);
  }

  const auto result = risk::calculate_cumulative_reservation_conversion(original, previous, next,
                                                                        metadata, notional_scale);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidScale);
  CHECK(result.error().context.field == "reservation_conversion.scale");
}

// --------------------------------------------------------
// Every quantity participates in metadata-owned step validation; a matching decimal precision
// cannot excuse an off-step original, retained endpoint, or new endpoint.
TEST_CASE("reservation conversion preserves quantity alignment failures",
          "[risk][reservation-conversion]") {
  const auto metadata = create_conversion_metadata_or_throw(10, 0U, 0U, 2, 2);
  auto original = create_conversion_exposure_or_throw(6, 60);
  auto previous = create_conversion_exposure_or_throw(0, 0);
  auto next = create_conversion_decimal_or_throw<model::Quantity>(4);

  SECTION("misaligned original quantity") { original = create_conversion_exposure_or_throw(5, 50); }
  SECTION("misaligned previous quantity") { previous = create_conversion_exposure_or_throw(1, 10); }
  SECTION("misaligned next quantity") {
    next = create_conversion_decimal_or_throw<model::Quantity>(3);
  }

  const auto result =
      risk::calculate_cumulative_reservation_conversion(original, previous, next, metadata, 0U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::MisalignedQuantity);
  CHECK(result.error().context.field == "quantity");
}

// --------------------------------------------------------
// Both original contract-value overflow and an unrepresentable fractional residual must preserve
// arithmetic failure rather than wrap, clamp, or publish a partial conversion.
TEST_CASE("reservation conversion preserves multiplication and residual subtraction overflow",
          "[risk][reservation-conversion]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  auto metadata = create_conversion_metadata_or_throw(2);
  auto original = create_conversion_exposure_or_throw(maximum, maximum);
  auto next = create_conversion_decimal_or_throw<model::Quantity>(1);

  SECTION("original notional multiplication cannot fit") {}
  SECTION("aligned fractional residual cannot fit its signed coefficient") {
    metadata = create_conversion_metadata_or_throw(1, 1U, 1U);
    original = create_conversion_exposure_or_throw(maximum, 922337203685477581LL);
    next = create_conversion_decimal_or_throw<model::Quantity>(1, 1U);
  }

  const auto result = risk::calculate_cumulative_reservation_conversion(
      original, create_conversion_exposure_or_throw(0, 0), next, metadata, 0U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::ArithmeticOverflow);
}

// --------------------------------------------------------

} // namespace
