# ADR-0012: Fake-Backed Recovery and Crash Consistency

> **Purpose:** Fix M4 recovery authority, fake durability, journal/snapshot ordering, restart
> identities, reconciliation completeness, crash convergence, callback replay, and one-shot intent.

- **Status:** Accepted
- **Date:** 2026-08-23
- **Scope:** M4 in-memory recovery protocol, reconciliation, crash matrix, and longitudinal driver
- **Related:** [Normalized private events and extended OMS](0010-normalized-private-events-and-oms.md),
  [inventory, reservation conversion and account safety](0011-inventory-reservation-and-account-safety.md),
  [dedicated testnet account](0003-dedicated-testnet-account.md),
  [serialized data-plane execution](0001-serialized-data-plane-execution.md), and
  [M4 capacity policy and canonical evidence](0013-m4-policy-and-canonical-evidence.md)

## Context

M4 must define restart and reconnect behavior before any authenticated private venue integration or
durable store exists. This ordering prevents M7-M9 implementations from choosing incompatible
identity, truth, replay, or release rules after economic behavior is already live.

The repository also deliberately excludes synchronous filesystem or database work from the M3
submission path and later private-event owner turns. Consequently, M4 cannot promise that every
local-only intent or callback is durable before a crash. An order may have reached the authoritative
fake venue while its local attribution record remains volatile. ADR-0003 forbids inventing ownership
to close such a gap.

M4 therefore needs an honest safe-convergence contract, explicit asynchronous crash windows, and
closed deterministic fakes that prove the protocol without pretending to provide disk, process,
machine, or power-loss durability.

## Decision

### Stable recovery errors

M4 appends these stable `DomainErrorCode` values:

| Value | Error |
|---:|---|
| 920 | `InvalidRecoveryPolicy` |
| 921 | `InvalidJournalState` |
| 922 | `JournalCapacityExceeded` |
| 923 | `InvalidRecoverySnapshot` |
| 924 | `SnapshotCapacityExceeded` |
| 925 | `InvalidReconciliation` |
| 926 | `ReconciliationIncomplete` |
| 927 | `RecoveryGap` |
| 928 | `RecoveryCounterExhausted` |
| 929 | `RecoveryProvenanceMismatch` |

Recovery validates startup policy/provenance, committed snapshot, contiguous journal, authoritative
result shape/completeness, and live catch-up in that order. An earlier failure wins and prevents
later state construction. No earlier domain error assignment changes.

### Split source of truth

The authoritative fake reconciliation source owns simulated venue facts:

- exchange orders and their venue state;
- exchange order and trade identities;
- executions;
- current positions;
- account permissions and margin mode;
- authoritative completeness cuts.

AEGIS owns local facts:

- submission and cancellation intent;
- canonical local `OrderId` and cancel attempt identity;
- firm, desk, bot, strategy, route, and logical-account attribution;
- configuration, organization, metadata, runtime-policy, risk-policy, and submission-policy
  provenance;
- local-to-exchange mappings once proved;
- local event/trade disposition history;
- reference-intent consumption state.

Neither side may invent the other's facts. A venue order cannot acquire local ownership from side,
price, quantity, instrument, timing, or a client-looking byte string. Local intent cannot override
a contradictory complete authoritative order, execution, or position fact. A disagreement enters
the account-safety state defined by ADR-0011 and remains visible in audit evidence.

A venue cancellation result may carry an existing AEGIS-owned `CancelAttemptId` only as causal echo
evidence when the trusted adapter retained the exact request/response binding before transmission or
received a lossless authenticated native echo. The venue does not mint that identity, the identity
does not establish local order ownership, and reconciliation never invents or attaches one. Recovery
retains its exact presence and bytes as part of the normalized event and independently validates the
resolved order and retained attempt before it can close anything.

### Safe fixed-point convergence

M4 accepts exactly three convergence classes:

1. **Operational:** the namespace fence, local provenance, snapshot/journal, complete authoritative
   batch, current-position equality, and live fence are all valid; every known uncertainty is
   resolved; no safety reason remains.
2. **Safe reconciliation required:** known local state is reconstructable and conservative, but an
   incomplete batch, catch-up gap, or unresolved known order leaves reason 1 through 5 active; there
   is no unknown ownership or contradiction.
3. **Safe quarantined:** any unknown ownership, identity/provenance conflict, unexplained or
   unquantifiable position, permission/margin mismatch, critical-admission loss, evidence/state
   capacity failure, or provenance-incompatible media leaves reason 6 through 19 active.

The independent classification predicate is therefore closed: Operational requires every listed
positive condition; otherwise the highest-severity active ADR-0011 reason selects safe
reconciliation-required or safe quarantined. Recovery never invents ownership merely to match an
uninterrupted runtime. The same durable inputs and scripts reproduce the same class and canonical
state.

A compatible missing/unacknowledged journal input, invalid-but-proven-compatible snapshot, callback
gap, or missing non-ownership audit detail maps to `RecoveryGap = 5` and
`SafeReconciliationRequired`. A provenance mismatch maps to `ProvenanceMismatch = 19`; evidence
capacity to 17; arithmetic/state capacity to 18; and identity/content contradiction to its assigned
reason 12 through 15. These mappings are mutually exclusive in failure-precedence order, so an
"audit gap" has no second convergence interpretation.

The C13 oracle reconstructs the same acknowledged suffix twice and separately applies the same
authoritative batch and catch-up twice. The second pass must change no OMS economics, reservation,
inventory, mapping, ownership, safety, intent, notification decision, or identity high-water.

### M4 durability boundary

M4 models four distinct states for a journalled owner turn:

1. **planned:** validation, correlation, idempotency, arithmetic, and every capacity preflight have
   produced a complete immutable typed recovery input and assigned sequence in reserved storage;
2. **published input:** the no-fail owner commit has copied that prepared input into the bounded fake
   journal before business mutation;
3. **owner-local projection:** the same non-preemptible commit has applied OMS, reservation,
   inventory, safety, and audit;
4. **durable acknowledgement:** a later fake-persistence action has advanced one contiguous durable
   watermark through the published input.

The published record is explicitly a replayable input, not a statement that the owner-local
projection completed. Recovery may therefore apply it whether a crash occurred before or after the
volatile projection. Publication cannot fail inside commit because the plan already reserved the
complete record and sequence. A failure before commit consumes neither. Within one incarnation a
published sequence is immutable and not reused. If it was never acknowledged and is discarded by a
crash, it never entered recovery history; the next incarnation starts at durable watermark plus one
and may reuse that numeric sequence. Client identity non-reuse is separately guaranteed by the
acknowledged namespace fence.

Publication is not durability. The owner and submission paths never block on a filesystem,
database, remote service, or operating-system durability call. On crash the fake medium retains
only acknowledged journal inputs, committed snapshots, and the retained projection of acknowledged
namespace-registration records. Complete authoritative facts reconstruct a lost published-only/local projection when
possible; otherwise recovery selects the exact safe class above with an audit gap. M4 makes no real
process, host, operating-system, or power-loss durability claim; M9 must implement this abstract
protocol.

M4 does not add journal or persistence work to either named M3 submit benchmark interval. Local OMS
provenance already retained by M3 may be published or snapshotted on a later owner turn. A crash
before that boundary is deliberately part of the safe-convergence matrix.

### Recovery identities

M4 adds nominal local identities:

| Identity | Contract |
|---|---|
| `RecoveryLineageId` | exactly 16 opaque bytes injected with the external fake medium and stable across its incarnations |
| `RuntimeEpochId` | exactly 24 bytes: 16-byte restart namespace plus one nonzero, non-wrapping unsigned 64-bit counter |
| `ReconciliationEpochId` | exactly 32 bytes: complete `RuntimeEpochId` plus one nonzero, non-wrapping unsigned 64-bit counter |
| `RecoverySnapshotId` | exactly 24 bytes: 16-byte restart namespace plus one nonzero, non-wrapping unsigned 64-bit counter |
| `ReferenceIntentId` | exactly 24 bytes: registered restart namespace plus one nonzero, non-wrapping unsigned 64-bit counter |
| `JournalSequence` | nonzero, non-wrapping `u64` in one recovery lineage |
| `SnapshotCommitOrdinal` | nonzero, non-wrapping `u64` in one recovery lineage |
| `AuditOrdinal` | nonzero, non-wrapping `u64` qualified by runtime epoch |
| `DiagnosticOrdinal` | nonzero, non-wrapping `u64` qualified by runtime epoch |

The lineage is runtime-global and may contain multiple firms, accounts, and venues. Namespace,
journal, snapshot, and runtime-epoch records use ADR-0013 root provenance with no invented subject.
Account recovery epochs, reconciliation rows, and decisions add exact subject provenance. The
global restore completes before the per-account recovery state machine below runs.

Existing recovered `OrderId` values remain byte-for-byte unchanged. Every reconstructed production
runtime obtains a fresh existing 16-byte `OrderNamespace` for new client identities and begins that
namespace's order counter at one. Its first journal input is `NamespaceRegistered`; the fake medium
must durability-acknowledge that record and append its namespace to the lineage's non-truncated
registry projection before the runtime can publish Ready, accept an intent, submit, or mint any
client identity. Recovery rejects equality with every registered namespace. The registry is
append-only, bounded by ADR-0013, and independent of later OMS records. This startup fence makes
client-identity non-reuse absolute within the lineage rather than probabilistic. M9 must preserve
the same durable-before-use fence.

Every recovered relationship involving raw M1-M3 `SubmissionAttemptId`, `ReservationId`, fake-write,
callback, or other process-local ordinal is keyed by `(RuntimeEpochId, raw value)` in M4 recovery and
audit. For compatibility with current raw-key owner containers, each new runtime also initializes
its raw submission/reservation counter above the greatest reconstructed raw high-water. A value
minted after the old durable watermark belongs to the old epoch and cannot collide with a new
composite key. Exhaustion fails closed rather than wrapping.

`CancelAttemptId` is `(RuntimeEpochId, OrderId, raw cancel ordinal)`. Restore retains every surviving
old composite identity and its state exactly. A new runtime may restart the raw per-order ordinal at
one because the fresh, durability-acknowledged runtime epoch makes the complete identity distinct;
within one epoch the raw ordinal remains non-wrapping and monotonic. A published-only old attempt
may disappear at crash, but its complete identity can never be minted by the new epoch. This closes
the cancel-initiation crash window without adding synchronous persistence to a private owner turn.

Journal sequence and snapshot commit ordinal are lineage-global. Snapshot ID alone is not ordered
across namespaces; the greatest valid snapshot commit ordinal defines newest. Conflicting duplicate
lineage, namespace registration, sequence, or commit ordinal is a recovery fault. Deterministic
scenarios inject the lineage and a distinct namespace at every restart.

### Journal ordering

The fake journal is an append-only bounded sequence of immutable canonical typed recovery records.
Until ADR-0014 exists, canonical here means one complete validated semantic value, not an
`AEGISJRN` byte schema, encoder, digest, or real durable-media representation.

1. Side-effect-free planning validates and retains the complete typed semantic payload, reserves
   the complete record, and assigns the next sequence only after all headroom succeeds. ADR-0014
   later derives and validates the canonical semantic digest from that same value.
2. The no-fail commit publishes the prepared record in causal owner-turn order before the business
   projection that it drives.
3. A record includes lineage, predecessor sequence, runtime/reconciliation epoch when applicable,
   complete replay provenance, the complete typed semantic value, and applicable typed audit
   linkage. Its digest is absent until the accepted ADR-0014 encoder exists. A
   `NamespaceRegistered` record is root-scoped and has no runtime/reconciliation epoch, subject,
   replay provenance, or audit link.
4. A pre-commit failure releases the reservation and consumes no sequence; a published sequence is
   immutable within the incarnation, while an unacknowledged discarded sequence may be allocated
   again after restart from durable watermark plus one.
5. Durability acknowledgement advances only across one contiguous published prefix.
6. Acknowledgement beyond a gap, conflicting duplicate sequence, typed-value mismatch, overflow,
   or incompatible lineage/provenance is a recovery fault. ADR-0014 additionally makes canonical
   digest mismatch a recovery fault.

The closed `JournalRecordKind` assignments and semantic payloads are in ADR-0013. `PrivateEventInput`
replays through the normal event reconciler, which must derive the same complete tagged correlation
result and validate it against the journaled immutable first-admission resolution before
committing. A mismatch is a recovery conflict; replay never ignores, upgrades, or replaces the
journaled resolution. `ReconciliationInput` dispatches each order/execution row through that
reconciler and each position/permission/margin/completeness row through its typed validation/safety
planner. Submission projection, namespace, identity high-water, account fence, reference intent,
recovery decision, and notification records use their own validated idempotent
restore planners. No dispatcher directly edits reservation or inventory outside a complete accepted
plan, and no non-event record is cast into `PrivateOrderEvent`.

The journal records inputs and validated projections needed for reconstruction; it does not replace
the richer audit stream. A missing unacknowledged record is never inferred from an audit ordinal or
from a gap in a process-local counter.

### Snapshot ordering

A snapshot is a separately validated cold recovery image, not the existing quiescent
`MarketRuntimeEvidence` observation. It is captured only at a completed owner-turn boundary and
contains:

- recovery lineage, `RecoverySnapshotId`, lineage-global snapshot commit ordinal, runtime/
  reconciliation epoch, configuration, and every applicable provenance fingerprint/revision;
- exact journal cut and required durable watermark;
- complete retained OMS rows and exchange mappings;
- remaining reservations and all aggregate cells;
- known inventory and unattributed account exposure;
- event/trade dedupe and pending-fill state;
- account safety, reasons, and recovery gates;
- reference-intent state;
- identity namespaces, epochs, counters, and high-water values;
- complete body length and canonical digest.

The cold image retains both source rows and cached aggregate cells. Restore recomputes every
reservation, inventory, exposure and account projection from the source rows and requires exact
equality before publishing mutable state. Aggregate-only ownership evidence is invalid.

It deliberately excludes strategy object memory, public order books/readiness, pending executor
work items, ingress slots, raw pointers, locks, threads, drivers, credentials, venue-native account
mappings, and native session/protocol state. M2 requires fresh coherent public market state after a
restart; M4 recovery cannot manufacture it from a private-order snapshot.

Snapshot creation has separate capture, body-publication, and commit-marker steps. Body publication
is not commitment. The fake medium accepts the marker in one atomic in-memory operation only after
the complete body/digest and every required journal record through its cut are durably acknowledged;
marker acceptance is the fake snapshot durability boundary. A crash before marker acceptance leaves
an incomplete snapshot that recovery ignores; a crash after it retains the exact committed image.
The greatest valid snapshot commit ordinal is selected, with no ID-based or container-order
tie-break.

Replay begins strictly after the committed cut and requires a contiguous compatible journal suffix.
M4 may record that a committed snapshot makes an older journal prefix eligible for rotation, but
its fake media preserves the complete journal and discards nothing. M9 owns real durable rotation,
truncation, retention, compaction, encryption, and audit storage.

### Recovery state machine

Recovery has one runtime-global prefix, executed exactly once per new incarnation:

1. validate startup policy and root provenance and construct every configured account gate plus
   critical private/reconciliation buffer in scratch; no public runtime or Ready callback exists;
2. read without mutation and validate the lineage, namespace registry, greatest committed compatible
   snapshot, and entire contiguous acknowledged journal suffix in required failure precedence;
3. construct OMS, reservations, inventory, dedupe, safety, intent, notification, and identity owners
   in scratch; recompute cached aggregates; dispatch each validated suffix record through its typed
   reducer; record replay completion even for an empty suffix;
4. select a fresh namespace, prove registry/capacity/counter headroom, and plan the next
   `NamespaceRegistered` record without mutating media;
5. append and fake-durability-acknowledge that record, then apply the namespace/runtime epoch to the
   scratch owners; and
6. atomically publish the complete recovered owner set with every affected account still gated.

No namespace/media mutation occurs before existing cold media validates. The new namespace remains
unusable until step 5 acknowledgement, and no client identity or callback can occur before step 6.

The following suffix then runs once for each affected account in canonical firm/account/venue order:

1. start one reconciliation epoch and obtain an explicitly `Complete`, `Incomplete`, or `Conflict`
   authoritative fake batch;
2. validate the whole batch identity, semantic value, canonical row order, coverage object, and
   fixed capacities;
3. apply known order/execution rows through normal OMS reconciliation, retain the resulting
   authoritative-cut inventory projection, then compare position, permission, margin, and
   completeness rows through typed safety planners;
4. retain and quarantine every unknown or contradictory authoritative row by stable subject key;
5. apply buffered and overlapping live facts through the same event/trade dedupe path;
6. validate the one closed live-catch-up completion certificate defined below;
7. only now plan and commit every complete-negative release whose cut proof and catch-up both
   satisfy the closed guard;
8. plan and commit an explicit deterministic recovery decision;
9. clear safety only when every operational predicate is proved; and
10. commit canonical recovery audit, plan per-bot notification decisions, then publish callbacks.

Validated restore factories publish nothing until the complete cold image validates. Recovery never
serializes raw C++ object memory and never reconstructs state through evidence views. Private-event
and authoritative order/execution reducers compose the same OMS/reservation/inventory plan as live
facts. Current position is a validation oracle: it cannot overwrite known inventory, and only
execution facts can change known inventory. Position difference becomes unattributed exposure under
ADR-0011. Position equality is evaluated against the retained cut projection from per-account step
3. Catch-up executions in per-account step 5 then legitimately advance final live inventory without
being compared to the older cut position.

The fake-recovery runtime composition completes this pipeline, or publishes the safely quarantined
result, before any public `Ready` callback can consume a restored longitudinal intent. Recovery
commands carry stable bounded slot handles rather than copying a cold image into the executor's
fixed-size work item.

The shared contract does not assume a resumable venue cursor. `AuthoritativeCutId` is opaque and
AEGIS does not order or subtract it. M7 may implement a query, history scan, snapshot-plus-stream
fence, or another mechanism only if it produces the exact normalized completeness and catch-up
certificate below.

### Complete authoritative result

`ReconciliationBatchId` is `(RecoveryLineageId, ReconciliationEpochId, one nonzero non-wrapping
u64 batch ordinal)`. The same ID and semantic digest is an exact duplicate; the same ID with another
digest is `Conflict`. Only the active epoch may apply. A later epoch does not append duplicate
economic source rows: exchange orders update by `(account, venue, ExchangeOrderId)`, executions by
the normal trade key, known lookups by local `OrderId`, and positions by `(account, venue,
instrument)` replacement/check semantics.

The batch completion is exactly `Complete`, `Incomplete`, or `Conflict`. A result is `Complete` only
when it proves:

- exact logical account, venue, and supported instrument scope;
- one explicit authoritative cut/epoch and completion status;
- complete open-order coverage;
- exact client/exchange identity lookup or complete order-history coverage for every unresolved
  local identity;
- complete execution/trade coverage for the entire interval in which each uncertain order could
  have accepted or filled;
- current authoritative position for every supported instrument;
- required permission and margin-mode facts;
- no missing page, gap, partial response, stale cut, or incompatible live-catch-up interval.

Canonical rows use the ADR-0013 kinds. Known-order lookup explicitly reports Present or Absent for
every unresolved local identity. Open-order rows form the complete authoritative set at the cut.
Execution rows carry stable trade keys. The batch-level `ReconciliationCoverage` owns the exact
per-order execution-coverage intervals, so a row cannot falsely certify the interval around itself.
Current-position rows carry exact signed quantity and signed contract-face notional at policy scale;
after all covered known executions apply, quantity is compared with the retained cut inventory and
the difference notional is derived from the quantity difference exactly as ADR-0011 specifies. The
authoritative direct notional is validated separately and is never subtracted from the
partition-independent known allocation. Permission and margin rows validate policy but never
change position.

An `Incomplete` batch applies every individually valid idempotent fact it actually contains, retains
known uncertainty, and ends `SafeReconciliationRequired` unless a contained unknown/conflict
requires quarantine. A `Conflict` batch retains its raw bounded evidence and ends
`SafeQuarantined`. Neither status can authorize a complete negative or safety clear.

Open-order absence alone is never a complete negative. An uncertain known order may transition to
`ReconciledAbsent` and release its residual reservation only when all relevant coverage above is
complete, no order/execution/position fact or retained terminal-cumulative cancellation fact
supports remaining exposure, and live catch-up closes the cut without a gap.

Specifically, the complete-negative plan requires one `Absent` known-order row for the exact local
identity under an exact-client-lookup or complete-history proof; no authoritative open-order row
correlated by local bytes, exchange identity, or retained mapping; complete trade coverage beginning
no later than the row's earliest possible acceptance and ending exactly at the batch cut; no
unprocessed or pending execution interval; no retained authoritative terminal cumulative quantity;
authoritative position equal to known inventory after covered executions; compatible
permission/margin/instrument scope; and the completed catch-up certificate. A retained historical
local/exchange mapping is AEGIS-owned correlation evidence, not an authoritative open order, and
does not by itself block the negative proof. The plan preflights the residual release and commits
through the same OMS plan. No subset is release authority.

### Closed live-catch-up fence

The fake adapter emits exactly one `LiveCatchUpCertificate` per reconciliation epoch. It contains
the authoritative snapshot cut, one-based normalized catch-up ordinals, first ordinal, last ordinal,
count, canonical digest, and `NoGap`. For zero rows, first and last are zero and count is zero. For a
nonempty set, first is one, last equals count, and every ordinal `1..count` appears once. Rows may
overlap the authoritative batch and are deduplicated by normal event/trade keys.

The fake adapter owns coverage ordering behind the opaque authoritative cut; AEGIS verifies the
normalized ordinal sequence, digest, matching epoch/cut, and `NoGap = true`. There is no alternative
"final complete check." Missing, duplicate-ordinal, conflicting, wrong-cut, or partial certificates
are incomplete recovery. M7 must later prove how a native protocol produces an equivalent
certificate rather than weakening this fence.

Missing coverage retains exposure and `ReconciliationRequired`. Contradiction, unknown activity,
unexplained position, permission/margin mismatch, or out-of-scope activity retains conservative
facts and `Quarantined`. M7 must later demonstrate a native way to satisfy this contract; inability
to do so keeps the route gated rather than weakening M4.

### Clearing account safety

`ReconciliationRequired` may return to `Synchronized` only after namespace registration, restored
source/cache equality, contiguous typed replay, a `Complete` authoritative batch, every known-order
and execution application, exact authoritative-position equality, compatible permission/margin and
instrument scope, a complete no-gap live certificate, zero unresolved reason, and an explicit
`MakeOperational` decision all succeed.

Leaving `Quarantined` additionally requires every unknown/conflict reason to be resolved by complete
authoritative facts without inventing ownership. If any reason remains, the explicit decision
records continued quarantine and retains the safe account condition. M4 tests inject the decision;
M11 owns the real operator authorization and runbook.

Second application of the acknowledged suffix, authoritative batch, and catch-up is a C13
idempotency proof after convergence, not a runtime prerequisite for clearing safety.

Local silence, timeout expiration, reconnect success, cancel-on-disconnect, current-open-order
absence, or an ordinary callback never clears either state.

### Callback crash semantics

Normal newly applied private facts invoke the originating bot only after complete owner-local state
and audit commit. Recovery replay does not redispatch historical order-event callbacks because a
crash after delivery but before durable acknowledgement could otherwise duplicate bot economic
action.

After convergence, notification identity is `(ReconciliationEpochId, BotId)`. Bots are ordered by
canonical encoded `BotId`. The recovery plan appends `Planned = 1` before invoking each callback and
`Delivered = 2` after return; an existing state suppresses a second attempt for that epoch. A later
new reconciliation epoch may publish one new state notification. It reports the final committed
projection and epoch rather than pretending to recreate missed order callbacks. Unknown/unowned
activity has no originating bot.

A crash after state commit but before ordinary callback completion/delivery is recorded as an
explicit callback audit gap. A durably acknowledged `Planned` or `Delivered` notification decision
is not retried; an unacknowledged decision is absent after restart and a new reconciliation epoch may
notify again. For a fixed crash point and durability script the callback vector and gap evidence are
deterministic, but callback history across cuts is not promised identical to uninterrupted history.
Exactly-once external delivery requires durable bot state/acknowledgement and remains deferred.

### Longitudinal `SubmitReferenceIntent`

`SubmitReferenceIntent` is a bounded command with one `ReferenceIntentId`, encoded as the active
16-byte registered namespace plus a nonzero non-wrapping `u64`, and no caller-authored order fields.
It drives exactly this existing M3 request:

| Field | Value |
|---|---|
| Route | `route.deribit-testnet-btc-perpetual` |
| Instrument | `BTC-USD-PERPETUAL` |
| Side | Buy |
| Type | Limit |
| Time in force | GTC |
| Price | `100.0` |
| Quantity | `2` |

The intent waits through non-Ready callbacks. On the next Ready market-data callback, the owner
marks it consumed immediately before the one same-callback `BotContext::submit` invocation. It is
consumed regardless of `LocallyRejected`, `WriteInitiated`, or `SubmissionUnknown`. Replay,
reconnect, order callbacks, recovery notifications, and later Ready callbacks never invoke submit
for that intent again.

The recoverable states are `Pending = 1`, `Consumed = 2`, and `OutcomeUnknown = 3`. Their complete
transition table is:

| Source | Input | Target/action |
|---|---|---|
| absent | one new current-runtime injection | `Pending`; no submit |
| `Pending` | non-Ready callback | `Pending`; no submit |
| `Pending` | next Ready callback in the same uninterrupted runtime | set `Consumed` immediately, then invoke submit exactly once in that callback |
| recovered `Pending` | restart restore | `OutcomeUnknown`; never invoke submit |
| `Consumed` | any replay, reconnect, callback, or recovery | `Consumed`; never invoke submit |
| `OutcomeUnknown` | any replay, reconnect, callback, or recovery | `OutcomeUnknown`; never invoke submit |

No production proof can distinguish crash-before-call from crash-after-call when only an older
Pending record is durable, so every recovered Pending fails closed. A crash-point label known to a
test is not recovery authority. Only a newly injected Pending in the current incarnation can make
its first call. The external command source does not automatically re-present an unacknowledged
injection. This is at-most-once behavior, not a false exactly-once durability claim.

The normal uninterrupted longitudinal scenario proves exactly one attempt, one generated client
identity when validation reaches identity generation, and at most one fake write. Local rejection,
submission uncertainty, recovery, replay, reconnect, and later callbacks never create an automatic
retry.

### Closed deterministic fakes

The M4 journal, snapshot, and authoritative reconciliation adapters are concrete `final`, bounded,
in-memory types with validated immutable scripts. They expose only copy/append/read/select/acknowledge
operations over M4 domain values.

The preallocated fake media is owned outside one runtime incarnation so its scripted acknowledged
prefix and committed snapshots survive destruction of that runtime. A live runtime holds one
exclusive owner-turn append lease; read/reopen authority is used only between incarnations. This
requires no lock or background thread on submission or private-event paths.

They have no endpoint, URL, host, port, socket, DNS, HTTP, WebSocket, credential, secret,
authentication, private session, native query, native transmission, callback injection,
`std::function` transport, filesystem path, file/stream handle, database, coroutine, future,
background thread, or retry API. Their product link graph adds no dependency.

Compile-time probes, the fail-closed manifest scanner, scanner self-tests, deterministic scenarios,
and link-graph review jointly prove the structural boundary. Merely running a fake without network
is not sufficient proof.

### Crash injection and oracle

`RecoveryCrashPoint` assigns these stable values. Every `AfterEach...` point carries the one-based
canonical row/callback ordinal being cut. Zero and unassigned values reject.

Points 35 through 40 occur once for the runtime-global prefix. Points 41 through 55 additionally
carry the one-based canonical affected-account occurrence; their row/callback ordinal is nested
within that account where applicable.

| Value | Crash point |
|---:|---|
| 1 | `BeforeIntentConsumption` |
| 2 | `AfterIntentConsumptionBeforeSubmit` |
| 3 | `AfterIntentSubmitBeforeOutcomeRecord` |
| 4 | `AfterIntentOutcomeRecord` |
| 5 | `BeforeSubmissionIdentity` |
| 6 | `AfterSubmissionIdentity` |
| 7 | `AfterSubmissionReservation` |
| 8 | `AfterSubmissionOmsAdmission` |
| 9 | `AfterSubmissionEncoding` |
| 10 | `AfterFakeAcceptedSlotCopy` |
| 11 | `AfterWriteInitiatedClassification` |
| 12 | `AfterSubmissionUnknownClassification` |
| 13 | `BeforePrivateAdmission` |
| 14 | `AfterPrivateAdmission` |
| 15 | `AfterPrivateCorrelationAndDedupePlan` |
| 16 | `AfterPrivateRecoveryInputPublication` |
| 17 | `AfterPrivateOmsCommit` |
| 18 | `AfterPrivateReservationCommit` |
| 19 | `AfterPrivateInventoryCommit` |
| 20 | `AfterAccountSafetyCommit` |
| 21 | `AfterPrivateAuditCommitBeforeCallback` |
| 22 | `AfterEachPrivateCallback` |
| 23 | `AfterCancelRequest` |
| 24 | `AfterCancelWriteDefinitelyFailed` |
| 25 | `AfterCancelWriteInitiated` |
| 26 | `AfterCancelWriteOutcomeUnknown` |
| 27 | `AfterDefinitiveCancellation` |
| 28 | `AfterReplacementReservationWhileOriginalLive` |
| 29 | `AfterJournalPublicationBeforeDurability` |
| 30 | `AfterJournalDurabilityAcknowledgement` |
| 31 | `AfterSnapshotCapture` |
| 32 | `AfterSnapshotBodyPublication` |
| 33 | `AfterSnapshotCommitMarker` |
| 34 | `AfterSnapshotRotationEligibility` |
| 35 | `AfterRecoveryGatesPrepared` |
| 36 | `AfterRecoverySnapshotSelectionAndValidation` |
| 37 | `AfterEachJournalReplayRecord` |
| 38 | `AfterJournalReplayCompletion` |
| 39 | `AfterNamespaceRegistrationAcknowledgement` |
| 40 | `AfterRecoveredStatePublication` |
| 41 | `AfterReconciliationEpochStart` |
| 42 | `AfterAuthoritativeResultValidation` |
| 43 | `AfterEachKnownOrderOrExecutionFact` |
| 44 | `AfterEachPositionOrPolicyCheck` |
| 45 | `AfterUnknownActivityQuarantine` |
| 46 | `AfterEachCatchUpFact` |
| 47 | `AfterCatchUpCompletionFence` |
| 48 | `AfterCompleteNegativePlanBeforeCommit` |
| 49 | `AfterCompleteNegativeCommit` |
| 50 | `AfterRecoveryDecisionPlan` |
| 51 | `AfterRecoveryDecisionCommit` |
| 52 | `AfterAccountSafetyClear` |
| 53 | `AfterRecoveryAuditCommitBeforeNotification` |
| 54 | `AfterEachRecoveryStateNotification` |
| 55 | `AfterRecoveryCompletion` |

Points 17 through 20 deliberately observe process-crash windows within the otherwise no-fail
owner-local commit. No callback, durability acknowledgement, background action, or other turn runs
there; the prepared input is necessarily published-only and is discarded on crash. Acknowledgement
can occur only on a later explicit fake owner/control turn after the complete business/audit commit,
which is cut by point 30. Recovery never assumes a published-only record survived.

The generic private points 13 through 22 apply to acknowledgement, rejection, contiguous fill, gap
insertion, multi-fill gap drain, exact duplicate, projection-only late event, account-wide timeout/
disconnect, safety-only contradiction, and unknown activity. Cell writes inside one no-fail phase
are semantically equivalent because there is no publication, callback, fallible operation, or owner
turn between them; C13 cuts at every phase boundary and every emitted callback rather than at
arbitrary container assignments.

The normative lifecycle mapping is:

| Lifecycle | Required crash points |
|---|---|
| intent | 1-4 plus generic journal 29-30 |
| M3 submission projection and ambiguous initiation | 5-12 plus 29-30 |
| every private/projection/safety transition | 13-22 plus 29-30 |
| cancellation and replace overlap | 13-30 with the applicable named 23-28 point |
| snapshot | 31-34 |
| one runtime-global cold validation, typed replay, namespace durability, and owner publication | 35-40 |
| each account's complete, incomplete, and conflicting reconciliation in canonical account order | 41-45 |
| live catch-up and fence | 46-47 |
| complete-negative release after the fence | 48-49 |
| decision, safety clear, audit, and per-bot notification | 50-55 |

The M3 benchmark composition gains no crash observer, function call, queue, or branch inside its
timed submit path. A closed M4-only deterministic composition reaches points 5 through 12 through
scripted component boundaries and test-owned cut control. The ordinary M3 benchmark executable and
its interval remain byte-for-byte behaviorally separate from that composition.

At each point the test destroys the complete runtime, discards volatile and unacknowledged state,
retains only the acknowledged namespace registry/journal prefix, committed snapshot, and
authoritative fake facts, registers a fresh client namespace, constructs a new runtime, restores,
replays, reconciles, and catches up. It then reconstructs the acknowledged suffix a second time in
fresh scratch state and separately reapplies the authoritative batch/catch-up to prove both layers'
idempotency.

The independent oracle compares exact OMS rows/mappings, remaining reservations, signed inventory,
all seven known scopes, unattributed exposure, identity/dedupe state, account safety, reference
intent, high-water values, dispositions, notification decisions, callbacks/gaps, and canonical M4
evidence. It derives one of the three convergence classes from the closed predicate above.
Production mutation tables and algorithms are not reused as the test oracle.

No test uses an actual process abort, filesystem, database, socket, wall-clock race, or unseeded
randomness. Runtime destruction/reconstruction and scripted fake durability make every crash cut
replayable under normal, sanitizer, TSan, and Release builds.

### Canonical recovery evidence

ADR-0013 fixes the record kinds, row meanings, decisions, convergence values, capacities, and root/
subject provenance binding. ADR-0014 must accept exact `AEGISJRN`, `AEGISSNP`, `AEGISREC`, and
`AEGISOAS` bytes before any encoder or byte golden exists. Accepted semantic evidence is never
overwritten, and every exhaustion/gap produces the exact ADR-0011 account-safety result before
unsafe mutation.

### Performance decision

M4 adds no named performance workload or qualification threshold. The accepted quality budget
defines none. M4 private events occur on later owner turns and do not extend `BENCH-M3-SUBMIT-001`
or `BENCH-M3-SUBMIT-002`. M4 qualification regresses the existing M0, M2, and M3 collectors and
validators unchanged.

## Consequences

- Recovery makes an honest distinction between operational and quarantined convergence.
- An uncertain order cannot be released using open-order absence alone.
- Existing client identities survive restart while new identities use a fresh namespace.
- Replay and live facts share one idempotent OMS path.
- Historical callback replay cannot automatically duplicate bot action.
- One-shot intent ambiguity fails by not retrying.
- M4 fixes the protocol M9 must persist without adding live communication or real durability.

## Deferred

M5 owns dynamic authority/freshness/modes and control-plane observation. M6 owns public networking.
M7 owns credentials, authenticated private sessions, native queries/streams, account mapping, and
proof of authoritative completeness. M8 owns native order/cancel transmission and arming. M9 owns
real durable journal/snapshot/audit storage, venue-backed restart/reconnect recovery, retention, and
any accepted durable callback-delivery mechanism. M10 owns load/fault/soak qualification, M11 real
operator recovery, M12 P&L/reporting, M13 cross-venue behavior, and M14 combined production
qualification and any separately authorized pilot.
