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

[[nodiscard]] bool
fill_order_namespace_from_operating_system(OrderNamespace::Bytes& destination) noexcept {
#if defined(__APPLE__)
  return ::getentropy(destination.data(), destination.size()) == 0;
#elif defined(__linux__)
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
  static_cast<void>(destination);
  return false;
#endif
}

} // namespace

std::string OrderNamespace::to_hex() const { return bytes_to_hex(bytes_.data(), bytes_.size()); }

OrderId OrderId::from_parts(const OrderNamespace& order_namespace, std::uint64_t counter) noexcept {
  Bytes bytes{};
  for (std::size_t index = 0; index < OrderNamespace::byte_size; ++index) {
    bytes[index] = order_namespace.bytes()[index];
  }
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
DeterministicOrderIdProvider::create(OrderNamespace order_namespace,
                                     std::uint64_t initial_counter) {
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
  if (exhausted_) {
    return Result<OrderId>::failure(
        DomainError::at_field(DomainErrorCode::CounterExhausted, "order_counter"));
  }

  const auto order_id = OrderId::from_parts(namespace_, next_counter_);
  if (next_counter_ == std::numeric_limits<std::uint64_t>::max()) {
    exhausted_ = true;
  } else {
    ++next_counter_;
  }
  return Result<OrderId>::success(order_id);
}

Result<ProductionOrderIdProvider> ProductionOrderIdProvider::create() {
  return create_with_entropy(fill_order_namespace_from_operating_system);
}

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

ProductionOrderIdProvider::ProductionOrderIdProvider(OrderNamespace order_namespace) noexcept
    : provider_{order_namespace, 1U} {}

} // namespace aegis::model
