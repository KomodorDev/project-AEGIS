# Repository Layout

> **Purpose:** Describe the implemented M1-M3 source/test ownership boundaries, later growth
> direction, and dependency rules that expose current coupling and prevent empty scaffolding or
> hidden new cycles.

**Status:** The directories used through M3 are implemented locally. The intended dependency
direction has documented M2 and M3 same-library coupling described below; the folder graph is not
claimed to be acyclic. Strategy and bot contracts live with their serialized owner under `runtime`;
there is no separate empty `strategy` area. The M3 feature branch is published as
[PR #10](https://github.com/KomodorDev/project-AEGIS/pull/10), targeting `dev`, but is not merged;
areas named for later milestones remain **Proposed**. Runtime ownership
follows [ADR-0001](../decisions/0001-serialized-data-plane-execution.md); M1 value and provenance
boundaries follow [ADR-0004](../decisions/0004-domain-value-contracts.md) and
[ADR-0005](../decisions/0005-immutable-configuration-provenance.md); M2 runtime and market ownership
follow [ADR-0006](../decisions/0006-bounded-deterministic-runtime.md) and
[ADR-0007](../decisions/0007-market-state-validity.md). M3 execution, risk, OMS, fake-initiation,
and evidence ownership follows
[ADR-0008](../decisions/0008-canonical-submission-and-fixed-risk.md) and
[ADR-0009](../decisions/0009-outbound-oms-and-fake-initiation.md).

Return to the [architecture overview](../architecture.md).

## Design Goals

The source layout should make it easy to answer three questions:

1. Which subsystem owns a type or behavior?
2. Which code is exchange-native and which code is venue-independent?
3. Which dependencies point toward stable domain contracts rather than infrastructure details?

Directories are added only when the first real implementation or test needs them. Empty scaffolding
and `.gitkeep` files do not establish useful architecture.

## Implemented M1-M3 Source Shape

Headers shared across repository targets live under `include/aegis`; implementations mirror their
owning area under `src/aegis`. This is a source-build boundary, not a stable installed API or ABI.

```text
include/aegis/                 src/aegis/
├── configuration/            ├── configuration/
├── execution/                ├── execution/
├── market_data/              ├── market_data/
├── model/                    ├── model/
├── oms/                      ├── oms/
├── organization/             ├── organization/
├── risk/                     ├── risk/
├── runtime/                  ├── runtime/
├── trace/                    ├── trace/
└── version.hpp               └── version.cpp
```

| Area | Current responsibility |
|---|---|
| `model` | Dependency-light identifiers, stable results/errors, checked fixed-point values, typed time/revisions, deterministic/scripted and fail-closed production order-ID providers, instrument metadata, and SHA-256. It must not become a miscellaneous dumping ground. |
| `organization` | Immutable peer-firm, desk, bot, and derived attribution values. |
| `market_data` | Subscription declarations plus M2 recorded fixture, normalized update/event, transactional book, and validity contracts. It contains no venue networking. |
| `execution` | M1 execution-route declarations plus M3 canonical order/result contracts, owner-local authorization, submission policy/measurement values, exact fake encoding, and the structurally offline fake initiator; real transmission begins in M8. |
| `configuration` | Atomic startup validation, canonical `AEGISCFG` encoding, SHA-256 identity, and revision provenance across the M1 sections. |
| `risk` | M3 immutable `AEGISRSP` fixed policy, exact exposure, atomic multi-scope checks, reservations, and exact-once definitive release. Dynamic modes and control-plane policy calculation begin in M5. |
| `oms` | M3 bounded outbound identity admission and local encoding/initiation state. Private-event reconciliation and the complete exchange lifecycle begin in M4. |
| `runtime` | M2 policy, bounded admission, deterministic/dedicated drivers, bot/strategy ownership, Ready/state callbacks, and M3 fake-submission composition, private coordinator, diagnostics, evidence projection, and capability-limited bot context. |
| `trace` | Separate bounded M1 `AEGISTRS`, M2 `AEGISRTS`, and M3 `AEGISSTS` canonical evidence; external reporting and persistence are excluded. |

Component-owned types stay with their component. A normalized market event will belong to
`market_data`, an order request will belong to `execution`, and a risk mode will belong to `risk`.
A type moves to `model` only when it is genuinely neutral and shared.

The expected later areas remain proposed:

| Proposed area | Intended responsibility |
|---|---|
| `inventory` | Fill-derived positions, exposure attribution, and bot/desk/firm aggregation. Durable ledger ownership and P&L placement remain open. |
| `venues/<venue>` | Venue-native connectivity, parsing, symbol mapping, authentication, reconciliation, and order encoding. |

## Dependency Rules

These rules matter more than the folder names: they prevent venue details and runtime wiring from
leaking into reusable domain behavior.

Read each rule as a description of the current compile-time direction and its explicit exceptions,
not as permission to reach through another component's immutable API and mutate its state.

- `model` is a leaf and does not depend on another AEGIS subsystem.
- `organization` depends only on `model`.
- Leaf execution contracts such as route declarations, order requests, and the measurement-clock
  boundary depend on `model` and `organization`. `configuration` composes and cross-validates those
  route declarations plus market-data declarations. M2 `market_data` also directly consumes the
  immutable M2 runtime policy and runtime-trace preflight authority; this implemented same-library
  coupling is explicit technical debt rather than a model for new reverse dependencies.
- M3's higher execution layer deliberately crosses folder boundaries inside the one product library:
  `submission_route` consumes sealed `configuration`; `SubmitResult` carries stable `risk` scope
  evidence; and the fake encoder accepts only an `oms`-admitted record. In the other direction,
  `risk` and `oms` consume stable execution values. These are compile-time folder cycles, not
  separate CMake target cycles. They are documented constraints to simplify deliberately before a
  future library split, not permission to introduce process-global state or live infrastructure.
- `trace` depends on the stable value contracts needed by each canonical schema and immutable
  provenance. It does not own domain mutation and performs no file, database, network, or console
  I/O. Separate `AEGISTRS`, `AEGISRTS`, and `AEGISSTS` sinks prevent one milestone from consuming
  another milestone's preflighted capacity.
- Venue-independent code never includes or exposes a venue-native message type.
- Venue code may later implement normalized market-data and execution contracts; those contracts do
  not depend on venue implementations.
- Strategies consume normalized events and submit through a bot-bound execution boundary. They do
  not call adapters, sessions, or sockets.
- The strategy interface and bot context are owned by `runtime`: the observation-only M2 composition
  exposes no order authority, while the explicit M3 fake-submission composition adds only the
  normalized `execution::OrderRequest` and `SubmitResult` boundary.
- `runtime` is the composition root and may depend on subsystem implementations. Lower-level
  subsystems do not depend on the composition root to find one another; the narrow market-turn
  preflight authority is an injected interface owned by `market_data`. The preceding M2
  policy/trace includes are the current narrow exception to this intended direction.
- Control-plane and data-plane code exchange immutable values or snapshots. They do not share
  mutable domain objects.
- Cross-subsystem state access uses explicit ownership boundaries and stable identifiers, not
  process-global mutable singletons.
- M3 keeps the private coordinator in the `runtime` composition root. It constructs the route,
  policy, ledger, OMS, encoder, initiator, trace, and diagnostics once and then invokes them directly;
  no lower subsystem depends on `runtime` to find another component. Submission trace records
  observe stable values from those layers without becoming a mutation owner. `inventory` joins the
  composition in M4; no type moves into `model` merely to disguise a folder dependency.

Avoid generic directories such as `common`, `utils`, or `core`. A reusable helper remains with the
subsystem that gives it meaning until there is evidence of a genuinely independent abstraction.

## Implemented M1-M3 Test Shape

```text
tests/
├── support/
├── unit/
│   ├── configuration/
│   ├── execution/
│   ├── market_data/
│   ├── model/
│   ├── oms/
│   ├── organization/
│   ├── risk/
│   ├── runtime/
│   └── trace/
├── deterministic_scenarios/
└── tooling/
```

| Area | Coverage |
|---|---|
| `support` | Reusable accepted disabled-route reference plus separately fingerprinted enabled two-firm M3 configuration and policy builders. |
| `unit` | Deterministic behavior, boundary failures, compile-time type separation, ownership checks, transition matrices, and canonical golden vectors within one implemented area. |
| `deterministic_scenarios` | Complete M1 configuration/provenance replay, M2 recorded-market validity replay, and the ten-attempt M3 local-submission matrix under manual/dedicated owners, with no credentials, transports, or exchange access. |
| `tooling` | Fail-closed forbidden-capability scanner and M3 benchmark-evidence validator self-tests. |
| `venue_contract` (**Proposed**) | Future parsing and encoding against sanitized credential-free fixtures. |
| `integration` (**Proposed**) | Future venue/private-event, recovery, persistence, and control-plane boundaries that span later subsystem contracts. |

Tests are grouped by behavior rather than copied mechanically from build targets. No fixture may
contain a live credential, and no test may submit a real order without explicit authorization.

## Deliberately Open

M0 selected CMake and the test/measurement baseline; M1-M3 add no third-party product runtime
dependency. The following choices still wait for concrete milestone requirements:

- runtime networking, asynchronous-I/O, external serialization, or persistence libraries;
- the long-term installed/public API boundary beyond the current cross-target `include/aegis`
  headers;
- library target count, linkage model, or C++ modules;
- executable, service, deployment, or UI directory structure;
- generated code, persistence, or migration directories;
- placement and timing of realized and unrealized P&L calculations.

Canonical M1 configuration/provenance, M2 runtime trace, and M3 risk/submission/fake-order/trace
bytes are internal deterministic evidence formats; they do not select a networking, storage, or
general-purpose serialization framework.
