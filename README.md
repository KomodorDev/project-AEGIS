# AEGIS

> **Purpose:** Give a new contributor the shortest safe path to understand the integrated M1/M2
> baseline and locally complete M3 fake-submission slice without mistaking it for a connected system.

AEGIS (Asynchronous Exchange Gateway and Inventory System) is a deterministic trading and risk
engine under development. M1 provides the dependency-light values and sealed startup rulebook. M2
adds a bounded serialized owner, recorded fixture playback, transactional market books, and
Ready-only strategy dispatch. M3 locally adds route authorization, exact fixed risk, outbound OMS
admission, exact fake encoding, and an in-memory fake write-initiation boundary. The reference slice
remains deliberately narrow: one Deribit testnet account alias, one BTC inverse perpetual, one
active subscribed bot, and no exchange connectivity.

M3's canonical order, fixed startup risk, outbound OMS, and conservative fake-initiation contracts
are accepted in ADR-0008 and ADR-0009 and locally implemented on
`codex/m3-canonical-submission`. Its [M3 exit-evidence record](docs/milestones/m3-exit-evidence.md)
anchors the verified implementation and smoke benchmark producer. The branch is unpublished, has
no pull request or remote CI evidence, and is not merged into `dev`; the integrated baseline remains
M2. M3 adds only deterministic in-memory fakes and no exchange connectivity.

The accepted system design is in [the architecture overview](docs/architecture.md). Delivery is
organized by capability gates in [the implementation roadmap](docs/implementation-roadmap.md).
Current milestone evidence is mapped in the [M1 exit-evidence record](docs/milestones/m1-exit-evidence.md),
the integrated [M2 exit-evidence record](docs/milestones/m2-exit-evidence.md), and the local-only
[M3 exit-evidence record](docs/milestones/m3-exit-evidence.md). After M1 merged,
the recorded M2-only commits were deliberately rebased onto its `dev` merge commit. M2 then ended at
feature head `4b5d89e834b45fef30fca87689937770d2c2ab35`, which
[PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8) merged unchanged into `dev` as
`d7733bb16a52d5ec954338f861d798d8c6620dad`. The exit record keeps its revision-specific local
verification and smoke-benchmark evidence separate from that later integration fact.

## What M1 provides

These are configuration and evidence capabilities, not active trading services. Each item is usable
without credentials, networking or a runtime event loop.

- Validated nominal identifiers, typed clocks, sequences and revisions, stable result/error values,
  deterministic test providers, and restart-namespaced production order identities.
- Exact checked fixed-point price, quantity and notional arithmetic with explicit rounding, plus
  revisioned instrument tick, step, minimum and multiplier validation.
- One or more peer firm roots with immutable firm → desk → bot attribution. Logical accounts belong
  to firms, and a route cannot cross into another firm's account.
- Separate market-data subscriptions and execution-route declarations. The reference route is
  disabled, and a subscription never grants permission to trade.
- Atomic immutable startup configuration encoded as canonical `AEGISCFG` bytes and identified by a
  SHA-256 fingerprint with all applicable revisions.
- Bounded canonical `AEGISTRS` in-memory traces for deterministic reference-scenario comparison;
  capacity failure is explicit and file or network reporting remains off-path.

M1 does not contain a serialized market runtime, order-book processing, strategy callbacks, risk or
OMS behavior, exchange sessions, credentials, sockets, or order transmission. Those capabilities
belong to later milestone gates.

## What M2 provides

These are credential-free in-process runtime capabilities. They add no venue session, network,
credential, order, risk, OMS, or transmission boundary.

- A fixed-capacity producer-safe ingress and one serialized mutable owner, with deterministic and
  dedicated drivers, explicit capacity/closure decisions, queue age, and ordered source-loss fences.
- Strict recorded-frame parsing and policy-backed normalization into immutable source-attributed
  market commands.
- Fixed-depth transactional books with explicit `Synchronizing`, `Ready`, `Stale`, and `Invalid`
  states; only a complete valid snapshot can recover readiness.
- Canonical subscription dispatch, separate state and market callbacks, read-only Ready book views,
  bounded diagnostics, re-entry rejection, and exact pre-commit callback/trace reservation.
- Canonical `AEGISRTS` replay evidence. The complete M2 scenario reproduces all 28 callbacks and 71
  trace records byte-for-byte under two manual runs and the dedicated driver.

## What M3 provides locally

These are credential-free in-process submission capabilities. They stop at a structurally offline
fake accepted-slot boundary; they do not encode or transmit a venue-native order.

- A bot-bound `submit` capability whose firm, desk, bot, strategy, account, venue, and local order
  identity cannot be supplied by strategy code; the requested instrument must exactly match the
  authorized route projection.
- Explicit owner-local route authorization followed by deterministic limit-only, GTC, no-flags
  economic validation against exact metadata precision, minimum quantity, lot step, and price tick.
- One immutable canonical risk-policy snapshot, exact checked fixed-point inverse-contract
  exposure, bot/desk/firm/account/route/instrument/venue scopes, and atomic check-and-reserve with
  exact-once definitive rollback.
- Bounded outbound OMS admission with duplicate-identity and capacity results, exact deterministic
  `AEGISFOE` encoding, and a final in-memory fake initiator that is structurally incapable of live
  communication.
- Three disjoint local results: known local rejection, local fake `WriteInitiated`, and conservative
  `SubmissionUnknown`. The latter retains exposure and is never automatically retried. Neither
  `WriteInitiated` nor `SubmissionUnknown` is an exchange acknowledgement.
- Bounded canonical `AEGISSTS` evidence, bounded diagnostics, deterministic manual/dedicated replay,
  named submission benchmarks, and a fail-closed forbidden-capability audit.

## Quick start

The supported baseline is C++20 with CMake 3.25 or newer and Ninja. Python 3.11 or newer runs the
developer bootstrap and benchmark-evidence tooling. CI pins CMake 3.31.10 and Ninja 1.13.0. The
complete platform/compiler policy is recorded in
[ADR-0002](docs/decisions/0002-delivery-toolchain.md).

An optional isolated installation of the pinned developer tools is:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
source .venv/bin/activate
```

The tracked VS Code workspace setting points CMake Tools at this `.venv` and prepends its `bin`
directory so the matching Ninja is found. Create the environment before using CMake Tools; command-line
users may instead provide any supported CMake and Ninja installation through `PATH`.

From a fresh checkout, the one verification command is:

```sh
cmake --workflow --preset verify
```

That command configures, builds, and runs the unit and deterministic scenario tests with strict
warnings treated as errors. The first configure downloads immutable, checksum-verified Catch2 and
Google Benchmark source archives into the ignored build tree. It does not contact an exchange or
require credentials.

The `--workflow --preset` form selects an ordered configure/build/test workflow. A plain `--preset`
selects one configure preset, while `--build --preset` selects a previously configured build preset.

Other quality commands are:

```sh
cmake --preset format
cmake --build --preset format --target format-check
cmake --workflow --preset verify-asan-ubsan
cmake --workflow --preset verify-tsan
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
python3 tools/validate_benchmark_evidence.py
python3 tools/run_benchmarks.py --suite m2
python3 tools/validate_benchmark_evidence.py --suite m2
python3 tools/run_benchmarks.py --suite m3
python3 tools/validate_benchmark_evidence.py --suite m3
python3 tools/check_forbidden_capabilities.py
```

The benchmark runner first rebuilds its Release target, then writes raw timing output and a context
manifest. The validator independently checks workload identity, units, samples, fingerprints, build
mode, hashes, and Git provenance. The no-argument commands retain the M0 calibration contract; the
explicit M2 suite runs the executor, callback, and complete market-owner workloads. The explicit M3
suite runs successful fake initiation and inline risk rejection. An M2 or M3 result is only a
`REF-MAC-01` qualification when every required host-control field is truthfully set and validated;
otherwise it is smoke evidence. The M3 exit record deliberately reports its uncontrolled clean run
as smoke and makes no provisional p99 threshold claim.

The build contains no exchange session, native/live exchange order-transmission capability,
credential lookup, socket, or production endpoint. M3's accepted-slot copy is only an in-memory
fake and is never an exchange acknowledgement. The first market and account assumptions are
described in [the reference scenario](docs/reference-scenario.md).

## Milestone records

- [Serialized data-plane decision](docs/decisions/0001-serialized-data-plane-execution.md)
- [Delivery and toolchain decision](docs/decisions/0002-delivery-toolchain.md)
- [Dedicated testnet account policy](docs/decisions/0003-dedicated-testnet-account.md)
- [Deterministic domain value contracts](docs/decisions/0004-domain-value-contracts.md)
- [Immutable configuration provenance](docs/decisions/0005-immutable-configuration-provenance.md)
- [Bounded deterministic runtime](docs/decisions/0006-bounded-deterministic-runtime.md)
- [Market-state validity](docs/decisions/0007-market-state-validity.md)
- [Canonical bot-bound submission and fixed risk](docs/decisions/0008-canonical-submission-and-fixed-risk.md)
- [Outbound OMS and conservative fake initiation](docs/decisions/0009-outbound-oms-and-fake-initiation.md)
- [Deribit BTC perpetual reference scenario](docs/reference-scenario.md)
- [Correctness and performance budgets](docs/quality-budgets.md)
- [Deribit public-protocol spike](docs/protocol-spikes/deribit-btc-perpetual.md)
- [Repository layout and dependency rules](docs/architecture/repository-layout.md)
- [M0 exit evidence](docs/milestones/m0-exit-evidence.md)
- [M1 exit evidence](docs/milestones/m1-exit-evidence.md)
- [M2 exit evidence](docs/milestones/m2-exit-evidence.md)
- [M3 exit evidence](docs/milestones/m3-exit-evidence.md)
