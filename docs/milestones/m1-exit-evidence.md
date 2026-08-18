# M1 Exit Evidence

> **Purpose:** Map every M1 promise to concrete implementation and deterministic tests while keeping
> local feature-branch evidence distinct from the remote CI and integration evidence required to
> close the milestone.

**Status:** The M1 implementation is complete on `codex/m1-domain-kernel` through code commit
`d9f015b`. Normal, formatting, ASan+UBSan, TSan, release, and benchmark-evidence regression checks
passed locally on 2026-08-18. Remote CI, review, and integration into `dev` remain pending; M1 is not
closed or merged.

The code commit named above is the exact implementation snapshot covered by the recorded local
matrix. Later evidence or documentation-only commits do not move that boundary; any implementation
change requires a new code-tip reference and a fresh verification record.

Gate IDs beginning with `M1-S` identify delivered scope. IDs beginning with `M1-E` map the roadmap's
exit conditions.

## How to read this record

Scope rows connect each promise to implementation and tests. Exit-gate rows summarize the strongest
evidence currently available, while the final section keeps remote CI, review and merge facts
explicitly separate from local proof.

## Scope evidence

| Gate | Delivered behavior | Implementation and tests |
|---|---|---|
| `M1-S01` Strong identities and deterministic results | Firm, desk, bot, strategy, venue, logical/venue account, normalized/venue instrument, subscription, route, and order identities are nominal and validated. Stable result/error values carry deterministic field/index context. | [`identifier.hpp`](../../include/aegis/model/identifier.hpp), [`domain_error.hpp`](../../include/aegis/model/domain_error.hpp), [`result.hpp`](../../include/aegis/model/result.hpp), [`identifier_test.cpp`](../../tests/unit/model/identifier_test.cpp), and [`domain_error_result_test.cpp`](../../tests/unit/model/domain_error_result_test.cpp) |
| `M1-S02` Exact units and metadata | Prices, quantities, and notionals use checked coefficient-plus-scale arithmetic. Lossy operations require an assigned rounding mode; floating-point entry, `bool`, enum, and plain/wide character scale inputs, plus out-of-range integral narrowing, are rejected before entering the representation. Cross-unit operators are absent. Revisioned metadata validates tick, quantity step/minimum, contract style, multiplier, and currency units. | [`fixed_point.hpp`](../../include/aegis/model/fixed_point.hpp), [`instrument_metadata.hpp`](../../include/aegis/model/instrument_metadata.hpp), [`fixed_point_test.cpp`](../../tests/unit/model/fixed_point_test.cpp), and [`instrument_metadata_test.cpp`](../../tests/unit/model/instrument_metadata_test.cpp) |
| `M1-S03` Typed time and generated identities | Source/receive/processing timestamps, elapsed values, session epochs, sequences, and each revision kind are distinct. Tests inject clocks and fixed order namespaces; production obtains 128 bits of operating-system entropy and fails closed if unavailable, then uses a checked 64-bit counter. | [`time.hpp`](../../include/aegis/model/time.hpp), [`order_id.hpp`](../../include/aegis/model/order_id.hpp), [`time_test.cpp`](../../tests/unit/model/time_test.cpp), and [`order_id_test.cpp`](../../tests/unit/model/order_id_test.cpp) |
| `M1-S04` Immutable multi-firm attribution | A configuration holds one or more peer firms; each desk belongs to exactly one firm and each bot to exactly one desk. Attribution is derived and immutable. Logical accounts are firm-owned, and the public route-section factory rejects a route from one firm's bot into another firm's account. | [`organization.hpp`](../../include/aegis/organization/organization.hpp), [`organization_test.cpp`](../../tests/unit/organization/organization_test.cpp), the two-firm fixture in [`reference_configuration.cpp`](../../tests/support/reference_configuration.cpp), and the peer-firm/account tests in [`execution_route_test.cpp`](../../tests/unit/execution/execution_route_test.cpp) and [`startup_configuration_test.cpp`](../../tests/unit/configuration/startup_configuration_test.cpp) |
| `M1-S05` Separate subscriptions and routes | Market-data subscriptions and execution routes are independent canonical collections. Duplicate IDs/semantic grants, dangling references, invalid enum values, and mismatched venue/instrument/account bindings fail deterministically. The reference route is disabled. | [`subscription.hpp`](../../include/aegis/market_data/subscription.hpp), [`execution_route.hpp`](../../include/aegis/execution/execution_route.hpp), [`subscription_test.cpp`](../../tests/unit/market_data/subscription_test.cpp), and [`execution_route_test.cpp`](../../tests/unit/execution/execution_route_test.cpp) |
| `M1-S06` Atomic immutable provenance | Startup validation either returns one complete immutable snapshot or one deterministic error. Canonically sorted, tagged, length-prefixed big-endian `AEGISCFG` schema-one bytes include every decision and revision; SHA-256 supplies the stable fingerprint and golden vector. | [`startup_configuration.hpp`](../../include/aegis/configuration/startup_configuration.hpp), [`configuration_provenance.hpp`](../../include/aegis/configuration/configuration_provenance.hpp), [`startup_configuration_test.cpp`](../../tests/unit/configuration/startup_configuration_test.cpp), and [`sha256_test.cpp`](../../tests/unit/model/sha256_test.cpp) |
| `M1-S07` Deterministic bounded traces | A fixed-capacity in-memory sink validates four M1 event shapes, preserves accepted records on capacity failure, and emits canonical `AEGISTRS` schema-one bytes plus SHA-256. File, console, database, and network I/O are absent. | [`trace.hpp`](../../include/aegis/trace/trace.hpp), [`trace_test.cpp`](../../tests/unit/trace/trace_test.cpp), and [`m1_reference_scenario_test.cpp`](../../tests/deterministic_scenarios/m1_reference_scenario_test.cpp) |
| `M1-S08` Deferred capability boundary | M1 contains declarations and evidence, not a market runtime or trading capability. Startup values have no credential, secret, endpoint, URL, or venue-account field; no adapter, socket, OMS, risk, or transmission target exists. | Compile-time absence checks in [`startup_configuration_test.cpp`](../../tests/unit/configuration/startup_configuration_test.cpp), the owned source list in [`CMakeLists.txt`](../../CMakeLists.txt), and [repository layout](../architecture/repository-layout.md) |

## Exit-gate evidence

| Gate | Reproducible evidence | Current state |
|---|---|---|
| `M1-E01` Invalid identifiers, metadata, conversions, and hierarchy references fail deterministically | Identifier grammar tests; metadata validation and unit tests; exact conversion failures; organization, subscription, route, and atomic startup negative cases assert stable error codes, fields, and positions. | Local pass in all normal and sanitizer suites |
| `M1-E02` Bot identity is immutable and subscriptions grant no trading permission | `Organization` exposes derived `BotAttribution`; peer-firm tests retain firm/desk attribution; cross-firm account routing is rejected; “a subscription never creates or enables an execution route” passes; the reference route is disabled. | Local pass |
| `M1-E03` Identical inputs reproduce IDs and traces without weakening production identity | Fixed deterministic namespace/counter inputs reproduce the same 24-byte order IDs. Equivalent ordered/reordered reference configuration produces equal trace records, 1,285 canonical bytes, and the same digest. Production entropy success and fail-closed paths are tested independently. | Local pass; remote platform matrix pending |
| `M1-E04` Configuration and metadata revisions are available to later evidence | `ConfigurationProvenance` owns the overall fingerprint and configuration, organization, strategy, subscription, route, and per-instrument metadata revisions. Every M1 trace record copies the applicable provenance. Later admission/event types must consume this established contract when introduced. | M1 contract and trace proof complete |
| `M1-E05` Arithmetic boundaries, rounding direction, and overflow are covered | Tests cover strict parsing, all signed rounding directions, ties-to-even, exact/misaligned increments, quantization, min/max coefficients, rejected unsigned narrowing, scale overflow, multiply/divide overflow, division by zero, and invalid rounding values. | Local pass |

## Deterministic reference vectors

- The accepted reference startup snapshot encodes with `AEGISCFG` schema 1 and has fingerprint
  `e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310`.
- Its four-record bounded provenance trace encodes to 1,285 `AEGISTRS` schema-one bytes and has
  digest `242691fdfaa6377bf86b0bd3642ece7a8223e52936d3a67d7a2d5b5731033749`.
- Reordering every input collection does not change the canonical configuration or trace identity.
  A three-record sink rejects the fourth record without modifying its accepted prefix or digest.

These are configuration/provenance fixtures, not market-event replay or an order-submission test.
M2 owns runtime dispatch; M3 owns order admission and fake transport initiation.

## Local verification

The matrix layers ordinary behavior, compile-time type boundaries, sanitizer instrumentation and an
optimized build. A passing normal suite alone is therefore not treated as proof of portability,
undefined-behavior safety or release-mode correctness.

With the documented `.venv` activated, the closeout commands are:

```sh
cmake --workflow --preset verify
cmake --preset format
cmake --build --preset format --target format-check
cmake --workflow --preset verify-asan-ubsan
cmake --workflow --preset verify-tsan
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
python3 tools/validate_benchmark_evidence.py
git diff --check
```

The following results were produced on arm64 macOS 15.7.4 with AppleClang 16, CMake 3.31.10, and
Ninja 1.13.0:

| Check | Result on 2026-08-18 |
|---|---|
| Strict warning-clean debug build and tests | Pass: 71/71 tests, comprising 69 unit and 2 separately labeled deterministic-scenario cases |
| clang-format and Ruff check | Pass |
| AddressSanitizer plus UndefinedBehaviorSanitizer | Pass: 71/71 tests; no finding |
| ThreadSanitizer | Pass: 71/71 tests; no finding |
| Release build/tests and M0 benchmark-evidence regression | Pass: 71/71 release tests; `BENCH-M0-HARNESS-001` emitted and its bundle validated as uncontrolled smoke evidence only |
| `git diff --check` plus the new-file whitespace check | Pass |

M1 defines no product-performance workload or latency target: the first named product workloads
belong to M2. The benchmark commands above only regress `BENCH-M0-HARNESS-001` and its evidence
plumbing; their timing is not M1 or production latency evidence.

## Pending remote and integration evidence

M1 remains open until all of the following are recorded:

- a feature-branch push and pull request with `dev` verified as its base;
- passing remote formatting, GCC, Clang, AppleClang, ASan+UBSan, TSan, and benchmark-evidence jobs;
- review approval and merge into `dev`;
- the final PR, CI run, and merge commit added to this record.

No remote run, review, or merge is claimed by this document.
