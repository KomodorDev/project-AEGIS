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

// A restart namespace contributes 128 opaque bits to every generated ID. It is comparable and
// printable for evidence, but only providers decide how it is sourced.
class OrderNamespace {
public:
  static constexpr std::size_t byte_size = 16U;
  using Bytes = std::array<std::uint8_t, byte_size>;

  explicit constexpr OrderNamespace(Bytes bytes) noexcept : bytes_{bytes} {}

  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;

  friend constexpr bool operator==(const OrderNamespace&, const OrderNamespace&) = default;
  friend constexpr auto operator<=>(const OrderNamespace&, const OrderNamespace&) = default;

private:
  Bytes bytes_;
};

namespace detail {
// Interesting syntax: a forward-declared friend lets tests inject entropy outcomes through a narrow
// proxy without making the production entropy callback part of the public API.
struct ProductionOrderIdProviderTestAccess;
} // namespace detail

class DeterministicOrderIdProvider;
class ProductionOrderIdProvider;

// The canonical 24-byte identity is namespace bytes followed by an unsigned 64-bit counter. Its
// private factory prevents callers from inventing alternate encodings.
class OrderId {
public:
  static constexpr std::size_t byte_size = OrderNamespace::byte_size + sizeof(std::uint64_t);
  using Bytes = std::array<std::uint8_t, byte_size>;

  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;

  friend constexpr bool operator==(const OrderId&, const OrderId&) = default;
  friend constexpr auto operator<=>(const OrderId&, const OrderId&) = default;

private:
  explicit constexpr OrderId(Bytes bytes) noexcept : bytes_{bytes} {}

  [[nodiscard]] static OrderId from_parts(const OrderNamespace& order_namespace,
                                          std::uint64_t counter) noexcept;

  friend class DeterministicOrderIdProvider;
  friend class ProductionOrderIdProvider;

  Bytes bytes_;
};

// Interesting syntax: providers are deliberately move-only so one counter stream cannot be copied
// into two owners that could emit the same identity.
class OrderIdProvider {
public:
  OrderIdProvider(const OrderIdProvider&) = delete;
  OrderIdProvider& operator=(const OrderIdProvider&) = delete;
  OrderIdProvider(OrderIdProvider&&) = default;
  OrderIdProvider& operator=(OrderIdProvider&&) = default;
  virtual ~OrderIdProvider() = default;

  [[nodiscard]] virtual Result<OrderId> next() = 0;

protected:
  OrderIdProvider() = default;
};

// Deterministic construction exposes namespace and initial counter for replay tests, then emits the
// final uint64 counter exactly once before entering a permanent exhausted state.
class DeterministicOrderIdProvider final : public OrderIdProvider {
public:
  [[nodiscard]] static Result<DeterministicOrderIdProvider> create(OrderNamespace order_namespace) {
    return create_validated(order_namespace, 1U);
  }

  // Preserve an authored counter's sign and width through std::in_range. Bool is not a counter, and
  // negative or unrepresentable values fail with the stable order_counter field before conversion.
  template <detail::CheckedIntegerInput Counter>
  [[nodiscard]] static Result<DeterministicOrderIdProvider> create(OrderNamespace order_namespace,
                                                                   Counter initial_counter) {
    if (!std::in_range<std::uint64_t>(initial_counter)) {
      return Result<DeterministicOrderIdProvider>::failure(
          DomainError::at_field(DomainErrorCode::InvalidValue, "order_counter"));
    }
    return create_validated(order_namespace, static_cast<std::uint64_t>(initial_counter));
  }

  DeterministicOrderIdProvider(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider& operator=(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider(DeterministicOrderIdProvider&& other) noexcept;
  DeterministicOrderIdProvider& operator=(DeterministicOrderIdProvider&& other) noexcept;

  [[nodiscard]] Result<OrderId> next() override;

private:
  [[nodiscard]] static Result<DeterministicOrderIdProvider>
  create_validated(OrderNamespace order_namespace, std::uint64_t initial_counter);

  DeterministicOrderIdProvider(OrderNamespace order_namespace,
                               std::uint64_t initial_counter) noexcept;

  friend class ProductionOrderIdProvider;

  OrderNamespace namespace_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

// Production construction supplies a fresh operating-system namespace and delegates counter
// encoding and exhaustion behavior to the same deterministic core.
class ProductionOrderIdProvider final : public OrderIdProvider {
public:
  [[nodiscard]] static Result<ProductionOrderIdProvider> create();

  ProductionOrderIdProvider(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider& operator=(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider(ProductionOrderIdProvider&&) noexcept = default;
  ProductionOrderIdProvider& operator=(ProductionOrderIdProvider&&) noexcept = default;

  [[nodiscard]] Result<OrderId> next() override { return provider_.next(); }

private:
  // The callback returns success only after filling the entire namespace; null or false is a hard
  // startup failure with no deterministic or weak-random fallback.
  using EntropyFillCallback = bool (*)(OrderNamespace::Bytes&) noexcept;

  [[nodiscard]] static Result<ProductionOrderIdProvider>
  create_with_entropy(EntropyFillCallback entropy_fill);
  explicit ProductionOrderIdProvider(OrderNamespace order_namespace) noexcept;

  friend struct detail::ProductionOrderIdProviderTestAccess;

  DeterministicOrderIdProvider provider_;
};

} // namespace aegis::model
