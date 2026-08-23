// Purpose: define the dedicated thread-safe monotonic clock used only for noncanonical M3
// submission-duration measurement and deterministic timing tests.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace aegis::execution {

// ########################################################################
// The submission entry may run on a non-owner thread, so every clock implementation must support
// concurrent reads without granting access to any owner-local submission state.
class SubmissionMeasurementClock {
public:

  // --------------------------------------------------------
  SubmissionMeasurementClock(const SubmissionMeasurementClock&) = delete;
  SubmissionMeasurementClock& operator=(const SubmissionMeasurementClock&) = delete;
  SubmissionMeasurementClock(SubmissionMeasurementClock&&) = delete;
  SubmissionMeasurementClock& operator=(SubmissionMeasurementClock&&) = delete;
  virtual ~SubmissionMeasurementClock() = default;

  // --------------------------------------------------------
  // Return one process-local monotonic nanosecond reading, or absence when no valid reading exists.
  [[nodiscard]] std::optional<std::uint64_t> now_nanoseconds() noexcept {
    return read_now_nanoseconds();
  }

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the two final repository-owned clocks may construct the polymorphic base; downstream code
  // cannot inject blocking or communication work through an arbitrary derived implementation.
  friend class SteadySubmissionMeasurementClock;
  friend class DeterministicSubmissionMeasurementClock;

  // ########################################################################

  // --------------------------------------------------------
  SubmissionMeasurementClock() = default;

  // --------------------------------------------------------
  // Concrete clocks provide the thread-safe reading operation behind the fixed public contract.
  [[nodiscard]] virtual std::optional<std::uint64_t> read_now_nanoseconds() noexcept = 0;

  // --------------------------------------------------------
};

// ########################################################################
// The production clock reports elapsed steady-clock nanoseconds from a private construction origin;
// it exposes neither wall time nor an exchange-comparable timestamp.
class SteadySubmissionMeasurementClock final : public SubmissionMeasurementClock {
public:

  // --------------------------------------------------------
  // Capture the process-local origin before any submission can read this clock.
  SteadySubmissionMeasurementClock() noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Convert the current nondecreasing steady-clock delta to the public checked unsigned domain.
  [[nodiscard]] std::optional<std::uint64_t> read_now_nanoseconds() noexcept override;

  // --------------------------------------------------------
  std::chrono::steady_clock::time_point origin_;
};

// ########################################################################
// The deterministic clock atomically consumes immutable scripted readings in global call order;
// exhausted readers receive one immutable fallback without cursor wrap or allocation.
class DeterministicSubmissionMeasurementClock final : public SubmissionMeasurementClock {
public:

  // --------------------------------------------------------
  // Own all scripted readings before publication; absence can deliberately prove unavailable time.
  explicit DeterministicSubmissionMeasurementClock(
      std::vector<std::optional<std::uint64_t>> readings,
      std::optional<std::uint64_t> fallback = std::nullopt) noexcept
      : readings_{std::move(readings)}, fallback_{fallback} {}

  // --------------------------------------------------------
  [[nodiscard]] std::size_t readings_consumed() const noexcept {
    return next_reading_.load(std::memory_order_relaxed);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Claim at most one in-range immutable slot per caller; saturation never wraps the cursor.
  [[nodiscard]] std::optional<std::uint64_t> read_now_nanoseconds() noexcept override;

  // --------------------------------------------------------
  const std::vector<std::optional<std::uint64_t>> readings_;
  const std::optional<std::uint64_t> fallback_;
  std::atomic<std::size_t> next_reading_{0U};
};

// ########################################################################

} // namespace aegis::execution
