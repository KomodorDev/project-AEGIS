# ADR-0009: Outbound OMS and Conservative Fake Initiation

> **Purpose:** Fix M3 OMS admission, exact fake encoding, transport-acceptance uncertainty,
> reservation release, bounded evidence, and direct owner-turn composition.

- **Status:** Accepted
- **Date:** 2026-08-22
- **Scope:** M3 outbound OMS, deterministic fakes, result transitions, evidence, and direct path
- **Related:** [Canonical bot-bound submission and fixed risk](0008-canonical-submission-and-fixed-risk.md),
  [serialized data-plane execution](0001-serialized-data-plane-execution.md), and
  [bounded deterministic runtime](0006-bounded-deterministic-runtime.md)

## Context

Risk approval is not order admission, transport acceptance, or an exchange acknowledgement. After a
reservation exists, every later local failure must either prove that transport acceptance could not
have happened and release exactly once, or retain conservative exposure because acceptance may have
happened. Treating a lost local completion as a definite failure would invite an unsafe automatic
retry and duplicate exchange exposure in later live milestones.

M3 must prove this boundary without possessing any networking capability. Its encoder and initiator
therefore need useful production-shaped contracts while remaining structurally incapable of sockets,
DNS, HTTP, WebSocket, authentication, private sessions, or exchange communication.

## Decision

The following subsections define the accepted outbound OMS states, fake-initiation boundary, and
caller-visible outcomes.

### Minimal outbound OMS

The owner-local outbound OMS is the only component that admits a risk-approved order. It uses a
fixed-capacity collision-safe table keyed by the complete 24-byte `OrderId`; exact equality resolves
hash collisions. An admitted record owns or stably references:

- the one-based `SubmissionAttemptId`, local `OrderId`, and one-based `ReservationId`;
- immutable firm, desk, bot, and strategy attribution;
- route, venue, logical account, and instrument identity;
- side, limit type, GTC, exact price, and exact quantity;
- configuration fingerprint and organization, route, and metadata revisions;
- M2 runtime-policy, M3 risk-policy, and M3 submission-policy fingerprints plus the nonzero
  `RiskPolicyRevision`; and
- one assigned outbound state.

M3 states are `PendingEncoding = 1`, `PendingInitiation = 2`, `WriteInitiated = 3`,
`SubmissionUnknown = 4`, and `LocallyFailed = 5`. State changes occur only on the serialized owner.
The OMS retains the local identity in every admitted state, including `LocallyFailed`, so a caller
cannot recycle a failed identity and so duplicate admission remains deterministic.

The complete transition table is:

| Source | Operation | Target |
|---|---|---|
| no record | admission after exact duplicate and capacity checks | `PendingEncoding` |
| `PendingEncoding` | exact encoding succeeds | `PendingInitiation` |
| `PendingEncoding` | scripted encoding fails | `LocallyFailed` |
| `PendingInitiation` | definite pre-acceptance initiation failure | `LocallyFailed` |
| `PendingInitiation` | accepted and initiated | `WriteInitiated` |
| `PendingInitiation` | accepted but outcome lost | `SubmissionUnknown` |
| `WriteInitiated` | latched post-acceptance internal fault invalidates completed local evidence | `SubmissionUnknown` |

`LocallyFailed` and `SubmissionUnknown` are terminal. `WriteInitiated` has no ordinary outgoing M3
transition; its sole exceptional transition is the conservative internal-fault containment row
above. That downgrade does not release exposure, retry the order, or claim initiation did not occur.
It records that the runtime can no longer present trustworthy completed local evidence and therefore
requires reconciliation. A repeated operation or any operation from the wrong source state returns
`InvalidOmsState` without mutation. Admission tests exact duplicate identity before table capacity,
so `DuplicateOrderIdentity` wins when both conditions are true. Duplicate identity and fixed-capacity
exhaustion are ordinary OMS non-admission outcomes. They occur after risk reservation in the
canonical path, so the coordinator releases that reservation exactly once. No admitted record exists
for an OMS non-admission.

### Exact deterministic fake encoder

`DeterministicFakeOrderEncoder` is a concrete `final` in-memory type. Its constructor accepts only a
validated fake script and fixed byte capacity. It has no endpoint, URL, host, port, credential,
authentication, account-session, socket, callback, file, database, or general transport parameter.

The encoder script assigns `Encode = 1` and `Fail = 2`. It contains one assigned default plus a
canonical sorted list of unique one-based invocation-ordinal overrides. The initiator script uses
the three `FakeInitiationOutcome` assignments below with the same default-plus-overrides shape.
Missing overrides select the default; no ambient randomness or clock is consulted. Zero ordinals,
duplicate ordinals, unassigned actions, or an override beyond `AEGISSUP`'s explicit maximum
submission attempts makes `AEGISSUP` invalid. Encoder and initiator invocation ordinals advance only
when their respective component is actually reached, so an encoding failure does not consume an
initiator action.

The assigned `AEGISFOE` schema-one byte sequence is ordered exactly as follows: eight ASCII magic
bytes; unsigned 16-bit schema version; raw 24-byte `OrderId`; length-prefixed route, venue, logical
account, normalized instrument, venue instrument, firm, desk, bot, and strategy identifiers; side,
limit type, and GTC assigned bytes; signed 64-bit big-endian price coefficient and one-byte scale;
signed 64-bit big-endian quantity coefficient and one-byte scale; configuration fingerprint;
unsigned 64-bit organization, route, and metadata revisions; M2 runtime-policy fingerprint; M3
risk-policy fingerprint and revision; M3 submission-policy fingerprint; and unsigned 64-bit
`ReservationId`. Identifier lengths and the schema version are unsigned 16-bit big-endian values.
Every multi-byte scalar in `AEGISFOE`, including all revisions and `ReservationId`, is big-endian;
signed coefficients use their two's-complement bit pattern. No other tag, padding byte, native
order, exchange identifier, or checksum is present.

The encoder either produces one fixed-capacity `EncodedFakeOrder` or returns the scripted
`EncodingFailed` result before any initiator call. It may validate that its input is admitted and
internally consistent, but it may not round, rescale, quantize, infer, or reinterpret price or
quantity. Golden bytes plus decode/round-trip tests prove that encoded economics equal the values
approved by risk. The in-memory object also carries the `SubmissionAttemptId` outside its canonical
byte payload so the fake slot can retain test provenance; that identity is not an unlisted
`AEGISFOE` field.

### Fake asynchronous initiation and acceptance boundary

`DeterministicFakeWriteInitiator` is also a concrete `final` in-memory type with no communication
primitive or generic live-transport interface. It owns preallocated fixed-capacity accepted-write
slot storage and a canonical scripted outcome for each reached initiator invocation. One call has
exactly three outcomes:

| `FakeInitiationOutcome` | Value | Acceptance meaning | Submission result | Reservation |
|---|---:|---|---|---|
| `DefiniteFailureBeforeAcceptance` | 1 | Bytes were not copied into an accepted slot. | `LocallyRejected` / `InitiationDefinitelyFailed` | Release exactly once |
| `AcceptedAndInitiated` | 2 | Bytes were copied, then the fake asynchronous-write analogue was initiated. | `WriteInitiated` | Retain |
| `AcceptedThenOutcomeLost` | 3 | Bytes were copied, then local completion certainty was lost. | `SubmissionUnknown` / `InitiationOutcomeUnknown` | Retain |

The copy into one accepted slot is the exact boundary at which transport acceptance may have
occurred. Capacity is checked before that copy and therefore fails definitely. Nothing after the
copy may return a definite pre-acceptance failure. Accepted slots retain the exact encoded bytes and
byte length, outer `SubmissionAttemptId`, initiator invocation ordinal, and accepted-write ordinal
for deterministic inspection.

Every reached encoder or initiator call first consumes exactly one component invocation ordinal and
its selected action, including a scripted definite failure. If an initiator action requires an
accepted slot but capacity is already full, that invocation and action remain consumed, no bytes are
copied, no accepted-write ordinal is assigned, and the result is the same definite pre-acceptance
failure. An accepted-write ordinal is assigned only as part of a successful slot copy. The default
action means neither script can be exhausted, and no component action is retried automatically.

`SubmissionUnknown` is reconciliation-required, conservatively exposure-retaining, and never
automatically retried. M3 has no reconciliation mechanism, so it remains in that state until later
milestones provide authoritative private-session evidence.

### Exact reservation transition matrix

The coordinator owns rollback sequencing and applies this complete M3 matrix:

| Outcome | Reservation behavior |
|---|---|
| Inactive/wrong-owner/re-entry/evidence rejection | No reservation |
| Missing/invalid/stale policy | No submission-capable stack; no reservation |
| Unauthorized route or invalid canonical order | No reservation |
| Order-ID exhaustion | No reservation |
| Risk rejection or risk arithmetic failure | No reservation |
| Reservation capacity | No reservation |
| Duplicate identity or OMS capacity non-admission | Release once |
| Encoding failure | Mark admitted OMS record `LocallyFailed`, then release once |
| Definite initiation failure | Mark admitted OMS record `LocallyFailed`, then release once |
| Accepted and initiated | Mark `WriteInitiated`; retain held reservation |
| Accepted then outcome lost | Mark `SubmissionUnknown`; retain held reservation |

Release is attempted only by the coordinator branch that owns the transition. The OMS never
silently releases risk state and the risk ledger never infers OMS state. A single-use coordinator
rollback guard owns the valid reservation slot, identity, and exact inverse deltas; its pre-copy
rollback is total and `noexcept`, so every returned definitive outcome releases exactly once.
Foreign or repeated lower-level release attempts return `InvalidRiskReservationState`, but the
coordinator cannot manufacture either call. An impossible OMS, trace, or guard invariant terminates
the owner rather than continuing with unknown local accounting.

The latch has one exact result rule. A fault proven to occur before accepted-slot copy returns
`LocallyRejected` at `Internal` with `SubmissionRuntimeFaulted`; it carries the current attempt and
order identities exactly when those identities already exist, after the owned guard has released a
current reservation. A fault at or after accepted-slot copy, or one that loses certainty about that
boundary, returns `SubmissionUnknown`, retains
exposure, transitions authoritative OMS state to reconciliation-required `SubmissionUnknown`, and
requires reconciliation even if the scripted action had been successful. An already accepted
`WriteInitiated` trace record remains valid historical evidence when a later completion append is the
fault; no replacement canonical record is invented after that failure. Every later submit while the
latch is set returns `LocallyRejected` at `Internal` with
`SubmissionRuntimeFaulted`, no attempt or order identity, and no trace or diagnostic mutation. The
latched check occurs with the other context gates before attempt generation.

### One direct owner turn

The complete call stack is:

```text
strategy callback
→ BotContext::submit_order
→ owner-local route authorization and canonical validation
→ exact risk check-and-reserve
→ outbound OMS admission
→ deterministic fake encoding
→ deterministic fake write initiation
→ SubmitResult return to the same callback
```

The stack contains no `SerializedExecutor::try_admit`, `InlineCommandWorkItem`, executor driver,
coroutine,
future, promise, callback handoff, general-purpose queue, serialization boundary, remote call,
database access, file access, DNS, socket, HTTP, WebSocket, or blocking I/O. The final fake byte
encoding is a local deterministic transform, not a cross-process serialization boundary. Every
order necessarily traverses route authorization, risk, and OMS because `BotContext` holds only the
single coordinator entry point and exposes none of those lower capabilities.

### Bounded canonical submission evidence

M3 adds companion `AEGISSTS` schema one instead of appending to M2 `AEGISRTS`. M2 preflights its
complete callback fan-out before a market commit; sharing that sink would let a submission consume
headroom already counted for a later callback record. Separate fixed capacity preserves both
contracts.

One submission trace record has an assigned kind and fixed structural fields. The complete stream
encodes attempt ordinal, callback identity and attribution, request economics, route/account/venue
and instrument identity, local order and reservation identity when present, all configuration and
policy fingerprints/revisions, risk scope and observed/limit values when present, OMS state,
encoding identity, initiation outcome, release transition, and final `SubmitResult`. No pointer,
thread ID, wall duration, free-form text, credential, endpoint, or native exchange identifier enters
canonical bytes.

`AEGISSTS` uses ADR-0008's positional canonical profile. Its bytes are eight ASCII magic bytes,
unsigned 16-bit schema version one, an unsigned 32-bit record count, then records in append order;
its SHA-256 digest covers that complete sequence and is not embedded. Every record encodes these
fields in order:

1. unsigned 64-bit trace and submission-attempt ordinals, unsigned 16-bit event kind, unsigned
   64-bit owner-turn, callback ordinal, and callback processing nanoseconds;
2. length-prefixed firm, desk, bot, and strategy attribution followed by requested route and
   instrument, one-byte side/type/time-in-force values, and exact price and quantity decimals;
3. configuration fingerprint and revision, organization and route-catalog revisions, M2
   runtime-policy fingerprint, M3 risk-policy fingerprint and revision, and M3 submission-policy
   fingerprint;
4. optional authorized projection: one-byte presence, then route, venue, account, normalized and
   venue instrument identifiers plus metadata revision when present;
5. optional local identity: one-byte presence plus raw 24-byte `OrderId`; optional reservation:
   one-byte presence plus unsigned 64-bit `ReservationId`;
6. optional approved exposure: one-byte presence plus exact quantity and quote-notional decimals;
7. optional risk rejection evidence: one-byte presence, one-byte scope and measure kind, observed
   and limit decimals, then unsigned 64-bit observed and limit counts; decimal measures require both
   counts to be zero, while `OrderCount` requires both decimals to be canonical zero;
8. optional OMS state: one-byte presence plus one-byte state; optional encoding identity: one-byte
   presence, unsigned 64-bit encoder invocation ordinal, unsigned 16-bit exact `AEGISFOE` byte
   length, and the raw SHA-256 of those exact bytes;
9. optional initiation evidence: one-byte presence, unsigned 64-bit initiator invocation ordinal,
   one-byte outcome, and an optional accepted write encoded as a presence byte plus unsigned 64-bit
   ordinal;
10. one-byte `SubmissionReleaseTransition` (`None = 0`, `Released = 1`, `Retained = 2`); and
11. optional final result: one-byte presence plus one-byte disposition, one-byte stage, and unsigned
    16-bit reason.

Every presence byte is exactly zero or one, and an absent value contributes no payload bytes. All
multi-byte values are big-endian. Records are cumulative causal snapshots: once route projection,
identity, reservation, exposure, OMS, encoding, or initiation data becomes available, every later
outer-attempt record repeats it. Among outer-attempt events, only `SubmissionCompleted` requires the
final-result field. `ReentryRejected` is the explicit exception to cumulative outer fields: field 1
uses the active outer attempt/turn/callback, field 2's request positions contain the nested request,
fields 3–10 contain only common provenance with every optional absent and release `None`, and field
11 contains the nested `SubmissionReentry` result. A per-kind shape validator rejects a value before
append if it introduces data before the corresponding event or omits already-established data.

`SubmissionTraceEventKind` assigns `Attempt = 1`, `RouteAuthorized = 2`,
`CanonicalValidated = 3`, `IdentityGenerated = 4`, `RiskReserved = 5`, `RiskRejected = 6`,
`OmsAdmitted = 7`, `OmsNonAdmission = 8`, `Encoded = 9`, `EncodingFailed = 10`,
`InitiationDefinitelyFailed = 11`, `WriteInitiated = 12`, `SubmissionUnknown = 13`,
`ReservationReleased = 14`, `ReentryRejected = 15`, and `SubmissionCompleted = 16`. The longest
ordinary path consumes ten records; one outer attempt therefore preflights eleven records, including
the single possible first re-entry record.

The canonical sequences are fixed; commas mean consecutive records:

| Terminal branch | Exact event sequence |
|---|---|
| route rejection | `Attempt, SubmissionCompleted` |
| canonical rejection | `Attempt, RouteAuthorized, SubmissionCompleted` |
| order-identity exhaustion | `Attempt, RouteAuthorized, CanonicalValidated, SubmissionCompleted` |
| risk/arithmetic/reservation rejection | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskRejected, SubmissionCompleted` |
| OMS non-admission | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskReserved, OmsNonAdmission, ReservationReleased, SubmissionCompleted` |
| encoding failure | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskReserved, OmsAdmitted, EncodingFailed, ReservationReleased, SubmissionCompleted` |
| definite initiation failure | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskReserved, OmsAdmitted, Encoded, InitiationDefinitelyFailed, ReservationReleased, SubmissionCompleted` |
| initiated | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskReserved, OmsAdmitted, Encoded, WriteInitiated, SubmissionCompleted` |
| uncertain | `Attempt, RouteAuthorized, CanonicalValidated, IdentityGenerated, RiskReserved, OmsAdmitted, Encoded, SubmissionUnknown, SubmissionCompleted` |

The first recursive submit appends one `ReentryRejected` record within the active outer attempt's
reserved eleventh slot; repeated nested calls are coalesced. Capability, inactive, wrong-owner,
already-latched, attempt-exhaustion, and evidence-capacity results append no canonical record.
A current-attempt internal fault preserves the causal prefix, appends `ReservationReleased` when a
pre-copy reservation existed, then appends `SubmissionCompleted` if the sink remains valid. A
post-copy fault uses `SubmissionUnknown` before completion. If trace append itself is the invariant
failure, the already accepted canonical prefix is preserved and the runtime latches without
inventing missing records.

Before `OrderId` generation or reservation, the coordinator preflights the fixed maximum records for
every possible result plus one possible first re-entry record. The one owner stack is the reservation
of that headroom; no other submission trace producer exists. Each accepted attempt appends its exact
causal prefix. Capacity failure produces a local evidence rejection and a bounded noncanonical
diagnostic without creating a reservation. If an append fails after successful preflight, the
submission runtime latches an evidence fault and the enclosing market runtime requests a terminal
owner fault after the callback returns.

Repeated re-entry attempts during one active submission coalesce into the first canonical re-entry
record and a saturating diagnostic count. Diagnostics retain the first configured prefix and count
later valid observations as dropped. Diagnostic saturation never changes a submission result or
allows missing canonical evidence.

`SubmissionDiagnosticKind` assigns `EvidenceCapacityExceeded = 1`, `ReentryDetected = 2`,
`ReservationReleased = 3`, `UnknownExposureRetained = 4`, and
`InternalInvariantFailure = 5`, and `MeasurementUnavailable = 6`. A diagnostic has only optional
attempt, owner-turn, callback, order, reservation, stage, reason, and scope values plus an unsigned
occurrence count and the required configuration, risk-policy, and submission-policy fingerprints.
It has no free-form string, pointer, thread identity, external payload, or output capability.

Capability absence, inactive context, and wrong-owner calls happen before owner evidence access and
produce neither `AEGISSTS` nor an owner diagnostic. Attempt exhaustion and an already-latched
runtime likewise make no owner-local evidence mutation. Evidence-capacity rejection consumes its
attempt identity but cannot append canonical trace; it may append only the bounded diagnostic.
Re-entry uses the outer attempt's reserved eleventh record. Deterministic byte-for-byte replay claims
therefore cover accepted outer attempts and the first nested re-entry, not rejected calls that are
structurally outside the canonical sink.

If the entry or end measurement is unavailable on a call that has passed the owner-valid gates and
obtained attempt/evidence authority, the runtime records exactly one `MeasurementUnavailable`
diagnostic observation; prefix saturation may
drop its stored record but still increments the diagnostic drop count. The same anomaly on a
capability-absent, inactive, wrong-owner, already-latched, or attempt-exhausted call leaves the
result duration absent but cannot touch owner-local diagnostics.

Identical ordered market inputs, callback order, clocks, order namespace/counter, startup
configuration, instrument metadata, risk and submission policy, and fake fault script must reproduce
the complete submit-result vector, callback sequence, OMS/reservation state, accepted fake writes,
`AEGISSTS` bytes, and digest exactly. Manual and dedicated M2 owners execute the same coordinator.

### Performance evidence and structural prohibition

`BENCH-M3-SUBMIT-001/submission.authorized-limit-fake-initiation` and
`BENCH-M3-SUBMIT-002/submission.inline-risk-rejection` each run exactly 10,000 manually timed
samples through `BotContext::submit_order`. Percentiles use the noncanonical duration captured inside that
operation: its start is the first bot-bound entry operation; SUBMIT-001 ends at the accepted fake
initiation outcome; SUBMIT-002 ends after the intended local risk rejection is complete immediately
before return. Scoped allocation tracking brackets the call itself immediately before and after.
Fixture construction, preallocation, bootstrap, post-initiation OMS/trace finalization, network
round trips, and acknowledgement timing are excluded from success latency. No network or
acknowledgement exists in either fixture.

Both records report exact counters `p50_us`, `p99_us`, `p99_9_us`, and `sample_count`.
SUBMIT-001 additionally reports `orders_per_second` and `allocations_per_order`; SUBMIT-002 reports
`rejections_per_second` and `allocations_per_request`. The rate is the completed sample count divided
by the sum of the corresponding local-path durations. Every counter is finite and nonnegative,
`sample_count` is exactly 10,000, and percentile order is monotonic.

SUBMIT-001 preallocates at least 10,000 unique order identities, simultaneously held reservations,
OMS rows, and accepted-write slots plus enough evidence for all samples. It performs no synthetic
release or cleanup because M4 has not yet supplied an acknowledgement. SUBMIT-002 uses the
value-identical `OrderRequest`, configuration, organization, route, account, metadata, runtime
policy, and deterministic order namespace. Its risk policy differs only by making the bot-scope
`SingleOrderQuantity` the first and only failing limit, before any reservation or OMS call; its
submission policy has identical capacities and script actions and differs only in the bound risk
fingerprint and consequently its own fingerprint.

Each raw record carries this exact ordered label grammar, with no spaces:

```text
workload_id=<BENCH-M3 ID>;configuration_fingerprint_sha256=<64 lowercase hex>;configuration_revision=<decimal>;organization_revision=<decimal>;runtime_policy_fingerprint_sha256=<64 lowercase hex>;risk_policy_fingerprint_sha256=<64 lowercase hex>;risk_policy_revision=<decimal>;submission_policy_fingerprint_sha256=<64 lowercase hex>;route_id=<canonical ID>;route_revision=<decimal>;account_id=<canonical ID>;venue_id=<canonical ID>;instrument_id=<canonical ID>;metadata_revision=<decimal>;order_namespace_hex=<32 lowercase hex>
```

The context manifest contains an ordered `workload_provenance` array for SUBMIT-001 then SUBMIT-002,
with one field for every label key and the same values. The validator parses the label with an
anchored grammar, rejects duplicate or extra workload records, cross-checks every field against the
corresponding manifest object, proves the shared fields are identical, and proves the two expected
risk/submission fingerprint differences before allowing a threshold claim.

The provisional p99 limits are 50 microseconds for SUBMIT-001 and 25 microseconds for SUBMIT-002.
Controlled evidence has exactly two ordered raw-record-bound threshold claims, one for each p99.
Those claims may exist only under the exact `REF-MAC-01` host and control conditions. Every
uncontrolled or hosted CI run is labelled `smoke` and contains neither a qualification reference nor
any threshold claim; smoke output is never presented as qualification evidence.

M3 combines five independent proofs that its fakes cannot communicate live:

1. concrete final fake APIs have no endpoint, credential, session, callback, or socket input;
2. compile-time capability probes reject raw route, account, encoder, initiator, socket, and
   credential access from `BotContext`;
3. the product target links no networking, database, remote-service, or coroutine runtime;
4. synchronous call-order tests prove no executor admission or handoff in the direct path; and
5. a deterministic source/build scanner rejects forbidden capabilities in M3-owned paths with only
   narrow documented build-dependency URL exceptions.

No single text scan is treated as proof of execution structure; the layers are reviewed together.

## Consequences

The outbound-state decision makes conservative uncertainty explicit and constrains what local
success may claim.

- A definite pre-acceptance failure never leaks a reservation, while uncertainty never releases
  exposure optimistically.
- A successful local fake initiation cannot be confused with exchange acceptance or acknowledgement.
- The fake path exercises stable OMS and transport-boundary semantics without making live
  communication possible.
- M2 callback trace preflight remains valid because M3 owns a separate canonical evidence stream.
- The direct benchmark interval covers the complete required local path and nothing asynchronous.

## Deferred

M4 owns exchange acknowledgements, rejections, fills, cancellations, reconciliation, inventory,
full OMS lifecycle, reservation conversion/release from private events, and recovery semantics. M5
owns dynamic risk allocation, modes, market-readiness integration, and control-plane publication.
M6 owns public networking; M7 owns authenticated private-session observation; M8 owns venue-native
encoding and real sandbox transmission. Durable audit/recovery begins in M9, load/fault campaigns in
M10, operations in M11, shadow/P&L work in M12, a second venue in M13, and combined/live
authorization in M14 or a later explicit release decision.
