// Purpose: prove production and deterministic M3 submission clocks expose checked monotonic
// readings and consume scripted values safely across concurrent public-entry calls.

#include "aegis/execution/submission_measurement_clock.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// An external implementation can name the abstract contract but cannot construct its private base;
// this prevents arbitrary blocking or live-capability clocks from entering submission parameters.
class ExternalSubmissionMeasurementClock final : public execution::SubmissionMeasurementClock {
private:

  // --------------------------------------------------------
  // Return one harmless reading solely to complete the inaccessible external implementation probe.
  [[nodiscard]] std::optional<std::uint64_t> read_now_nanoseconds() noexcept override { return 0U; }

  // --------------------------------------------------------
};

// ########################################################################

static_assert(!std::is_default_constructible_v<ExternalSubmissionMeasurementClock>);

// --------------------------------------------------------
// The production clock exposes only process-local elapsed time and never moves backward.
TEST_CASE("steady submission measurement clock is locally monotonic",
          "[execution][submission][measurement][clock][m3]") {
  execution::SteadySubmissionMeasurementClock clock;
  const auto first = clock.now_nanoseconds();
  const auto second = clock.now_nanoseconds();

  REQUIRE(first);
  REQUIRE(second);
  CHECK(*second >= *first);
}

// --------------------------------------------------------
// Scripted absence, regression inputs, and exhaustion remain exact deterministic test data; the
// coordinator alone decides whether two readings form a valid duration.
TEST_CASE("deterministic submission measurement clock preserves scripted optional readings",
          "[execution][submission][measurement][clock][m3]") {
  execution::DeterministicSubmissionMeasurementClock clock{
      {std::optional<std::uint64_t>{10U}, std::nullopt, std::optional<std::uint64_t>{5U}},
      std::optional<std::uint64_t>{99U}};

  CHECK(clock.now_nanoseconds() == 10U);
  CHECK_FALSE(clock.now_nanoseconds());
  CHECK(clock.now_nanoseconds() == 5U);
  CHECK(clock.now_nanoseconds() == 99U);
  CHECK(clock.now_nanoseconds() == 99U);
  CHECK(clock.readings_consumed() == 3U);
}

// --------------------------------------------------------
// Concurrent callers claim every immutable scripted slot exactly once, proving the pre-owner entry
// read cannot race even when a wrong-thread submission overlaps its active callback.
TEST_CASE("deterministic submission measurement clock assigns concurrent reads exactly once",
          "[execution][submission][measurement][clock][thread-safe][m3]") {
  constexpr std::size_t reader_count = 32U;
  std::vector<std::optional<std::uint64_t>> readings;
  readings.reserve(reader_count);
  for (std::size_t index = 0U; index < reader_count; ++index) {
    readings.emplace_back(static_cast<std::uint64_t>(index + 1U));
  }
  execution::DeterministicSubmissionMeasurementClock clock{std::move(readings)};
  std::array<std::optional<std::uint64_t>, reader_count> observed{};
  std::vector<std::thread> readers;
  readers.reserve(reader_count);
  for (std::size_t index = 0U; index < reader_count; ++index) {
    readers.emplace_back([&clock, &observed, index] { observed[index] = clock.now_nanoseconds(); });
  }
  for (auto& reader : readers) {
    reader.join();
  }

  std::array<std::uint64_t, reader_count> values{};
  for (std::size_t index = 0U; index < reader_count; ++index) {
    REQUIRE(observed[index]);
    values[index] = *observed[index];
  }
  std::sort(values.begin(), values.end());
  for (std::size_t index = 0U; index < reader_count; ++index) {
    CHECK(values[index] == index + 1U);
  }
  CHECK(clock.readings_consumed() == reader_count);
  CHECK_FALSE(clock.now_nanoseconds());
}

// --------------------------------------------------------

} // namespace
