// Purpose: implement production steady-clock readings and deterministic thread-safe scripted
// readings for noncanonical M3 submission-duration measurement.

#include "aegis/execution/submission_measurement_clock.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace aegis::execution {

// --------------------------------------------------------
// Establish a process-local origin so production readings remain small and epoch-independent.
SteadySubmissionMeasurementClock::SteadySubmissionMeasurementClock() noexcept
    : origin_{std::chrono::steady_clock::now()} {}

// --------------------------------------------------------
// Return elapsed nanoseconds only when steady-clock ordering and the signed conversion remain
// valid.
std::optional<std::uint64_t>
SteadySubmissionMeasurementClock::claim_next_nanosecond_reading() noexcept {
  const auto observed = std::chrono::steady_clock::now();
  if (observed < origin_) {
    return std::nullopt;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(observed - origin_).count();
  if (elapsed < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(elapsed);
}

// --------------------------------------------------------
// Atomically assign scripted positions while leaving every exhausted reader on the fixed fallback.
std::optional<std::uint64_t>
DeterministicSubmissionMeasurementClock::claim_next_nanosecond_reading() noexcept {
  auto index = next_reading_.load(std::memory_order_relaxed);
  while (index < readings_.size()) {
    if (next_reading_.compare_exchange_weak(index, index + 1U, std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
      return readings_[index];
    }
  }
  return fallback_;
}

// --------------------------------------------------------

} // namespace aegis::execution
