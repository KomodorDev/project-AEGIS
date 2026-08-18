// Purpose: prove exact decimal parsing, canonical storage, checked arithmetic, and rounding policy.

#include "aegis/model/fixed_point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {

using namespace aegis::model;

template <typename Left, typename Right>
concept HasProductOperator = requires(Left left, Right right) { left* right; };

static_assert(!std::is_same_v<Price, Quantity>);
static_assert(!std::is_same_v<Quantity, Notional>);
static_assert(!std::is_convertible_v<Price, Quantity>);
static_assert(!std::is_constructible_v<Price, double>);
static_assert(!std::is_constructible_v<Quantity, float>);
static_assert(!HasProductOperator<Price, Quantity>);

[[nodiscard]] FixedPoint decimal(std::string_view text) {
  auto result = FixedPoint::parse_ascii(text);
  REQUIRE(result);
  return result.value();
}

TEST_CASE("decimal parsing is strict and storage is canonical", "[model][fixed-point]") {
  const auto value = decimal("0012.3400");
  CHECK(value.coefficient() == 1234);
  CHECK(value.scale() == 2U);
  CHECK(value.to_string() == "12.34");

  const auto zero = decimal("-0.000");
  CHECK(zero.coefficient() == 0);
  CHECK(zero.scale() == 0U);
  CHECK(zero.to_string() == "0");

  CHECK(decimal("1.0") == decimal("1"));
  CHECK(decimal("-0.10") < decimal("0"));
}

TEST_CASE("decimal parsing rejects non-ordinary notation and representation overflow",
          "[model][fixed-point]") {
  for (const auto text : {"", "-", ".1", "1.", "+1", "1e2", "NaN", " 1", "1,2"}) {
    const auto result = FixedPoint::parse_ascii(text);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidDecimal);
  }

  const auto excessive_scale = FixedPoint::parse_ascii("0.0000000000000000001");
  REQUIRE_FALSE(excessive_scale);
  CHECK(excessive_scale.error().code == DomainErrorCode::InvalidScale);

  CHECK(decimal("9223372036854775807").coefficient() == std::numeric_limits<std::int64_t>::max());
  CHECK(decimal("-9223372036854775808").coefficient() == std::numeric_limits<std::int64_t>::min());

  for (const auto text : {"9223372036854775808", "-9223372036854775809"}) {
    const auto result = FixedPoint::parse_ascii(text);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::ArithmeticOverflow);
  }
  const auto invalid_scale = FixedPoint::from_scaled(1, 19U);
  REQUIRE_FALSE(invalid_scale);
  CHECK(invalid_scale.error().code == DomainErrorCode::InvalidScale);
}

TEST_CASE("addition and subtraction preserve exact cross-scale values and fail on overflow",
          "[model][fixed-point]") {
  const auto sum = decimal("1.2").checked_add(decimal("0.03"));
  REQUIRE(sum);
  CHECK(sum.value() == decimal("1.23"));

  const auto difference = decimal("1.2").checked_subtract(decimal("2.03"));
  REQUIRE(difference);
  CHECK(difference.value() == decimal("-0.83"));

  const auto add_overflow = decimal("9223372036854775807").checked_add(decimal("1"));
  REQUIRE_FALSE(add_overflow);
  CHECK(add_overflow.error().code == DomainErrorCode::ArithmeticOverflow);

  const auto subtract_overflow = decimal("-9223372036854775808").checked_subtract(decimal("1"));
  REQUIRE_FALSE(subtract_overflow);
  CHECK(subtract_overflow.error().code == DomainErrorCode::ArithmeticOverflow);
}

TEST_CASE("rescaling requires an explicit policy and handles every signed direction",
          "[model][fixed-point]") {
  const auto exact = decimal("1.25").rescale(1U, RoundingMode::Exact);
  REQUIRE_FALSE(exact);
  CHECK(exact.error().code == DomainErrorCode::PrecisionLoss);

  CHECK(decimal("1.25").rescale(1U, RoundingMode::TowardZero).value() == decimal("1.2"));
  CHECK(decimal("1.25").rescale(1U, RoundingMode::AwayFromZero).value() == decimal("1.3"));
  CHECK(decimal("1.25").rescale(1U, RoundingMode::Floor).value() == decimal("1.2"));
  CHECK(decimal("1.25").rescale(1U, RoundingMode::Ceiling).value() == decimal("1.3"));
  CHECK(decimal("1.25").rescale(1U, RoundingMode::NearestTiesToEven).value() == decimal("1.2"));
  CHECK(decimal("1.35").rescale(1U, RoundingMode::NearestTiesToEven).value() == decimal("1.4"));

  CHECK(decimal("-1.25").rescale(1U, RoundingMode::TowardZero).value() == decimal("-1.2"));
  CHECK(decimal("-1.25").rescale(1U, RoundingMode::AwayFromZero).value() == decimal("-1.3"));
  CHECK(decimal("-1.25").rescale(1U, RoundingMode::Floor).value() == decimal("-1.3"));
  CHECK(decimal("-1.25").rescale(1U, RoundingMode::Ceiling).value() == decimal("-1.2"));
  CHECK(decimal("-1.25").rescale(1U, RoundingMode::NearestTiesToEven).value() == decimal("-1.2"));
  CHECK(decimal("-1.35").rescale(1U, RoundingMode::NearestTiesToEven).value() == decimal("-1.4"));
}

TEST_CASE("multiplication and division are target-scaled and checked", "[model][fixed-point]") {
  const auto product = decimal("0.2").multiply(decimal("0.5"), 1U, RoundingMode::Exact);
  REQUIRE(product);
  CHECK(product.value() == decimal("0.1"));

  const auto product_overflow =
      decimal("9223372036854775807").multiply(decimal("2"), 0U, RoundingMode::Exact);
  REQUIRE_FALSE(product_overflow);
  CHECK(product_overflow.error().code == DomainErrorCode::ArithmeticOverflow);

  const auto quotient = decimal("1").divide(decimal("8"), 3U, RoundingMode::Exact);
  REQUIRE(quotient);
  CHECK(quotient.value() == decimal("0.125"));

  const auto third_exact = decimal("1").divide(decimal("3"), 2U, RoundingMode::Exact);
  REQUIRE_FALSE(third_exact);
  CHECK(third_exact.error().code == DomainErrorCode::PrecisionLoss);
  CHECK(decimal("1").divide(decimal("3"), 2U, RoundingMode::TowardZero).value() == decimal("0.33"));
  CHECK(decimal("-1").divide(decimal("3"), 2U, RoundingMode::Floor).value() == decimal("-0.34"));
  CHECK(decimal("-1").divide(decimal("3"), 2U, RoundingMode::Ceiling).value() == decimal("-0.33"));

  const auto division_by_zero = decimal("1").divide(decimal("0"), 2U, RoundingMode::Exact);
  REQUIRE_FALSE(division_by_zero);
  CHECK(division_by_zero.error().code == DomainErrorCode::DivisionByZero);
}

TEST_CASE("multiple tests and quantization are exact across different scales",
          "[model][fixed-point]") {
  CHECK(decimal("1.2").is_multiple_of(decimal("0.3")).value());
  CHECK(decimal("0.5").is_multiple_of(decimal("0.25")).value());
  CHECK_FALSE(decimal("0.6").is_multiple_of(decimal("0.25")).value());

  CHECK(decimal("1.24").quantize(decimal("0.05"), RoundingMode::Floor).value() == decimal("1.2"));
  CHECK(decimal("1.24").quantize(decimal("0.05"), RoundingMode::Ceiling).value() ==
        decimal("1.25"));
  CHECK(decimal("-1.24").quantize(decimal("0.05"), RoundingMode::Floor).value() ==
        decimal("-1.25"));
  CHECK(decimal("-1.24").quantize(decimal("0.05"), RoundingMode::Ceiling).value() ==
        decimal("-1.2"));
}

TEST_CASE("unassigned rounding modes fail instead of selecting an implicit policy",
          "[model][fixed-point]") {
  const auto invalid = static_cast<RoundingMode>(255U);

  for (const auto& result : {decimal("1.25").rescale(1U, invalid),
                             decimal("1").multiply(decimal("2"), 0U, invalid),
                             decimal("1").divide(decimal("2"), 1U, invalid),
                             decimal("1.25").quantize(decimal("0.5"), invalid)}) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidValue);
    CHECK(result.error().context.field == "rounding_mode");
  }
}

} // namespace
