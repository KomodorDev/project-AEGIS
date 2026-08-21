# Initial Correctness and Performance Budgets

> **Purpose:** Define how AEGIS quality is measured, name stable benchmark/load workloads, and record
> the initial correctness and latency targets that later milestones must prove or deliberately revise.

**Status:** M0 calibration and all three M2 workloads are executable. Workload identifiers are
stable; thresholds remain provisional and may be revised only with recorded measurements and
rationale.

## Measurement rules

These rules make results comparable by requiring both the measured value and enough machine/build
context to explain it. A number without that context is not qualification evidence.

Every reported performance-result bundle must include the workload ID, Git revision and HEAD tree,
dirty state and content fingerprint, build preset, compiler and version, operating system, host and
CPU model, total memory, logical/physical core count, CMake/build-tool/benchmark-library versions,
executable and result hashes, power mode, sample count, warm-up policy, and whether the host was
isolated. Product latency
qualification uses a monotonic clock and reports microseconds at p50, p99, and p99.9. Small isolated
microbenchmarks may additionally report nanoseconds per operation when that is the useful scale; that
number is never substituted for the required end-to-end latency percentiles. The M0
runner-calibration workload has no product latency distribution and instead reports Google
Benchmark's mean, median, standard deviation, and coefficient of variation. Throughput is reported as
operations per second. Queue depth, allocations, admission failures, and dropped/coalesced events are
counts, not inferred from timing.

Absolute thresholds are provisionally evaluated on `REF-MAC-01`: MacBookPro18,2 with Apple M1 Max
(10 physical/logical cores), 32 GiB memory, macOS 15, and AppleClang 16. A qualifying run uses AC
power with Low Power Mode disabled, reports no thermal pressure, and has no concurrent build or test
load. Every result still records the exact current OS and tool versions. Shared CI runners only prove
that the harness executes and retain machine-readable smoke evidence; they are never compared with
the absolute thresholds.

## Correctness budgets

Correctness budgets are zero-tolerance invariants: unlike latency targets, they are not relaxed just
because a host is slower or shared.

| Budget | Target |
|---|---:|
| Compiler warnings in AEGIS-owned targets | 0 |
| Unit or deterministic scenario failures | 0 |
| clang-format differences | 0 |
| Address, undefined-behavior, or thread sanitizer findings | 0 |
| Trace mismatches for identical ordered input, configuration, clock values, generated identifiers, and applicable metadata/policy revisions | 0 per run |
| Silently lost private execution or reconciliation facts | 0 |
| Unreported bounded-admission failures | 0 |
| Credentials or production routes required by default build/test | 0 |

Where a bounded public-data policy later permits coalescing or shedding, it must invalidate freshness
or trigger resynchronization as specified by the owning milestone. It never counts as silent loss.

## Named workloads

Stable IDs let documentation, benchmark output, CI artifacts, and future trend reports refer to the
same operation even if its implementation evolves.

| Workload ID | Definition | Primary units | First owner |
|---|---|---|---|
| `BENCH-M0-HARNESS-001` | Benchmark-runner calibration loop with no product behavior | ns/iteration, iterations/s | M0 |
| `BENCH-M2-EXEC-001` | Successfully admit one no-op command, then measure its serialized owner execution through turn completion | ns/turn, turns/s, allocations/turn | M2 |
| `BENCH-M2-CALLBACK-001` | Invoke the deterministic reference strategy on a prebuilt coherent read-only view, measured strictly from callback entry to return | latency µs p50/p99/p99.9, callbacks/s, allocations/callback | M2 |
| `BENCH-M2-MD-001` | Run one full synchronous owner turn: apply a valid BTC-PERPETUAL delta to a fixed 20-level-per-side book and dispatch one deterministic bot callback | latency µs p50/p99/p99.9, events/s, allocations/event | M2 |
| `BENCH-M3-SUBMIT-001` | Authorized canonical limit request through route, risk reservation, OMS, encoding, and successful fake write initiation | latency µs p50/p99/p99.9, orders/s, allocations/order | M3 |
| `BENCH-M3-SUBMIT-002` | Same request rejected inline by risk before OMS admission | latency µs p50/p99/p99.9, rejections/s, allocations/request | M3 |
| `LOAD-M10-REF-001` | One instrument and bot for 10 minutes at 20,000 market events/s plus 100 submit attempts/s | callback and submission latency, queue age/depth, utilization, rates, counts, RSS MiB, trace hash | M10 |
| `LOAD-M10-BURST-001` | The reference workload plus a 10-second public-data burst at 100,000 events/s | same as sustained workload plus time to coherent recovery | M10 |

The M0 harness measurement is evidence that the runner, release build, JSON output, and artifact path
work. It has no product-latency threshold because it deliberately performs no AEGIS behavior.

## Provisional qualification targets

These are starting thresholds for future executable workloads. Measurements and rationale may revise
them, but a code change must not silently redefine the target.

These targets apply when their owning workloads become executable:

| Measurement | Initial target |
|---|---:|
| `BENCH-M2-CALLBACK-001` callback duration | p99 ≤ 100 µs |
| `BENCH-M3-SUBMIT-001` accepted fake-write latency | p99 ≤ 50 µs |
| `BENCH-M3-SUBMIT-002` risk-rejection latency | p99 ≤ 25 µs |
| `LOAD-M10-REF-001` executor queue age | p99 ≤ 250 µs |
| `LOAD-M10-REF-001` executor utilization | ≤ 70% |
| Critical-event drops, unreported admission failures, or trace mismatches | 0 |
| Reservation or inventory drift after either load workload | 0 |

The timing interval for submission begins at entry to the bot-bound `submit` boundary and ends when a
fake asynchronous write is successfully initiated or a local rejection is returned. It excludes
network round trips and exchange acknowledgement. Callback duration begins immediately before the
strategy callback and ends when that callback returns; it excludes parsing, order-book mutation, and
other owner work. A market-event or executor-turn interval begins when the admitted event starts
owner execution and ends after all synchronous owner-local work for that turn, including callback
dispatch, is complete. These intervals must be reported separately.

## Running the M0 harness

This M0 command checks the measurement plumbing only; it does not claim production trading latency.

```sh
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
python3 tools/validate_benchmark_evidence.py
```

The default runner writes `benchmark-results/m0-harness.json` plus
`benchmark-results/m0-context.json`. It rebuilds the release benchmark immediately before measuring;
the context manifest records the required environment/tool fields and SHA-256 digests of the raw
result, executable, and non-committed worktree state. The validator checks the schema, hashes,
workload identity, release mode, and current Git provenance before CI uploads the pair. Set
`AEGIS_BENCHMARK_POWER_MODE` and
`AEGIS_BENCHMARK_HOST_ISOLATION`, and `AEGIS_BENCHMARK_THERMAL_STATE` to truthful descriptions for a
controlled run; their default value is `uncontrolled`, which makes the result smoke evidence only.

## Running the M2 workloads

The M2 suite uses the same Release executable but an exact isolated filter:

```sh
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py --suite m2
python3 tools/validate_benchmark_evidence.py --suite m2
```

It writes `benchmark-results/m2-runtime.json` and `benchmark-results/m2-context.json`. Each workload
uses exactly 10,000 measured samples; fixture construction, capacity preallocation, runtime
bootstrap, and the initial 20×20 snapshot are outside the measured intervals. The executor record
reports `ns_per_turn`, `turns_per_second`, and `allocations_per_turn`. Callback and market-owner
records report `p50_us`, `p99_us`, `p99_9_us`, their named operation rate, and their scoped
allocation count.

Every M2 raw record carries the same `AEGISCFG` and runtime-policy SHA-256 identities. The independent
validator checks exact workload names, units, samples, counters, percentile order, file/executable
hashes, Git/worktree provenance, and those fingerprints. It classifies ordinary results as smoke and
permits the callback p99 threshold claim only when the manifest exactly identifies `REF-MAC-01` and
the power, host-isolation, and thermal fields use the published controlled values. Missing or
free-form control claims never qualify a timing result.
