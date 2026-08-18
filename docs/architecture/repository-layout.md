# Repository Layout

> **Purpose:** Describe the implemented M1 source and test ownership boundaries, the allowed
> dependency directions, and where later components should grow without empty scaffolding.

**Status:** The directories and dependency rules used through M1 are implemented. Areas named for
later milestones remain **Proposed** until their first real behavior is added. Runtime ownership
follows [ADR-0001](../decisions/0001-serialized-data-plane-execution.md); M1 value and provenance
boundaries follow [ADR-0004](../decisions/0004-domain-value-contracts.md) and
[ADR-0005](../decisions/0005-immutable-configuration-provenance.md).

Return to the [architecture overview](../architecture.md).

## Design Goals

The source layout should make it easy to answer three questions:

1. Which subsystem owns a type or behavior?
2. Which code is exchange-native and which code is venue-independent?
3. Which dependencies point toward stable domain contracts rather than infrastructure details?

Directories are added only when the first real implementation or test needs them. Empty scaffolding
and `.gitkeep` files do not establish useful architecture.

## Implemented M1 Source Shape

Headers shared across repository targets live under `include/aegis`; implementations mirror their
owning area under `src/aegis`. This is a source-build boundary, not a stable installed API or ABI.

```text
include/aegis/                 src/aegis/
├── configuration/            ├── configuration/
├── execution/                ├── execution/
├── market_data/              ├── market_data/
├── model/                    ├── model/
├── organization/             ├── organization/
├── trace/                    ├── trace/
└── version.hpp               └── version.cpp
```

| Area | Current responsibility |
|---|---|
| `model` | Dependency-light identifiers, stable results/errors, checked fixed-point values, typed time/revisions, order-ID providers, instrument metadata, and SHA-256. It must not become a miscellaneous dumping ground. |
| `organization` | Immutable peer-firm, desk, bot, and derived attribution values. |
| `market_data` | M1 subscription declarations only. Normalized events, market state, and dispatch begin in M2. |
| `execution` | M1 execution-route declarations and validation only. Order requests, authorization, risk, OMS, and transmission begin in M3. |
| `configuration` | Atomic startup validation, canonical `AEGISCFG` encoding, SHA-256 identity, and revision provenance across the M1 sections. |
| `trace` | Bounded structured M1 records plus canonical `AEGISTRS` encoding and digest; external reporting and persistence are excluded. |

Component-owned types stay with their component. A normalized market event will belong to
`market_data`, an order request will belong to `execution`, and a risk mode will belong to `risk`.
A type moves to `model` only when it is genuinely neutral and shared.

The expected later areas remain:

| Proposed area | Intended responsibility |
|---|---|
| `strategy` | Strategy contract, configured bot runtime, and bot-bound submission context. |
| `risk` | Risk modes and budgets, inline checks, reservations, and control-plane policy calculation. |
| `inventory` | Fill-derived positions, exposure attribution, and bot/desk/firm aggregation. Durable ledger ownership and P&L placement remain open. |
| `venues/<venue>` | Venue-native connectivity, parsing, symbol mapping, authentication, reconciliation, and order encoding. |
| `runtime` | Process composition, lifecycle, and the serialized data-plane executor; it must not own domain rules. |

## Dependency Rules

These rules matter more than the folder names: they prevent venue details and runtime wiring from
leaking into reusable domain behavior.

- `model` is a leaf and does not depend on another AEGIS subsystem.
- `organization` depends only on `model`.
- M1 `market_data` and `execution` declarations depend on `model` and `organization`, but not on one
  another. `configuration` composes and cross-validates their immutable values.
- `trace` depends on `model` and immutable configuration provenance. It performs no file, database,
  network, or console I/O.
- Venue-independent code never includes or exposes a venue-native message type.
- Venue code may later implement normalized market-data and execution contracts; those contracts do
  not depend on venue implementations.
- Strategies consume normalized events and submit through a bot-bound execution boundary. They do
  not call adapters, sessions, or sockets.
- `runtime` is the future composition root and may depend on subsystem implementations. Subsystems do
  not depend on `runtime` to find one another.
- Control-plane and data-plane code exchange immutable values or snapshots. They do not share
  mutable domain objects.
- Cross-subsystem state access uses explicit ownership boundaries and stable identifiers, not
  process-global mutable singletons.
- The exact dependency direction among execution, risk, and inventory will be finalized with the
  OMS and reservation-lifecycle decision. The layout must not hide a cycle by moving their types into
  `model`.

Avoid generic directories such as `common`, `utils`, or `core`. A reusable helper remains with the
subsystem that gives it meaning until there is evidence of a genuinely independent abstraction.

## Implemented M1 Test Shape

```text
tests/
├── support/
├── unit/
│   ├── configuration/
│   ├── execution/
│   ├── market_data/
│   ├── model/
│   ├── organization/
│   └── trace/
└── deterministic_scenarios/
```

| Area | Coverage |
|---|---|
| `support` | Reusable accepted reference and two-firm configuration builders. |
| `unit` | Deterministic behavior, boundary failures, compile-time type separation, and canonical golden vectors within one M1 area. |
| `deterministic_scenarios` | The complete reference configuration and its bounded provenance trace, driven without credentials, clocks, transports, or exchange access. |
| `venue_contract` (**Proposed**) | Future parsing and encoding against sanitized credential-free fixtures. |
| `integration` (**Proposed**) | Future boundaries such as normalization-to-strategy and risk-to-OMS admission. |

Tests are grouped by behavior rather than copied mechanically from build targets. No fixture may
contain a live credential, and no test may submit a real order without explicit authorization.

## Deliberately Open

M0 selected CMake and the test/measurement baseline; M1 added no third-party runtime dependency. The
following choices still wait for concrete milestone requirements:

- runtime networking, asynchronous-I/O, external serialization, or persistence libraries;
- the long-term installed/public API boundary beyond the current cross-target `include/aegis`
  headers;
- library target count, linkage model, or C++ modules;
- executable, service, deployment, or UI directory structure;
- generated code, persistence, or migration directories;
- placement and timing of realized and unrealized P&L calculations.

Canonical M1 configuration and trace bytes are internal deterministic evidence formats; they do not
select a networking, storage, or general-purpose serialization framework.
