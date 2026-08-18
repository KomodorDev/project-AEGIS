// Purpose: implement canonical order-ID encoding and fail-closed operating-system entropy.

#include "aegis/model/order_id.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/random.h>
#endif

namespace aegis::model {
namespace {

// Hex output is canonical lowercase ASCII with two digits per byte and no locale dependency.
[[nodiscard]] std::string bytes_to_hex(const std::uint8_t* bytes, std::size_t size) {
  constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result(size * 2U, '0');
  for (std::size_t index = 0; index < size; ++index) {
    const auto byte = bytes[index];
    result[index * 2U] = digits[static_cast<std::size_t>(byte >> 4U)];
    result[index * 2U + 1U] = digits[static_cast<std::size_t>(byte & 0x0fU)];
  }
  return result;
}

// Fill all 128 namespace bits from the strongest supported operating-system primitive. Unsupported,
// zero-progress, and non-retryable error paths all fail closed.
[[nodiscard]] bool
fill_order_namespace_from_operating_system(OrderNamespace::Bytes& destination) noexcept {
#if defined(__APPLE__)
  // getentropy is all-or-nothing for this small fixed-size request.
  return ::getentropy(destination.data(), destination.size()) == 0;
#elif defined(__linux__)
  // getrandom may return partial data or EINTR, so accumulate until all bytes are initialized.
  std::size_t offset = 0U;
  while (offset < destination.size()) {
    const auto result = ::getrandom(destination.data() + offset, destination.size() - offset, 0U);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (result == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(result);
  }
  return true;
#else
  // No unreviewed fallback source is acceptable for a production restart namespace.
  static_cast<void>(destination);
  return false;
#endif
}

} // namespace

std::string OrderNamespace::to_hex() const { return bytes_to_hex(bytes_.data(), bytes_.size()); }

OrderId OrderId::from_parts(const OrderNamespace& order_namespace, std::uint64_t counter) noexcept {
  Bytes bytes{};

  // Namespace bytes are copied verbatim so the entropy contribution has one stable representation.
  for (std::size_t index = 0; index < OrderNamespace::byte_size; ++index) {
    bytes[index] = order_namespace.bytes()[index];
  }

  // The counter is appended most-significant byte first, making IDs identical on every host endian.
  for (std::size_t offset = 0; offset < sizeof(counter); ++offset) {
    constexpr std::size_t bits_per_byte = 8U;
    const auto shift = (sizeof(counter) - 1U - offset) * bits_per_byte;
    bytes[OrderNamespace::byte_size + offset] =
        static_cast<std::uint8_t>((counter >> shift) & 0xffU);
  }
  return OrderId{bytes};
}

std::string OrderId::to_hex() const { return bytes_to_hex(bytes_.data(), bytes_.size()); }

Result<DeterministicOrderIdProvider>
DeterministicOrderIdProvider::create_validated(OrderNamespace order_namespace,
                                               std::uint64_t initial_counter) {
  // Counter zero is reserved; all public integral range checks have completed before this boundary.
  if (initial_counter == 0U) {
    return Result<DeterministicOrderIdProvider>::failure(
        DomainError::at_field(DomainErrorCode::InvalidValue, "order_counter"));
  }
  return Result<DeterministicOrderIdProvider>::success(
      DeterministicOrderIdProvider{order_namespace, initial_counter});
}

DeterministicOrderIdProvider::DeterministicOrderIdProvider(OrderNamespace order_namespace,
                                                           std::uint64_t initial_counter) noexcept
    : namespace_{order_namespace}, next_counter_{initial_counter} {}

// Custom moves transfer the sole right to continue a sequence and disable the former owner, so
// moved-from providers cannot duplicate IDs.
DeterministicOrderIdProvider::DeterministicOrderIdProvider(
    DeterministicOrderIdProvider&& other) noexcept
    : namespace_{other.namespace_}, next_counter_{other.next_counter_},
      exhausted_{other.exhausted_} {
  other.exhausted_ = true;
}

DeterministicOrderIdProvider&
DeterministicOrderIdProvider::operator=(DeterministicOrderIdProvider&& other) noexcept {
  if (this != &other) {
    namespace_ = other.namespace_;
    next_counter_ = other.next_counter_;
    exhausted_ = other.exhausted_;
    other.exhausted_ = true;
  }
  return *this;
}

Result<OrderId> DeterministicOrderIdProvider::next() {
  // Exhaustion is sticky, including on a moved-from provider.
  if (exhausted_) {
    return Result<OrderId>::failure(
        DomainError::at_field(DomainErrorCode::CounterExhausted, "order_counter"));
  }

  const auto order_id = OrderId::from_parts(namespace_, next_counter_);
  // Emit UINT64_MAX once, then transition without incrementing through zero.
  if (next_counter_ == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
  } else {
    ++next_counter_;
  }
  return Result<OrderId>::success(order_id);
}

// Production always selects the real operating-system source; injection remains private test
// access.
Result<ProductionOrderIdProvider> ProductionOrderIdProvider::create() {
  return create_with_entropy(fill_order_namespace_from_operating_system);
}

// Convert the entropy callback's all-bytes-or-failure contract into one stable startup error. Bytes
// written by a failing callback are discarded rather than treated as usable entropy.
Result<ProductionOrderIdProvider>
ProductionOrderIdProvider::create_with_entropy(EntropyFillCallback entropy_fill) {
  OrderNamespace::Bytes bytes{};
  if (entropy_fill == nullptr || !entropy_fill(bytes)) {
    return Result<ProductionOrderIdProvider>::failure(
        DomainError::at_field(DomainErrorCode::EntropyUnavailable, "order_namespace"));
  }
  return Result<ProductionOrderIdProvider>::success(
      ProductionOrderIdProvider{OrderNamespace{bytes}});
}

// A successfully randomized namespace always starts its checked sequence at counter one.
ProductionOrderIdProvider::ProductionOrderIdProvider(OrderNamespace order_namespace) noexcept
    : provider_{order_namespace, 1U} {}

} // namespace aegis::model
