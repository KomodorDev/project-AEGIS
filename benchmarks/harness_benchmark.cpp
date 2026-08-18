#include <benchmark/benchmark.h>
#include <cstdint>

namespace {

void harness_noop(benchmark::State& state) {
  std::uint64_t token = 0;

  for ([[maybe_unused]] const auto iteration : state) {
    benchmark::DoNotOptimize(token);
    token ^= 0x9e3779b97f4a7c15ULL;
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations());
}

} // namespace

BENCHMARK(harness_noop)->Name("BENCH-M0-HARNESS-001/harness.noop")->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
