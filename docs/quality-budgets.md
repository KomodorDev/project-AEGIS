# Initial Correctness and Performance Budgets

> **Purpose:** Define how AEGIS quality is measured, name stable benchmark/load workloads, and record
> the initial correctness and latency targets that later milestones must prove or deliberately revise.

**Status:** Provisional M0 baseline. Workload definitions are stable identifiers; thresholds may be
revised with recorded measurements and rationale.

## Measurement rules

These rules make results comparable by requiring both the measured value and enough machine/build
context to explain it. A number without that context is not qualification evidence.

Every reported performance-result bundle must include the workload ID, git revision, build preset,
compiler and version, operating system, CPU model, logical/physical core count, power mode, sample
count, warm-up policy, and whether the host was isolated. Product latency workloads use a monotonic
clock and report microseconds at p50, p99, and p99.9. The M0 runner-calibration workload has no product
latency distribution and instead reports Google Benchmark's mean, median, standard deviation, and
coefficient of variation. Throughput is reported as operations per second. Queue depth, allocations,
admission failures, and dropped/coalesced events are counts, not inferred from timing.

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
| `BENCH-M2-EXEC-001` | Admit and run one no-op serialized executor turn | ns/turn, turns/s, allocations/turn | M2 |
| `BENCH-M2-MD-001` | Apply one valid BTC-PERPETUAL delta to a fixed 20-level book and synchronously dispatch one deterministic bot callback | ns/event, events/s, allocations/event | M2 |
| `BENCH-M3-SUBMIT-001` | Authorized canonical limit request through route, risk reservation, OMS, encoding, and successful fake write initiation | latency percentiles, orders/s, allocations/order | M3 |
| `BENCH-M3-SUBMIT-002` | Same request rejected inline by risk before OMS admission | latency percentiles, rejections/s, allocations/request | M3 |
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
| `BENCH-M2-MD-001` callback duration | p99 ≤ 100 µs |
| `BENCH-M3-SUBMIT-001` accepted fake-write latency | p99 ≤ 50 µs |
| `BENCH-M3-SUBMIT-002` risk-rejection latency | p99 ≤ 25 µs |
| `LOAD-M10-REF-001` executor queue age | p99 ≤ 250 µs |
| `LOAD-M10-REF-001` executor utilization | ≤ 70% |
| Critical-event drops, unreported admission failures, or trace mismatches | 0 |
| Reservation or inventory drift after either load workload | 0 |

The timing interval for submission begins at entry to the bot-bound `submit` boundary and ends when a
fake asynchronous write is successfully initiated or a local rejection is returned. It excludes
network round trips and exchange acknowledgement. Callback timing begins when the serialized executor
starts the turn and ends when all synchronous owner-local work for that turn is complete.

## Running the M0 harness

This M0 command checks the measurement plumbing only; it does not claim production trading latency.

```sh
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
```

The runner writes `benchmark-results/m0-harness.json` plus
`benchmark-results/m0-context.json`. The context manifest records the required environment fields and
the SHA-256 digest of the raw result. Set `AEGIS_BENCHMARK_POWER_MODE` and
`AEGIS_BENCHMARK_HOST_ISOLATION`, and `AEGIS_BENCHMARK_THERMAL_STATE` to truthful descriptions for a
controlled run; their default value is `uncontrolled`, which makes the result smoke evidence only.
