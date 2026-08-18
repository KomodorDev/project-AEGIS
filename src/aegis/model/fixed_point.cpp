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

constexpr std::uint64_t negative_limit = std::uint64_t{1U} << 63U;
constexpr std::uint64_t positive_limit =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

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

struct DecimalDigits {
  std::array<std::uint8_t, 64U> values{};
  std::size_t size{1U};
};

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

// The caller establishes lhs >= rhs.
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

void remove_decimal_zero(DecimalDigits& value) noexcept {
  for (std::size_t index = 1U; index < value.size; ++index) {
    value.values[index - 1U] = value.values[index];
  }
  value.values[value.size - 1U] = 0U;
  if (value.size > 1U) {
    --value.size;
  }
}

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

enum class FractionRelation { Zero, LessThanHalf, Half, GreaterThanHalf };

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

[[nodiscard]] Result<std::uint64_t> round_magnitude(std::uint64_t truncated, std::uint64_t limit,
                                                    bool negative, FractionRelation fraction,
                                                    RoundingMode rounding) {
  if (fraction == FractionRelation::Zero) {
    return Result<std::uint64_t>::success(truncated);
  }
  if (rounding == RoundingMode::Exact) {
    return Result<std::uint64_t>::failure(
        DomainError::at_field(DomainErrorCode::PrecisionLoss, "fixed_point"));
  }

  bool increment = false;
  switch (rounding) {
  case RoundingMode::Exact:
    break;
  case RoundingMode::TowardZero:
    break;
  case RoundingMode::AwayFromZero:
    increment = true;
    break;
  case RoundingMode::Floor:
    increment = negative;
    break;
  case RoundingMode::Ceiling:
    increment = !negative;
    break;
  case RoundingMode::NearestTiesToEven:
    increment = fraction == FractionRelation::GreaterThanHalf ||
                (fraction == FractionRelation::Half && (truncated % 2U) != 0U);
    break;
  }
  if (increment) {
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

Result<FixedPoint> FixedPoint::from_scaled(std::int64_t coefficient, std::uint8_t scale) {
  if (scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  return Result<FixedPoint>::success(canonical(coefficient, scale));
}

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

  std::uint64_t parsed = 0U;
  std::size_t integer_digits = 0U;
  const auto limit = negative ? negative_limit : positive_limit;
  while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
    const auto digit = static_cast<std::uint64_t>(text[position] - '0');
    if (parsed > (limit - digit) / 10U) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    parsed = (parsed * 10U) + digit;
    ++position;
    ++integer_digits;
  }
  if (integer_digits == 0U) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  std::uint8_t scale = 0U;
  if (position < text.size() && text[position] == '.') {
    ++position;
    const auto fraction_start = position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
      if (scale == maximum_scale) {
        return Result<FixedPoint>::failure(
            DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
      }
      const auto digit = static_cast<std::uint64_t>(text[position] - '0');
      if (parsed > (limit - digit) / 10U) {
        return Result<FixedPoint>::failure(
            DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
      }
      parsed = (parsed * 10U) + digit;
      ++position;
      ++scale;
    }
    if (position == fraction_start) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
    }
  }
  if (position != text.size()) {
    return Result<FixedPoint>::failure(
        DomainError::at_field(DomainErrorCode::InvalidDecimal, "fixed_point"));
  }

  auto coefficient = signed_coefficient(parsed, negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), scale));
}

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
  const auto common_scale = std::max(scale_, other.scale_);
  const auto lhs =
      digits_from(magnitude(coefficient_), static_cast<std::size_t>(common_scale - scale_));
  const auto rhs = digits_from(magnitude(other.coefficient_),
                               static_cast<std::size_t>(common_scale - other.scale_));
  const bool lhs_negative = coefficient_ < 0;
  const bool rhs_negative = other.coefficient_ < 0;

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
  const auto common_scale = std::max(scale_, other.scale_);
  const auto lhs =
      digits_from(magnitude(coefficient_), static_cast<std::size_t>(common_scale - scale_));
  const auto rhs = digits_from(magnitude(other.coefficient_),
                               static_cast<std::size_t>(common_scale - other.scale_));
  const bool lhs_negative = coefficient_ < 0;
  const bool rhs_negative = !(other.coefficient_ < 0);

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

Result<FixedPoint> FixedPoint::rescale(std::uint8_t target_scale, RoundingMode rounding) const {
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  if (target_scale >= scale_) {
    return Result<FixedPoint>::success(*this);
  }

  const auto divisor = powers_of_ten[static_cast<std::size_t>(scale_ - target_scale)];
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
  return Result<FixedPoint>::success(canonical(coefficient.value(), target_scale));
}

Result<FixedPoint> FixedPoint::multiply(FixedPoint other, std::uint8_t target_scale,
                                        RoundingMode rounding) const {
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
  const bool negative = (coefficient_ < 0) != (other.coefficient_ < 0);
  const auto product = multiply_magnitudes(magnitude(coefficient_), magnitude(other.coefficient_));
  if (product.size == 1U && product.values[0U] == 0U) {
    return Result<FixedPoint>::success(canonical(0, 0U));
  }

  const auto source_scale = static_cast<int>(scale_) + static_cast<int>(other.scale_);
  const auto shift = static_cast<int>(target_scale) - source_scale;
  FractionRelation fraction = FractionRelation::Zero;
  DecimalDigits retained;
  if (shift >= 0) {
    const auto decimal_shift = static_cast<std::size_t>(shift);
    retained.size = product.size + decimal_shift;
    for (std::size_t index = 0U; index < product.size; ++index) {
      retained.values[index + decimal_shift] = product.values[index];
    }
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

  const auto limit = negative ? negative_limit : positive_limit;
  auto truncated = digits_to_magnitude(retained, limit);
  if (!truncated) {
    return Result<FixedPoint>::failure(truncated.error());
  }
  auto rounded = round_magnitude(truncated.value(), limit, negative, fraction, rounding);
  if (!rounded) {
    return Result<FixedPoint>::failure(rounded.error());
  }
  auto coefficient = signed_coefficient(rounded.value(), negative, "fixed_point");
  if (!coefficient) {
    return Result<FixedPoint>::failure(coefficient.error());
  }
  return Result<FixedPoint>::success(canonical(coefficient.value(), target_scale));
}

Result<FixedPoint> FixedPoint::divide(FixedPoint divisor, std::uint8_t target_scale,
                                      RoundingMode rounding) const {
  if (!is_valid_rounding_mode(rounding)) {
    return Result<FixedPoint>::failure(invalid_rounding_error());
  }
  if (target_scale > maximum_scale) {
    return Result<FixedPoint>::failure(invalid_scale_error());
  }
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
  const auto exponent =
      static_cast<int>(target_scale) + static_cast<int>(divisor.scale_) - static_cast<int>(scale_);
  FractionRelation fraction = FractionRelation::Zero;

  if (exponent >= 0) {
    for (int index = 0; index < exponent; ++index) {
      const auto step = next_decimal_digit(remainder, denominator);
      if (truncated > (limit - static_cast<std::uint64_t>(step.digit)) / 10U) {
        return Result<FixedPoint>::failure(
            DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
      }
      truncated = (truncated * 10U) + static_cast<std::uint64_t>(step.digit);
      remainder = step.remainder;
    }
    if (truncated > limit) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    fraction = binary_fraction(remainder, denominator);
  } else {
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
  return Result<FixedPoint>::success(canonical(coefficient.value(), target_scale));
}

Result<bool> FixedPoint::is_multiple_of(FixedPoint increment) const {
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
    if ((value % unit) != 0U) {
      return Result<bool>::success(false);
    }
    const auto quotient = value / unit;
    const auto factor = powers_of_ten[static_cast<std::size_t>(scale_ - increment.scale_)];
    return Result<bool>::success((quotient % factor) == 0U);
  }

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
