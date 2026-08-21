# ADR-0007: Normalize Market Updates and Expose Only Valid State

> **Purpose:** Fix the M2 fixture, normalized update, transactional book, readiness, callback, and
> replay-trace contracts that prevent strategies from observing incoherent market state.

- **Status:** Accepted
- **Date:** 2026-08-19
- **Scope:** M2 recorded fixtures, normalized market events, books, validity, and strategy dispatch
- **Related:** [Bounded deterministic runtime](0006-bounded-deterministic-runtime.md),
  [domain value contracts](0004-domain-value-contracts.md),
  [immutable configuration provenance](0005-immutable-configuration-provenance.md), and
  [reference scenario](../reference-scenario.md)

## Context

M2 must prove that credential-free recorded input reaches only subscribed strategies and only after
the complete market state is coherent. The architecture accepts owner-local books but leaves exact
synchronization behavior open. Without one normalized contract and transition table, duplicate,
out-of-order, malformed, stale, metadata-incompatible, or checksum-failed input could produce
platform-dependent behavior or resurrect a book without a trustworthy snapshot.

M2 is not a Deribit protocol implementation. Its fixture grammar and integrity check exercise a
venue-neutral boundary; native channel selection, native checksum rules, reconnect, and
resubscription remain M6.

## Decision

### Four explicit boundaries

M2 keeps raw, normalized, and strategy-facing values distinct:

1. `IngressFrameAttempt` is the caller-owned bounded attempt. Its source attribution is optional and
   untrusted so an unconfigured or malformed envelope can fail without naming arbitrary state.
2. `RecordedFrame` is constructed only after the envelope maps to the sealed source registry. It
   owns the credential-free bytes and carries validated source, venue, normalized and venue
   instrument, session identity, owner-assigned receive sequence and timestamp.
3. `NormalizedMarketUpdate` is the complete temporary result of strict parsing and normalization.
   It carries source identity, session epoch, source sequence and optional predecessor, source and
   receive times/sequences, active metadata revision, snapshot/delta/session/staleness meaning,
   bounded level changes, and a fixture integrity verdict.
4. `MarketEvent` and `ReadyBookView` are created only after a complete book commit. They add owner
   processing time, `BookGeneration`, and `BookRevision` and expose a const turn-scoped coherent
   view.

Source and receive sequences are nominally distinct even though both store unsigned 64-bit values.
`BookGeneration` and `BookRevision` are also distinct. Production values are one-based and use
optional absence before any commit. Primitive boundaries without a separate presence marker reserve
zero for that absence; tagged `AEGISRTS` fields encode presence explicitly. The first valid snapshot
assigns generation and revision one; every later valid snapshot increments both, while a valid delta
increments only the globally monotonic revision. Revision does not reset at a new generation.
Attempt, receive, owner-turn, and callback ordinals are likewise one-based; all counters fail before
wrap.

The repository-owned fixture begins with an assigned magic and schema version, has a strict ASCII
grammar, rejects trailing or partial input, and carries no endpoint, credential, private session, or
order data. Parser failures contain an assigned code and byte offset. Parsing and normalization build
temporary values; failure cannot mutate a book or reuse partial output from a prior frame.

### Transactional bounded books

Each entry in ADR-0006's canonical source registry has one owner-local current book and one stable
one-based trace ordinal. The registry maps the source to the sealed M1 venue/instrument metadata and
the unique M2 `(venue, instrument, OrderBook)` key. Price and quantity use the exact M1 nominal
fixed-point types and must match that metadata revision, scale, tick, and quantity step. Retained
depth is an immutable runtime-policy value with a compiled upper bound; the reference scenario and
`BENCH-M2-MD-001` retain exactly 20 bid and 20 ask levels.

A snapshot describes the complete retained book and replaces the prior candidate. A delta contains
absolute quantities; zero quantity deletes one price. Duplicate prices inside one update, invalid
sides, non-positive retained quantities, misaligned values, excess levels or changes, and a crossed
or locked final book reject the complete update. A coherent book may have either side empty; each
optional best price is reported independently, and crossing is evaluated only when both sides exist.
Normalized-update digest bytes sort `Bid` before `Ask` and exact price ascending within each side;
this semantic-hash order is independent from the book's bid-descending/ask-ascending view order.

Application copies the current book into fixed-capacity scratch state, applies every change,
canonicalizes bid-descending and ask-ascending order, validates the final candidate and integrity
verdict, reserves all critical trace records, and only then commits by swap. A failed validation,
reservation, or callback-fan-out preflight leaves the prior book and revision unchanged.

No strategy receives a mutable book reference. `ReadyBookView` is valid only for the synchronous
callback in which it is supplied and cannot be retained as an owner-independent handle.

### Deterministic continuity and validity

Every accepted snapshot or delta has a canonical payload digest over schema, update kind, source and
session identity, source sequence and predecessor presence/value, source timestamp, metadata
revision, integrity token/verdict, and side/price/quantity changes in canonical order. Receive and
processing values are excluded. The owner retains the last committed session, source sequence, and
digest so an exact duplicate can be distinguished from a same-sequence conflict. Source continuity
uses the explicit predecessor supplied by the normalized adapter contract rather than assuming every
venue increments by one.

The owner classifies one fully parsed input in this order: source registration; control kind;
session age; same-session sequence age or equality; exact duplicate versus conflict; eligibility of
a newer snapshot/delta; metadata revision; integrity verdict; complete candidate-book validation;
trace/fan-out reservation; commit; state publication; market publication. Older session/sequence
input is ignored before its metadata or integrity token can invalidate current state. In the active
session, a recovery snapshot must have a source sequence strictly newer than the last committed
sequence; an equal digest is a duplicate and a different digest is a conflict. A snapshot from a
newer session establishes its own continuity anchor.

The complete transition policy is:

| Current state and input | Commit | Next state | Strategy notification |
|---|---:|---|---|
| Runtime start or explicit resynchronization | No | `Synchronizing` | One state callback per matching subscription |
| Newer `SessionStarted` control | No | `Synchronizing` and clear continuity | One callback only if state changes |
| Same/older `SessionStarted` control | No | Unchanged | None |
| Eligible valid full snapshot for the active/newer session and metadata | Replace | `Ready` in a new generation | `Ready` callback only if state changes, then market-data callback |
| `Ready` plus valid delta whose predecessor equals the last committed sequence | Apply | `Ready` | Market-data callback |
| Exact duplicate session, sequence, and canonical digest | No | Unchanged | None |
| Same session and sequence with different digest | No | `Invalid` | One state callback on transition |
| Older session or older source sequence | No | Unchanged | None |
| Forward gap or wrong predecessor | No | `Invalid` | One state callback on transition |
| Newer session plus non-snapshot input | No | `Synchronizing` | One state callback on transition |
| Metadata-revision mismatch | No | `Invalid` | One state callback on transition |
| Integrity/checksum or final-book structural failure | No | `Invalid` | One state callback on transition |
| Attributable malformed/unsupported active-stream frame | No | `Invalid` | Sanitized state callback only |
| Unattributable or unconfigured malformed/unsupported frame | No | Unchanged | None |
| `Ready` freshness check before the deadline | No | `Ready` | None |
| `Ready` freshness check at or beyond the deadline | No | `Stale` | One state callback on transition |
| Freshness check while non-ready | No | Unchanged | None |
| Delta while `Synchronizing`, `Stale`, or `Invalid` | No | Unchanged | None |
| Source discontinuity fence from rejected admission | No | `Invalid` | One state callback on transition |

Only a valid full snapshot may return `Synchronizing`, `Stale`, or `Invalid` to `Ready`. The stale
deadline begins at the owner processing/commit time of the last valid snapshot or delta. A duplicate
or ignored older event does not refresh it. Staleness is evaluated only on an explicit owner turn;
elapsed time equal to or greater than the sealed threshold transitions `Ready` to `Stale`. Ambient
time cannot change replay.

M2 metadata is immutable for the runtime lifetime. An update must carry the exact active metadata
revision. Dynamic metadata adoption and compatibility across revisions require a later decision.

M2 supplies a deterministic fixture integrity policy. The normalized contract exposes an integrity
verdict without embedding a venue algorithm. Deribit-native checksum coverage, depth, channel,
interval, and sequencing quirks begin in M6.

### Strategy and subscription dispatch

`on_market_data(const MarketEvent&, const ReadyBookView&, BotContext&)` is invoked only after a
successful `Ready` commit. `on_market_state(const MarketStateEvent&, BotContext&)` communicates
`Synchronizing`, `Ready`, `Stale`, and `Invalid` transitions without presenting a tradable book or
raw malformed input. When a snapshot establishes readiness, state dispatch completes before market
data dispatch.

Matching uses the sealed subscription key `(venue, instrument, OrderBook)` and emits exactly once per
matching grant in canonical `SubscriptionId` order. Container iteration order never determines
callbacks. Each configured bot owns one non-shared mutable strategy instance; firm and desk
attribution are derived from the sealed M1 organization. Multi-firm configurations remain valid even
though the deterministic reference scenario has one firm.

`BotContext` exposes immutable bot attribution, configuration and runtime-policy provenance, and
callback identity only. The runtime, not strategy code, owns canonical callback traces. The context
has no order submission,
execution route, risk, OMS, venue adapter, socket, credential, private session, or transmission
capability. Orders remain M3.

Callbacks are synchronous, non-blocking, non-reentrant, and run to completion under ADR-0006. A
callback cannot trigger recursive dispatch. M2 measures duration but does not place live duration in
canonical trace bytes.

### Diagnostics and canonical replay trace

Input dispositions and failures use assigned enums and bounded structural fields, never hot-path
free-form prose. Malformed fixture bytes never enter a callback. An attributable malformed frame may
cause only a sanitized state transition describing the resulting validity, not the raw payload.

M1 `AEGISTRS` schema-one bytes and golden vectors remain unchanged. M2 adds companion `AEGISRTS`
schema one. Its fixed-capacity canonical records include ordinal, kind, configuration and runtime
policy fingerprints, source and session identity, source and receive sequences, metadata revision,
book generation/revision, state or input disposition, bot and subscription identity, and optional
best bid/ask only for a coherent `Ready` callback. No pointer, thread ID, ambient timestamp, raw
malformed payload, or callback wall duration enters the canonical encoding.

The runtime emits exactly one automatic canonical record for each input disposition, state
transition, state callback, and market callback. Before a book commit it preflights that exact fan-out
plus one possible first re-entry record per invoked callback; repeated re-entry attempts in the same
callback increment a bounded noncanonical counter without consuming more trace records. Strategy
code cannot append canonical runtime records. A full sink follows ADR-0006's fail-closed
`EvidenceExhausted` lifecycle and preserves its accepted prefix. Replaying the same
sealed configuration and runtime policy, fixture bytes, admission outcomes, scripted clock, and
identifiers must reproduce the complete callback vector and `AEGISRTS` bytes and digest exactly.

## Consequences

- A strategy can distinguish all four market states while receiving prices only through a coherent
  `ReadyBookView`.
- A malformed or rejected input cannot partially mutate a book or be mistaken for a market event.
- Gaps, conflicts, checksum failures, stale data, and incompatible metadata fail closed until a
  snapshot starts a new generation.
- The contract supports multiple peer firms without changing the single-firm deterministic example.
- M6 can implement real public venue behavior behind the normalized boundary without changing M2's
  owner, callback, or validity guarantees.

## Deferred

Networking, sockets, native venue parsing, live checksums, reconnect/resubscription, dynamic metadata
adoption, historical books, market-by-order depth, private sessions, credentials, orders, risk, OMS,
and transmission remain outside M2.
