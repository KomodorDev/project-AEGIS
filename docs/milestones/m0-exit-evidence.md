# M0 Exit Evidence

> **Purpose:** Map every M0 promise to a concrete artifact or reproducible command, while keeping
> local evidence distinct from the remote CI evidence required for milestone closure.

**Status:** Local evidence complete on 2026-08-18. M0 closes only after the feature branch passes the
remote CI workflow and the evidence table is updated with that run.

Gate IDs beginning with `M0-S` identify delivered scope; IDs beginning with `M0-E` identify the exit
conditions that must be evidenced before M0 can close.

## Scope evidence

| Gate | Evidence |
|---|---|
| `M0-S01` Toolchain choices | [ADR-0002](../decisions/0002-delivery-toolchain.md), `CMakeLists.txt`, and `CMakePresets.json` |
| `M0-S02` CI quality gates | `.github/workflows/ci.yml`: format, warning-clean compiler matrix, unit tests, ASan+UBSan, and TSan |
| `M0-S03` Benchmark harness | `aegis_benchmarks`, workload `BENCH-M0-HARNESS-001`, raw JSON plus environment-context manifest, independent bundle validation, and artifact upload |
| `M0-S04` Venue protocol spike | [Deribit BTC perpetual spike](../protocol-spikes/deribit-btc-perpetual.md) |
| `M0-S05` Reference scenario | [M0 reference scenario](../reference-scenario.md) |
| `M0-S06` Account assumptions | [ADR-0003](../decisions/0003-dedicated-testnet-account.md) with explicit validation and quarantine rules |
| `M0-S07` Budgets | [Correctness and performance budgets](../quality-budgets.md) with named workloads and units |
| `M0-S08` No empty scaffolding | Every added source, test, benchmark, CMake, tool, and documentation directory contains a used artifact |

## Exit-gate evidence

| Gate | Reproducible evidence | State |
|---|---|---|
| `M0-E01` Fresh checkout configures, builds, and tests with one command locally and in CI | `cmake --workflow --preset verify`; identical command in README and compiler-matrix CI jobs | Local pass: arm64 macOS 15.7.4 / AppleClang 16; remote matrix pending |
| `M0-E02` Scenario, venue, and environment are explicit | Deribit testnet / `BTC-PERPETUAL` / one firm, desk, bot, account alias, subscription, and disabled route | Complete |
| `M0-E03` Account rules are explicit and testable | Dedicated `segregated_sm` subaccount, staged permissions, no external activity, authority split, and fail-closed mismatch matrix | Complete as an operating contract; executable venue checks begin in M7 |
| `M0-E04` Measurements have names, workloads, and units | `BENCH-M0-HARNESS-001` plus M2/M3/M10 workload definitions and provisional targets | Local release/JSON smoke pass; remote artifact pending |
| `M0-E05` Default path needs no credential or production endpoint | Build graph contains only local AEGIS code and checksum-pinned test dependencies; no exchange client or transmission target exists | Complete |

## Commands to record before closure

```sh
cmake --workflow --preset verify
cmake --preset format
cmake --build --preset format --target format-check
cmake --workflow --preset verify-asan-ubsan
cmake --workflow --preset verify-tsan
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
python3 tools/validate_benchmark_evidence.py
```

The 2026-08-18 local run passed the normal, formatting, ASan+UBSan, TSan, and release workflows with
AppleClang 16 on arm64 macOS 15.7.4. The benchmark emitted the expected
`BENCH-M0-HARNESS-001/harness.noop` JSON records and its required context manifest; the independent
validator confirmed the workload identity, release build, hashes, and repository provenance. Its
timing is calibration only and is not a product latency result.

The same `verify` command also passed from an uncached temporary source copy using the documented
minimum CMake 3.25.2 and Ninja 1.13.0, including first-time checksum-verified dependency acquisition.

The remote evidence must be a GitHub Actions run for a pull request whose base branch is `dev`. CI
timings are benchmark-harness evidence only; they do not qualify the provisional latency targets.
