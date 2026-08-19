// Purpose: define fixed-size namespace-plus-counter order identities and their providers;
// production namespaces come only from fail-closed operating-system entropy.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/result.hpp"

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace aegis::model {

// ########################################################################
// A restart namespace contributes 128 opaque bits to every generated ID. It is comparable and
// printable for evidence, but only providers decide how it is sourced.
class OrderNamespace {
public:

  // ########################################################################
  // This fixed-width byte alias makes namespace size part of the public type contract.
  static constexpr std::size_t byte_size = 16U;
  using Bytes = std::array<std::uint8_t, byte_size>;

  // ########################################################################

  // --------------------------------------------------------
  // Own one complete fixed-width restart namespace.
  explicit constexpr OrderNamespace(Bytes bytes) noexcept : bytes_{bytes} {}

  // --------------------------------------------------------
  // Borrow the fixed-width binary namespace.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render the same namespace in fixed-width hexadecimal form.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Compare namespace identities structurally and lexicographically.
  friend constexpr bool operator==(const OrderNamespace&, const OrderNamespace&) = default;
  friend constexpr auto operator<=>(const OrderNamespace&, const OrderNamespace&) = default;

  // --------------------------------------------------------
private:
  Bytes bytes_;
};

// ########################################################################
namespace detail {

// ########################################################################
// Interesting syntax: a forward-declared friend lets tests inject entropy outcomes through a narrow
// proxy without making the production entropy callback part of the public API.
struct ProductionOrderIdProviderTestAccess;

// ########################################################################
} // namespace detail

// ########################################################################
// The deterministic provider is a trusted factory for canonical order identities.
class DeterministicOrderIdProvider;

// ########################################################################
// The production provider is a trusted factory backed by operating-system entropy.
class ProductionOrderIdProvider;

// ########################################################################
// The canonical 24-byte identity is namespace bytes followed by an unsigned 64-bit counter. Its
// private factory prevents callers from inventing alternate encodings.
class OrderId {
public:

  // ########################################################################
  // This fixed-width byte alias makes the canonical identity size part of the public type contract.
  static constexpr std::size_t byte_size = OrderNamespace::byte_size + sizeof(std::uint64_t);
  using Bytes = std::array<std::uint8_t, byte_size>;

  // ########################################################################

  // --------------------------------------------------------
  // Borrow the complete canonical identity bytes.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render the same identity in fixed-width hexadecimal form.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Compare complete canonical identities structurally and lexicographically.
  friend constexpr bool operator==(const OrderId&, const OrderId&) = default;
  friend constexpr auto operator<=>(const OrderId&, const OrderId&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Restrict raw-byte construction to the trusted provider factories.
  explicit constexpr OrderId(Bytes bytes) noexcept : bytes_{bytes} {}

  // --------------------------------------------------------
  // Encode one namespace/counter pair into the single canonical byte layout.
  [[nodiscard]] static OrderId from_parts(const OrderNamespace& order_namespace,
                                          std::uint64_t counter) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // Only the two provider implementations may mint canonical order identities.
  friend class DeterministicOrderIdProvider;
  friend class ProductionOrderIdProvider;

  // ########################################################################

  Bytes bytes_;
};

// ########################################################################
// Interesting syntax: providers are deliberately move-only so one counter stream cannot be copied
// into two owners that could emit the same identity.
class OrderIdProvider {
public:

  // --------------------------------------------------------
  // Prevent copying while permitting one counter stream to transfer between owners.
  OrderIdProvider(const OrderIdProvider&) = delete;
  OrderIdProvider& operator=(const OrderIdProvider&) = delete;
  OrderIdProvider(OrderIdProvider&&) = default;
  OrderIdProvider& operator=(OrderIdProvider&&) = default;
  virtual ~OrderIdProvider() = default;

  // --------------------------------------------------------
  // Generate the next identity or report stable counter exhaustion.
  [[nodiscard]] virtual Result<OrderId> next() = 0;

  // --------------------------------------------------------
protected:

  // --------------------------------------------------------
  // Allow construction only as the base subobject of a concrete provider.
  OrderIdProvider() = default;

  // --------------------------------------------------------
};

// ########################################################################
// Deterministic construction exposes namespace and initial counter for replay tests, then emits the
// final uint64 counter exactly once before entering a permanent exhausted state.
class DeterministicOrderIdProvider final : public OrderIdProvider {
public:

  // --------------------------------------------------------
  // Create a deterministic stream beginning at the assigned initial counter of one.
  [[nodiscard]] static Result<DeterministicOrderIdProvider> create(OrderNamespace order_namespace) {
    return create_validated(order_namespace, 1U);
  }

  // --------------------------------------------------------
  // Preserve an authored counter's sign and width through std::in_range. CheckedIntegerInput
  // excludes bool, enum, floating, and plain/wide/Unicode character sources; signed/unsigned char
  // remain supported, while negative or unrepresentable values fail at order_counter before
  // conversion.
  template <detail::CheckedIntegerInput Counter>
  [[nodiscard]] static Result<DeterministicOrderIdProvider> create(OrderNamespace order_namespace,
                                                                   Counter initial_counter) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject negative or unrepresentable authored counters before conversion.
    if (!std::in_range<std::uint64_t>(initial_counter)) {
      return Result<DeterministicOrderIdProvider>::failure(
          DomainError::at_field(DomainErrorCode::InvalidValue, "order_counter"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Delegate only a validated fixed-width counter to the stable constructor boundary.
    return create_validated(order_namespace, static_cast<std::uint64_t>(initial_counter));

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Keep deterministic streams single-owner while allowing explicit ownership transfer.
  DeterministicOrderIdProvider(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider& operator=(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider(DeterministicOrderIdProvider&& other) noexcept;
  DeterministicOrderIdProvider& operator=(DeterministicOrderIdProvider&& other) noexcept;

  // --------------------------------------------------------
  // Emit the current counter exactly once and advance, or preserve permanent exhaustion.
  [[nodiscard]] Result<OrderId> next() override;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Enforce counter-domain semantics after public source-width validation.
  [[nodiscard]] static Result<DeterministicOrderIdProvider>
  create_validated(OrderNamespace order_namespace, std::uint64_t initial_counter);

  // --------------------------------------------------------
  // Assemble one provider whose namespace and initial counter are already valid.
  DeterministicOrderIdProvider(OrderNamespace order_namespace,
                               std::uint64_t initial_counter) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // The production wrapper may transfer a validated entropy namespace into this shared core.
  friend class ProductionOrderIdProvider;

  // ########################################################################

  OrderNamespace namespace_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

// ########################################################################
// Production construction supplies a fresh operating-system namespace and delegates counter
// encoding and exhaustion behavior to the same deterministic core.
class ProductionOrderIdProvider final : public OrderIdProvider {
public:

  // --------------------------------------------------------
  // Create a production stream only after obtaining a complete OS-entropy namespace.
  [[nodiscard]] static Result<ProductionOrderIdProvider> create();

  // --------------------------------------------------------
  // Keep production streams single-owner while allowing explicit ownership transfer.
  ProductionOrderIdProvider(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider& operator=(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider(ProductionOrderIdProvider&&) noexcept = default;
  ProductionOrderIdProvider& operator=(ProductionOrderIdProvider&&) noexcept = default;

  // --------------------------------------------------------
  // Delegate generation and exhaustion behavior to the shared deterministic core.
  [[nodiscard]] Result<OrderId> next() override { return provider_.next(); }

  // --------------------------------------------------------
private:

  // ########################################################################
  // The callback returns success only after filling the entire namespace; null or false is a hard
  // startup failure with no deterministic or weak-random fallback.
  using EntropyFillCallback = bool (*)(OrderNamespace::Bytes&) noexcept;

  // ########################################################################

  // --------------------------------------------------------
  // Exercise the fail-closed entropy boundary through the private production/test seam.
  [[nodiscard]] static Result<ProductionOrderIdProvider>
  create_with_entropy(EntropyFillCallback entropy_fill);

  // --------------------------------------------------------
  // Transfer one successfully sourced namespace into the shared counter provider.
  explicit ProductionOrderIdProvider(OrderNamespace order_namespace) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // The narrow test proxy may exercise private entropy outcomes without exposing them publicly.
  friend struct detail::ProductionOrderIdProviderTestAccess;

  // ########################################################################

  DeterministicOrderIdProvider provider_;
};

// ########################################################################
} // namespace aegis::model
