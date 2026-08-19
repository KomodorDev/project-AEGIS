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
// ########################################################################
// Instrument metadata receives narrowly scoped access to nominal decimal kernels for unit
// conversion.
class InstrumentMetadata;
// ########################################################################
// Precision-changing operations never select a rounding policy implicitly.
enum class RoundingMode : std::uint8_t {
  Exact = 0,
  TowardZero = 1,
  AwayFromZero = 2,
  Floor = 3,
  Ceiling = 4,
  NearestTiesToEven = 5,
};
// ########################################################################
// Store one canonical signed coefficient and decimal scale. Decision-path APIs use the nominal
// wrappers below rather than exposing this arithmetic kernel as an interchangeable business value.
class FixedPoint {
public:
  static constexpr std::uint8_t maximum_scale = 18U;
  // --------------------------------------------------------
  // Construction accepts already-scaled integers or strict ordinary decimal text; both paths
  // preserve exactness and canonicalize redundant fractional zeros.
  // Interesting syntax: CheckedIntegerInput is deliberately narrower than std::integral, and the
  // deleted floating overload completes the compile-time firewall for ambiguous numeric sources.
  // Boolean values are not authored integers. Deducing both remaining inputs preserves their width
  // and signedness for std::in_range; conversion to the kernel representation follows both gates.
  // Plain, wide, and Unicode character types plus enums are also excluded; signed char and unsigned
  // char remain numeric integers and take the same checked path as wider standard integer types.
  template <detail::CheckedIntegerInput Coefficient, detail::CheckedIntegerInput Scale>
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient coefficient, Scale scale) {
    // ++++++++++++++++++++++++++++++++++++++++
    // Reject coefficients outside the signed kernel representation before conversion.
    if (!std::in_range<std::int64_t>(coefficient)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "fixed_point"));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable scales before conversion.
    if (!std::in_range<std::uint64_t>(scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only validated fixed-width inputs to canonical decimal construction.
    return from_validated_scaled(static_cast<std::int64_t>(coefficient),
                                 static_cast<std::uint64_t>(scale));
    // ++++++++++++++++++++++++++++++++++++++++
  }
  template <typename Coefficient, typename Scale>
    requires(std::floating_point<std::remove_cvref_t<Coefficient>> ||
             std::floating_point<std::remove_cvref_t<Scale>>)
  [[nodiscard]] static Result<FixedPoint> from_scaled(Coefficient, Scale) = delete;
  // --------------------------------------------------------
  // Parse strict ordinary decimal ASCII without passing through binary floating point.
  [[nodiscard]] static Result<FixedPoint> parse_ascii(std::string_view text);
  // --------------------------------------------------------
  // Return the canonical signed coefficient.
  [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return coefficient_; }
  // --------------------------------------------------------
  // Return the canonical decimal scale.
  [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return scale_; }
  // --------------------------------------------------------
  // Render the exact canonical decimal spelling.
  [[nodiscard]] std::string to_string() const;
  // --------------------------------------------------------
  // Add exactly without selecting a rounding policy.
  [[nodiscard]] Result<FixedPoint> checked_add(FixedPoint other) const;
  // --------------------------------------------------------
  // Subtract exactly without selecting a rounding policy.
  [[nodiscard]] Result<FixedPoint> checked_subtract(FixedPoint other) const;
  // --------------------------------------------------------
  // Keep a target scale in its source type until std::in_range rejects negative signed or
  // unrepresentable wide values with InvalidScale; only normalized uint64_t values cross into the
  // out-of-line arithmetic implementation.
  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> rescale(Scale target_scale, RoundingMode rounding) const {
    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable target scales before conversion.
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width scale to the stable arithmetic implementation.
    return rescale_validated(static_cast<std::uint64_t>(target_scale), rounding);
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Multiply exactly, then apply only the caller's explicit target scale and rounding policy.
  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint other, Scale target_scale,
                                            RoundingMode rounding) const {
    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable target scales before conversion.
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width scale to the stable arithmetic implementation.
    return multiply_validated(other, static_cast<std::uint64_t>(target_scale), rounding);
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Divide exactly, then apply only the caller's explicit target scale and rounding policy.
  template <detail::CheckedIntegerInput Scale>
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint divisor, Scale target_scale,
                                          RoundingMode rounding) const {
    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable target scales before conversion.
    if (!std::in_range<std::uint64_t>(target_scale)) {
      return Result<FixedPoint>::failure(
          DomainError::at_field(DomainErrorCode::InvalidScale, "fixed_point"));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width scale to the stable arithmetic implementation.
    return divide_validated(divisor, static_cast<std::uint64_t>(target_scale), rounding);
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Interesting syntax: deleting only floating-point scale overloads provides a compile-time
  // precision firewall while valid wide integer scales still reach checked runtime validation.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> rescale(Scale, RoundingMode) const = delete;
  // --------------------------------------------------------
  // Reject floating-point target scales at the multiply overload boundary.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> multiply(FixedPoint, Scale, RoundingMode) const = delete;
  // --------------------------------------------------------
  // Reject floating-point target scales at the divide overload boundary.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<FixedPoint> divide(FixedPoint, Scale, RoundingMode) const = delete;
  // --------------------------------------------------------
  // Test exact increment alignment across scales without binary floating point.
  [[nodiscard]] Result<bool> is_multiple_of(FixedPoint increment) const;
  // --------------------------------------------------------
  // Snap to an exact increment only under the caller's explicit rounding policy.
  [[nodiscard]] Result<FixedPoint> quantize(FixedPoint increment, RoundingMode rounding) const;
  // --------------------------------------------------------
  // Canonical equality is structural; ordering remains exact even when operands use different
  // scales.
  friend bool operator==(FixedPoint lhs, FixedPoint rhs) noexcept;
  friend std::strong_ordering operator<=>(FixedPoint lhs, FixedPoint rhs) noexcept;
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Restrict canonical kernel construction to validated factories and internal arithmetic.
  explicit constexpr FixedPoint(std::int64_t coefficient, std::uint8_t scale) noexcept
      : coefficient_{coefficient}, scale_{scale} {}
  // --------------------------------------------------------
  // Public templates normalize source types; these fixed-width helpers enforce decimal-scale and
  // rounding semantics without multiplying implementation overloads for every integer type.
  [[nodiscard]] static FixedPoint canonical(std::int64_t coefficient, std::uint8_t scale) noexcept;
  // --------------------------------------------------------
  // Validate decimal scale semantics and canonicalize an already representable coefficient.
  [[nodiscard]] static Result<FixedPoint> from_validated_scaled(std::int64_t coefficient,
                                                                std::uint64_t scale);
  // --------------------------------------------------------
  // Implement rescaling after the public source-type boundary has normalized the target scale.
  [[nodiscard]] Result<FixedPoint> rescale_validated(std::uint64_t target_scale,
                                                     RoundingMode rounding) const;
  // --------------------------------------------------------
  // Implement multiplication after the public source-type boundary has normalized the target scale.
  [[nodiscard]] Result<FixedPoint> multiply_validated(FixedPoint other, std::uint64_t target_scale,
                                                      RoundingMode rounding) const;
  // --------------------------------------------------------
  // Implement division after the public source-type boundary has normalized the target scale.
  [[nodiscard]] Result<FixedPoint> divide_validated(FixedPoint divisor, std::uint64_t target_scale,
                                                    RoundingMode rounding) const;
  // --------------------------------------------------------
  std::int64_t coefficient_;
  std::uint8_t scale_;
};
// ########################################################################
namespace detail {
// ########################################################################
// Price operations report failures through the public price field.
struct PriceTag {
  static constexpr std::string_view field = "price";
};
// ########################################################################
// Quantity operations report failures through the public quantity field.
struct QuantityTag {
  static constexpr std::string_view field = "quantity";
};
// ########################################################################
// Notional operations report failures through the public notional field.
struct NotionalTag {
  static constexpr std::string_view field = "notional";
};
// ########################################################################
// Interesting syntax: the tag parameter creates distinct Price, Quantity, and Notional types while
// sharing one implementation, so unrelated financial dimensions cannot convert implicitly.
template <typename Tag> class DecimalValue {
public:
  // --------------------------------------------------------
  // Factories delegate validation to FixedPoint and replace its generic error field with the
  // nominal domain that the caller supplied. Reusing CheckedIntegerInput keeps the nominal and
  // kernel callable surfaces identical.
  template <CheckedIntegerInput Coefficient, CheckedIntegerInput Scale>
  [[nodiscard]] static Result<DecimalValue> from_scaled(Coefficient coefficient, Scale scale) {
    // ++++++++++++++++++++++++++++++++++++++++
    // Apply the shared checked integer and canonical decimal boundary.
    auto value = FixedPoint::from_scaled(coefficient, scale);
    // ++++++++++++++++++++++++++++++++++++++++
    // Preserve the error code while naming the caller's nominal financial domain.
    if (!value) {
      auto error = value.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the validated kernel inside its nominal wrapper.
    return Result<DecimalValue>::success(DecimalValue{value.value()});
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Repeat the kernel's floating-point firewall at the nominal API boundary so invalid calls cannot
  // silently shed their Price, Quantity, or Notional context through conversion.
  template <typename Coefficient, typename Scale>
    requires(std::floating_point<std::remove_cvref_t<Coefficient>> ||
             std::floating_point<std::remove_cvref_t<Scale>>)
  [[nodiscard]] static Result<DecimalValue> from_scaled(Coefficient, Scale) = delete;
  // --------------------------------------------------------
  // Parse strict decimal ASCII and remap any failure to this nominal financial domain.
  [[nodiscard]] static Result<DecimalValue> parse_ascii(std::string_view text) {
    // ++++++++++++++++++++++++++++++++++++++++
    // Apply the shared strict decimal parser and canonicalization boundary.
    auto value = FixedPoint::parse_ascii(text);
    // ++++++++++++++++++++++++++++++++++++++++
    // Preserve the error code while naming the caller's nominal financial domain.
    if (!value) {
      auto error = value.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the validated kernel inside its nominal wrapper.
    return Result<DecimalValue>::success(DecimalValue{value.value()});
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Return the canonical signed coefficient from the wrapped kernel.
  [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return value_.coefficient(); }
  // --------------------------------------------------------
  // Return the canonical decimal scale from the wrapped kernel.
  [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return value_.scale(); }
  // --------------------------------------------------------
  // Render the exact canonical decimal spelling.
  [[nodiscard]] std::string to_string() const { return value_.to_string(); }
  // --------------------------------------------------------
  // Arithmetic preserves the nominal wrapper and consistently remaps kernel failures to its tag.
  [[nodiscard]] Result<DecimalValue> checked_add(DecimalValue other) const {
    return wrap(value_.checked_add(other.value_));
  }
  // --------------------------------------------------------
  // Subtract within one nominal domain and remap kernel failures to its public field.
  [[nodiscard]] Result<DecimalValue> checked_subtract(DecimalValue other) const {
    return wrap(value_.checked_subtract(other.value_));
  }
  // --------------------------------------------------------
  // Forward the original integral scale type so the kernel, rather than an implicit conversion in
  // the nominal wrapper, owns negative and out-of-range rejection.
  template <CheckedIntegerInput Scale>
  [[nodiscard]] Result<DecimalValue> rescale(Scale target_scale, RoundingMode rounding) const {
    return wrap(value_.rescale(target_scale, rounding));
  }
  // --------------------------------------------------------
  // Nominal rescaling rejects binary floating-point targets for the same exactness reason.
  template <typename Scale>
    requires std::floating_point<std::remove_cvref_t<Scale>>
  [[nodiscard]] Result<DecimalValue> rescale(Scale, RoundingMode) const = delete;
  // --------------------------------------------------------
  // Test exact increment alignment while remapping failures to this nominal domain.
  [[nodiscard]] Result<bool> is_multiple_of(DecimalValue increment) const {
    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate the exact cross-scale divisibility check to the shared kernel.
    auto result = value_.is_multiple_of(increment.value_);
    // ++++++++++++++++++++++++++++++++++++++++
    // Preserve the error code while naming the caller's nominal financial domain.
    if (!result) {
      auto error = result.error();
      error.context.field = std::string{Tag::field};
      return Result<bool>::failure(std::move(error));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Return the successful alignment decision unchanged.
    return result;
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Quantize within one nominal domain under an explicit rounding policy.
  [[nodiscard]] Result<DecimalValue> quantize(DecimalValue increment, RoundingMode rounding) const {
    return wrap(value_.quantize(increment.value_, rounding));
  }
  // --------------------------------------------------------
  // Interesting syntax: hidden friends are found by argument-dependent lookup only for matching
  // DecimalValue instantiations, keeping cross-domain comparisons out of overload resolution.
  friend bool operator==(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend std::strong_ordering operator<=>(DecimalValue lhs, DecimalValue rhs) noexcept {
    return lhs.value_ <=> rhs.value_;
  }
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Restrict kernel wrapping to validated factories and arithmetic helpers.
  explicit constexpr DecimalValue(FixedPoint value) noexcept : value_{value} {}
  // --------------------------------------------------------
  // Preserve the kernel error code while making its field identify the public nominal domain.
  [[nodiscard]] static Result<DecimalValue> wrap(Result<FixedPoint> result) {
    // ++++++++++++++++++++++++++++++++++++++++
    // Remap only the stable field while retaining the kernel failure code and index.
    if (!result) {
      auto error = result.error();
      error.context.field = std::string{Tag::field};
      return Result<DecimalValue>::failure(std::move(error));
    }
    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the successful kernel inside its nominal wrapper.
    return Result<DecimalValue>::success(DecimalValue{result.value()});
    // ++++++++++++++++++++++++++++++++++++++++
  }
  // --------------------------------------------------------
  // Expose the exact kernel only to the narrowly friended metadata conversion boundary.
  [[nodiscard]] constexpr FixedPoint fixed_point() const noexcept { return value_; }
  // --------------------------------------------------------
  FixedPoint value_;

  // ########################################################################
  // Interesting syntax: narrowly scoped friendship lets metadata perform its declared unit
  // conversion without making the underlying arithmetic value public to ordinary callers.
  friend class ::aegis::model::InstrumentMetadata;
  // ########################################################################
};
// ########################################################################
} // namespace detail
// ########################################################################
// Public aliases expose three non-interchangeable financial dimensions over the shared exact
// kernel.
using Price = detail::DecimalValue<detail::PriceTag>;
using Quantity = detail::DecimalValue<detail::QuantityTag>;
using Notional = detail::DecimalValue<detail::NotionalTag>;
// ########################################################################
} // namespace aegis::model
