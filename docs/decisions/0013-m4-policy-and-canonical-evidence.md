# ADR-0013: M4 Capacity Policy and Canonical Evidence Semantics

> **Purpose:** Fix the immutable M4 capacity policy, stable evidence meanings, provenance scopes,
> and fail-closed headroom rules while reserving byte encoding for a required later M4 schema ADR.

- **Status:** Accepted
- **Date:** 2026-08-23
- **Scope:** M4 capacities, semantic evidence records, provenance, diagnostics, and exhaustion
- **Related:** [Normalized private events and extended OMS](0010-normalized-private-events-and-oms.md),
  [inventory, reservation conversion and account safety](0011-inventory-reservation-and-account-safety.md),
  and [fake-backed recovery and crash consistency](0012-fake-backed-recovery-and-crash-consistency.md)

## Context

M4 introduces bounded event identities, fill gaps, mappings, inventory rows, account-safety facts,
journal records, snapshots, reconciliation rows, callbacks, and audit evidence. Positive-but-unnamed
capacities would let components assume incompatible bounds. Extending M3's `AEGISSUP` bytes would
also change an accepted M3 fingerprint.

M4 needs stable semantic records before implementing OMS and recovery, but exact binary encodings
for nested OMS, inventory, snapshot, and reconciliation rows are safest after their accepted domain
types exist. Claiming byte-exact schema one now would force tests to invent missing widths and digest
preimages. This ADR therefore accepts the policy bytes and semantic record contracts, reserves the
evidence names, and requires a separate accepted schema ADR before any evidence encoder exists.

## Decision

### M1-M3 compatibility boundary

The byte layout and every existing assignment in `AEGISCFG`, `AEGISTRS`, `AEGISRTS`, `AEGISRSP`,
`AEGISSUP`, `AEGISSTS`, and `AEGISFOE` schema one remain unchanged. `AEGISSTS`'s schema-one
`SubmissionReason` vocabulary appends only values 58 and 59 fixed by ADR-0011; historical records,
fixtures, and digests do not contain those values and remain byte-for-byte unchanged.

`AEGISM4P` is an accepted eight-byte ASCII identifier with schema value `1`. The identifiers
`AEGISPEV`, `AEGISOAS`, `AEGISJRN`, `AEGISSNP`, `AEGISREC`, and `AEGISM4D` are reserved for M4 but
do not yet name an accepted byte schema. ADR-0014 is required to assign their exact stream framing,
nested row layouts, presence bytes, widths, canonical byte order, and digest preimages before an
encoder, decoder, byte golden, or schema-version claim is implemented.

### Runtime-global lineage and provenance

One `RecoveryLineageId` covers the complete multi-firm, multi-account runtime represented by one
external fake medium. Journal sequence, namespace registry, snapshot commit ordinal, and runtime
epoch are lineage-global. Recovery decisions and authoritative reconciliation remain account/venue
scoped within that global lineage.

`M4RootProvenance` contains, in this logical order:

1. configuration fingerprint;
2. organization revision;
3. runtime-policy fingerprint;
4. risk-policy revision;
5. risk-policy fingerprint;
6. submission-policy fingerprint; and
7. M4-policy fingerprint.

`M4SubjectProvenance` is a closed optional group layered on the root. Its logical order is account,
venue, firm, desk, bot, strategy, route plus route revision, and instrument plus metadata revision.
Presence rules are:

| Record scope | Required subject fields | Forbidden subject fields |
|---|---|---|
| lineage/runtime | none | every subject field |
| account/venue | account, venue, configured firm | desk, bot, strategy, route, instrument unless the record declares a supported instrument subject |
| known local order | every subject field | none |
| unknown authoritative subject | account, venue, configured firm when independently proved; supported instrument/metadata when independently proved | every unproved attribution field |

Namespace registration, global identity high-water, and snapshot headers use root provenance only.
Snapshot sections carry their own sorted subject rows. Absence is a typed state, never an empty or
sentinel identifier.

### Exact `AEGISM4P` encoding

Only the policy receives an exact byte encoding in this ADR. Its primitive profile is:

- unsigned integers are big-endian at the named width;
- SHA-256 fingerprints are exact 32 bytes;
- configured identifiers are encoded by their already accepted canonical domain encodings; and
- no object padding, native pointer, platform `size_t`, locale, or host byte order is used.

The bytes are:

```text
magic "AEGISM4P"[8] || schema:u16=1 ||
configuration_fingerprint[32] || organization_revision:u64 ||
runtime_policy_fingerprint[32] || risk_policy_revision:u64 ||
risk_policy_fingerprint[32] || submission_policy_fingerprint[32] ||
capacity_1:u64 || ... || capacity_26:u64
```

The capacity fields, in exact order, are:

1. `max_private_admissions`;
2. `max_reconciliation_admissions`;
3. `max_account_safety_fences`;
4. `max_private_event_records`;
5. `max_event_identity_records`;
6. `max_trade_identity_records`;
7. `max_exchange_order_mappings`;
8. `max_pending_fill_intervals_per_order`;
9. `max_cancel_attempts`;
10. `max_inventory_source_rows`;
11. `max_inventory_aggregate_cells`;
12. `max_unattributed_exposure_rows`;
13. `max_account_safety_records`;
14. `max_transition_effects_per_turn`;
15. `max_order_callbacks_per_turn`;
16. `max_private_diagnostics`;
17. `max_private_audit_records`;
18. `max_journal_records`;
19. `max_snapshot_records`;
20. `max_reconciliation_batches`;
21. `max_reconciliation_rows_per_batch`;
22. `max_live_catchup_facts`;
23. `max_recovery_epochs`;
24. `max_namespace_registrations`;
25. `max_recovery_notifications`;
26. `max_reference_intents`.

The policy fingerprint is SHA-256 over those complete bytes. Every value is explicit, positive, no
greater than `UINT32_MAX`, and small enough that every checked worst-case product and future
length-delimited record is representable. There are no defaults, environment overrides, runtime
growth, or M4 operator mutation.

Validation additionally requires:

- account safety fences are at least the configured logical-account count;
- exchange-order mappings are at least the outbound OMS row capacity;
- inventory source rows are at least the reservation capacity;
- inventory aggregate cells cover every canonical risk-policy key across all seven scopes;
- namespace registrations are at least recovery epochs plus one;
- the reference fixture has exactly one reference-intent slot;
- checked `drain_width = 1 + max_pending_fill_intervals_per_order` is representable;
- transition effects and callbacks are each at least `drain_width`;
- per-turn audit scratch is at least `2 + drain_width`; and
- reconciliation and catch-up scratch each represent the complete canonical fixture.

Total append-only evidence can still fill over time. Every owner turn preflights its exact remaining
needs. Failure uses the dedicated account fence and never partially mutates economics.

### Stable semantic event and audit values

`PrivateEventDisposition : u8` assigns `Applied = 1`, `BufferedGap = 2`,
`AppliedFromBuffer = 3`, `ProjectionOnly = 4`, `ExactEventDuplicate = 5`,
`ExactTradeDuplicate = 6`, `SafetyContained = 7`, and `ForbiddenRejected = 8`.

`OriginatingEventIdentityKind : u8` assigns `None = 0`, `Local = 1`, `Venue = 2`,
`Reconciliation = 3`, and `RecoveryAction = 4`. The tagged value contains exactly:

- no payload for `None`;
- `LocalOrderEventId` for `Local`;
- venue, account, private-source epoch, and private-event ID for `Venue`;
- reconciliation epoch, authoritative cut, and row ordinal for `Reconciliation`; or
- reconciliation epoch and recovery-action ordinal for `RecoveryAction`.

`M4AuditKind : u8` assigns `EventDisposition = 1`, `OmsTransition = 2`,
`ReservationTransition = 3`, `InventoryTransition = 4`, `AccountSafetyTransition = 5`,
`OrderCallbackDecision = 6`, `CallbackFault = 7`, `RecoveryGap = 8`, and
`RecoveryNotificationDecision = 9`.

`CallbackDecision : u8` assigns `None = 0`, `Planned = 1`, `Delivered = 2`,
`SuppressedDuplicate = 3`, `SuppressedBuffered = 4`, `SuppressedReplay = 5`, and `Faulted = 6`.
`RecoveryGapKind : u8` assigns `None = 0`, `PublishedNotAcknowledged = 1`,
`MissingJournalInput = 2`, `InvalidSnapshot = 3`, `LocalProvenanceMissing = 4`, and
`CallbackDeliveryAmbiguous = 5`.

One semantic `PrivateEventEvidence` owns root/subject provenance, runtime epoch, admission ordinal,
receive time, the complete normalized event, semantic equality fields from ADR-0010, disposition,
optional reconciliation epoch/journal sequence, and diagnostic linkage. One semantic
`OrderAuditRecord` owns runtime epoch/audit ordinal/kind, tagged origin, root/subject provenance,
all present order/exchange/trade/cancel identities, exact OMS before/after, exact reservation
before/after including closure cause, canonical inventory effect rows, account-safety before/after,
callback decision/range, recovery gap, and optional domain error.

Canonical inventory effect order is firm, scope kind, subject, instrument, then currency. Exact
semantic duplicate comparison is field-by-field over every normalized semantic field and excludes
only receive time and evidence ordinals named by ADR-0010. Until ADR-0014 accepts a digest preimage,
production stores/compares the full bounded typed semantic value and must not invent a private byte
encoding.

### Stable journal semantics

`JournalRecordKind : u8` assigns:

| Value | Kind | Replay owner |
|---:|---|---|
| 1 | `NamespaceRegistered` | lineage namespace-registry planner |
| 2 | `SubmissionProjection` | validated submission/OMS/reservation restore planner |
| 3 | `PrivateEventInput` | normal private-event reconciler |
| 4 | `ReconciliationInput` | typed reconciliation dispatcher |
| 5 | `AccountSafetyFence` | account-safety restore planner |
| 6 | `ReferenceIntentState` | reference-intent restore planner |
| 7 | `IdentityHighWater` | identity restore planner |
| 8 | `RecoveryDecision` | recovery-state restore planner |
| 9 | `RecoveryNotificationDecision` | notification restore planner |

`IdentityCounterKind : u8` assigns `SubmissionAttempt = 1`, `Order = 2`, `Reservation = 3`,
`CancelAttempt = 4`, `LocalOrderEvent = 5`, `FakeWrite = 6`, `Callback = 7`, `Audit = 8`,
`Diagnostic = 9`, `Journal = 10`, `Snapshot = 11`, `Reconciliation = 12`,
`ReferenceIntent = 13`, `AdmissionAttempt = 14`, and `ReceiveSequence = 15`.

Every journal record semantically owns lineage, sequence/predecessor, runtime epoch, kind, root and
applicable subject provenance, complete typed payload, semantic equality value, and optional audit
link. Its closed payload is:

| Kind | Complete semantic payload |
|---|---|
| namespace | order namespace and registry count after append |
| submission projection | composite runtime/raw attempt; order and reservation IDs; full M3 request side/type/TIF/price/quantity; all caller-unforgeable attribution; account/route/venue/instrument and metadata; original approved quantity/notional and all seven scope keys; reservation state/remainder/cumulative allocation/closure; M3 OMS row/state; encoding/write identity and result; full submit disposition/stage/reason; applicable counter highs |
| private event | complete normalized typed event before disposition |
| reconciliation input | batch identity, row kind/ordinal, cut, and complete typed row |
| safety fence | account, attempted admission ordinal, source identity/value, reason, and loss count |
| reference intent | intent ID, state, and optional complete submit result |
| identity high-water | assigned counter kind, qualified epoch, high-water value, and the complete `OrderId` subject exactly when the kind is `CancelAttempt`; the qualified epoch and order together scope that raw cancel ordinal |
| recovery decision | reconciliation epoch, decision, convergence class, and complete active reason set |
| recovery notification | reconciliation epoch, bot, and Planned or Delivered state |

The submission projection is deliberately complete enough to recover from an empty snapshot without
asking venue authority to invent AEGIS request economics, attribution, reservation, or provenance.

### Stable snapshot semantics

`SnapshotSectionKind : u8` assigns `Identity = 1`, `Oms = 2`, `ExchangeMapping = 3`,
`Reservation = 4`, `InventorySource = 5`, `InventoryAggregate = 6`,
`UnattributedExposure = 7`, `EventIdentity = 8`, `TradeIdentity = 9`,
`AccountSafety = 10`, `ReferenceIntent = 11`, and `RecoveryNotification = 12`.

A cold snapshot owns root provenance, lineage, snapshot ID/commit ordinal, runtime and optional
reconciliation epoch, journal cut/watermark, and these complete semantic section rows:

- identity: every registered namespace, qualified counter kind/high-water, runtime epoch, and each
  `(RuntimeEpochId, OrderId)` with its greatest raw cancel ordinal;
- OMS: every complete retained admission plus mutable OMS/cancel/gap projection;
- exchange mapping: account, venue, exchange ID, and local order;
- reservation: complete original/mutable reservation evidence and closure;
- inventory source: known fill source/order and signed quantity/notional attribution;
- inventory aggregate: canonical seven-scope cell key and exact signed/reserved/worst values;
- unattributed exposure: kind, stable key, quantifiable projection or bounded retained fact;
- event/trade identity: complete key, full semantic comparison value, and disposition;
- account safety: state, first reason, first quarantine reason, ordered active reasons/provenance;
- reference intent: identity, recoverable state, and optional submit result; and
- recovery notification: reconciliation epoch, bot, and Planned/Delivered state.

Rows sort by section value and then their canonical typed identity/subject keys. The snapshot also
owns body completeness, aggregate recomputation result, and commit-marker state. ADR-0014 must turn
these semantic rows into exact bytes before snapshot encoding.

### Stable reconciliation and diagnostic semantics

`ReconciliationCompletion : u8` assigns `Complete = 1`, `Incomplete = 2`, and `Conflict = 3`.
`ReconciliationRowKind : u8` assigns `KnownOrderLookup = 1`, `OpenOrder = 2`, `Execution = 3`,
`CurrentPosition = 4`, `Permission = 5`, and `MarginMode = 6`.
`KnownOrderLookupResult : u8` assigns `Present = 1` and `Absent = 2`.
`KnownOrderProofKind : u8` assigns `ExactClientIdentityLookup = 1` and
`CompleteOrderHistory = 2`.
`PermissionResult : u8` assigns `Compatible = 1` and `Incompatible = 2`.
`MarginModeResult : u8` assigns `Compatible = 1` and `Incompatible = 2`.
`AuthoritativeOpenOrderState : u8` assigns `Working = 1`, `PartiallyFilled = 2`, and
`CancelPending = 3`. `AuthoritativeCategory` is one through 64 opaque bytes. Optional native client
bytes are zero through 128 opaque bytes and never prove ownership unless all 24 bytes parse as the
exact retained `OrderId` and every correlation field agrees.

Every reconciliation time and interval endpoint is an existing `SourceTimestamp` in the one
comparable source-time domain certified for that batch. The batch owns one authoritative cut ID and
cut time. The row payloads are:

- known lookup: local order, result, proof kind, result-dependent exchange order, and inclusive
  earliest/latest source times for the possible acceptance interval;
- open order: exchange order, optional client bytes, instrument, side, original/remaining quantity,
  limit price, assigned open state, metadata revision, and source time;
- execution: exchange order/client locator, trade, instrument, authoritative source-row side,
  incremental/cumulative quantity, execution price, metadata revision, and source time;
- current position: instrument, signed quantity, signed direct contract-face notional, and cut;
- permission: account, result, and `AuthoritativeCategory`; and
- margin mode: account, result, and `AuthoritativeCategory`.

`Present` requires exactly one `ExchangeOrderId`; `Absent` forbids one. Every lookup satisfies
`earliest <= latest <= batch cut time`. Every open-order quantity is positive, remaining is positive
and no greater than original, price is positive, and supported metadata scale/step/tick validation
applies. `Working` requires remaining equal original; `PartiallyFilled` requires remaining below
original; `CancelPending` permits either. Execution common/known/unknown validation follows
ADR-0010 after the source-row transformation below. Position zero is valid. Current-position cut
identity and cut time must equal the batch cut. Every applicable row source time is no later than
that cut. An invalid row cannot be skipped from an otherwise Complete batch; it makes the batch
`Conflict`.

`KnownOrderCoverage` contains one unresolved local `OrderId`, its exact proof kind, inclusive
earliest possible acceptance time, inclusive coverage-end time, and `Complete` Boolean. It must
exactly match that order's lookup proof kind and endpoints; a complete row ends exactly at the batch
cut time. `ExecutionCoverageInterval` contains the same local identity, a start no later than the
lookup/known-coverage earliest possible acceptance, an end exactly equal to the batch cut time, and
`Complete` Boolean. Exactly one of each coverage row is required for every unresolved known order,
sorted by `OrderId`.
`ReconciliationCoverage` has these six closed components in order:

1. complete open-order set Boolean;
2. the complete known-order-coverage vector, with every row `Complete`;
3. the complete execution-coverage interval vector, with every row `Complete`;
4. complete current-position set Boolean;
5. complete permission set Boolean; and
6. complete margin-mode set Boolean.

Batch completion may be `Complete` only when all four Booleans and every required known-order and
execution coverage row are true/present, every supported instrument/account has its required row,
all interval/cut guards above hold, and no row conflict exists. A false/missing component makes it
`Incomplete`; contradictory identity/content or interval/cut mismatch makes it `Conflict`.

Rows sort by row-kind value and then: local order for lookup; account/venue/exchange order for open
order; trade key for execution; account/venue/instrument for position; account for permission and
margin. Duplicate stable keys with identical complete typed values are one explicit duplicate batch
disposition; changed values under one batch identity are Conflict. A later batch updates open-order
and position subjects by stable key and still passes event/trade idempotency.

After every covered execution row is deduplicated and applied to the cut projection, each known
open-order row must satisfy `remaining = local original - local cumulative at cut`. `Working`
requires zero cumulative and post-reducer primary `Working`; `PartiallyFilled` requires positive
sub-original cumulative and post-reducer primary `PartiallyFilled`; `CancelPending` requires the
same quantity-derived `Working` or `PartiallyFilled` primary and does not itself prove a local cancel
attempt. Any residual or state mismatch makes the batch `Conflict` before an operational decision;
an `Incomplete` batch never releases the retained residual.

The deterministic reducer mapping is:

| Row | Known/consistent effect | Unknown effect | Contradictory effect |
|---|---|---|---|
| lookup Present | correlate and normalize acknowledgement; compare retained account/venue | `UnknownOrder` | safety-contained contradiction |
| lookup Absent | retain negative evidence only; never release alone | `UnknownOrder` for unknown local identity | safety-contained contradiction |
| open order | compare side/price/original quantity/metadata; normalize acknowledgement; fills still require execution rows | create/update `UnknownOpenOrder` by exchange key | safety-contained contradiction |
| execution | require source-row side equal retained OMS side, omit side from the known normalized event, then use the normal OMS plan | pass required authoritative source-row side into the unknown normalized event and create/update unknown-trade projection | side mismatch or another contradiction makes the batch `Conflict` before economic mutation |
| current position | validate against cut inventory and replace/check position-difference row | retain unattributed difference | `UnexplainedPosition`/safety contradiction |
| permission | validate accepted configured authority | retain mismatch | `PermissionMismatch` |
| margin mode | validate accepted configured mode | retain mismatch | `MarginModeMismatch` |

An unknown open-order row is the only authoritative subject allowed to supply an original-quantity
baseline and retained side for later unknown execution/cancellation consistency checks under the
same account/venue/exchange key. A later execution row supplies its own authoritative side and must
equal that retained side when the open-order row exists; mismatch makes the batch `Conflict`. The
open-order row never supplies local bot, desk, strategy, route, or ownership. Reconciliation
execution rows always require source-row side; this is authoritative input evidence, not a
caller-owned normalized known-order field. A matching known row validates and strips it as specified
above. An unknown row retains and passes it through, so the ADR-0010 optional-side case remains
available to venue-origin events but not to this complete reconciliation row shape.

`RecoveryDecision : u8` assigns `MakeOperational = 1`,
`KeepReconciliationRequired = 2`, and `KeepQuarantined = 3`. `ConvergenceClass : u8` assigns
`Operational = 1`, `SafeReconciliationRequired = 2`, and `SafeQuarantined = 3`.

One reconciliation record semantically owns lineage/epoch/cut/batch identity, completion, root and
account subject provenance, the complete `ReconciliationCoverage`, canonical typed rows, the exact
live-catch-up certificate, decision, convergence, active reasons, and journal/audit links.

`M4DiagnosticStage : u8` assigns `Admission = 1`, `Shape = 2`, `Provenance = 3`,
`Identity = 4`, `Correlation = 5`, `Oms = 6`, `Economics = 7`, `Evidence = 8`,
`Recovery = 9`, `Callback = 10`, and `Internal = 11`.
`DiagnosticSafetyAction : u8` assigns `None = 0`, `ReconciliationRequired = 1`,
`Quarantined = 2`, and `RuntimeFaulted = 3`.

A diagnostic owns runtime epoch/ordinal, optional tagged source identity and full semantic value,
root/subject provenance, domain error, stage, safety action, and optional audit link. Diagnostics
never become an alternative business ledger.

### Bounded evidence and required schema ADR

Private rejection detail is bounded to 256 bytes by ADR-0010. A retained normalized unquantifiable
fact uses ADR-0011's compile-time fixed typed storage envelope no larger than 4,096 bytes; ADR-0014
must define any later canonical byte representation. No safety
record owns an unbounded string, vector, callback, closure, or heap-polymorphic payload.

Accepted semantic evidence is append-only and never overwritten. Exact duplicates do not apply a
second economic transition. Buffer insertion and buffer application are distinct linked semantic
records. General evidence exhaustion uses the preallocated per-account fence to latch
`EvidenceCapacityExhausted` and `Quarantined`; an already active fence increments its checked loss
count. Loss-counter exhaustion faults the runtime closed.

ADR-0014 is a required M4 contract, not a deferral to M9. It must be accepted and committed before
the first `AEGISPEV`, `AEGISOAS`, `AEGISJRN`, `AEGISSNP`, `AEGISREC`, or `AEGISM4D` encoder or byte
golden. It must define every nested row layout, exact optional/presence group, integer width, vector
framing, sort key, maximum encoded length, and semantic/body/reason-set digest preimage. The emitter
implementation and tests must land in the same or a later commit.

## Consequences

- Every M4 component binds one explicit capacity policy and root provenance fingerprint.
- Runtime-global identity records never invent an account or venue subject.
- One gap-closing fill turn is representable at startup or rejected before mutable state exists.
- Independent semantic tests can use full typed values without importing production tables.
- Byte goldens cannot silently invent a format; ADR-0014 must precede their implementation.
- M1-M3 layouts, old assignments, and historical fingerprints remain unchanged.

## Deferred

M9 owns real durable-media formats, migrations, retention, compaction, encryption, and durability
qualification after M4 fixes the canonical in-memory/fake byte artifacts. M10 owns performance
sizing and backlog qualification. M11 owns dynamic operator configuration and evidence export.
