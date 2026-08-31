# ADR-0001: Serialize the v1 Data Plane on One Dedicated Thread

> **Purpose:** Fix the single-owner, run-to-completion execution model and direct submission-path
> constraints for the first AEGIS data plane.

- **Status:** Accepted
- **Date:** 2026-08-17
- **Scope:** First implementation of the AEGIS data plane
- **Related:** [AEGIS architecture](../architecture.md)

## Context

AEGIS processes native exchange traffic, normalizes market data, invokes strategies, checks risk, manages orders and updates inventory. These operations share latency-sensitive mutable state, including:

- normalized market state and subscriptions;
- bot and strategy runtime state;
- current risk budgets and modes;
- reserved exposure;
- open-order and order-lifecycle state;
- bot, desk and firm positions;
- venue-session and outbound-write state.

Bots may consume and trade across multiple venues. Risk is aggregated hierarchically across bot, desk and firm scopes. A fill on one venue can therefore affect whether a later order on another venue is permitted.

The critical submission path must remain:

```text
Strategy callback
→ submit a small bounded-field OrderRequest by const reference
→ authorize execution route
→ inline risk check and exposure reservation
→ OMS admission
→ exchange-native encoding
→ asynchronous socket-write initiation
```

Introducing a worker queue, service boundary, serialization step, network request or thread hop between the strategy and the inline risk check would add latency and make the atomicity of risk decisions harder to reason about.

Making every component independently concurrent would instead require locks, atomic protocols or distributed ownership for reservations, orders and positions before the project has evidence that this complexity is necessary. For the first implementation, clarity, correctness, deterministic behavior and the ability to learn from a complete vertical slice are more valuable than speculative parallelism.

## Decision

The entire v1 data plane will be owned by one dedicated thread executing one serialized executor.

“Serialized executor” describes an execution guarantee, not a particular C++ or asynchronous-I/O library. The executor processes one data-plane callback at a time and establishes an observation order over processed events. That order is not a claim that unrelated events from different exchange connections have a single true exchange-time ordering.

The dedicated data-plane owner includes:

- venue I/O handlers and session state;
- native message parsing and validation;
- symbol mapping and normalization;
- normalized market state and subscription dispatch;
- bot and strategy callbacks;
- active execution routes;
- the inline pre-trade risk guard;
- exposure reservations;
- OMS and open-order state;
- immediate bot, desk and firm position state;
- exchange-native order encoding;
- asynchronous socket-write initiation.

Kernel I/O or an eventual library implementation may perform work outside this thread. No such work may directly mutate AEGIS data-plane state. Its completion must be delivered as a later callback on the serialized executor.

### Callback Execution Rules

Each executor callback is a non-reentrant, run-to-completion turn:

- Only the dedicated data-plane thread may mutate data-plane state.
- A callback must not perform blocking I/O, wait on another thread, sleep or run a nested event loop.
- A callback cannot be interrupted by another data-plane callback.
- An asynchronous operation is requested during the current turn. An immediate local initiation failure may be handled before the turn returns; a completion or failure after successful initiation is processed during a later turn.
- Dispatching a strategy or bot callback must not re-enter the executor recursively.
- Work that is not required for the immediate decision must be reported or processed asynchronously outside the hot path.

If a future implementation uses coroutines, the submission/risk/OMS transaction must not suspend. A handler may initiate asynchronous work and return; any resumed continuation is a distinct later callback and cannot retain borrowed mutable state from the earlier turn.

### Order-Submission Semantics

When a strategy submits an order, route authorization, inline risk checking and reservation, OMS
admission, encoding, and write initiation execute as direct calls on the same data-plane thread and
in the same executor turn. M3 uses only the deterministic in-memory fake; M8 must preserve this
direct structure when it adds venue-native behavior.

There is no general-purpose queue, service call, serialization boundary or executor handoff between the strategy callback and pre-trade risk enforcement. The risk check and reservation are logically atomic because no other data-plane operation can interleave with them. This does not require making each field a C++ atomic object; atomicity is obtained through single-owner serialization.

A local failure after capacity has been reserved must not leave that capacity unaccounted for. M3
fixes the exact local reservation and outbound OMS transitions in
[ADR-0008](0008-canonical-submission-and-fixed-risk.md) and
[ADR-0009](0009-outbound-oms-and-fake-initiation.md); later private-event transitions remain
deferred.

### Exchange-Event Semantics

Acknowledgements, exchange rejections, fills and asynchronous transport completions or failures
arrive as later executor turns. ADR-0009 fixes M3 fake encoding and local-initiation transitions;
M4 must fix the later private-event transitions, while M8 must map native transport behavior onto the
same definite-versus-uncertain boundary without leaking reserved capacity.

For every processed order event:

1. The venue/account session receives and correlates the native event.
2. The shared venue adapter parses and normalizes it.
3. The OMS reconciles the relevant order.
4. Reservation, exposure and immediate position effects are applied.
5. The bot receives the normalized order event.
6. Reporting to the control plane occurs asynchronously.

The state changes in steps 3 and 4 must be complete before a bot callback caused by that event can submit another order, and before any later executor turn performs another risk decision. This ensures that processed fills and reservation changes are visible to subsequent inline checks.

### Control-Plane Interaction

The risk coordinator remains the logical authority for firm-wide risk policy and aggregation, but it does not participate synchronously in order submission.

It publishes complete, immutable budget-and-mode snapshots to the data plane. Each snapshot carries a monotonically increasing revision assigned by the risk authority.

Snapshot adoption follows these rules:

- The control plane cannot mutate the inline risk guard directly.
- A snapshot is delivered as work for the serialized executor.
- It is installed only between callbacks, never partway through a callback.
- A callback observes one coherent snapshot for its entire turn.
- Only a strictly newer valid revision replaces the active snapshot.
- Because snapshots are complete rather than incremental patches, a skipped revision does not require replaying intermediate revisions.
- An older or duplicate revision is ignored.

The data plane publishes market, inventory, order, exposure and health observations asynchronously to the control plane. The placement and timing of realized and unrealized P&L calculation remain open. The graphical UI consumes control-plane state and never reads or mutates live data-plane objects.

### State Ownership

The table assigns each mutable or published state family to exactly one v1 owner and states the only
permitted cross-plane interaction.

| State | Owner in v1 | Cross-plane rule |
|---|---|---|
| Venue connection, parser, sequence and account-session state | Data-plane thread | External I/O completions become serialized callbacks. |
| Normalized market state and subscription dispatch | Data-plane thread | Read or copied only through defined observations. |
| Bot and strategy runtime state | Data-plane thread | Configuration changes cannot mutate it from another thread. |
| Active execution routes | Data-plane thread | Route identity and update policy remain separate decisions. |
| Active risk-budget and risk-mode snapshot | Data-plane thread | Immutable snapshot published by the risk authority. |
| Available capacity and exposure reservations | Data-plane thread | Reported asynchronously; never mutated by the control plane. |
| OMS and open-order state | Data-plane thread | Order observations are copied out asynchronously. |
| Immediate bot, desk and firm positions used by inline risk | Data-plane thread | Position observations feed control-plane aggregation asynchronously. |
| Firm-wide aggregation, allocation policy and risk authority | Control plane | Publishes complete versioned snapshots. |
| Monitoring, operator workflows and graphical UI state | Control plane | Cannot participate in or block the order path. |

No mutable object is jointly owned by the data and control planes. Crossing that boundary requires an immutable snapshot, value event or observation.

### Session-Local Write Sequencing

Some asynchronous transports permit only one outstanding write per connection or require encoded buffers to remain alive until completion. A bounded write sequencer may therefore be introduced for each venue/account session if the selected I/O mechanism requires it.

Such a sequencer must:

- remain owned by the same data-plane executor;
- exist after route authorization, risk reservation and OMS admission;
- sequence only transport-ready outbound messages;
- avoid an additional thread or service hop;
- preserve association between the OMS order and its reservation.

It is not an “order-intent” queue and is not a separate risk-approval mechanism. Its capacity, overflow behavior, ordering policy and interaction with write failures remain deferred.

## Invariants

The implementation must preserve these properties:

1. Exactly one thread mutates v1 data-plane state.
2. Data-plane callbacks never overlap or re-enter one another.
3. Strategies and bots cannot access venue adapters or sessions directly.
4. Every order passes through execution-route authorization, inline pre-trade risk and the OMS.
5. The direct submission chain uses no general-purpose queue, remote call or executor hop before it reaches the same-owner venue-session write-initiation request.
6. Risk checking and exposure reservation occur as one uninterrupted logical operation.
7. A data-plane callback sees one coherent risk snapshot revision.
8. A processed order event updates OMS, reservation, exposure and immediate position state before notifying the bot or enabling a subsequent risk decision.
9. Control-plane aggregation, monitoring, persistence and UI work cannot block the order path.
10. Off-thread code cannot read or mutate live data-plane objects through shared mutable references.

## Consequences

The accepted single-owner model produces the following benefits and deliberate costs.

### Benefits

These properties make state transitions reviewable and deterministic in the first implementation.

- Single ownership makes mutation and ordering explicit.
- Check-and-reserve semantics can be implemented without a distributed locking protocol.
- Cross-venue bots see a coherent ordered view of processed market, order and position events.
- Given the same ordered inputs, configuration, clock values and generated identifiers, behavior can be replayed deterministically.
- Unit and scenario tests can drive the same callback turns without requiring real concurrency.
- The first implementation remains small enough to understand before introducing parallel execution.
- Logical subsystem boundaries remain visible even though their hot-path calls are in-process and inline.

### Costs and Limitations

These constraints are accepted until measurements justify revisiting the ownership decision.

- One slow callback delays every later data-plane event.
- CPU-heavy parsing, strategy computation, logging or reporting can create head-of-line blocking.
- A single core bounds aggregate data-plane throughput.
- A fault in one data-plane handler has a larger failure domain.
- Fair scheduling between market data, private order events, control updates and outbound work will eventually need explicit policy.
- Control-plane views are asynchronously updated and therefore may lag the immediate state used by inline risk.
- The design depends on handlers remaining bounded and non-blocking; code review and measurement must enforce that rule.

These limitations are accepted for v1. They are reasons to measure the implementation, not reasons to add concurrency before a workload demonstrates the need.

## Alternatives Considered

The following designs were evaluated and rejected for the first implementation because they weaken
ownership clarity or add unproven coordination cost.

### General Worker Pool with Shared Mutable State

Rejected for v1. It would require locks, atomic protocols or optimistic concurrency across strategies, reservations, OMS state and positions. It would also make event ordering and deterministic replay harder to explain and test.

### Queue or Actor Between Every Subsystem

Rejected for the latency-sensitive path. Component-local actors make ownership explicit, but separate mailboxes for strategy, risk, OMS and venue transmission would add queueing and executor hops to every order.

### Separate Risk Thread or Risk Service

Rejected. A synchronous call would add coordination latency or a network boundary. An asynchronous request would make immediate approval impossible without introducing an order-intent queue. Centralized authority is retained through snapshot publication instead.

### One Executor per Venue or Account

Rejected for the initial implementation. Bots may trade and hedge across venues, while risk and positions aggregate across those routes. Early venue partitioning would force cross-partition coordination for common strategy decisions and hierarchical reservations.

### One Executor per Bot or Desk

Rejected for the initial implementation. Venue sessions are shared infrastructure, and firm- and desk-level budgets must remain coherent across bots. This design would move concurrency into shared routing, account and risk state before its cost is justified.

### Blocking Network I/O on the Data-Plane Thread

Rejected. A slow exchange or connection would stop market-data processing, risk updates and every bot. Socket operations must be asynchronous.

## Future Partitioning Seams

The logical boundaries between venue sessions, normalization, subscriptions, strategies, risk, OMS, inventory and reporting must remain explicit even though v1 executes them on one thread.

If sharding becomes necessary, likely candidates include:

- moving public market-data parsing into venue-specific owners;
- partitioning private I/O by venue/account session;
- grouping bots by instrument, desk or another measured affinity;
- moving additional reporting and calculation work into the control plane.

These are candidate seams, not selected future designs. Any partition must preserve per-order route/risk/OMS guarantees and define how cross-venue bots and hierarchical risk obtain coherent state. Mutable references must not cross a new ownership boundary.

The serialized event order should be recordable so that a production input stream can be replayed when evaluating a partitioned design.

## Revisit Triggers

Revisit this decision only with repeatable measurements against an explicit workload and latency objective. Relevant evidence includes:

- sustained executor utilization leaving insufficient processing headroom;
- increasing executor queue age or depth under expected peak traffic;
- unacceptable tail latency from native message receipt to strategy callback;
- unacceptable tail latency from strategy submission to socket-write initiation;
- delayed processing of acknowledgements or fills;
- socket backlogs or dropped market data caused by executor saturation;
- a handler whose measured CPU cost cannot be reduced or moved off-path;
- a fault-isolation requirement that one data-plane owner cannot satisfy;
- evidence that a specific partition improves capacity or latency without weakening risk and ordering guarantees.

Before sharding, first remove blocking work, unbounded callbacks, synchronous logging, unnecessary allocation and control-plane computation from the executor.

## Deferred Decisions

This ADR intentionally does not choose:

- an asynchronous-I/O, networking or executor library;
- dependency management or build tooling;
- executor scheduling, priority and overload policy;
- write-sequencer capacity, overflow and retry behavior;
- the final execution-route identifier or account-selection interface;
- detailed OMS states and reservation-release transitions;
- exact `ReduceOnly` semantics;
- whether entering `Halted` cancels existing orders;
- reconnect, recovery, exchange reconciliation and duplicate-event policy;
- persistence, journaling and audit-log design;
- placement and timing of realized and unrealized P&L calculation;
- exception containment and process-restart policy;
- CPU affinity, real-time scheduling or other low-level tuning;
- a future sharding key or multi-executor topology.
