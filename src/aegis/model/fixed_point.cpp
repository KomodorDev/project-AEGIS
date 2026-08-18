// Purpose: implement portable checked decimal arithmetic without extended integer types.

#include "aegis/model/fixed_point.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace aegis::model {
namespace {

// Powers needed by scale reduction and divisibility checks are bounded by maximum_scale.
constexpr std::array<std::uint64_t, 19U> powers_of_ten{
    1ULL,
    10ULL,
    100ULL,
    1'000ULL,
    10'000ULL,
    100'000ULL,
    1'000'000ULL,
    10'000'000ULL,
    100'000'000ULL,
    1'000'000'000ULL,
    10'000'000'000ULL,
    100'000'000'000ULL,
    1'000'000'000'000ULL,
    10'000'000'000'000ULL,
    100'000'000'000'000ULL,
    1'000'000'000'000'000ULL,
    10'000'000'000'000'000ULL,
    100'000'000'000'000'000ULL,
    1'000'000'000'000'000'000ULL,
};

// Carry signed coefficients as a separate sign and magnitude. The asymmetric limits preserve the
// full INT64_MIN range without ever negating that value in signed arithmetic.
constexpr std::uint64_t negative_limit = std::uint64_t{1U} << 63U;
constexpr std::uint64_t positive_limit =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

// Convert through -(value + 1) before restoring the final unit so INT64_MIN never undergoes the
// undefined signed negation that a direct absolute-value operation would require.
[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

// Restore a signed coefficient only after checking the asymmetric positive/negative bounds; the
// negative limit is returned through its dedicated representation-safe branch.
[[nodiscard]] Result<std::int64_t> signed_coefficient(std::uint64_t value, bool negative,
                                                      std::string field) {
  const auto limit = negative ? negative_limit : positive_limit;
  if (value > limit) {
    return Result<std::int64_t>::failure(
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, std::move(field)));
  }
  if (!negative) {
    return Result<std::int64_t>::success(static_cast<std::int64_t>(value));
  }
  if (value == negative_limit) {
    return Result<std::int64_t>::success(std::numeric_limits<std::int64_t>::min());
  }
  return Result<std::int64_t>::success(-static_cast<std::int64_t>(value));
}

// DecimalDigits is a least-significant-digit-first scratch value. Its fixed storage lets alignment,
// products, and long division finish before a result is checked back into a signed coefficient.
struct DecimalDigits {
  std::array<std::uint8_t, 64U> values{};
  std::size_t size{1U};
};

// Build and normalize scratch magnitudes without introducing leading zero digits.
[[nodiscard]] DecimalDigits digits_from(std::uint64_t value, std::size_t decimal_shift = 0U) {
  DecimalDigits result;
  if (value == 0U) {
    return result;
  }

  result.size = decimal_shift;
  while (value != 0U) {
    result.values[result.size] = static_cast<std::uint8_t>(value % 10U);
    ++result.size;
    value /= 10U;
  }
  return result;
}

void normalize(DecimalDigits& value) noexcept {
  while (value.size > 1U && value.values[value.size - 1U] == 0U) {
    --value.size;
  }
}

// Compare and combine unsigned magnitudes after callers have aligned their decimal scales.
[[nodiscard]] int compare_magnitude(const DecimalDigits& lhs, const DecimalDigits& rhs) noexcept {
  if (lhs.size != rhs.size) {
    return lhs.size < rhs.size ? -1 : 1;
  }
  for (std::size_t index = lhs.size; index > 0U; --index) {
    const auto position = index - 1U;
    if (lhs.values[position] != rhs.values[position]) {
      return lhs.values[position] < rhs.values[position] ? -1 : 1;
    }
  }
  return 0;
}

[[nodiscard]] DecimalDigits add_magnitudes(const DecimalDigits& lhs,
                                           const DecimalDigits& rhs) noexcept {
  DecimalDigits result;
  result.size = std::max(lhs.size, rhs.size);
  std::uint16_t carry = 0U;
  for (std::size_t index = 0U; index < result.size; ++index) {
    const auto sum = static_cast<std::uint32_t>(lhs.values[index]) +
                     static_cast<std::uint32_t>(rhs.values[index]) +
                     static_cast<std::uint32_t>(carry);
    result.values[index] = static_cast<std::uint8_t>(sum % 10U);
    carry = static_cast<std::uint16_t>(sum / 10U);
  }
  if (carry != 0U) {
    result.values[result.size] = static_cast<std::uint8_t>(carry);
    ++result.size;
  }
  return result;
}

// The caller establishes lhs >= rhs, so borrow propagation cannot produce a negative magnitude.
[[nodiscard]] DecimalDigits subtract_magnitudes(const DecimalDigits& lhs,
                                                const DecimalDigits& rhs) noexcept {
  DecimalDigits result;
  result.size = lhs.size;
  std::int16_t borrow = 0;
  for (std::size_t index = 0U; index < lhs.size; ++index) {
    auto difference = static_cast<std::int16_t>(lhs.values[index]) -
                      static_cast<std::int16_t>(rhs.values[index]) - borrow;
    if (difference < 0) {
      difference = static_cast<std::int16_t>(difference + 10);
      borrow = 1;
    } else {
      borrow = 0;
    }
    result.values[index] = static_cast<std::uint8_t>(difference);
  }
  normalize(result);
  return result;
}

// Removing the least-significant decimal zero reduces coefficient and scale together, preserving
// the represented value while moving it toward canonical form.
void remove_decimal_zero(DecimalDigits& value) noexcept {
  for (std::size_t index = 1U; index < value.size; ++index) {
    value.values[index - 1U] = value.values[index];
  }
  value.values[value.size - 1U] = 0U;
  if (value.size > 1U) {
    --value.size;
  }
}

// Append one long-division digit as value*10+digit in the least-significant-first representation.
void append_decimal_digit(DecimalDigits& value, std::uint8_t digit) noexcept {
  for (std::size_t index = value.size; index > 0U; --index) {
    value.values[index] = value.values[index - 1U];
  }
  value.values[0U] = digit;
  ++value.size;
  normalize(value);
}

// Propagate a rounding carry in scratch storage before any signed-width conversion is attempted.
void increment_magnitude(DecimalDigits& value) noexcept {
  std::size_t index = 0U;
  while (index < value.size && value.values[index] == 9U) {
    value.values[index] = 0U;
    ++index;
  }
  if (index == value.size) {
    value.values[index] = 1U;
    ++value.size;
    return;
  }
  ++value.values[index];
}

// Convert scratch digits only after proving every multiply-and-add stays within the supplied
// sign-specific coefficient limit.
[[nodiscard]] Result<std::uint64_t> digits_to_magnitude(const DecimalDigits& value,
                                                        std::uint64_t limit) {
  std::uint64_t result = 0U;
  for (std::size_t index = value.size; index > 0U; --index) {
    const auto digit = static_cast<std::uint64_t>(value.values[index - 1U]);
    if (result > (limit - digit) / 10U) {
      return Result<std::uint64_t>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    result = (result * 10U) + digit;
  }
  return Result<std::uint64_t>::success(result);
}

// Form the full schoolbook product in scratch storage so intermediate precision never depends on a
// compiler-specific extended integer type.
[[nodiscard]] DecimalDigits multiply_magnitudes(std::uint64_t lhs, std::uint64_t rhs) noexcept {
  if (lhs == 0U || rhs == 0U) {
    return DecimalDigits{};
  }
  const auto lhs_digits = digits_from(lhs);
  const auto rhs_digits = digits_from(rhs);
  std::array<std::uint32_t, 64U> accumulator{};
  for (std::size_t lhs_index = 0U; lhs_index < lhs_digits.size; ++lhs_index) {
    for (std::size_t rhs_index = 0U; rhs_index < rhs_digits.size; ++rhs_index) {
      accumulator[lhs_index + rhs_index] +=
          static_cast<std::uint32_t>(lhs_digits.values[lhs_index]) *
          static_cast<std::uint32_t>(rhs_digits.values[rhs_index]);
    }
  }

  DecimalDigits result;
  result.size = lhs_digits.size + rhs_digits.size;
  std::uint32_t carry = 0U;
  for (std::size_t index = 0U; index < result.size; ++index) {
    const auto total = accumulator[index] + carry;
    result.values[index] = static_cast<std::uint8_t>(total % 10U);
    carry = total / 10U;
  }
  while (carry != 0U) {
    result.values[result.size] = static_cast<std::uint8_t>(carry % 10U);
    ++result.size;
    carry /= 10U;
  }
  normalize(result);
  return result;
}

// Classify discarded information once, then apply the same exact/directional/ties-to-even policy to
// rescaling, multiplication, and division.
enum class FractionRelation { Zero, LessThanHalf, Half, GreaterThanHalf };

// Decide whether to increment independently of storage width. Exact fails on every nonzero
// discarded fraction, and ties-to-even depends on the parity of the retained magnitude.
[[nodiscard]] Result<bool> rounding_increment(bool negative, FractionRelation fraction,
                                              bool truncated_is_odd, RoundingMode rounding) {
  if (fraction == FractionRelation::Zero) {
    return Result<bool>::success(false);
  }
  if (rounding == RoundingMode::Exact) {
    return Result<bool>::failure(
        DomainError::at_field(DomainErrorCode::PrecisionLoss, "fixed_point"));
  }

  switch (rounding) {
  case RoundingMode::Exact:
  case RoundingMode::TowardZero:
    return Result<bool>::success(false);
  case RoundingMode::AwayFromZero:
    return Result<bool>::success(true);
  case RoundingMode::Floor:
    return Result<bool>::success(negative);
  case RoundingMode::Ceiling:
    return Result<bool>::success(!negative);
  case RoundingMode::NearestTiesToEven:
    return Result<bool>::success(fraction == FractionRelation::GreaterThanHalf ||
                                 (fraction == FractionRelation::Half && truncated_is_odd));
  }
  return Result<bool>::failure(
      DomainError::at_field(DomainErrorCode::InvalidValue, "rounding_mode"));
}

// Inspect the highest discarded decimal digit plus a sticky bit for all lower digits, producing the
// exact zero/below-half/tie/above-half classification shared by every rounding policy.
[[nodiscard]] FractionRelation decimal_fraction(const DecimalDigits& value,
                                                std::size_t discarded_digits) noexcept {
  if (discarded_digits == 0U) {
    return FractionRelation::Zero;
  }
  const auto leading_position = discarded_digits - 1U;
  const auto leading =
      leading_position < value.size ? value.values[leading_position] : std::uint8_t{0U};
  bool sticky = false;
  const auto sticky_end = std::min(leading_position, value.size);
  for (std::size_t index = 0U; index < sticky_end; ++index) {
    sticky = sticky || value.values[index] != 0U;
  }
  if (leading == 0U && !sticky) {
    return FractionRelation::Zero;
  }
  if (leading < 5U) {
    return FractionRelation::LessThanHalf;
  }
  if (leading > 5U || sticky) {
    return FractionRelation::GreaterThanHalf;
  }
  return FractionRelation::Half;
}

// Apply a previously classified fraction without letting a rounding increment cross the signed
// coefficient boundary.
[[nodiscard]] Result<std::uint64_t> round_magnitude(std::uint64_t truncated, std::uint64_t limit,
                                                    bool negative, FractionRelation fraction,
                                                    RoundingMode rounding) {
  auto increment = rounding_increment(negative, fraction, (truncated % 2U) != 0U, rounding);
  if (!increment) {
    return Result<std::uint64_t>::failure(increment.error());
  }
  if (increment.value()) {
    if (truncated == limit) {
      return Result<std::uint64_t>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    ++truncated;
  }
  return Result<std::uint64_t>::success(truncated);
}

[[nodiscard]] FractionRelation binary_fraction(std::uint64_t remainder,
                                               std::uint64_t divisor) noexcept {
  // Callers maintain remainder < divisor, so doubling is sufficient to classify the exact fraction.
  if (remainder == 0U) {
    return FractionRelation::Zero;
  }
  const auto doubled = remainder * 2U;
  if (doubled < divisor) {
    return FractionRelation::LessThanHalf;
  }
  if (doubled > divisor) {
    return FractionRelation::GreaterThanHalf;
  }
  return FractionRelation::Half;
}

struct DecimalDivisionStep {
  std::uint8_t digit;
  std::uint64_t remainder;
};

// Computes (remainder * 10) / divisor without ever forming the potentially overflowing product.
[[nodiscard]] DecimalDivisionStep next_decimal_digit(std::uint64_t remainder,
                                                     std::uint64_t divisor) noexcept {
  std::uint64_t accumulated = 0U;
  std::uint8_t digit = 0U;
  for (std::uint8_t iteration = 0U; iteration < 10U; ++iteration) {
    accumulated += remainder;
    if (accumulated >= divisor) {
      accumulated -= divisor;
      ++digit;
    }
  }
  return DecimalDivisionStep{digit, accumulated};
}

[[nodiscard]] DomainError invalid_scale_error() {
  return DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point");
}

[[nodiscard]] bool is_valid_rounding_mode(RoundingMode rounding) noexcept {
  switch (rounding) {
  case RoundingMode::Exact:
  case RoundingMode::TowardZero:
  case RoundingMode::AwayFromZero:
  case RoundingMode::Floor:
  case RoundingMode::Ceiling:
  case RoundingMode::NearestTiesToEven:
    return true;
  }
  return false;
}

[[nodiscard]] DomainError invalid_rounding_error() {
  return DomainError::at_field(DomainErrorCode::InvalidValue, "rounding_mode");
}

} // namespace

// Canonical values use scale zero for zero and contain no removable trailing decimal zeros.
FixedPoint FixedPoint::canonical(std::int64_t coefficient, std::uint8_t scale) noexcept {
  if (coefficient == 0) {
    return FixedPoint{0, 0U};
  }
  while (scale > 0U && (coefficient % 10) == 0) {
    coefficient /= 10;
    --scale;
  }
  return FixedPoint{coefficient, scale};
}

Result<FixedPoint> FixedPoint::from_validated_scaled(std::int64_t coefficient,
                                                     std::uint64_t scale) {
  if (scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  return Result<FixedPoint>::success(canonical(coefficient, static_cast<std::uint8_t>(scale)));
}

// Parse in phases so syntax, scale, and coefficient failures remain distinct and deterministic.
Result<FixedPoint> FixedPoint::parse_ascii(std::string_view text) {
  if (text.empty()) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  std::size_t position = 0U;
  bool negative = false;
  if (text[position] == '-') {
    negative = true;
    ++position;
  }
  if (position == text.size()) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  // Accept only an unsigned run of ordinary ASCII digits before the optional decimal point.
  const auto integer_start = position;
  std::size_t integer_digits = 0U;
  while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
    ++position;
    ++integer_digits;
  }
  if (integer_digits == 0U) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  const auto integer_end = position;
  auto fraction_start = position;
  auto fraction_end = position;
  // Bound textual precision while scanning; exponent notation and an empty fraction remain invalid.
  if (position < text.size() && text[position] == '.') {
    ++position;
    fraction_start = position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if ((position - fraction_start) == maximum_scale) {
        return Result<FixedPoint>::failure(
            DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
      }
      ++position;
    }
    if (position == fraction_start) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
    }
    fraction_end = position;
  }
  if (position != text.size()) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  // Remove redundant fractional zeros before accumulation so only canonical significant digits
  // consume the signed coefficient range.
  while (fraction_end > fraction_start && text[fraction_end - 1U] == '0') {
    --fraction_end;
  }
  const auto scale = static_cast<std::uint8_t>(fraction_end - fraction_start);
  const auto limit = negative ? negative_limit : positive_limit;
  std::uint64_t parsed = 0U;
  // Fold both digit runs under the sign-specific limit before restoring the signed coefficient.
  const auto append_digits = [&](std::size_t begin, std::size_t end) -> bool {
    for (auto index = begin; index < end; ++index) {
      const auto digit = static_cast<std::uint64_t>(text[index] - '0');
      if (parsed > (limit - digit) / 10U) {
        return false;
      }
      parsed = (parsed * 10U) + digit;
    }
    return true;
  };
  if (!append_digits(integer_start, integer_end) || !append_digits(fraction_start, fraction_end)) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
  }

  auto coefficient = signed_coefficient(parsed, negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), scale));
}

// Render the canonical coefficient directly, inserting only the zeros required by its stored scale.
std::string FixedPoint::to_string() const {
  const auto absolute = magnitude(coefficient_);
  auto digits = std::to_string(absolute);
  if (scale_ != 0U) {
    const auto scale_size = static_cast<std::size_t>(scale_);
    if (digits.size() <= scale_size) {
      digits.insert(0U, (scale_size + 1U) - digits.size(), '0');
    }
    digits.insert(digits.size() - scale_size, 1U, '.');
  }
  if (coefficient_ < 0) {
    digits.insert(0U, 1U, '-');
  }
  return digits;
}

Result<FixedPoint> FixedPoint::checked_add(FixedPoint other) const {
  // Align decimal points in scratch storage instead of multiplying either signed coefficient.
  const auto common_scale = std::max(scale_, other.scale_);
  const auto lhs =
      digits_from(magnitude(coefficient_), static_cast<std::size_t>(common_scale - scale_));
  const auto rhs = digits_from(magnitude(other.coefficient_),
                               static_cast<std::size_t>(common_scale - other.scale_));
  const bool lhs_negative = coefficient_ < 0;
  const bool rhs_negative = other.coefficient_ < 0;

  // Equal signs add magnitudes; differing signs subtract the smaller magnitude from the larger.
  DecimalDigits result;
  bool result_negative = false;
  if (lhs_negative == rhs_negative) {
    result = add_magnitudes(lhs, rhs);
    result_negative = lhs_negative;
  } else {
    const auto order = compare_magnitude(lhs, rhs);
    if (order >= 0) {
      result = subtract_magnitudes(lhs, rhs);
      result_negative = lhs_negative;
    } else {
      result = subtract_magnitudes(rhs, lhs);
      result_negative = rhs_negative;
    }
  }

  // Canonicalize before converting back so removable fractional zeros do not cause false overflow.
  auto result_scale = common_scale;
  while (result_scale > 0U && result.values[0U] == 0U) {
    remove_decimal_zero(result);
    --result_scale;
  }
  normalize(result);
  if (result.size == 1U && result.values[0U] == 0U) {
    result_negative = false;
  }

  const auto limit = result_negative ? negative_limit : positive_limit;
  auto result_magnitude = digits_to_magnitude(result, limit);
  if (!result_magnitude) {
    return Result<FixedPoint>::failure(result_magnitude.error());
  }
  auto coefficient = signed_coefficient(result_magnitude.value(), result_negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), result_scale));
}

Result<FixedPoint> FixedPoint::checked_subtract(FixedPoint other) const {
  // Align decimal points and invert only the rhs sign; never negate a possibly minimal coefficient.
  const auto common_scale = std::max(scale_, other.scale_);
  const auto lhs =
      digits_from(magnitude(coefficient_), static_cast<std::size_t>(common_scale - scale_));
  const auto rhs = digits_from(magnitude(other.coefficient_),
                               static_cast<std::size_t>(common_scale - other.scale_));
  const bool lhs_negative = coefficient_ < 0;
  const bool rhs_negative = !(other.coefficient_ < 0);

  // Reuse sign-magnitude addition after the logical sign inversion.
  DecimalDigits result;
  bool result_negative = false;
  if (lhs_negative == rhs_negative) {
    result = add_magnitudes(lhs, rhs);
    result_negative = lhs_negative;
  } else {
    const auto order = compare_magnitude(lhs, rhs);
    if (order >= 0) {
      result = subtract_magnitudes(lhs, rhs);
      result_negative = lhs_negative;
    } else {
      result = subtract_magnitudes(rhs, lhs);
      result_negative = rhs_negative;
    }
  }

  // Remove canonical zeros before enforcing the asymmetric signed coefficient boundary.
  auto result_scale = common_scale;
  while (result_scale > 0U && result.values[0U] == 0U) {
    remove_decimal_zero(result);
    --result_scale;
  }
  normalize(result);
  if (result.size == 1U && result.values[0U] == 0U) {
    result_negative = false;
  }
  const auto limit = result_negative ? negative_limit : positive_limit;
  auto result_magnitude = digits_to_magnitude(result, limit);
  if (!result_magnitude) {
    return Result<FixedPoint>::failure(result_magnitude.error());
  }
  auto coefficient = signed_coefficient(result_magnitude.value(), result_negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), result_scale));
}

Result<FixedPoint> FixedPoint::rescale_validated(std::uint64_t target_scale,
                                                 RoundingMode rounding) const {
  // Validate policy and scale before narrowing the public input to the stored scale type.
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  const auto validated_scale = static_cast<std::uint8_t>(target_scale);
  if (validated_scale >= scale_) {
    // Canonical storage never pads a value with insignificant zeros merely to report a wider scale.
    return Result<FixedPoint>::success(*this);
  }

  // Reducing scale divides once, classifies the exact remainder, and rounds under the signed limit.
  const auto divisor = powers_of_ten[static_cast<std::size_t>(scale_ - validated_scale)];
  const auto absolute = magnitude(coefficient_);
  const auto truncated = absolute / divisor;
  const auto remainder = absolute % divisor;
  const bool negative = coefficient_ < 0;
  const auto limit = negative ? negative_limit : positive_limit;
  auto rounded =
      round_magnitude(truncated, limit, negative, binary_fraction(remainder, divisor), rounding);
  if (!rounded) {
    return Result<FixedPoint>::failure(rounded.error());
  }
  auto coefficient = signed_coefficient(rounded.value(), negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), validated_scale));
}

Result<FixedPoint> FixedPoint::multiply_validated(FixedPoint other, std::uint64_t target_scale,
                                                  RoundingMode rounding) const {
  // Establish the output policy before constructing the potentially wider scratch product.
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  const auto validated_scale = static_cast<std::uint8_t>(target_scale);
  const bool negative = (coefficient_ < 0) != (other.coefficient_ < 0);
  const auto product = multiply_magnitudes(magnitude(coefficient_), magnitude(other.coefficient_));
  if (product.size == 1U && product.values[0U] == 0U) {
    return Result<FixedPoint>::success(canonical(0, 0U));
  }

  // Compare the natural product scale with the requested precision and retain only significant
  // digits; canonical output does not materialize insignificant target-scale zeros.
  const auto source_scale = static_cast<int>(scale_) + static_cast<int>(other.scale_);
  const auto shift = static_cast<int>(validated_scale) - source_scale;
  auto result_scale = validated_scale;
  FractionRelation fraction = FractionRelation::Zero;
  DecimalDigits retained;
  if (shift >= 0) {
    // A wider requested scale adds no significant digits, so preserve the natural canonical scale
    // instead of appending zeros that could create an artificial coefficient overflow.
    retained = product;
    result_scale = static_cast<std::uint8_t>(source_scale);
  } else {
    const auto discarded = static_cast<std::size_t>(-shift);
    fraction = decimal_fraction(product, discarded);
    if (discarded >= product.size) {
      retained = DecimalDigits{};
    } else {
      retained.size = product.size - discarded;
      for (std::size_t index = 0U; index < retained.size; ++index) {
        retained.values[index] = product.values[index + discarded];
      }
      normalize(retained);
    }
  }

  // Round and canonicalize in scratch storage before attempting the bounded coefficient conversion.
  auto increment =
      rounding_increment(negative, fraction, (retained.values[0U] % 2U) != 0U, rounding);
  if (!increment) {
    return Result<FixedPoint>::failure(increment.error());
  }
  if (increment.value()) {
    increment_magnitude(retained);
  }
  while (result_scale > 0U && retained.values[0U] == 0U) {
    remove_decimal_zero(retained);
    --result_scale;
  }
  normalize(retained);

  const auto limit = negative ? negative_limit : positive_limit;
  auto truncated = digits_to_magnitude(retained, limit);
  if (!truncated) {
    return Result<FixedPoint>::failure(truncated.error());
  }
  auto coefficient = signed_coefficient(truncated.value(), negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), result_scale));
}

Result<FixedPoint> FixedPoint::divide_validated(FixedPoint divisor, std::uint64_t target_scale,
                                                RoundingMode rounding) const {
  // Reject invalid policy, scale, and zero divisor before beginning quotient generation.
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  const auto validated_scale = static_cast<std::uint8_t>(target_scale);
  if (divisor.coefficient_ == 0) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::DivisionByZero, "fixed_point"));
  }
  if (coefficient_ == 0) {
    return Result<FixedPoint>::success(canonical(0, 0U));
  }

  const bool negative = (coefficient_ < 0) != (divisor.coefficient_ < 0);
  const auto limit = negative ? negative_limit : positive_limit;
  const auto numerator = magnitude(coefficient_);
  const auto denominator = magnitude(divisor.coefficient_);
  auto truncated = numerator / denominator;
  auto remainder = numerator % denominator;
  // The exponent states how many quotient digits must be generated (positive) or discarded
  // (negative) after accounting for both operand scales and requested result precision.
  const auto exponent = static_cast<int>(validated_scale) + static_cast<int>(divisor.scale_) -
                        static_cast<int>(scale_);
  auto result_scale = validated_scale;
  FractionRelation fraction = FractionRelation::Zero;

  if (exponent >= 0) {
    // Extend the quotient by ordinary long division, retaining the final remainder for rounding.
    auto retained = digits_from(truncated);
    for (int index = 0; index < exponent; ++index) {
      const auto step = next_decimal_digit(remainder, denominator);
      append_decimal_digit(retained, step.digit);
      remainder = step.remainder;
    }
    // Round and remove canonical zeros while the quotient is still scratch digits; converting first
    // would reject boundary coefficients whose redundant zeros make the intermediate look too wide.
    fraction = binary_fraction(remainder, denominator);
    auto increment =
        rounding_increment(negative, fraction, (retained.values[0U] % 2U) != 0U, rounding);
    if (!increment) {
      return Result<FixedPoint>::failure(increment.error());
    }
    if (increment.value()) {
      increment_magnitude(retained);
    }
    while (result_scale > 0U && retained.values[0U] == 0U) {
      remove_decimal_zero(retained);
      --result_scale;
    }
    normalize(retained);
    auto converted = digits_to_magnitude(retained, limit);
    if (!converted) {
      return Result<FixedPoint>::failure(converted.error());
    }
    truncated = converted.value();
    fraction = FractionRelation::Zero;
  } else {
    // Discard low quotient digits. Any original division remainder is sticky because it represents
    // additional nonzero precision below every discarded integer digit.
    const auto discarded = static_cast<std::size_t>(-exponent);
    std::uint8_t leading = 0U;
    bool sticky = remainder != 0U;
    for (std::size_t index = 0U; index < discarded; ++index) {
      const auto digit = static_cast<std::uint8_t>(truncated % 10U);
      truncated /= 10U;
      if (index + 1U == discarded) {
        leading = digit;
      } else {
        sticky = sticky || digit != 0U;
      }
    }
    if (leading == 0U && !sticky) {
      fraction = FractionRelation::Zero;
    } else if (leading < 5U) {
      fraction = FractionRelation::LessThanHalf;
    } else if (leading > 5U || sticky) {
      fraction = FractionRelation::GreaterThanHalf;
    } else {
      fraction = FractionRelation::Half;
    }
  }

  // Enforce the signed boundary before and after the possible rounding increment.
  if (truncated > limit) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
  }
  auto rounded = round_magnitude(truncated, limit, negative, fraction, rounding);
  if (!rounded) {
    return Result<FixedPoint>::failure(rounded.error());
  }
  auto coefficient = signed_coefficient(rounded.value(), negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), result_scale));
}

Result<bool> FixedPoint::is_multiple_of(FixedPoint increment) const {
  // Zero is never a usable increment, while every zero value is aligned to a nonzero increment.
  if (increment.coefficient_ == 0) {
    return Result<bool>::failure(
        DomainError::at_field(DomainErrorCode::DivisionByZero, "fixed_point"));
  }
  const auto value = magnitude(coefficient_);
  const auto unit = magnitude(increment.coefficient_);
  if (value == 0U) {
    return Result<bool>::success(true);
  }

  if (scale_ >= increment.scale_) {
    // A finer-scaled value must first contain the increment magnitude, then enough decimal zeros to
    // account for the remaining scale difference.
    if ((value % unit) != 0U) {
      return Result<bool>::success(false);
    }
    const auto quotient = value / unit;
    const auto factor = powers_of_ten[static_cast<std::size_t>(scale_ - increment.scale_)];
    return Result<bool>::success((quotient % factor) == 0U);
  }

  // A coarser-scaled value contributes powers of ten. Cancel their 2/5 factors from the increment
  // instead of multiplying the value and risking overflow.
  auto reduced_unit = unit;
  const auto scale_difference = static_cast<std::size_t>(increment.scale_ - scale_);
  for (std::size_t index = 0U; index < scale_difference; ++index) {
    if ((reduced_unit % 2U) == 0U) {
      reduced_unit /= 2U;
    }
    if ((reduced_unit % 5U) == 0U) {
      reduced_unit /= 5U;
    }
  }
  return Result<bool>::success((value % reduced_unit) == 0U);
}

Result<FixedPoint> FixedPoint::quantize(FixedPoint increment, RoundingMode rounding) const {
  // Quantization requires a positive unit and returns already-aligned values unchanged.
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (increment.coefficient_ <= 0) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidValue, "fixed_point_increment"));
  }
  auto aligned = is_multiple_of(increment);
  if (!aligned) {
    return Result<FixedPoint>::failure(aligned.error());
  }
  if (aligned.value()) {
    return Result<FixedPoint>::success(*this);
  }

  // Round to an integral count of increments, then reconstruct the decimal value exactly.
  auto units = divide(increment, 0U, rounding);
  if (!units) {
    return Result<FixedPoint>::failure(units.error());
  }
  return units.value().multiply(increment, increment.scale_, RoundingMode::Exact);
}

bool operator==(FixedPoint lhs, FixedPoint rhs) noexcept {
  return lhs.coefficient_ == rhs.coefficient_ && lhs.scale_ == rhs.scale_;
}

std::strong_ordering operator<=>(FixedPoint lhs, FixedPoint rhs) noexcept {
  // Resolve equality and sign before aligning magnitudes in scratch storage for exact comparison.
  if (lhs == rhs) {
    return std::strong_ordering::equal;
  }
  const bool lhs_negative = lhs.coefficient_ < 0;
  const bool rhs_negative = rhs.coefficient_ < 0;
  if (lhs_negative != rhs_negative) {
    return lhs_negative ? std::strong_ordering::less : std::strong_ordering::greater;
  }

  const auto common_scale = std::max(lhs.scale_, rhs.scale_);
  const auto lhs_digits =
      digits_from(magnitude(lhs.coefficient_), static_cast<std::size_t>(common_scale - lhs.scale_));
  const auto rhs_digits =
      digits_from(magnitude(rhs.coefficient_), static_cast<std::size_t>(common_scale - rhs.scale_));
  const auto order = compare_magnitude(lhs_digits, rhs_digits);
  if (order == 0) {
    return std::strong_ordering::equal;
  }
  const bool lhs_less = lhs_negative ? order > 0 : order < 0;
  return lhs_less ? std::strong_ordering::less : std::strong_ordering::greater;
}

} // namespace aegis::model
