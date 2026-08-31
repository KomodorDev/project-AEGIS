// Purpose: prove exact decimal parsing, canonical storage, checked arithmetic, and rounding policy.

#include "aegis/model/fixed_point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

namespace {

using namespace aegis::model;

// ########################################################################
// Interesting syntax: this requires-expression proves that nominal values expose no generic product
// operator, without adding an intentionally ill-formed expression to the test translation unit.
template <typename Left, typename Right>
concept HasProductOperator = requires(Left left, Right right) { left* right; };

// Interesting syntax: requires-expression probes verify that constraints and deleted overloads
// remove forbidden types at overload resolution. Supported narrow-integer runtime cases below also
// instantiate function bodies, guarding against a constraint/body mismatch around std::in_range.
template <typename Value, typename Coefficient, typename Scale>
concept HasFromScaled =
    requires(Coefficient coefficient, Scale scale) { Value::from_scaled(coefficient, scale); };

template <typename Value, typename Scale>
concept HasRescale =
    requires(Value value, Scale scale) { value.rescale(scale, RoundingMode::Exact); };

// Multiplication and division have independent scale-taking overload sets, so probe both rather
// than assuming the rescale constraint protects every precision-changing entry point.
template <typename Scale>
concept HasScaledMultiply =
    requires(FixedPoint value, Scale scale) { value.multiply(value, scale, RoundingMode::Exact); };

template <typename Scale>
concept HasScaledDivide =
    requires(FixedPoint value, Scale scale) { value.divide(value, scale, RoundingMode::Exact); };

// ########################################################################
// Model a legacy unscoped enum that would otherwise convert implicitly to an integer scale.
enum LegacyScale { LegacyScaleZero = 0 };

// ########################################################################
// Price, Quantity, and Notional are compile-time domains rather than aliases of one interchangeable
// decimal type.
static_assert(!std::is_same_v<Price, Quantity>);
static_assert(!std::is_same_v<Quantity, Notional>);
static_assert(!std::is_convertible_v<Price, Quantity>);

// Binary floating-point values must be non-invocable at both kernel and nominal construction and
// rescaling boundaries.
static_assert(!std::is_constructible_v<Price, double>);
static_assert(!std::is_constructible_v<Quantity, float>);
static_assert(!HasFromScaled<FixedPoint, double, int>);
static_assert(!HasFromScaled<FixedPoint, int, double>);
static_assert(!HasFromScaled<Price, double, int>);
static_assert(!HasFromScaled<Quantity, int, float>);
static_assert(!HasFromScaled<Notional, double, double>);
static_assert(!HasFromScaled<Price, long double, int>);

// Bool, enum, and plain/wide/Unicode character sources are non-invocable; signed char, unsigned
// char, and wider standard integers remain callable so their original ranges can be checked before
// representation narrowing.
static_assert(!HasFromScaled<FixedPoint, bool, int>);
static_assert(!HasFromScaled<Price, bool, int>);
static_assert(!HasFromScaled<FixedPoint, char, int>);
static_assert(!HasFromScaled<Price, int, char>);
static_assert(!HasFromScaled<FixedPoint, int, bool>);
static_assert(!HasFromScaled<Price, int, LegacyScale>);
static_assert(HasFromScaled<FixedPoint, int, int>);
static_assert(HasFromScaled<FixedPoint, std::uint64_t, int>);
static_assert(HasFromScaled<Price, std::uint64_t, int>);
static_assert(!HasRescale<FixedPoint, double>);
static_assert(!HasRescale<Price, long double>);
static_assert(!HasRescale<FixedPoint, bool>);
static_assert(!HasRescale<Price, LegacyScale>);
static_assert(!HasRescale<FixedPoint, char>);

// Product and quotient target scales independently enforce the same non-domain-input firewall used
// by construction and rescaling, rather than accepting implicit bool, enum, or float conversions.
static_assert(!HasScaledMultiply<float>);
static_assert(!HasScaledDivide<long double>);
static_assert(!HasScaledMultiply<bool>);
static_assert(!HasScaledDivide<LegacyScale>);
static_assert(!HasScaledMultiply<char>);
static_assert(!HasScaledDivide<wchar_t>);
static_assert(!HasProductOperator<Price, Quantity>);

// ########################################################################

// --------------------------------------------------------
// Parse every fixture literal through the production path so malformed test data fails immediately.
[[nodiscard]] FixedPoint parse_fixed_point_or_throw(std::string_view text) {
  auto result = FixedPoint::parse_ascii(text);
  REQUIRE(result);
  return result.value();
}

// --------------------------------------------------------
// Canonical storage removes representational differences without changing exact numeric ordering.
TEST_CASE("decimal parsing is strict and storage is canonical", "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonicalize redundant leading and fractional zeros while retaining the exact value.
  const auto value = parse_fixed_point_or_throw("0012.3400");
  CHECK(value.coefficient() == 1234);
  CHECK(value.scale() == 2U);
  CHECK(value.to_string() == "12.34");

  // ++++++++++++++++++++++++++++++++++++++++
  // Collapse every signed zero representation to the unique scale-zero value.
  const auto zero = parse_fixed_point_or_throw("-0.000");
  CHECK(zero.coefficient() == 0);
  CHECK(zero.scale() == 0U);
  CHECK(zero.to_string() == "0");

  // ++++++++++++++++++++++++++++++++++++++++
  // Numeric equality and ordering ignore representational differences after canonicalization.
  CHECK(parse_fixed_point_or_throw("1.0") == parse_fixed_point_or_throw("1"));
  CHECK(parse_fixed_point_or_throw("-0.10") < parse_fixed_point_or_throw("0"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The parser accepts only ordinary decimal notation and distinguishes syntax, scale, and range
// errors.
TEST_CASE("decimal parsing rejects non-ordinary notation and representation overflow",
          "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject empty, incomplete, signed-plus, exponent, locale, and whitespace notation uniformly.
  for (const auto text : {"", "-", ".1", "1.", "+1", "1e2", "NaN", " 1", "1,2"}) {
    const auto result = FixedPoint::parse_ascii(text);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidDecimal);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Distinguish excessive authored scale from general decimal syntax failure.
  const auto excessive_scale = FixedPoint::parse_ascii("0.0000000000000000001");
  REQUIRE_FALSE(excessive_scale);
  CHECK(excessive_scale.error().code == DomainErrorCode::InvalidScale);

  const auto excessive_textual_scale = FixedPoint::parse_ascii("1.0000000000000000000");
  REQUIRE_FALSE(excessive_textual_scale);
  CHECK(excessive_textual_scale.error().code == DomainErrorCode::InvalidScale);

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept both signed coefficient limits exactly.
  CHECK(parse_fixed_point_or_throw("9223372036854775807").coefficient() ==
        std::numeric_limits<std::int64_t>::max());
  CHECK(parse_fixed_point_or_throw("-9223372036854775808").coefficient() ==
        std::numeric_limits<std::int64_t>::min());

  // Redundant fractional zeros at either signed limit must canonicalize before range evaluation.
  CHECK(parse_fixed_point_or_throw("9223372036854775807.0") ==
        parse_fixed_point_or_throw("9223372036854775807"));
  CHECK(parse_fixed_point_or_throw("-9223372036854775808.000") ==
        parse_fixed_point_or_throw("-9223372036854775808"));

  // Canonicalization must also preserve boundary coefficients when significant fractional digits
  // remain after the redundant zero is removed.
  CHECK(parse_fixed_point_or_throw("922337203685477580.70") ==
        parse_fixed_point_or_throw("922337203685477580.7"));
  CHECK(parse_fixed_point_or_throw("-922337203685477580.80") ==
        parse_fixed_point_or_throw("-922337203685477580.8"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject coefficients immediately outside the signed representation range.
  for (const auto text : {"9223372036854775808", "-9223372036854775809"}) {
    const auto result = FixedPoint::parse_ascii(text);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::ArithmeticOverflow);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate scale inputs at their authored sign and width before stored-scale narrowing.
  const auto invalid_scale = FixedPoint::from_scaled(1, 19U);
  REQUIRE_FALSE(invalid_scale);
  CHECK(invalid_scale.error().code == DomainErrorCode::InvalidScale);

  // Preserve the signed scale until validation so -1 reports InvalidScale instead of unsigned wrap.
  const auto negative_scale = FixedPoint::from_scaled(1, -1);
  REQUIRE_FALSE(negative_scale);
  CHECK(negative_scale.error().code == DomainErrorCode::InvalidScale);

  // Wide scale input must be rejected at its original width rather than wrapping through uint8_t.
  const auto formerly_wrapped_scale = FixedPoint::from_scaled(1, std::uint64_t{256U});
  REQUIRE_FALSE(formerly_wrapped_scale);
  CHECK(formerly_wrapped_scale.error().code == DomainErrorCode::InvalidScale);

  // ++++++++++++++++++++++++++++++++++++++++
  // UINT64_MAX guards the former unsafe coefficient narrowing while the adjacent signed maximum
  // proves a wide source is accepted when its value is representable.
  const auto maximum_unsigned_coefficient =
      FixedPoint::from_scaled(std::numeric_limits<std::uint64_t>::max(), 0U);
  REQUIRE_FALSE(maximum_unsigned_coefficient);
  CHECK(maximum_unsigned_coefficient.error() ==
        DomainError::create_at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));

  const auto maximum_signed_as_unsigned = FixedPoint::from_scaled(
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), 0U);
  REQUIRE(maximum_signed_as_unsigned);
  CHECK(maximum_signed_as_unsigned.value().coefficient() ==
        std::numeric_limits<std::int64_t>::max());

  // ++++++++++++++++++++++++++++++++++++++++
  // Nominal wrappers must preserve the same overflow code while replacing the generic kernel field.
  const auto invalid_price = Price::from_scaled(std::numeric_limits<std::uint64_t>::max(), 0U);
  REQUIRE_FALSE(invalid_price);
  CHECK(invalid_price.error() ==
        DomainError::create_at_field(DomainErrorCode::ArithmeticOverflow, "price"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Instantiating both narrow signed and unsigned source types proves supported character-width
  // integers compile through std::in_range and preserve exact value semantics at runtime.
  const auto narrow_integers =
      FixedPoint::from_scaled(static_cast<signed char>(12), static_cast<unsigned char>(1));
  REQUIRE(narrow_integers);
  CHECK(narrow_integers.value() == parse_fixed_point_or_throw("1.2"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Cross-scale addition and subtraction remain exact through both signed overflow boundaries.
TEST_CASE("addition and subtraction preserve exact cross-scale values and fail on overflow",
          "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve exact values while aligning unlike scales for addition and subtraction.
  const auto sum =
      parse_fixed_point_or_throw("1.2").checked_add(parse_fixed_point_or_throw("0.03"));
  REQUIRE(sum);
  CHECK(sum.value() == parse_fixed_point_or_throw("1.23"));

  const auto difference =
      parse_fixed_point_or_throw("1.2").checked_subtract(parse_fixed_point_or_throw("2.03"));
  REQUIRE(difference);
  CHECK(difference.value() == parse_fixed_point_or_throw("-0.83"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject overflow in both signed directions rather than wrapping coefficients.
  const auto add_overflow = parse_fixed_point_or_throw("9223372036854775807")
                                .checked_add(parse_fixed_point_or_throw("1"));
  REQUIRE_FALSE(add_overflow);
  CHECK(add_overflow.error().code == DomainErrorCode::ArithmeticOverflow);

  const auto subtract_overflow = parse_fixed_point_or_throw("-9223372036854775808")
                                     .checked_subtract(parse_fixed_point_or_throw("1"));
  REQUIRE_FALSE(subtract_overflow);
  CHECK(subtract_overflow.error().code == DomainErrorCode::ArithmeticOverflow);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every directional policy is explicit, including signed floor/ceiling and ties-to-even behavior.
TEST_CASE("rescaling requires an explicit policy and handles every signed direction",
          "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact rescaling rejects discarded information rather than selecting an implicit policy.
  const auto exact = parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::Exact);
  REQUIRE_FALSE(exact);
  CHECK(exact.error().code == DomainErrorCode::PrecisionLoss);

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin every directional and ties-to-even policy for positive values.
  CHECK(parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::TowardZero).value() ==
        parse_fixed_point_or_throw("1.2"));
  CHECK(parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::AwayFromZero).value() ==
        parse_fixed_point_or_throw("1.3"));
  CHECK(parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::Floor).value() ==
        parse_fixed_point_or_throw("1.2"));
  CHECK(parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::Ceiling).value() ==
        parse_fixed_point_or_throw("1.3"));
  CHECK(parse_fixed_point_or_throw("1.25").rescale(1U, RoundingMode::NearestTiesToEven).value() ==
        parse_fixed_point_or_throw("1.2"));
  CHECK(parse_fixed_point_or_throw("1.35").rescale(1U, RoundingMode::NearestTiesToEven).value() ==
        parse_fixed_point_or_throw("1.4"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin the corresponding sign-sensitive behavior for negative values.
  CHECK(parse_fixed_point_or_throw("-1.25").rescale(1U, RoundingMode::TowardZero).value() ==
        parse_fixed_point_or_throw("-1.2"));
  CHECK(parse_fixed_point_or_throw("-1.25").rescale(1U, RoundingMode::AwayFromZero).value() ==
        parse_fixed_point_or_throw("-1.3"));
  CHECK(parse_fixed_point_or_throw("-1.25").rescale(1U, RoundingMode::Floor).value() ==
        parse_fixed_point_or_throw("-1.3"));
  CHECK(parse_fixed_point_or_throw("-1.25").rescale(1U, RoundingMode::Ceiling).value() ==
        parse_fixed_point_or_throw("-1.2"));
  CHECK(parse_fixed_point_or_throw("-1.25").rescale(1U, RoundingMode::NearestTiesToEven).value() ==
        parse_fixed_point_or_throw("-1.2"));
  CHECK(parse_fixed_point_or_throw("-1.35").rescale(1U, RoundingMode::NearestTiesToEven).value() ==
        parse_fixed_point_or_throw("-1.4"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Target-scaled products and quotients preserve exact results and report precision or range
// failures.
TEST_CASE("multiplication and division are target-scaled and checked", "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve an exact target-scaled product and reject a genuinely overflowing product.
  const auto product = parse_fixed_point_or_throw("0.2").multiply(parse_fixed_point_or_throw("0.5"),
                                                                  1U, RoundingMode::Exact);
  REQUIRE(product);
  CHECK(product.value() == parse_fixed_point_or_throw("0.1"));

  const auto product_overflow =
      parse_fixed_point_or_throw("9223372036854775807")
          .multiply(parse_fixed_point_or_throw("2"), 0U, RoundingMode::Exact);
  REQUIRE_FALSE(product_overflow);
  CHECK(product_overflow.error().code == DomainErrorCode::ArithmeticOverflow);

  // ++++++++++++++++++++++++++++++++++++++++
  // Requesting maximum precision cannot create insignificant zeros that overflow a boundary value.
  const auto maximum_product =
      parse_fixed_point_or_throw("9223372036854775807")
          .multiply(parse_fixed_point_or_throw("1"), 18U, RoundingMode::Exact);
  REQUIRE(maximum_product);
  CHECK(maximum_product.value() == parse_fixed_point_or_throw("9223372036854775807"));

  // The asymmetric negative coefficient limit remains representable through exact multiplication.
  const auto minimum_product =
      parse_fixed_point_or_throw("-9223372036854775808")
          .multiply(parse_fixed_point_or_throw("1"), 18U, RoundingMode::Exact);
  REQUIRE(minimum_product);
  CHECK(minimum_product.value() == parse_fixed_point_or_throw("-9223372036854775808"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve an exact quotient and make non-terminating precision policy explicit.
  const auto quotient = parse_fixed_point_or_throw("1").divide(parse_fixed_point_or_throw("8"), 3U,
                                                               RoundingMode::Exact);
  REQUIRE(quotient);
  CHECK(quotient.value() == parse_fixed_point_or_throw("0.125"));

  const auto third_exact = parse_fixed_point_or_throw("1").divide(parse_fixed_point_or_throw("3"),
                                                                  2U, RoundingMode::Exact);
  REQUIRE_FALSE(third_exact);
  CHECK(third_exact.error().code == DomainErrorCode::PrecisionLoss);
  CHECK(parse_fixed_point_or_throw("1")
            .divide(parse_fixed_point_or_throw("3"), 2U, RoundingMode::TowardZero)
            .value() == parse_fixed_point_or_throw("0.33"));
  CHECK(parse_fixed_point_or_throw("-1")
            .divide(parse_fixed_point_or_throw("3"), 2U, RoundingMode::Floor)
            .value() == parse_fixed_point_or_throw("-0.34"));
  CHECK(parse_fixed_point_or_throw("-1")
            .divide(parse_fixed_point_or_throw("3"), 2U, RoundingMode::Ceiling)
            .value() == parse_fixed_point_or_throw("-0.33"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Long division must normalize its scratch quotient before checking the signed coefficient bound.
  const auto maximum_quotient =
      parse_fixed_point_or_throw("9223372036854775807")
          .divide(parse_fixed_point_or_throw("1"), 18U, RoundingMode::Exact);
  REQUIRE(maximum_quotient);
  CHECK(maximum_quotient.value() == parse_fixed_point_or_throw("9223372036854775807"));

  // Dividing INT64_MIN by one preserves it, while division by negative one correctly rejects the
  // unrepresentable positive magnitude.
  const auto minimum_quotient =
      parse_fixed_point_or_throw("-9223372036854775808")
          .divide(parse_fixed_point_or_throw("1"), 18U, RoundingMode::Exact);
  REQUIRE(minimum_quotient);
  CHECK(minimum_quotient.value() == parse_fixed_point_or_throw("-9223372036854775808"));

  const auto minimum_negated =
      parse_fixed_point_or_throw("-9223372036854775808")
          .divide(parse_fixed_point_or_throw("-1"), 18U, RoundingMode::Exact);
  REQUIRE_FALSE(minimum_negated);
  CHECK(minimum_negated.error().code == DomainErrorCode::ArithmeticOverflow);

  // ++++++++++++++++++++++++++++++++++++++++
  // Division by zero remains a distinct domain failure after all range-boundary cases.
  const auto division_by_zero = parse_fixed_point_or_throw("1").divide(
      parse_fixed_point_or_throw("0"), 2U, RoundingMode::Exact);
  REQUIRE_FALSE(division_by_zero);
  CHECK(division_by_zero.error().code == DomainErrorCode::DivisionByZero);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Alignment and quantization work across unequal scales without introducing binary-float error.
TEST_CASE("multiple tests and quantization are exact across different scales",
          "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Determine exact divisibility after aligning unlike decimal scales.
  CHECK(
      parse_fixed_point_or_throw("1.2").is_multiple_of(parse_fixed_point_or_throw("0.3")).value());
  CHECK(
      parse_fixed_point_or_throw("0.5").is_multiple_of(parse_fixed_point_or_throw("0.25")).value());
  CHECK_FALSE(
      parse_fixed_point_or_throw("0.6").is_multiple_of(parse_fixed_point_or_throw("0.25")).value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Quantize both signs with explicit floor and ceiling policies.
  CHECK(parse_fixed_point_or_throw("1.24")
            .quantize(parse_fixed_point_or_throw("0.05"), RoundingMode::Floor)
            .value() == parse_fixed_point_or_throw("1.2"));
  CHECK(parse_fixed_point_or_throw("1.24")
            .quantize(parse_fixed_point_or_throw("0.05"), RoundingMode::Ceiling)
            .value() == parse_fixed_point_or_throw("1.25"));
  CHECK(parse_fixed_point_or_throw("-1.24")
            .quantize(parse_fixed_point_or_throw("0.05"), RoundingMode::Floor)
            .value() == parse_fixed_point_or_throw("-1.25"));
  CHECK(parse_fixed_point_or_throw("-1.24")
            .quantize(parse_fixed_point_or_throw("0.05"), RoundingMode::Ceiling)
            .value() == parse_fixed_point_or_throw("-1.2"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Unknown rounding enumerators fail closed across every precision-changing operation.
TEST_CASE("unassigned rounding modes fail instead of selecting an implicit policy",
          "[model][fixed-point]") {
  const auto invalid = static_cast<RoundingMode>(255U);

  for (const auto& result :
       {parse_fixed_point_or_throw("1.25").rescale(1U, invalid),
        parse_fixed_point_or_throw("1").multiply(parse_fixed_point_or_throw("2"), 0U, invalid),
        parse_fixed_point_or_throw("1").divide(parse_fixed_point_or_throw("2"), 1U, invalid),
        parse_fixed_point_or_throw("1.25").quantize(parse_fixed_point_or_throw("0.5"), invalid)}) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidValue);
    CHECK(result.error().context.field == "rounding_mode");
  }
}

// --------------------------------------------------------
// Every scale-changing API rejects a wide value before narrowing it to the stored scale type.
TEST_CASE("wide target scales are rejected before narrowing", "[model][fixed-point]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Apply the same wide-scale firewall to rescaling, multiplication, and division.
  constexpr auto invalid_scale = std::uint64_t{256U};
  for (const auto& result :
       {parse_fixed_point_or_throw("1").rescale(invalid_scale, RoundingMode::Exact),
        parse_fixed_point_or_throw("1").multiply(parse_fixed_point_or_throw("1"), invalid_scale,
                                                 RoundingMode::Exact),
        parse_fixed_point_or_throw("1").divide(parse_fixed_point_or_throw("1"), invalid_scale,
                                               RoundingMode::Exact)}) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == DomainErrorCode::InvalidScale);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Rescaling also preserves a signed target until validation, so -1 cannot wrap to an unsigned
  // scale before the domain error is selected.
  const auto negative_scale = parse_fixed_point_or_throw("1").rescale(-1, RoundingMode::Exact);
  REQUIRE_FALSE(negative_scale);
  CHECK(negative_scale.error() ==
        DomainError::create_at_field(DomainErrorCode::InvalidScale, "fixed_point"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
