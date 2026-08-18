// Purpose: prove deterministic order identities, checked counter boundaries, and fail-closed
// production namespace entropy without exposing a weak production fallback.

#include "aegis/model/order_id.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace aegis::model::detail {

// Interesting syntax: the declared friend proxy exposes only the private entropy seam needed to
// force startup failures; ordinary callers still see only ProductionOrderIdProvider::create().
struct ProductionOrderIdProviderTestAccess {
  using EntropyFillCallback = bool (*)(OrderNamespace::Bytes&) noexcept;

  [[nodiscard]] static Result<ProductionOrderIdProvider>
  create_with_entropy(EntropyFillCallback entropy_fill) {
    return ProductionOrderIdProvider::create_with_entropy(entropy_fill);
  }
};

} // namespace aegis::model::detail

namespace {

using namespace aegis::model;

// The deterministic fixture makes namespace bytes visually traceable in canonical hex vectors;
// changing the first byte also creates a distinct restart namespace.
[[nodiscard]] OrderNamespace sequential_namespace(std::uint8_t first_byte = 0U) {
  OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(first_byte + static_cast<std::uint8_t>(index));
  }
  return OrderNamespace{bytes};
}

// Byte widths are part of the persisted ID contract, not implementation-dependent object sizes.
static_assert(OrderNamespace::byte_size == 16U);
static_assert(OrderId::byte_size == 24U);

// Interesting syntax: a requires-expression probes factory participation without constructing a
// provider, protecting the public integral boundary at compile time.
template <typename Counter>
concept HasOrderProviderFactory = requires(OrderNamespace order_namespace, Counter counter) {
  DeterministicOrderIdProvider::create(order_namespace, counter);
};

static_assert(HasOrderProviderFactory<std::uint64_t>);
static_assert(HasOrderProviderFactory<std::int64_t>);
static_assert(!HasOrderProviderFactory<double>);
static_assert(!HasOrderProviderFactory<bool>);
static_assert(!HasOrderProviderFactory<char>);
static_assert(HasOrderProviderFactory<signed char>);

// This golden vector pins namespace placement, big-endian counter encoding, and increment order.
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

// Replay providers with identical injected state must reproduce the same next identity exactly.
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

// A fresh namespace, rather than wall time or host identity, separates equal counters across
// restart.
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

// Invalid signed entry, narrow valid integers, and UINT64_MAX exercise every counter boundary
// without wrapping the sequence back to zero.
TEST_CASE("order counters reject zero and fail after their final value", "[model][order-id]") {
  const auto invalid = DeterministicOrderIdProvider::create(sequential_namespace(), 0U);
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error().code == DomainErrorCode::InvalidValue);

  const auto negative = DeterministicOrderIdProvider::create(sequential_namespace(), -1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() == DomainError::at_field(DomainErrorCode::InvalidValue, "order_counter"));

  auto narrow_counter =
      DeterministicOrderIdProvider::create(sequential_namespace(), static_cast<unsigned char>(1));
  REQUIRE(narrow_counter);

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

// The production smoke path proves the current platform can source a namespace and produce a full
// canonical identity without exposing the random bytes as a test oracle.
TEST_CASE("the production provider obtains a supported operating-system namespace",
          "[model][order-id]") {
  auto created = ProductionOrderIdProvider::create();
  REQUIRE(created);
  auto provider = std::move(created).value();
  const auto order_id = provider.next();
  REQUIRE(order_id);
  CHECK(order_id.value().to_hex().size() == OrderId::byte_size * 2U);
}

// Entropy failure must reject startup even if a source wrote plausible bytes, and a missing
// callback must fail identically instead of selecting a fallback namespace.
TEST_CASE("the production provider fails closed when its entropy source is unavailable",
          "[model][order-id]") {
  const auto unavailable = [](OrderNamespace::Bytes& destination) noexcept {
    destination.fill(0xa5U);
    return false;
  };

  const auto created =
      detail::ProductionOrderIdProviderTestAccess::create_with_entropy(unavailable);

  REQUIRE_FALSE(created);
  CHECK(created.error() ==
        DomainError::at_field(DomainErrorCode::EntropyUnavailable, "order_namespace"));

  const auto null_source =
      detail::ProductionOrderIdProviderTestAccess::create_with_entropy(nullptr);
  REQUIRE_FALSE(null_source);
  CHECK(null_source.error() ==
        DomainError::at_field(DomainErrorCode::EntropyUnavailable, "order_namespace"));
}

} // namespace
