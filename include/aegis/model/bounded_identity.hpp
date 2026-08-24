// Purpose: provide neutral fixed-storage mechanics for component-owned binary identities and
// one-based ordinals without assigning OMS or recovery meaning in the model layer.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::model {
namespace detail {

// --------------------------------------------------------
// Encode one nonzero counter in canonical unsigned big-endian order after fixed prefix bytes.
template <std::size_t PrefixSize, std::size_t TotalSize>
constexpr void append_identity_counter(std::array<std::uint8_t, TotalSize>& output,
                                       const std::array<std::uint8_t, PrefixSize>& prefix,
                                       std::uint64_t counter) noexcept {
  std::copy(prefix.begin(), prefix.end(), output.begin());
  for (std::size_t index = 0U; index < sizeof(counter); ++index) {
    const auto shift = static_cast<unsigned int>((sizeof(counter) - 1U - index) * 8U);
    output[PrefixSize + index] = static_cast<std::uint8_t>((counter >> shift) & 0xffU);
  }
}

// --------------------------------------------------------

} // namespace detail

// ########################################################################
// Tag traits supply component-owned field/error meaning while this kernel retains only bounded
// bytes. One through 128 bytes is the accepted M4 opaque-identity profile.
template <typename Tag> class BoundedOpaqueIdentity final {
public:
  static constexpr std::size_t maximum_byte_size = 128U;
  using Storage = std::array<std::byte, maximum_byte_size>;

  // --------------------------------------------------------
  // Validate length and copy every byte, including embedded zero bytes, into fixed inline storage.
  [[nodiscard]] static Result<BoundedOpaqueIdentity> from_bytes(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > maximum_byte_size) {
      return Result<BoundedOpaqueIdentity>::failure(
          DomainError::at_field(Tag::invalid_code, std::string{Tag::field}));
    }
    return Result<BoundedOpaqueIdentity>::success(BoundedOpaqueIdentity{bytes});
  }

  // --------------------------------------------------------
  // Borrow only retained active bytes and hide zeroed inline tail storage.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {bytes_.data(), static_cast<std::size_t>(size_)};
  }

  // --------------------------------------------------------
  // Compare exact semantic bytes rather than padding or unused inline tail.
  friend bool operator==(const BoundedOpaqueIdentity& lhs,
                         const BoundedOpaqueIdentity& rhs) noexcept {
    return lhs.bytes().size() == rhs.bytes().size() &&
           std::equal(lhs.bytes().begin(), lhs.bytes().end(), rhs.bytes().begin());
  }

  // --------------------------------------------------------
  // Canonical ordering is lexicographic over active bytes and then active length.
  friend std::strong_ordering operator<=>(const BoundedOpaqueIdentity& lhs,
                                          const BoundedOpaqueIdentity& rhs) noexcept {
    const auto common = std::min(lhs.bytes().size(), rhs.bytes().size());
    for (std::size_t index = 0U; index < common; ++index) {
      const auto left = std::to_integer<std::uint8_t>(lhs.bytes_[index]);
      const auto right = std::to_integer<std::uint8_t>(rhs.bytes_[index]);
      if (left < right) {
        return std::strong_ordering::less;
      }
      if (left > right) {
        return std::strong_ordering::greater;
      }
    }
    return lhs.bytes().size() <=> rhs.bytes().size();
  }

  // --------------------------------------------------------
  // Hide raw construction so every observable value has already passed the length contract.
private:

  // --------------------------------------------------------
  // Copy a previously validated active span and preserve zero-filled unused storage.
  explicit BoundedOpaqueIdentity(std::span<const std::byte> bytes) noexcept
      : size_{static_cast<std::uint8_t>(bytes.size())} {
    std::copy(bytes.begin(), bytes.end(), bytes_.begin());
  }

  // --------------------------------------------------------
  // Retain bytes and active length together so padding never participates in identity.
  Storage bytes_{};
  std::uint8_t size_{};
};

// ########################################################################

// ########################################################################
// Fixed opaque identities preserve exact injected bytes while the component tag prevents mixing.
template <typename Tag, std::size_t Size> class FixedOpaqueIdentity final {
public:
  static constexpr std::size_t byte_size = Size;
  using Bytes = std::array<std::uint8_t, byte_size>;

  // --------------------------------------------------------
  // Accept an exact fixed-width component value without applying opaque-variable bounds.
  explicit constexpr FixedOpaqueIdentity(Bytes bytes) noexcept : bytes_{bytes} {}

  // --------------------------------------------------------
  // Expose all fixed semantic bytes for canonical sorting and recovery retention.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Fixed values compare every byte in canonical array order.
  friend constexpr bool operator==(const FixedOpaqueIdentity&,
                                   const FixedOpaqueIdentity&) = default;
  friend constexpr auto operator<=>(const FixedOpaqueIdentity&,
                                    const FixedOpaqueIdentity&) = default;

  // --------------------------------------------------------
  // Prevent default or partial construction of a fixed identity.
private:
  // Retain exactly the component-defined width without object-memory encoding.
  Bytes bytes_;
};

// ########################################################################

// ########################################################################
// Namespace-counter identities share the OrderId byte profile but preserve component-owned tags.
template <typename Tag> class NamespaceCounterIdentity final {
public:
  static constexpr std::size_t byte_size = OrderNamespace::byte_size + sizeof(std::uint64_t);
  using Bytes = std::array<std::uint8_t, byte_size>;
  static constexpr std::string_view field = Tag::field;
  static constexpr DomainErrorCode exhaustion_code = Tag::exhaustion_code;

  // --------------------------------------------------------
  // Validate a type-preserving authored counter before encoding its exact namespace prefix.
  template <detail::CheckedIntegerInput Counter>
  [[nodiscard]] static Result<NamespaceCounterIdentity>
  from_parts(const OrderNamespace& order_namespace, Counter counter) {
    if (!std::in_range<std::uint64_t>(counter) || counter == 0) {
      return Result<NamespaceCounterIdentity>::failure(
          DomainError::at_field(Tag::invalid_code, std::string{Tag::field}));
    }
    const auto validated_counter = static_cast<std::uint64_t>(counter);
    Bytes bytes{};
    detail::append_identity_counter<OrderNamespace::byte_size>(bytes, order_namespace.bytes(),
                                                               validated_counter);
    return Result<NamespaceCounterIdentity>::success(
        NamespaceCounterIdentity{order_namespace, validated_counter, bytes});
  }

  // --------------------------------------------------------
  // Expose the canonical 24-byte projection used for evidence and deterministic keys.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Preserve the typed namespace so recovery never has to decode packed object bytes.
  [[nodiscard]] constexpr const OrderNamespace& order_namespace() const noexcept {
    return order_namespace_;
  }

  // --------------------------------------------------------
  // Preserve the validated numeric component for high-water checks and non-reuse proofs.
  [[nodiscard]] constexpr std::uint64_t counter() const noexcept { return counter_; }

  // --------------------------------------------------------
  // Equality and order include both semantic components and their canonical projection.
  friend constexpr bool operator==(const NamespaceCounterIdentity&,
                                   const NamespaceCounterIdentity&) = default;
  friend constexpr auto operator<=>(const NamespaceCounterIdentity&,
                                    const NamespaceCounterIdentity&) = default;

  // --------------------------------------------------------
  // Restrict construction to the checked component factory.
private:

  // --------------------------------------------------------
  // Store typed components beside their already-derived canonical bytes.
  explicit constexpr NamespaceCounterIdentity(OrderNamespace order_namespace, std::uint64_t counter,
                                              Bytes bytes) noexcept
      : order_namespace_{order_namespace}, counter_{counter}, bytes_{bytes} {}

  // --------------------------------------------------------
  // Retain every semantic component so downstream code never slices the byte projection.
  OrderNamespace order_namespace_;
  std::uint64_t counter_;
  Bytes bytes_;
};

// ########################################################################

// ########################################################################
// One move-only provider owns a namespace-counter stream and enters sticky exhaustion immediately
// after publishing UINT64_MAX, preventing copies or moved-from objects from duplicating identities.
template <typename Identity> class NamespaceCounterIdentityProvider final {
public:

  // --------------------------------------------------------
  // Begin an ordinary identity stream at the accepted first counter.
  [[nodiscard]] static Result<NamespaceCounterIdentityProvider>
  create(OrderNamespace order_namespace) {
    return create(order_namespace, 1U);
  }

  // --------------------------------------------------------
  // Restore or inject a checked nonzero high-water successor without implicit signed conversion.
  template <detail::CheckedIntegerInput Counter>
  [[nodiscard]] static Result<NamespaceCounterIdentityProvider>
  create(OrderNamespace order_namespace, Counter initial_counter) {
    const auto validation = Identity::from_parts(order_namespace, initial_counter);
    if (!validation) {
      return Result<NamespaceCounterIdentityProvider>::failure(validation.error());
    }
    return Result<NamespaceCounterIdentityProvider>::success(NamespaceCounterIdentityProvider{
        order_namespace, static_cast<std::uint64_t>(initial_counter)});
  }

  // --------------------------------------------------------
  // A counter owner is unique; copying would duplicate future identities.
  NamespaceCounterIdentityProvider(const NamespaceCounterIdentityProvider&) = delete;
  NamespaceCounterIdentityProvider& operator=(const NamespaceCounterIdentityProvider&) = delete;

  // --------------------------------------------------------
  // Transfer the stream once and poison the moved-from provider before it can issue again.
  NamespaceCounterIdentityProvider(NamespaceCounterIdentityProvider&& other) noexcept
      : order_namespace_{other.order_namespace_}, next_counter_{other.next_counter_},
        exhausted_{other.exhausted_} {
    other.exhausted_ = true;
  }

  // --------------------------------------------------------
  // Replace stream ownership while leaving the source permanently exhausted.
  NamespaceCounterIdentityProvider& operator=(NamespaceCounterIdentityProvider&& other) noexcept {
    if (this != &other) {
      order_namespace_ = other.order_namespace_;
      next_counter_ = other.next_counter_;
      exhausted_ = other.exhausted_;
      other.exhausted_ = true;
    }
    return *this;
  }

  // --------------------------------------------------------
  // Publish the current value once, then advance or enter sticky terminal exhaustion.
  [[nodiscard]] Result<Identity> next() {
    if (exhausted_) {
      return Result<Identity>::failure(
          DomainError::at_field(Identity::exhaustion_code, std::string{Identity::field}));
    }
    auto identity = Identity::from_parts(order_namespace_, next_counter_);
    if (next_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      exhausted_ = true;
    } else {
      ++next_counter_;
    }
    return identity;
  }

  // --------------------------------------------------------
  // Only validated factories may establish a provider's next value.
private:

  // --------------------------------------------------------
  // Retain the fixed namespace and first not-yet-issued counter without allocating.
  NamespaceCounterIdentityProvider(OrderNamespace order_namespace,
                                   std::uint64_t initial_counter) noexcept
      : order_namespace_{order_namespace}, next_counter_{initial_counter} {}

  // --------------------------------------------------------
  // The exhaustion latch distinguishes a published UINT64_MAX from an unused counter.
  OrderNamespace order_namespace_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

// ########################################################################

// ########################################################################
// Component-tagged ordinals reject zero and report the tag's exact exhaustion code before wrap.
template <typename Tag> class OneBasedComponentOrdinal final {
public:

  // --------------------------------------------------------
  // Validate source integer width and reject zero before narrowing the ordinal.
  template <detail::CheckedIntegerInput Value>
  [[nodiscard]] static Result<OneBasedComponentOrdinal> from_value(Value value) {
    if (!std::in_range<std::uint64_t>(value) || value == 0) {
      return Result<OneBasedComponentOrdinal>::failure(
          DomainError::at_field(Tag::invalid_code, std::string{Tag::field}));
    }
    return Result<OneBasedComponentOrdinal>::success(
        OneBasedComponentOrdinal{static_cast<std::uint64_t>(value)});
  }

  // --------------------------------------------------------
  // Expose the typed value for sequence comparisons and canonical encoding.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Advance once or report the component-specific error before unsigned wrap.
  [[nodiscard]] Result<OneBasedComponentOrdinal> next() const {
    if (value_ == std::numeric_limits<std::uint64_t>::max()) {
      return Result<OneBasedComponentOrdinal>::failure(
          DomainError::at_field(Tag::exhaustion_code, std::string{Tag::field}));
    }
    return Result<OneBasedComponentOrdinal>::success(OneBasedComponentOrdinal{value_ + 1U});
  }

  // --------------------------------------------------------
  // Ordinal equality and order use only the accepted one-based value.
  friend constexpr bool operator==(OneBasedComponentOrdinal, OneBasedComponentOrdinal) = default;
  friend constexpr auto operator<=>(OneBasedComponentOrdinal, OneBasedComponentOrdinal) = default;

  // --------------------------------------------------------
  // Restrict direct construction to checked factories and next-value derivation.
private:

  // --------------------------------------------------------
  // Store a value already proven nonzero and representable.
  explicit constexpr OneBasedComponentOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  // One unsigned word is the complete semantic ordinal state.
  std::uint64_t value_;
};

// ########################################################################

} // namespace aegis::model
