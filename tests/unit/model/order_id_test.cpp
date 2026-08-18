// Purpose: prove order identities are deterministic in tests and safe across counter boundaries.

#include "aegis/model/order_id.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace {

using namespace aegis::model;

[[nodiscard]] OrderNamespace sequential_namespace(std::uint8_t first_byte = 0U) {
  OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(first_byte + static_cast<std::uint8_t>(index));
  }
  return OrderNamespace{bytes};
}

static_assert(OrderNamespace::byte_size == 16U);
static_assert(OrderId::byte_size == 24U);

TEST_CASE("deterministic order IDs use canonical namespace and big-endian counter bytes",
          "[model][order-id]") {
  const auto order_namespace = sequential_namespace();
  CHECK(order_namespace.to_hex() == "000102030405060708090a0b0c0d0e0f");

  auto created = DeterministicOrderIdProvider::create(order_namespace, 1U);
  REQUIRE(created);
  auto provider = std::move(created).value();

  const auto first = provider.next();
  const auto second = provider.next();
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value().to_hex() == "000102030405060708090a0b0c0d0e0f0000000000000001");
  CHECK(second.value().to_hex() == "000102030405060708090a0b0c0d0e0f0000000000000002");
}

TEST_CASE("fixed deterministic inputs reproduce identical order identities", "[model][order-id]") {
  auto first_created = DeterministicOrderIdProvider::create(sequential_namespace(), 42U);
  auto second_created = DeterministicOrderIdProvider::create(sequential_namespace(), 42U);
  REQUIRE(first_created);
  REQUIRE(second_created);
  auto first_provider = std::move(first_created).value();
  auto second_provider = std::move(second_created).value();

  const auto first = first_provider.next();
  const auto second = second_provider.next();
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value() == second.value());
}

TEST_CASE("different restart namespaces cannot collide at the same counter", "[model][order-id]") {
  auto before_restart_created = DeterministicOrderIdProvider::create(sequential_namespace(0U), 1U);
  auto after_restart_created = DeterministicOrderIdProvider::create(sequential_namespace(1U), 1U);
  REQUIRE(before_restart_created);
  REQUIRE(after_restart_created);
  auto before_restart = std::move(before_restart_created).value();
  auto after_restart = std::move(after_restart_created).value();

  const auto before_id = before_restart.next();
  const auto after_id = after_restart.next();
  REQUIRE(before_id);
  REQUIRE(after_id);
  CHECK(before_id.value() != after_id.value());
}

TEST_CASE("order counters reject zero and fail after their final value", "[model][order-id]") {
  const auto invalid = DeterministicOrderIdProvider::create(sequential_namespace(), 0U);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == DomainErrorCode::InvalidValue);

  auto created = DeterministicOrderIdProvider::create(sequential_namespace(),
                                                      std::numeric_limits<std::uint64_t>::max());
  REQUIRE(created);
  auto provider = std::move(created).value();

  const auto last = provider.next();
  REQUIRE(last);
  CHECK(last.value().to_hex().ends_with("ffffffffffffffff"));

  const auto exhausted = provider.next();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::CounterExhausted);
  CHECK(exhausted.error().context.field == "order_counter");
}

TEST_CASE("the production provider obtains a supported operating-system namespace",
          "[model][order-id]") {
  auto created = ProductionOrderIdProvider::create();
  REQUIRE(created);
  auto provider = std::move(created).value();
  const auto order_id = provider.next();
  REQUIRE(order_id);
  CHECK(order_id.value().to_hex().size() == OrderId::byte_size * 2U);
}

} // namespace
