// Purpose: provide exact, checked decimal values for prices, quantities, and notionals.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/result.hpp"

#include <compare>
#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aegis::model {

class InstrumentMetadata;

// Precision-changing operations never select a rounding policy implicitly.
enum class RoundingMode : std::uint8_t {
  Exact = 0,
  TowardZero = 1,
  AwayFromZero = 2,
  Floor = 3,
  Ceiling = 4,
  NearestTiesToEven = 5,
};

// Store one canonical signed coefficient and decimal scale. Decision-path APIs use the nominal
// wrappers below rather than exposing this arithmetic kernel as an interchangeable business value.
class FixedPoint {
public:
  static constexpr std::uint8_t maximum_scale = 18U;

  // Construction accepts already-scaled integers or strict ordinary decimal text; both paths
  // preserve exactness and canonicalize redundant fractional zeros.
  // Interesting syntax: the constrained overload plus a deleted floating-point overload makes
  // binary-float construction ill-formed instead of permitting an implicit lossy conversion.
  // Boolean values are not authored integers. Deducing both remaining inputs preserves their width
  // and signedness for std::in_range; conversion to the kernel representation follows both gates.
  template <detail::CheckedIntegerInput Coefficient, detail::CheckedIntegerInput Scale>
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient coefficient, Scale scale) {
    if (!std::in_range<std::int64_t>(coefficient)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    if (!std::in_range<std::uint64_t>(scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    return from_validated_scaled(static_cast<std::int64_t>(coefficient),
                                 static_cast<std::uint64_t>(scale));
  }
  template <typename Coefficient, typename Scale>
    requires(std::floating_point<std::remove_cvref_t<Coefficient>> ||
             std::floating_point<std::remove_cvref_t<Scale>>)
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient, Scale) = delete;
  [[nodiscard]] static Result<FixedPoint> parse_ascii(std::string_view text);

  [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return coefficient_; }
  [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return scale_; }
  [[nodiscard]] std::string to_string() const;

  // Addition and subtraction stay exact. Operations that may discard precision require the caller
  // to provide a target scale and explicit rounding policy.
  [[nodiscard]] Result<FixedPoint> checked_add(FixedPoint other) const;
  [[nodiscard]] Result<FixedPoint> checked_subtract(FixedPoint other) const;

  // Keep a target scale in its source type until std::in_range rejects negative signed or
  // unrepresentable wide values with InvalidScale; only normalized uint64_t values cross into the
  // out-of-line arithmetic implementation.
  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> rescale(Scale target_scale, RoundingMode rounding) const {
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    return rescale_validated(static_cast<std::uint64_t>(target_scale), rounding);
  }

  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint other, Scale target_scale,
                                            RoundingMode rounding) const {
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    return multiply_validated(other, static_cast<std::uint64_t>(target_scale), rounding);
  }

  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint divisor, Scale target_scale,
                                          RoundingMode rounding) const {
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    return divide_validated(divisor, static_cast<std::uint64_t>(target_scale), rounding);
  }

  // Interesting syntax: deleting only floating-point scale overloads provides a compile-time
  // precision firewall while valid wide integer scales still reach checked runtime validation.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> rescale(Scale, RoundingMode) const = delete;
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint, Scale, RoundingMode) const = delete;
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint, Scale, RoundingMode) const = delete;

  // Increment checks and quantization operate on exact decimal multiples, including cross-scale
  // values, without converting through binary floating point.
  [[nodiscard]] Result<bool> is_multiple_of(FixedPoint increment) const;
  [[nodiscard]] Result<FixedPoint> quantize(FixedPoint increment, RoundingMode rounding) const;

  // Canonical equality is structural; ordering remains exact even when operands use different
  // scales.
  friend bool operator==(FixedPoint lhs, FixedPoint rhs) noexcept;
  friend std::strong_ordering operator<=>(FixedPoint lhs, FixedPoint rhs) noexcept;

private:
  explicit constexpr FixedPoint(std::int64_t coefficient, std::uint8_t scale) noexcept
      : coefficient_{coefficient}, scale_{scale} {}

  // Public templates normalize source types; these fixed-width helpers enforce decimal-scale and
  // rounding semantics without multiplying implementation overloads for every integer type.
  [[nodiscard]] static FixedPoint canonical(std::int64_t coefficient, std::uint8_t scale) noexcept;
  [[nodiscard]] static Result<FixedPoint> from_validated_scaled(std::int64_t coefficient,
                                                                std::uint64_t scale);
  [[nodiscard]] Result<FixedPoint> rescale_validated(std::uint64_t target_scale,
                                                     RoundingMode rounding) const;
  [[nodiscard]] Result<FixedPoint> multiply_validated(FixedPoint other, std::uint64_t target_scale,
                                                      RoundingMode rounding) const;
  [[nodiscard]] Result<FixedPoint> divide_validated(FixedPoint divisor, std::uint64_t target_scale,
                                                    RoundingMode rounding) const;

  std::int64_t coefficient_;
  std::uint8_t scale_;
};

namespace detail {

struct PriceTag {
  static constexpr std::string_view field = "price";
};
struct QuantityTag {
  static constexpr std::string_view field = "quantity";
};
struct NotionalTag {
  static constexpr std::string_view field = "notional";
};

// Interesting syntax: the tag parameter creates distinct Price, Quantity, and Notional types while
// sharing one implementation, so unrelated financial dimensions cannot convert implicitly.
template <typename Tag> class DecimalValue {
public:
  // Factories delegate validation to FixedPoint and replace its generic error field with the
  // nominal domain that the caller supplied.
  template <CheckedIntegerInput Coefficient, CheckedIntegerInput Scale>
  [[nodiscard]] static Result<DecimalValue> from_scaled(Coefficient coefficient, Scale scale) {
    auto value = FixedPoint::from_scaled(coefficient, scale);
    if (!value) {
      auto error = value.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    return Result<DecimalValue>::success(DecimalValue{value.value()});
  }

  // Repeat the kernel's floating-point firewall at the nominal API boundary so invalid calls cannot
  // silently shed their Price, Quantity, or Notional context through conversion.
  template <typename Coefficient, typename Scale>
    requires(std::floating_point<std::remove_cvref_t<Coefficient>> ||
             std::floating_point<std::remove_cvref_t<Scale>>)
  [[nodiscard]] static Result<DecimalValue> from_scaled(Coefficient, Scale) = delete;

  [[nodiscard]] static Result<DecimalValue> parse_ascii(std::string_view text) {
    auto value = FixedPoint::parse_ascii(text);
    if (!value) {
      auto error = value.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    return Result<DecimalValue>::success(DecimalValue{value.value()});
  }

  [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return value_.coefficient(); }
  [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return value_.scale(); }
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }

  // Arithmetic preserves the nominal wrapper and consistently remaps kernel failures to its tag.
  [[nodiscard]] Result<DecimalValue> checked_add(DecimalValue other) const {
    return wrap(value_.checked_add(other.value_));
  }

  [[nodiscard]] Result<DecimalValue> checked_subtract(DecimalValue other) const {
    return wrap(value_.checked_subtract(other.value_));
  }

  // Forward the original integral scale type so the kernel, rather than an implicit conversion in
  // the nominal wrapper, owns negative and out-of-range rejection.
  template <CheckedIntegerInput Scale>
  [[nodiscard]] Result<DecimalValue> rescale(Scale target_scale, RoundingMode rounding) const {
    return wrap(value_.rescale(target_scale, rounding));
  }

  // Nominal rescaling rejects binary floating-point targets for the same exactness reason.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<DecimalValue> rescale(Scale, RoundingMode) const = delete;

  [[nodiscard]] Result<bool> is_multiple_of(DecimalValue increment) const {
    auto result = value_.is_multiple_of(increment.value_);
    if (!result) {
      auto error = result.error();
      error.context.field = std::string{Tag::field};
      return Result<bool>::failure(std::move(error));
    }
    return result;
  }

  [[nodiscard]] Result<DecimalValue> quantize(DecimalValue increment, RoundingMode rounding) const {
    return wrap(value_.quantize(increment.value_, rounding));
  }

  // Interesting syntax: hidden friends are found by argument-dependent lookup only for matching
  // DecimalValue instantiations, keeping cross-domain comparisons out of overload resolution.
  friend bool operator==(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend std::strong_ordering operator<=>(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

private:
  explicit constexpr DecimalValue(FixedPoint value) noexcept : value_{value} {}

  // Preserve the kernel error code while making its field identify the public nominal domain.
  [[nodiscard]] static Result<DecimalValue> wrap(Result<FixedPoint> result) {
    if (!result) {
      auto error = result.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    return Result<DecimalValue>::success(DecimalValue{result.value()});
  }

  [[nodiscard]] constexpr FixedPoint fixed_point() const noexcept { return value_; }

  FixedPoint value_;

  // Interesting syntax: narrowly scoped friendship lets metadata perform its declared unit
  // conversion without making the underlying arithmetic value public to ordinary callers.
  friend class ::aegis::model::InstrumentMetadata;
};

} // namespace detail

using Price = detail::DecimalValue<detail::PriceTag>;
using Quantity = detail::DecimalValue<detail::QuantityTag>;
using Notional = detail::DecimalValue<detail::NotionalTag>;

} // namespace aegis::model
