// Purpose: implement explicit cross-clock timing and injected monotonic clock providers.

#include "aegis/model/time.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace aegis::model {

Result<ElapsedNanoseconds> processing_delay(ProcessingTimestamp processing,
                                            ReceiveTimestamp receive) {
  if (processing.nanoseconds() < receive.nanoseconds()) {
    return Result<ElapsedNanoseconds>::failure(
        DomainError::at_field(DomainErrorCode::InvalidTimestampOrder, "processing_timestamp"));
  }
  return Result<ElapsedNanoseconds>::success(
      ElapsedNanoseconds{processing.nanoseconds() - receive.nanoseconds()});
}

Result<void> DeterministicClockProvider::advance(std::uint64_t nanoseconds) {
  if (nanoseconds > std::numeric_limits<std::uint64_t>::max() - current_nanoseconds_) {
    return Result<void>::failure(
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));
  }
  current_nanoseconds_ += nanoseconds;
  return Result<void>::success();
}

SystemClockProvider::SystemClockProvider() noexcept : origin_{std::chrono::steady_clock::now()} {}

std::uint64_t SystemClockProvider::monotonic_nanoseconds() noexcept {
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - origin_)
                           .count();
  if (elapsed <= 0) {
    return 0U;
  }
  return static_cast<std::uint64_t>(elapsed);
}

} // namespace aegis::model
