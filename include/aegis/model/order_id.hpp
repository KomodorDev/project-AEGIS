// Purpose: generate fixed-size order identities from a restart-specific namespace and counter.

#pragma once

#include "aegis/model/result.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace aegis::model {

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
struct ProductionOrderIdProviderTestAccess;
}

class DeterministicOrderIdProvider;
class ProductionOrderIdProvider;

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

class DeterministicOrderIdProvider final : public OrderIdProvider {
public:
  [[nodiscard]] static Result<DeterministicOrderIdProvider>
  create(OrderNamespace order_namespace, std::uint64_t initial_counter = 1U);

  DeterministicOrderIdProvider(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider& operator=(const DeterministicOrderIdProvider&) = delete;
  DeterministicOrderIdProvider(DeterministicOrderIdProvider&& other) noexcept;
  DeterministicOrderIdProvider& operator=(DeterministicOrderIdProvider&& other) noexcept;

  [[nodiscard]] Result<OrderId> next() override;

private:
  DeterministicOrderIdProvider(OrderNamespace order_namespace,
                               std::uint64_t initial_counter) noexcept;

  friend class ProductionOrderIdProvider;

  OrderNamespace namespace_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

class ProductionOrderIdProvider final : public OrderIdProvider {
public:
  [[nodiscard]] static Result<ProductionOrderIdProvider> create();

  ProductionOrderIdProvider(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider& operator=(const ProductionOrderIdProvider&) = delete;
  ProductionOrderIdProvider(ProductionOrderIdProvider&&) noexcept = default;
  ProductionOrderIdProvider& operator=(ProductionOrderIdProvider&&) noexcept = default;

  [[nodiscard]] Result<OrderId> next() override { return provider_.next(); }

private:
  using EntropyFillCallback = bool (*)(OrderNamespace::Bytes&) noexcept;

  [[nodiscard]] static Result<ProductionOrderIdProvider>
  create_with_entropy(EntropyFillCallback entropy_fill);
  explicit ProductionOrderIdProvider(OrderNamespace order_namespace) noexcept;

  friend struct detail::ProductionOrderIdProviderTestAccess;

  DeterministicOrderIdProvider provider_;
};

} // namespace aegis::model
