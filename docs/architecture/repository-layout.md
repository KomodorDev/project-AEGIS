# Provisional Repository Layout

**Status: Proposed.** This document describes intended ownership and dependency boundaries. It does not create source directories, select build targets or make the displayed tree a permanent API. Runtime ownership follows [ADR-0001](../decisions/0001-serialized-data-plane-execution.md).

Return to the [architecture overview](../architecture.md).

## Design Goals

The source layout should make it easy to answer three questions:

1. Which subsystem owns a type or behavior?
2. Which code is exchange-native and which code is venue-independent?
3. Which dependencies point toward stable domain contracts rather than infrastructure details?

Directories should be added only when the first real implementation or test needs them. Empty scaffolding and `.gitkeep` files do not establish useful architecture.

## Proposed Source Shape

```text
src/
└── aegis/
    ├── model/
    ├── market_data/
    ├── strategy/
    ├── execution/
    ├── risk/
    ├── inventory/
    ├── venues/
    │   ├── binance/
    │   └── deribit/
    └── runtime/
```

| Area | Intended responsibility |
|---|---|
| `model` | Dependency-light identifiers, numeric value types and universally shared primitives. It must not become a miscellaneous dumping ground. |
| `market_data` | Normalized market events, subscriptions, market-state handling and dispatch. |
| `strategy` | Strategy contract, configured bot runtime and the bot-bound submission context. |
| `execution` | Execution-route authorization, normalized order requests, order submission and OMS behavior. |
| `risk` | Risk modes and budgets, inline checks, reservations and control-plane policy calculation. |
| `inventory` | Fill-derived immediate positions, exposure attribution and bot/desk/firm aggregation. Durable ledger ownership and P&L calculation placement remain open. |
| `venues/binance` | Binance-native connectivity, parsing, symbol mapping, authentication and order encoding. |
| `venues/deribit` | Deribit-native connectivity, parsing, symbol mapping, authentication and order encoding. |
| `runtime` | Process composition, lifecycle and the serialized data-plane executor; it must not own domain rules. |

Component-owned types stay with their component. For example, a normalized market event belongs to `market_data`, an order request belongs to `execution`, and a risk mode belongs to `risk`. A type moves to `model` only when it is genuinely neutral and shared.

## Dependency Rules

- `model` is a leaf: it does not depend on another AEGIS subsystem.
- Venue-independent code never includes or exposes Binance- or Deribit-native message types.
- Venue code may implement normalized market-data and execution contracts; those contracts do not depend on venue implementations.
- Strategies consume normalized events and submit through a bot-bound execution boundary. They do not call adapters, sessions or sockets.
- `runtime` is the composition root and may depend on subsystem implementations. Subsystems do not depend on `runtime` to find each other.
- Control-plane and data-plane code exchange immutable values or snapshots. They do not share mutable domain objects.
- Cross-subsystem state access uses explicit ownership boundaries and stable identifiers, not process-global mutable singletons.
- The exact dependency direction among execution, risk and inventory will be finalized with the OMS and reservation-lifecycle decision. The layout must not hide a cycle by moving their types into `model`.

Avoid generic directories such as `common`, `utils` or `core`. A reusable helper should remain with the subsystem that gives it meaning until there is evidence of a genuinely independent abstraction.

## Proposed Test Shape

```text
tests/
├── unit/
├── venue_contract/
├── integration/
└── deterministic_scenarios/
```

| Area | Intended coverage |
|---|---|
| `unit` | Deterministic behavior within one subsystem. |
| `venue_contract` | Binance and Deribit parsing/encoding against sanitized, credential-free fixtures. |
| `integration` | Boundaries such as normalization-to-strategy and risk-to-OMS admission. |
| `deterministic_scenarios` | Deterministic end-to-end market, order, fill and risk flows using test clocks and transports. |

Tests should be grouped by behavior rather than copied mechanically from build targets. No fixture may contain live credentials, and no test may submit a real order without explicit authorization.

## Deliberately Open

This proposal does not decide:

- build system or dependency manager;
- networking, serialization or test libraries;
- co-located headers versus a public `include/aegis` tree;
- library target count, linkage model or C++ modules;
- executable, service, deployment or UI directory structure;
- generated code, persistence or migration directories;
- placement and timing of realized and unrealized P&L calculations.

Those choices should follow concrete implementation needs rather than empty scaffolding.
