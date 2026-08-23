// Purpose: share deterministic nearest-rank latency, elapsed-time, rate, sample, and allocation
// reporting across the M2 and M3 benchmark workloads.

#pragma once

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace aegis_benchmark_support {

// ########################################################################
// One summary reports nearest-rank latency percentiles in the required microsecond unit.
struct LatencySummary {
  double p50_microseconds;
  double p99_microseconds;
  double p99_9_microseconds;
};

// ########################################################################

// --------------------------------------------------------
// Convert one monotonic clock interval into its unsigned nanosecond duration.
[[nodiscard]] std::uint64_t
elapsed_nanoseconds(std::chrono::steady_clock::time_point started,
                    std::chrono::steady_clock::time_point finished) noexcept;

// --------------------------------------------------------
// Convert integer nanoseconds to the seconds required by Google Benchmark manual timing.
[[nodiscard]] double seconds(std::uint64_t nanoseconds) noexcept;

// --------------------------------------------------------
// Sort one owned distribution and return its fixed nearest-rank percentiles.
[[nodiscard]] LatencySummary summarize(std::vector<std::uint64_t>& samples);

// --------------------------------------------------------
// Publish the common distribution, throughput, sample, and scoped-allocation counters.
void publish_distribution(benchmark::State& state, std::vector<std::uint64_t>& samples,
                          std::uint64_t allocation_count, std::string_view throughput_name,
                          std::string_view allocation_name);

// --------------------------------------------------------

} // namespace aegis_benchmark_support
