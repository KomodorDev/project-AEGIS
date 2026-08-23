# M2 Exit Evidence

> **Purpose:** Record the original pinned stack, deliberate post-M1 rebase, implemented M2 scope,
> exit-gate proof, named workload evidence, and later integration without moving evidence anchors.

**Status:** M2 implementation is integrated. The feature branch ended at
`4b5d89e834b45fef30fca87689937770d2c2ab35` after the exact replay anchor
`a3c090819f219681bfe3b903565c87490028ccbf`, post-audit source-sequence hardening, and a final GCC
portability test fix. Normal, formatting, ASan+UBSan, TSan, release, deterministic replay, and M0/M2
benchmark-evidence checks passed locally as recorded below. The M2 timing bundle remains smoke
evidence and makes no `REF-MAC-01` threshold claim.

M1 merged with its reviewed head unchanged. On 2026-08-20, the eleven original M2 delivery commits
were deliberately rebased onto M1's `dev` merge commit. The local evidence was captured before M2
publication. On 2026-08-21, [PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8) later merged
the exact feature head into `dev` as `d7733bb16a52d5ec954338f861d798d8c6620dad`. That merge records
integration; it does not move the revision-specific verification or timing evidence to a new
producer revision, and it creates no new controlled-host performance-qualification claim.

## Dependency and rebase record

| Field | Recorded value |
|---|---|
| M1 branch | `codex/m1-domain-kernel` |
| Pinned M1 review head and original M2 base | `796321825d701f3add83af104b7924eb2826fd07` |
| M1 pull request | `https://github.com/KomodorDev/project-AEGIS/pull/7` |
| M1 pull-request base | `dev` |
| `dev` at stack creation | `d2efad648f9859b2d08d9d31a22f14a7c5fe7d91` |
| M1 merge commit | `6287e01be33300e0e7ea24c76dd64a869c67612b` |
| M1 merged | 2026-08-19 18:34:32 UTC, with the reviewed head unchanged |
| M2 branch | `codex/m2-deterministic-runtime` |
| Original M2-only range | `796321825d701f3add83af104b7924eb2826fd07..cc641110a2ec39f10fb89eb43f963711b9d2276d` |
| Exact replayed M2 range | `6287e01be33300e0e7ea24c76dd64a869c67612b..a3c090819f219681bfe3b903565c87490028ccbf` |
| Final M2 feature range | `6287e01be33300e0e7ea24c76dd64a869c67612b..4b5d89e834b45fef30fca87689937770d2c2ab35` |
| M2 feature head | `4b5d89e834b45fef30fca87689937770d2c2ab35` |
| M2 pull request | [PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8) |
| M2 pull-request base | `dev` |
| M2 merge commit and M3 starting baseline | `d7733bb16a52d5ec954338f861d798d8c6620dad` |
| M2 integrated | 2026-08-21, with the feature-head and merge-commit trees identical |
| Stack created | 2026-08-19, after local, remote-tracking, and live PR heads matched the pin |
| Deliberate rebase | 2026-08-20, after fetching and verifying the exact M1 merge ancestry |

The M1 review head remained at the pin through merge. `git range-diff` showed all eleven original M2
delivery commits patch-identical and in the same order; the original and rebased benchmark commits
have the same tree `c27d2f781f745db55e66333ab56d33018dded522`. No M2 commit was written to the M1
branch. PR #8 later merged the final M2 feature head unchanged: both the feature head and merge
commit have tree `2c95f9d19644c68842c21769a41faa73044ad2ff`. The merge commit is the integration
anchor, while the revision-specific verification and benchmark records below remain attached to
their original feature-branch producers.

## Accepted M2 contracts

- [ADR-0006](../decisions/0006-bounded-deterministic-runtime.md) fixes bounded admission, one owner,
  queue age, overload fencing, non-reentrancy, and deterministic/dedicated driver equivalence.
- [ADR-0007](../decisions/0007-market-state-validity.md) fixes normalized fixture boundaries,
  transactional books, four-state validity, snapshot-only recovery, Ready-only market callbacks,
  canonical subscription dispatch, and exact runtime traces.
- `AEGISCFG` and `AEGISTRS` schema-one bytes remain M1 compatibility anchors. M2 adds separately
  fingerprinted runtime policy and runtime trace schemas.

## Scope evidence

| Gate | Required behavior | Current evidence |
|---|---|---|
| `M2-S01` | Fixed-capacity serialized executor, deterministic and dedicated drivers, explicit admission failure, capacity, queue age, and source discontinuity fencing | `runtime_policy.*`, `serialized_executor.*`, `dedicated_executor_driver.*`, and `market_runtime.*`; executor/concurrency, capacity-fence, queue-age, and driver-parity tests; commits `78f0f43`, `3ed9626`, `a1083dc` |
| `M2-S02` | Minimum normalized market update and post-commit event contracts with distinct source/receive identity, metadata revision, generation, and revision | `market_event.*`, `market_limits.hpp`, nominal runtime/market counters, and `runtime_policy.*`; `market_event_test.cpp` and `runtime_policy_test.cpp`; commit `b3ba89d` |
| `M2-S03` | Strict credential-free recorded fixture/parser boundary with no final networking primitive | `recorded_fixture.*`; grammar, truncation, bound, unsupported-input, source-resolution, normalization, and recovery tests; commit `6ae0060` |
| `M2-S04` | Owner-local transactional books and explicit `Synchronizing`, `Ready`, `Stale`, and `Invalid` transitions | `order_book.*`, `market_state_machine.*`, and composed `market_runtime.*`; transactional, continuity, integrity, metadata, staleness, control, and exact-preflight tests; commits `ab4cfc8`, `a1083dc` |
| `M2-S05` | Bot runtime, separate market/state callbacks, bot-bound context, and canonical subscription dispatch | `bot_runtime.*` and `market_runtime.*`; exact routing, attribution, multi-firm order, callback measurement, and re-entry tests; commits `3ea53d8`, `a1083dc` |
| `M2-S06` | Bounded structured diagnostics, trace preflight, callback measurement, owner checks, and re-entry rejection | `runtime_diagnostics.*`, `runtime_trace.*`, executor owner faults, and exact state/dispatch preflight; accepted-prefix, exhaustion, budget/clock, post-commit fault, and re-entry tests; commits `99a3c7d`, `3ed9626`, `a1083dc` |
| `M2-S07` | Exact callback and `AEGISRTS` replay plus multi-firm support and the single-firm reference scenario | `m2_reference_scenario_test.cpp` compares two manual runs and one dedicated run; canonical multi-firm dispatch is pinned in `bot_runtime_test.cpp`; commit `df8e454` |

## Exit-gate evidence

| Gate | Required proof | Current evidence |
|---|---|---|
| `M2-E01` | One matching recorded update invokes exactly the subscribed strategy grant; unrelated bots receive no market callback | BotRuntime exact-routing test plus the full scenario's unrelated registered strategy count of zero |
| `M2-E02` | Malformed input reaches no market callback, changes no book, and cannot corrupt the following valid input | Parser recovery, composed malformed-input recovery, and scenario `MalformedRejected` followed by a valid snapshot |
| `M2-E03` | Duplicate, older, gap, conflict, integrity, metadata, stale, and resnapshot behavior follows one deterministic transition table | State-machine matrix plus the scenario's ordered 25-disposition vector covering every named branch, session reset, source discontinuity, and snapshot-only recovery |
| `M2-E04` | Strategies distinguish `Ready`, `Synchronizing`, `Stale`, and `Invalid`, while only `Ready` supplies a book view | State/event tests and the scenario's complete 18-state vector; only its ten Ready market callbacks carry book views |
| `M2-E05` | A callback observes either the complete prior book or complete committed candidate, never a partial update | Transactional candidate tests, composed snapshot/delta test, and exact complete bid/ask vectors for all ten scenario market callbacks |
| `M2-E06` | Repeating one replay yields identical callback vectors, canonical runtime-trace bytes, and digest | Two manual replays and one dedicated replay compare structurally: 26 turns, 28 callbacks, 71 records, 29,610 bytes, digest `e63b89b8c3a826fd1104f9e9949364606be5efbeedc112cb980260ccb70fda0b` |
| `M2-E07` | All mutable data-plane state is owned by one bound executor; wrong-owner and recursive entry fail before mutation | Bound-owner, wrong-owner, active-turn re-entry, callback nested-drive, producer-coordination, and manual/dedicated parity tests |

## Named quality workloads

| Workload | Required interval and metrics | Current evidence |
|---|---|---|
| `BENCH-M2-EXEC-001` | Successful admission, then owner execution of one no-op serialized turn; ns/turn, turns/s, allocations/turn | 10,000 samples: 54.4353 ns/turn; 18,370,432 turns/s; 0 allocations/turn |
| `BENCH-M2-CALLBACK-001` | Entry-to-return of the deterministic strategy on a prebuilt coherent view; microsecond p50/p99/p99.9, callbacks/s, allocations/callback | 10,000 samples: p50 0.167 µs; p99 0.208 µs; p99.9 0.209 µs; 6,453,070 callbacks/s; 0 allocations/callback |
| `BENCH-M2-MD-001` | One owner turn applying a valid delta to a fixed 20-level-per-side BTC perpetual book plus one callback; microsecond p50/p99/p99.9, events/s, allocations/event | 10,000 samples: p50 4.333 µs; p99 8.292 µs; p99.9 24.542 µs; 221,661 events/s; 12 allocations/event |

Only `BENCH-M2-CALLBACK-001` has an absolute M2 target: p99 at or below 100 microseconds on
`REF-MAC-01` under the controlled conditions in the quality budgets. The other two workloads require
complete evidence without an invented threshold. This local run used the reference hardware and AC
power with Low Power Mode disabled, but host isolation and thermal state remained `uncontrolled`.
The validator therefore classified it as smoke and emitted no callback-threshold claim. CI results
are likewise smoke and correctness evidence, not controlled-host qualification.

## Verification record

The clean pre-rebase implementation/benchmark revision
`cc641110a2ec39f10fb89eb43f963711b9d2276d`
(`c27d2f781f745db55e66333ab56d33018dded522`) passed. Its patch-identical rebased counterpart is
`a3c090819f219681bfe3b903565c87490028ccbf` with the same tree:

- normal: 172/172 tests, comprising 169 unit cases and three deterministic scenarios;
- ASan+UBSan: 172/172, with no finding;
- TSan: 172/172, with no finding;
- strict warning-as-error builds and repository formatting/diff checks; and
- M0 calibration plus M2 workload generation and independent evidence validation.

The completed feature branch through `4b5d89e834b45fef30fca87689937770d2c2ab35` additionally rejects
zero source and predecessor sequence sentinels before book state, proves fixture/domain separation,
contains a zero-sequence frame at the composed runtime boundary, and makes the replay context
GCC-safe. That feature branch passed 173/173 tests, comprising 170 unit cases and three deterministic
scenarios, under normal, ASan+UBSan, and TSan profiles; formatting and diff checks also passed.

The M2 scenario alone executes 471 assertions under each normal/sanitized profile. The reproducible
benchmark commands are:

```sh
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py --suite m0
python3 tools/validate_benchmark_evidence.py --suite m0
python3 tools/run_benchmarks.py --suite m2
python3 tools/validate_benchmark_evidence.py --suite m2
```

The clean M2 smoke bundle records configuration fingerprint
`e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310` and runtime-policy
fingerprint `6b54358bf90d5c565aa816e45955d67e274ed0636120b29a188e36fd43714c1c`.
Its executable SHA-256 is `f12e9ff5986e58d9c6552fb35149a682e56c2c54708bccce58ca74973296253d`;
`m2-runtime.json` is
`eb34d59297cdae0a401627d3f70e690e2796fab7b73a42401ddacd625a668e0c`; and
`m2-context.json` is
`a68fe692a127a363a8b563a9d3fd408de7f5d1f61427458900e31f78698049be`.

The context records Release/Ninja, AppleClang 16, macOS 15.7.4, MacBookPro18,2, Apple M1 Max,
32 GiB memory, ten logical/physical cores, 10,000 samples per workload, fixed setup/warmup policy,
AC power with Low Power Mode disabled, and uncontrolled isolation/thermal fields.

## Deferred and forbidden capability audit

The M2-owned production, test, benchmark, build, and tooling paths were audited for network includes
and calls, endpoint/credential literals, private-session capability, order requests/submission, OMS,
risk decisions, and transmission. No such implementation exists. Benign matches are negative
compile-time capability probes, explicit “no capability” comments, network-byte-order encoding,
checksum-pinned dependency URLs, CI's `persist-credentials: false`, and Linux CPU-topology probing.

M3 owns orders; M6 owns real public connectivity and venue-native protocol policy; M7 owns private
sessions; M8 owns transmission.

## Integration status

- M1 merged into `dev` as `6287e01be33300e0e7ea24c76dd64a869c67612b`; its reviewed head remained
  exactly `796321825d701f3add83af104b7924eb2826fd07`.
- The eleven original M2 delivery commits were deliberately replayed onto that merge commit with an
  exact `git range-diff`; M1's branch was not modified.
- At local evidence capture, M2 had no pull request. That remains a historical fact about when the
  revision-specific evidence above was produced.
- [PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8) subsequently targeted `dev` from exact
  feature head `4b5d89e834b45fef30fca87689937770d2c2ab35` and merged as
  `d7733bb16a52d5ec954338f861d798d8c6620dad` on 2026-08-21.
- The feature head and merge commit share tree `2c95f9d19644c68842c21769a41faa73044ad2ff`,
  so integration introduced no additional source change.
- M2 is integrated. Its local verification and smoke benchmark remain attributed to their recorded
  feature-branch revisions; the merge does not relabel smoke timing as qualification.
