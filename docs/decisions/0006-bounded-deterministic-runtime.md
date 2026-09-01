# ADR-0006: Bound Serialized Runtime Admission and Replay

> **Purpose:** Fix the M2 executor, admission, ownership, overload, clock, and replay contracts that
> make one serialized data-plane owner deterministic and observable.

- **Status:** Accepted
- **Date:** 2026-08-19
- **Scope:** M2 runtime policy, bounded ingress, serialized turns, and deterministic replay
- **Related:** [Serialized data-plane ownership](0001-serialized-data-plane-execution.md),
  [domain value contracts](0004-domain-value-contracts.md),
  [market-state validity](0007-market-state-validity.md), and
  [quality budgets](../quality-budgets.md)

## Context

ADR-0001 accepts one dedicated serialized owner but deliberately leaves its concrete executor,
scheduling, and overload behavior open. M2 must make those choices before recorded market input can
drive mutable books and strategies. An unbounded queue, recursive callback, ambient clock read, or
silent rejected input could make a plausible market book differ between runs or remain tradable
after data was lost.

The executor is an ownership and sequencing mechanism, not a general task system. It must support a
real dedicated owner and a deterministic fixture driver without creating two implementations of a
turn or admitting orders, sockets, credentials, private sessions, risk, OMS, or transmission.

## Decision

The following subsections define the adopted bounded runtime, its owner-turn protocol, and its
evidence contract.

### Immutable runtime policy

M2 adds one validated immutable runtime policy that references the sealed M1 configuration
fingerprint. It has its own canonical `AEGISRTP` schema-one encoding and SHA-256 fingerprint, so
replay evidence identifies capacity and freshness decisions without changing `AEGISCFG` schema one.

`AEGISRTP` schema one fixes the compiled frame, normalized-change, and book-depth ceilings. A policy
instance fixes, at minimum, pending ingress capacity, configured source capacity, maximum recorded
frame bytes, maximum changes per market update, retained book depth, stale threshold, maximum
matching callbacks per turn, diagnostic capacity, runtime-trace capacity, maximum turns per bounded
drive, and the callback measurement budget. Every value is positive, mutually consistent, within
the schema ceiling, and validated before a runtime can be constructed. Changing a compiled ceiling
requires a schema decision. The callback budget is an observation threshold, not authority to
preempt a running callback.

Mutual consistency includes evidence feasibility. For the largest configured source fan-out of
`g` matching subscription grants, runtime-trace capacity is at least `2 + 4g`: one input record,
one state-transition record, two callbacks per grant, and one reserved first re-entry record per
callback. Validation computes this relationship in a wide unsigned intermediate before comparing
it with the configured capacity. This minimum proves that one recovery-snapshot turn is
representable; the authored capacity must still cover the complete bounded replay scenario.

The policy owns a canonical market-source registry sorted by validated `MarketSourceId`. Each entry
maps one source to exactly one configured venue, instrument, venue instrument, order-book channel,
and metadata revision, and receives a stable one-based source ordinal in that order. M2 permits only
one source for a `(venue, instrument, channel)` key; redundant-feed arbitration requires a later
decision. Source capacity is the registry size, so every possible discontinuity fence is allocated
at construction. `MarketSourceId` is at most 64 bytes and uses the exact `source.` prefix followed by
lowercase ASCII alphanumeric slug segments separated by `.` or `-`, with no empty segment.

Dynamic policy adoption is outside M2. A runtime uses one sealed startup configuration and one
runtime policy for its lifetime.

### Bounded admission and overload

Ingress is a fixed-capacity FIFO of bounded immutable commands. Capacity counts pending slots; at
most one additional command may be active on the owner. The queue never overwrites, grows, blocks a
producer waiting for capacity, or hides a rejection. `try_admit` returns a `Result` whose successful
value is one of these ordinary admission decisions:

| Outcome | Meaning |
|---|---|
| `Accepted` | The command owns one pending slot and receives its admission receipt. |
| `CapacityExceeded` | No slot was consumed; the complete command remains rejected. |
| `Closed` | Shutdown has begun and no later command may be admitted. |

Every ordinary attempt receives a monotonic ingress ordinal, including `CapacityExceeded` and
`Closed`. Every successful admission additionally receives a distinct monotonic receive sequence
and injected `ReceiveTimestamp`; its receipt exposes the attempt ordinal, sequence, pending depth,
and configured capacity. Attempt- or receive-counter exhaustion and an injected clock value that
regresses from the executor's last observation are terminal `Result` errors that occur before
another ordinal can be assigned; they fail the runtime closed and cannot be confused with an
ordinary admission decision. Clock providers expose a non-fallible monotonic reading; a scripted
clock-advance failure is returned to its controller before admission is attempted.

Rejecting an attributable market frame is an integrity event, not ordinary telemetry. A
construction-time table provides one preallocated discontinuity fence per configured source. On
`CapacityExceeded`, the ingress coordinator stores that source's earliest failed attempt ordinal and
increments its bounded loss count. The owner merges queue fronts and fences by ingress ordinal,
finishes older accepted work first, then consumes the fence before any later work for that source.
The market-state policy consequently becomes non-ready and requires a fresh snapshot. Repeated
failures cannot allocate, erase the earliest fence, or leave the source silently `Ready`.

A discontinuity fence is a first-class bounded owner turn even though it occupies its preallocated
source slot rather than a pending FIFO slot. It carries the failed attempt ordinal, has no queue age
or receive sequence, increments the completed-turn count, and produces a control-turn report. The
owner chooses the lowest ordinal across the FIFO head and pending fences. Fence creation wakes the
dedicated driver; closure drains the merged accepted-command/fence prefix. Deterministic and
dedicated drivers use the same merged runnable predicate.

An unattributable or unconfigured rejected frame still returns `CapacityExceeded`, but cannot mutate
an arbitrary market source. Its caller must stop or resynchronize that source before presenting more
input.

Producer-safe queue indices, synchronization primitives, admission counters, and discontinuity
fences are ingress-coordination state. They may be shared only through the executor's closed API.
Books, readiness, bot state, strategy state, dispatch ordinals, diagnostics, and traces are mutable
data-plane state and remain owner-local.

### Owner binding and bounded turns

Exactly one execution context binds as owner before a turn may begin. `execute_next_turn` executes
zero or one oldest runnable command or discontinuity fence to completion and returns a report; it
never waits for future work. A bounded `execute_pending_turns(max_turns)` calls the same primitive no
more than its validated limit and reports completed turns plus remaining command and fence counts.
Closing rejects new admission while preserving ordinal drainage of the merged
accepted-command/fence prefix; an empty closed executor reports no completed turn rather than
inventing work.
Wrong-owner, unbound, counter-exhausted, and clock-regression conditions return stable failures.

A turn is non-preemptible and non-reentrant. Nested `execute_next_turn`, nested
`execute_pending_turns`, recursive subscription
dispatch, or recursive strategy callback execution returns `ReentryDetected` before mutable work
begins. Owner-side admission during a turn may enqueue work for a later turn but cannot execute it
until the current call stack returns.

The command representation has a compile-time or policy-bound payload. The queue does not own a
general-purpose `std::function`, arbitrary heap closure, or reference whose lifetime can end before
execution. All per-turn fan-out and scratch storage is bounded before the turn begins.

### Clocks and observable timing

Admission time and processing-start time come from the injected monotonic clock contract established
by ADR-0004. Queue age is the checked difference `processing start - receive time`. The owner rejects
clock regression rather than saturating, wrapping, or publishing a misleading duration.

Callbacks run to completion. M2 measures entry-to-return duration and exposes a budget-overrun metric
after return, but cannot safely interrupt in-process strategy code. Live duration and host scheduling
jitter are performance evidence, not canonical replay fields. Canonical traces may contain scripted
receive and processing values because the deterministic driver supplies them explicitly.

### One turn processor, two drivers

The dedicated driver owns one standard-library thread and binds the executor inside it. It uses
`std::jthread` where the supported library provides C++20 stop-token support and an explicit owned
stop flag plus joined `std::thread` fallback otherwise. Both forms stop only between turns and have
the same observable contract. The driver sleeps only while no admitted command or discontinuity
fence exists and wakes for ingress, a fence, or shutdown; it does not move mutable domain state to
producers.

The deterministic driver binds one caller as the exclusive owner, advances a scripted clock only
when instructed, and supports `execute_next_turn` and bounded `execute_pending_turns`. Both drivers
invoke the same turn
processor and therefore share admission, ordering, ownership, error, and reporting behavior.

Replay identity includes the M1 configuration fingerprint, M2 runtime-policy fingerprint, ordered
fixture bytes, admission outcomes and ordinals, scripted clock values, and deterministic identifiers.
Given those inputs, callback order and canonical runtime-trace bytes must match exactly.

### Bounded evidence

Diagnostics are structured assigned values with fixed fields. Their sink has fixed capacity,
preserves the accepted prefix, and exposes saturation explicitly. Admission rejection is always
returned directly and source discontinuity is maintained independently, so a full diagnostic sink
cannot conceal overload. Ordinary admission failures are not appended to owner-local `AEGISRTS`
from producer threads: replay input records their decisions and ordinals, while an attributable
capacity loss becomes the owner's ordered `SourceDiscontinuity` disposition.

Critical input-disposition, state-transition, and callback trace records are counted before a book
commit. If the runtime trace cannot reserve the complete required record set, the turn changes no
book or strategy state, leaves any discontinuity fence pending, closes admission, suppresses all
later callbacks, and enters an externally observable `EvidenceExhausted` runtime fault. That
fail-closed lifecycle transition does not depend on writing another record into the full sink. M2
does not rotate or drain canonical traces; the configured capacity must cover the bounded scenario,
and exhaustion intentionally halts this runtime rather than silently losing evidence. No critical
record is overwritten or evicted.

## Consequences

The bounded-runtime decision makes overload and replay behavior explicit while accepting
fixed-capacity planning work.

- Queue capacity and scheduling behavior become part of reproducible runtime provenance.
- A transient overload conservatively invalidates the affected source instead of claiming
  continuity that was not processed.
- The deterministic and dedicated paths exercise one implementation of every owner turn.
- Queue age, capacity, admission failure, callback duration, and re-entry become measurable without
  introducing a second mutable owner.
- Blocking callbacks remain a strategy contract violation that M2 can measure but not preempt;
  process isolation and restart policy remain later work.

## Deferred

Priorities, coalescing, feed shedding, retries, public networking, asynchronous-I/O libraries,
watchdogs, process-level strategy isolation, exception recovery, dynamic runtime-policy adoption,
and cross-plane backpressure remain outside M2. M6 must translate public-feed overload into this
accepted discontinuity contract; M10 owns broader load and fault qualification.
