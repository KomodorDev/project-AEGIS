// Purpose: implement the shared deterministic latency distribution and Google Benchmark counter
// publication contract without coupling any production target to benchmark libraries.

#include "support/distribution_metrics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegis_benchmark_support {
namespace {

// --------------------------------------------------------
// Select one deterministic nearest-rank percentile from an already sorted sample vector.
[[nodiscard]] std::uint64_t calculate_nearest_rank(const std::vector<std::uint64_t>& sorted_samples,
                                                   std::size_t numerator, std::size_t denominator) {
  const auto rank = (sorted_samples.size() * numerator + denominator - 1U) / denominator;
  return sorted_samples.at(rank - 1U);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Clamp no value: steady-clock regression is an invalid benchmark fixture and maps to zero only to
// keep this noexcept leaf usable from measurement providers that report failure separately.
std::uint64_t
calculate_elapsed_nanoseconds(std::chrono::steady_clock::time_point started,
                              std::chrono::steady_clock::time_point finished) noexcept {
  if (finished < started) {
    return 0U;
  }
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
  return static_cast<std::uint64_t>(elapsed);
}

// --------------------------------------------------------
// Publish manual iteration duration in Google Benchmark's required seconds representation.
double nanoseconds_to_seconds(std::uint64_t nanoseconds) noexcept {
  return static_cast<double>(nanoseconds) / 1'000'000'000.0;
}

// --------------------------------------------------------
// Sort after measurement so percentile bookkeeping never contaminates a timed interval.
LatencySummary summarize_latency_samples(std::vector<std::uint64_t>& samples) {
  std::sort(samples.begin(), samples.end());
  constexpr double nanoseconds_per_microsecond = 1'000.0;
  return LatencySummary{
      static_cast<double>(calculate_nearest_rank(samples, 50U, 100U)) / nanoseconds_per_microsecond,
      static_cast<double>(calculate_nearest_rank(samples, 99U, 100U)) / nanoseconds_per_microsecond,
      static_cast<double>(calculate_nearest_rank(samples, 999U, 1'000U)) /
          nanoseconds_per_microsecond,
  };
}

// --------------------------------------------------------
// Keep the M2 output contract byte-for-name stable while making the same policy reusable by M3.
void publish_latency_distribution(benchmark::State& state, std::vector<std::uint64_t>& samples,
                                  std::uint64_t allocation_count, std::string_view throughput_name,
                                  std::string_view allocation_name) {
  if (samples.empty()) {
    return;
  }
  const auto summary = summarize_latency_samples(samples);
  const auto sample_count = static_cast<double>(samples.size());
  state.counters["p50_us"] = summary.p50_microseconds;
  state.counters["p99_us"] = summary.p99_microseconds;
  state.counters["p99_9_us"] = summary.p99_9_microseconds;
  state.counters[std::string{throughput_name}] =
      benchmark::Counter{sample_count, benchmark::Counter::kIsRate};
  state.counters[std::string{allocation_name}] =
      static_cast<double>(allocation_count) / sample_count;
  state.counters["sample_count"] = sample_count;
  state.SetItemsProcessed(static_cast<std::int64_t>(samples.size()));
}

// --------------------------------------------------------

} // namespace aegis_benchmark_support
