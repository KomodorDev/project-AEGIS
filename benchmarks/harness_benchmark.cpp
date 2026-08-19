// Purpose: verify and calibrate the M0 benchmark pipeline without measuring product logic.

// Google Benchmark supplies the runner; fixed-width integers keep the tiny workload portable.
#include <benchmark/benchmark.h>
#include <cstdint>

// Give file-local helpers internal linkage so they cannot collide with other translation units.
namespace {

// --------------------------------------------------------
// Execute a stable, intentionally trivial mutation once per iteration selected by the runner.
void harness_noop(benchmark::State& state) {
  // ++++++++++++++++++++++++++++++++++++++++
  // Initialize the value whose mutation supplies the intentionally tiny workload.
  std::uint64_t token = 0;
  // ++++++++++++++++++++++++++++++++++++++++
  // Interesting syntax: [[maybe_unused]] documents that the range value drives timing only.
  // DoNotOptimize and ClobberMemory stop the optimizer from deleting/reordering the workload.
  for ([[maybe_unused]] const auto iteration : state) {
    benchmark::DoNotOptimize(token);
    token ^= 0x9e3779b97f4a7c15ULL;
    benchmark::ClobberMemory();
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Report iterations as processed items so benchmark throughput metadata remains meaningful.
  state.SetItemsProcessed(state.iterations());
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Register a stable workload ID and report its duration in nanoseconds.
BENCHMARK(harness_noop)->Name("BENCH-M0-HARNESS-001/harness.noop")->Unit(benchmark::kNanosecond);
// --------------------------------------------------------
// Interesting syntax: this macro generates main() and starts every registered benchmark.
BENCHMARK_MAIN();
// --------------------------------------------------------
