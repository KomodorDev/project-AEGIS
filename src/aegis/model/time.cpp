// Purpose: implement explicit cross-clock timing and injected monotonic clock providers.

#include "aegis/model/time.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace aegis::model {

// --------------------------------------------------------
// This is the only supported receive-to-processing subtraction; checking order first prevents an
// unsigned underflow from masquerading as a very large latency.
Result<ElapsedNanoseconds> calculate_processing_delay(ProcessingTimestamp processing,
                                                      ReceiveTimestamp receive) {
  if (processing.nanoseconds() < receive.nanoseconds()) {
    return Result<ElapsedNanoseconds>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidTimestampOrder, "processing_timestamp"));
  }
  return Result<ElapsedNanoseconds>::create_success(
      ElapsedNanoseconds{processing.nanoseconds() - receive.nanoseconds()});
}

// --------------------------------------------------------
// The public template has already mapped negative or unrepresentable input to ArithmeticOverflow at
// clock_nanoseconds. Subtraction-based capacity checking preserves that field and leaves state
// unchanged when the normalized increment would overflow.
Result<void> DeterministicClockProvider::advance_validated_nanoseconds(std::uint64_t nanoseconds) {
  if (nanoseconds > std::numeric_limits<std::uint64_t>::max() - current_nanoseconds_) {
    return Result<void>::create_failure(
        DomainError::create_at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));
  }
  current_nanoseconds_ += nanoseconds;
  return Result<void>::create_success();
}

// --------------------------------------------------------
// Each system clock instance establishes a process-local origin rather than exposing wall time.
SystemClockProvider::SystemClockProvider() noexcept : origin_{std::chrono::steady_clock::now()} {}

// --------------------------------------------------------
// Duration conversion is relative to that origin and yields an unsigned process-local counter.
std::uint64_t SystemClockProvider::monotonic_nanoseconds() noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Convert the steady-clock delta to the public nanosecond unit.
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - origin_)
                           .count();

  // ++++++++++++++++++++++++++++++++++++++++
  // Defensive clamping preserves an unsigned monotonic contract if the platform reports zero or a
  // transient negative duration at the origin boundary.
  if (elapsed <= 0) {
    return 0U;
  }
  return static_cast<std::uint64_t>(elapsed);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::model
