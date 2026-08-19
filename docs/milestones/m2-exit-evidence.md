# M2 Exit Evidence

> **Purpose:** Record the pinned stacked base, planned M2 scope, exit-gate proof, named workload
> evidence, and the dependency conditions that prevent premature integration claims.

**Status:** Implementation is in progress on `codex/m2-deterministic-runtime`. This branch is
temporarily stacked on the unmerged M1 review head; no M2 pull request may be opened until M1 is
merged into `dev` and the M2-only commits are deliberately rebased.

## Stacked-base record

| Field | Recorded value |
|---|---|
| M1 branch | `codex/m1-domain-kernel` |
| Pinned M1 base | `796321825d701f3add83af104b7924eb2826fd07` |
| M1 pull request | `https://github.com/KomodorDev/project-AEGIS/pull/7` |
| M1 pull-request base | `dev` |
| `dev` at stack creation | `d2efad648f9859b2d08d9d31a22f14a7c5fe7d91` |
| M2 branch | `codex/m2-deterministic-runtime` |
| M2-only range | `796321825d701f3add83af104b7924eb2826fd07..HEAD` |
| Stack created | 2026-08-19, after local, remote-tracking, and live PR heads matched the pin |

If either M1 branch moves during review, M2 must record the new tip and reconcile deliberately. It
must not merge or rebase that movement silently. After M1 merges, only the recorded M2-only range may
be replayed onto the updated `dev` branch. No M2 commit belongs on the M1 branch.

## Accepted M2 contracts

- [ADR-0006](../decisions/0006-bounded-deterministic-runtime.md) fixes bounded admission, one owner,
  queue age, overload fencing, non-reentrancy, and deterministic/dedicated driver equivalence.
- [ADR-0007](../decisions/0007-market-state-validity.md) fixes normalized fixture boundaries,
  transactional books, four-state validity, snapshot-only recovery, Ready-only market callbacks,
  canonical subscription dispatch, and exact runtime traces.
- `AEGISCFG` and `AEGISTRS` schema-one bytes remain M1 compatibility anchors. M2 adds separately
  fingerprinted runtime policy and runtime trace schemas.

## Scope evidence

Implementation links and results will replace the planned state as each independently green commit
lands.

| Gate | Required behavior | Current evidence |
|---|---|---|
| `M2-S01` | Fixed-capacity serialized executor, deterministic and dedicated drivers, explicit admission failure, capacity, queue age, and source discontinuity fencing | Planned |
| `M2-S02` | Minimum normalized market update and post-commit event contracts with distinct source/receive identity, metadata revision, generation, and revision | Planned |
| `M2-S03` | Strict credential-free recorded fixture/parser boundary with no final networking primitive | Planned |
| `M2-S04` | Owner-local transactional books and explicit `Synchronizing`, `Ready`, `Stale`, and `Invalid` transitions | Planned |
| `M2-S05` | Bot runtime, separate market/state callbacks, bot-bound context, and canonical subscription dispatch | Planned |
| `M2-S06` | Bounded structured diagnostics, trace preflight, callback measurement, owner checks, and re-entry rejection | Planned |
| `M2-S07` | Exact callback and `AEGISRTS` replay plus multi-firm support and the single-firm reference scenario | Planned |

## Exit-gate evidence

| Gate | Required proof | Current evidence |
|---|---|---|
| `M2-E01` | One matching recorded update invokes exactly the subscribed strategy grant; unrelated bots receive no market callback | Planned |
| `M2-E02` | Malformed input reaches no market callback, changes no book, and cannot corrupt the following valid input | Planned |
| `M2-E03` | Duplicate, older, gap, conflict, integrity, metadata, stale, and resnapshot behavior follows one deterministic transition table | Planned |
| `M2-E04` | Strategies distinguish `Ready`, `Synchronizing`, `Stale`, and `Invalid`, while only `Ready` supplies a book view | Planned |
| `M2-E05` | A callback observes either the complete prior book or complete committed candidate, never a partial update | Planned |
| `M2-E06` | Repeating one replay yields identical callback vectors, canonical runtime-trace bytes, and digest | Planned |
| `M2-E07` | All mutable data-plane state is owned by one bound executor; wrong-owner and recursive entry fail before mutation | Planned |

## Named quality workloads

| Workload | Required interval and metrics | Current evidence |
|---|---|---|
| `BENCH-M2-EXEC-001` | Successful admission and one no-op serialized turn; ns/turn, turns/s, allocations/turn | Planned |
| `BENCH-M2-CALLBACK-001` | Entry-to-return of the deterministic strategy on a prebuilt coherent view; microsecond p50/p99/p99.9, callbacks/s, allocations/callback | Planned |
| `BENCH-M2-MD-001` | One owner turn applying a valid delta to a fixed 20-level BTC perpetual book plus one callback; microsecond p50/p99/p99.9, events/s, allocations/event | Planned |

Only `BENCH-M2-CALLBACK-001` has an absolute M2 target: p99 at or below 100 microseconds on
`REF-MAC-01` under the controlled conditions in the quality budgets. The other two workloads require
complete evidence without an invented threshold. CI results are smoke and correctness evidence, not
controlled-host qualification.

## Verification record

Normal, formatting, sanitizer, release, deterministic replay, benchmark, review, and integration
results are pending implementation. Every recorded bundle must identify the Git revision and tree
state, configuration and runtime-policy fingerprints, preset, compiler, OS, host, CPU and memory,
tool versions, executable/result hashes, power/isolation/thermal state, samples, and warmup.

## Deferred and forbidden capability audit

M2 must contain no sockets, endpoint URLs, live venue connection, credentials, private sessions,
order request or submission API, OMS, risk decision, or transmission capability. M3 owns orders; M6
owns real public connectivity and venue-native protocol policy; M7 owns private sessions; M8 owns
transmission.

## Integration status

- M1 remains under review and must not be merged, retargeted, marked ready, or modified by M2.
- M2 has no pull request while its dependency is unmerged.
- Final M2 review and merge evidence remains pending even after local exit checks pass.
