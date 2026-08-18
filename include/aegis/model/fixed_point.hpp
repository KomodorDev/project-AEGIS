// Purpose: provide exact, checked decimal values for prices, quantities, and notionals.

#pragma once

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

enum class RoundingMode : std::uint8_t {
  Exact = 0,
  TowardZero = 1,
  AwayFromZero = 2,
  Floor = 3,
  Ceiling = 4,
  NearestTiesToEven = 5,
};

// This representation is the arithmetic kernel. Decision-path APIs use the nominal wrappers below.
class FixedPoint {
public:
  static constexpr std::uint8_t maximum_scale = 18U;

  template <std::integral Coefficient>
    requires(!std::same_as<std::remove_cvref_t<Coefficient>, bool>)
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient coefficient,
                                                      std::uint64_t scale) {
    if (!std::in_range<std::int64_t>(coefficient)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    return from_validated_scaled(static_cast<std::int64_t>(coefficient), scale);
  }
  template <typename Coefficient, typename Scale>
    requires(std::floating_point<std::remove_cvref_t<Coefficient>> ||
             std::floating_point<std::remove_cvref_t<Scale>>)
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient, Scale) = delete;
  [[nodiscard]] static Result<FixedPoint> parse_ascii(std::string_view text);

  [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return coefficient_; }
  [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return scale_; }
  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] Result<FixedPoint> checked_add(FixedPoint other) const;
  [[nodiscard]] Result<FixedPoint> checked_subtract(FixedPoint other) const;
  [[nodiscard]] Result<FixedPoint> rescale(std::uint64_t target_scale, RoundingMode rounding) const;
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint other, std::uint64_t target_scale,
                                            RoundingMode rounding) const;
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint divisor, std::uint64_t target_scale,
                                          RoundingMode rounding) const;
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> rescale(Scale, RoundingMode) const = delete;
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint, Scale, RoundingMode) const = delete;
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint, Scale, RoundingMode) const = delete;
  [[nodiscard]] Result<bool> is_multiple_of(FixedPoint increment) const;
  [[nodiscard]] Result<FixedPoint> quantize(FixedPoint increment, RoundingMode rounding) const;

  friend bool operator==(FixedPoint lhs, FixedPoint rhs) noexcept;
  friend std::strong_ordering operator<=>(FixedPoint lhs, FixedPoint rhs) noexcept;

private:
  explicit constexpr FixedPoint(std::int64_t coefficient, std::uint8_t scale) noexcept
      : coefficient_{coefficient}, scale_{scale} {}

  [[nodiscard]] static FixedPoint canonical(std::int64_t coefficient, std::uint8_t scale) noexcept;
  [[nodiscard]] static Result<FixedPoint> from_validated_scaled(std::int64_t coefficient,
                                                                std::uint64_t scale);

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

template <typename Tag> class DecimalValue {
public:
  template <std::integral Coefficient>
    requires(!std::same_as<std::remove_cvref_t<Coefficient>, bool>)
  [[nodiscard]] static Result<DecimalValue> from_scaled(Coefficient coefficient,
                                                        std::uint64_t scale) {
    auto value = FixedPoint::from_scaled(coefficient, scale);
    if (!value) {
      auto error = value.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    return Result<DecimalValue>::success(DecimalValue{value.value()});
  }

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

  [[nodiscard]] Result<DecimalValue> checked_add(DecimalValue other) const {
    return wrap(value_.checked_add(other.value_));
  }

  [[nodiscard]] Result<DecimalValue> checked_subtract(DecimalValue other) const {
    return wrap(value_.checked_subtract(other.value_));
  }

  [[nodiscard]] Result<DecimalValue> rescale(std::uint64_t target_scale,
                                             RoundingMode rounding) const {
    return wrap(value_.rescale(target_scale, rounding));
  }

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

  friend bool operator==(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend std::strong_ordering operator<=>(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }

private:
  explicit constexpr DecimalValue(FixedPoint value) noexcept : value_{value} {}

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

  friend class ::aegis::model::InstrumentMetadata;
};

} // namespace detail

using Price = detail::DecimalValue<detail::PriceTag>;
using Quantity = detail::DecimalValue<detail::QuantityTag>;
using Notional = detail::DecimalValue<detail::NotionalTag>;

} // namespace aegis::model
