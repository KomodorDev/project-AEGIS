# ADR-0011: Inventory, Reservation Conversion and Account Safety

> **Purpose:** Fix M4 signed inventory, cumulative reservation conversion, seven-scope exposure,
> unknown-activity containment, account quarantine, and atomic private-event mutation.

- **Status:** Accepted
- **Date:** 2026-08-23
- **Scope:** M4 inventory, confirmed exposure, reservation lifecycle, account safety, and capacity
- **Related:** [Normalized private events and extended OMS](0010-normalized-private-events-and-oms.md),
  [canonical bot-bound submission and fixed risk](0008-canonical-submission-and-fixed-risk.md),
  [dedicated testnet account](0003-dedicated-testnet-account.md),
  [serialized data-plane execution](0001-serialized-data-plane-execution.md), and
  [M4 capacity policy and canonical evidence](0013-m4-policy-and-canonical-evidence.md)

## Context

M3 reserves the complete worst-case exposure of every locally admitted order and treats confirmed
position as zero. M4 fills must move economic weight from that remaining reservation into signed
confirmed inventory without creating a window in which risk sees neither side of the transfer.
Partial fills also expose a rounding problem: independently rounding each incremental fill can make
the final notional depend on how a venue partitions one economic execution across messages.

An unknown venue order or trade cannot honestly be assigned to a bot, desk, strategy, or route.
Nevertheless, discarding it or leaving the account enabled would understate risk. ADR-0003 already
requires account quarantine and forbids silent adoption, cancellation, retry, or flattening. M4 must
turn that rule into bounded owner-local state without importing M5's later dynamic risk modes.

## Decision

The following subsections define the accepted reservation conversion, signed inventory, unattributed
exposure, and account-safety rules.

### Stable inventory and account errors

M4 appends these stable `DomainErrorCode` values:

| Value | Error |
|---:|---|
| 910 | `InvalidInventoryState` |
| 911 | `InventoryCapacityExceeded` |
| 912 | `InvalidReservationConversion` |
| 913 | `AccountNotSynchronized` |
| 914 | `InvalidAccountSafetyState` |
| 915 | `PrivateEvidenceExhausted` |

No earlier error value is renumbered. Arithmetic primitives continue to return their existing exact
fixed-point error codes; the M4 wrapper records the failing inventory/reservation field and stable
quarantine reason without disguising overflow as capacity or state failure.

### One confirmed-position source of truth

The owner-local inventory ledger is the single mutable source of signed confirmed aggregate
position truth. Risk reads or jointly plans inventory projections; it does not maintain an
independently mutable copy. OMS owns per-order cumulative fill, terminal target, pending intervals,
exchange mapping, and event/trade disposition. Audit owns execution-history detail. Inventory may
reference the source event, trade, and audit identity for provenance, but it does not independently
deduplicate or decide cumulative fill.

For every known local fill the ledger retains:

- firm, desk, bot, and strategy attribution inherited from the retained OMS row;
- logical account, route, venue, and normalized instrument;
- exact signed confirmed quantity;
- exact signed quote face-notional allocation at the policy scale;
- the source OMS order and latest applied event/trade/audit identity;
- execution price for order state and evidence only; and
- configuration, organization, route, metadata, runtime-policy, risk-policy, and submission-policy
  provenance.

Buy fills add signed quantity and face notional. Sell fills subtract them. Zero signed position is
valid; arithmetic overflow, invalid scale, impossible sign, and inconsistent provenance fail closed.

P&L, average cost, mark price, fees, settlement, collateral, and accounting valuation are not
inventory authority in M4 and remain deferred.

### Reservation evidence and states

The existing `Held = 1` and `Released = 2` assignments remain unchanged. M4 appends
`ConsumedByFill = 3`.

`ReservationClosureCause : u8` assigns `Unassigned = 0`, `DefiniteLocalFailure = 1`,
`ExchangeRejected = 2`, `DefinitiveCancellation = 3`, `CompleteAuthoritativeNegative = 4`, and
`FullFill = 5`. `Held` requires `Unassigned`; `Released` requires values 1 through 4; and
`ConsumedByFill` requires `FullFill`. Any other pairing rejects.

Each M4-capable reservation retains both the immutable original approved exposure and mutable
remaining exposure:

- original and remaining exact quantity;
- original once-rounded and remaining quote face notional;
- cumulative converted quantity and confirmed face-notional allocation;
- side, instrument, currency, all seven scope keys, order identity, and provenance;
- state and a stable closure cause.

`Held` means some original order exposure may still be live. A partial fill keeps the reservation
`Held` with a smaller remainder. `ConsumedByFill` means cumulative fill reached the complete
approved quantity. `Released` means a definitive non-fill fact removed the remaining possible order
exposure. Repeating either closure is an invariant error and cannot decrement an aggregate twice.

`Released` and `ConsumedByFill` are both terminal reusable slot states. A slot may be reused only for
a monotonically newer reservation identity after its held-count and residual aggregate contribution
have been removed exactly once. Historical reservation closure remains in journal, audit, and
dedupe evidence rather than occupying the reusable risk slot. `release` subtracts only mutable
remaining exposure. Full-fill conversion sets remaining exposure to zero, changes state once, and
decrements held count once. Existing `ReservationEvidence::exposure` keeps its M3 meaning as the
immutable original approved exposure; M4 appends remaining/cumulative evidence instead of
reinterpreting that field.

M3's raw `ReservationId` remains equal to its creating `SubmissionAttemptId`. M4 recovery evidence
qualifies process-local identities with its runtime/recovery epoch while preserving every old
schema-one raw value.

### Partition-independent partial conversion

Let:

- `Q` be the original approved order quantity;
- `N` be M3's once-rounded positive approved quote face notional;
- `C` be the new cumulative filled quantity;
- `R = Q - C` be the remaining quantity;
- `M` be the instrument's quote-currency contract multiplier.

The cumulative allocation is:

```text
remaining_quantity(C) = R

remaining_notional(C) =
    N                                      when C == 0
    AwayFromZero(R * M at policy scale)    when 0 < C < Q
    0                                      when C == Q

confirmed_notional_allocation(C) = N - remaining_notional(C)
```

The newly confirmed notional delta is the new cumulative confirmed allocation minus the previous
cumulative allocation. It is signed only after the positive allocation has been calculated and
checked.

This rule:

- conserves the original approved notional exactly;
- depends only on cumulative quantity rather than message partition;
- keeps the remaining live order conservatively rounded;
- reaches exactly `N` at full fill; and
- prevents separately rounded incremental fills from accumulating drift.

At intermediate quantities, a small fill may not yet move one output quantum of face-notional
allocation. The exact signed confirmed quantity and execution remain recorded. The complete
same-side original risk endpoint remains represented by confirmed allocation plus residual
reservation, so this allocation cannot remove the order's approved worst-case exposure.

Every multiplication, rescale, subtraction, sign conversion, absolute value, and aggregate update
is checked. An invalid candidate fails before OMS, reservation, or inventory mutation.

### Seven-scope known-order aggregation

Known-order reservation and inventory effects use the same firm-qualified v1 subjects fixed by
ADR-0008:

1. Bot
2. Desk
3. Firm
4. Account
5. Route
6. Instrument
7. Venue

For scope projection `s` and instrument `i`:

```text
worst_quantity[s,i] = max(
    abs(confirmed_quantity[s,i] + reserved_buy_quantity[s,i]),
    abs(confirmed_quantity[s,i] - reserved_sell_quantity[s,i]))
```

For instrument and quote currency `(i,c)`:

```text
instrument_worst_notional[s,i,c] = max(
    abs(confirmed_notional[s,i,c] + reserved_buy_notional[s,i,c]),
    abs(confirmed_notional[s,i,c] - reserved_sell_notional[s,i,c]))
```

The quote-currency aggregate is the checked sum of each instrument's absolute directional maximum.
One instrument cannot offset another. One firm cannot share or offset another firm's bucket.
Gross reserved notional remains the sum of residual buy and sell reservation face values without
netting.

A fill plan resolves all affected keys for all seven scopes, calculates the complete replacement
candidate for every cell, and checks arithmetic before committing any cell. Collection or map
iteration order cannot affect the result.

### Open-order count

Open-order count is the number of `Held` reservations, not the number of acknowledged OMS rows.

- Acknowledgement changes no count.
- Partial fill keeps count one for that order.
- Cancel request, any cancel-write outcome, timeout, disconnect, and incomplete reconciliation keep
  count one.
- Full fill changes `Held` to `ConsumedByFill` and removes one count.
- Exchange rejection, definitive cancellation, or complete authoritative negative changes `Held`
  to `Released` and removes one count.
- Every count removal occurs exactly once.

An order whose cancellation result reports an unapplied higher cumulative fill remains `Held` until
the missing fills are reconciled and the authoritative terminal target is reached.

### Cancel-plus-new overlap

A new replacement order uses the ordinary M3 check-and-reserve path and obtains its own `OrderId`
and reservation. The original remains fully or partially held until its own definitive terminal
fact. Therefore both orders contribute simultaneously to open count, gross reserved notional,
directional reservation, and worst-case exposure while coexistence is possible.

No special replacement netting, transfer, credit, or optimistic original release exists.

### Unknown and external activity

An event or reconciliation row without provable retained OMS ownership is never inserted into the
known-order inventory projection. Its authoritative economics are retained in a separate bounded
`UnattributedAccountExposure` source table. `UnattributedExposureKind : u8` assigns
`UnknownOpenOrder = 1`, `ProvisionalUnknownTrade = 2`,
`AuthoritativePositionDifference = 3`, and `Unquantifiable = 4`.

The stable source keys and numeric rules are:

| Kind | Stable key | Exact projection |
|---|---|---|
| unknown open order | account, venue, exchange-order ID | authoritative remaining buy/sell quantity and contract-face notional as reservation-style exposure |
| provisional unknown trade | account, venue, trade ID | authoritative signed incremental quantity and away-from-zero contract-face notional until a complete position cut covers it |
| position difference | account, venue, instrument | latest complete-cut authoritative signed position minus known inventory after every covered known execution is applied |
| unquantifiable | semantic fact identity | retained normalized typed fact in a fixed storage envelope no larger than 4,096 bytes, plus failure; numeric risk is unavailable rather than zero |

The independently provable firm, account, venue, and instrument projections include quantifiable
rows. Bot, desk, strategy, and route are absent and receive no invented bucket. Unknown open orders
use the same directional worst/gross formulas as known residual reservations. Provisional unknown
trades use signed confirmed-style quantity/notional. A complete authoritative position cut marks
every covered provisional trade row evidence-only and replaces, rather than adds to, the single
position-difference row for that account/venue/instrument. A repeated cut or repeated stable source
key therefore cannot double count.

For a complete cut, known executions apply first. Let `Pq` be authoritative signed position quantity
and `Kq` recomputed known quantity at that cut. The checked difference is `Dq = Pq - Kq`.
Unattributed signed face notional is zero when `Dq == 0`; otherwise it is the sign of `Dq` applied
after away-from-zero conversion of `abs(Dq) * M` at policy scale. The known partition-independent
allocation is deliberately not subtracted from a direct position conversion because their valid
rounding paths can differ.

The authoritative row also carries `Pn`, the direct signed contract-face conversion of `Pq`; it is
validated independently against the configured multiplier. It does not overwrite known inventory
or define the difference notional. `Dq == 0` removes the prior numeric difference only when the cut
and live fence cover it completely; nonzero `Dq` retains `UnexplainedPosition`. Missing,
malformed, unsupported, contradictory, overflowing, or over-envelope normalized facts become
`Unquantifiable` metadata with their complete bounded semantic identity and keep every numeric risk
decision for that account unavailable while submissions remain blocked.

Complete open-order batches update existing unknown rows by stable exchange key and may resolve an
absent row only through the explicit recovery decision in ADR-0012. Raw unknown facts remain in
audit even after a later complete position cut. Unknown activity is never automatically adopted,
retried, cancelled, or flattened. Only a known locally owned order may receive a cancel request.

### Account correctness states

M4 assigns:

| Value | State | Submission consequence |
|---:|---|---|
| 1 | `Synchronized` | ordinary M3 submissions may proceed if every other gate passes |
| 2 | `ReconciliationRequired` | reject new exposure-increasing submissions; known-order cancel remains available |
| 3 | `Quarantined` | reject new submissions; retain unknown/conflicting facts; known-order cancel only |

These are data-correctness states, not M5 `Normal`, `ReduceOnly`, or `Halted` risk modes. M4's
limit/GTC vocabulary has no accepted reduce-only semantic, so no ordinary new order is treated as
safe while the account is not synchronized.

All routes using an affected logical account inherit the submission block. Peer accounts and peer
firms remain unaffected unless their own evidence requires a transition.

The M3 submission result appends `AccountReconciliationRequired = 58` and
`AccountQuarantined = 59` without changing any earlier value. Both are local rejections at
`SubmissionStage::Risk`. The account gate runs after a unique local `OrderId` is generated and
before reservation arithmetic, preserving route, canonical validation, identity, risk/reservation,
OMS, encoding, and initiation order. `AEGISSTS` schema one accepts the appended reason values but
changes no historical record bytes or golden. A transition to known `SubmissionUnknown` records the
account safety cause before control can make a later risk decision.

`AccountSafetyReason` has these stable assignments:

| Value | Reason |
|---:|---|
| 1 | `SubmissionUnknown` |
| 2 | `TimeoutObserved` |
| 3 | `DisconnectObserved` |
| 4 | `IncompleteReconciliation` |
| 5 | `RecoveryGap` |
| 6 | `UnknownOrder` |
| 7 | `UnknownTrade` |
| 8 | `UnexplainedPosition` |
| 9 | `PermissionMismatch` |
| 10 | `MarginModeMismatch` |
| 11 | `OutOfScopeInstrument` |
| 12 | `CorrelationConflict` |
| 13 | `EventIdentityConflict` |
| 14 | `TradeIdentityConflict` |
| 15 | `AuthoritativeContradiction` |
| 16 | `CriticalAdmissionLoss` |
| 17 | `EvidenceCapacityExhausted` |
| 18 | `ArithmeticOrStateCapacityFailure` |
| 19 | `ProvenanceMismatch` |

Reasons 1 through 5 deterministically enter `ReconciliationRequired` from `Synchronized`. Reasons 6
through 19 deterministically enter `Quarantined` from either lower state. `Quarantined` never
downgrades because a later lower-severity reason arrives. The first overall reason/provenance and,
when escalation occurs, the first quarantine reason/provenance remain immutable canonical fields;
later unique reasons append to a bounded ordered reason set. Exact duplicates are idempotent.

Incomplete reconciliation or catch-up maps to reason 4; a journal, snapshot, replay, or durability
gap maps to reason 5. Critical admission loss maps to 16. General evidence exhaustion uses the
preallocated fence and maps to 17, so it always enters `Quarantined` rather than contradicting the
event contract. Arithmetic/state capacity failure after admission maps to 18. No safety state or
reason clears except the explicit complete recovery decision in ADR-0012.

ADR-0012 defines the complete authoritative evidence and explicit decision required to leave these
states. Local silence, a reconnect, or open-order absence alone never clears them.

### Critical private admission

M4 extends, rather than replaces, ADR-0006's exact `AdmissionOrdinal` and attempt semantics. One
global non-wrapping attempt ordinal is assigned to every public, private, and reconciliation attempt,
including `Accepted`, `CapacityExceeded`, and `Closed`; successful copies additionally use the same
global receive sequence and injected receive timestamp. Private and reconciliation commands have
separate fixed-capacity FIFO reserves named by ADR-0013. Public traffic cannot consume either
reserve, and neither reserve silently spills into another queue.

The owner selects the lowest attempt ordinal across the public head, private head, reconciliation
head, public-source fences, and account-safety fences. A fence is a first-class owner turn with no
receive sequence. This preserves the accepted FIFO/fence order rather than creating a second M4
ordinal.

Every configured logical account owns one preallocated safety-fence slot. If a private fact cannot
be accepted:

1. the source receives an explicit non-acceptance result and retains the unconsumed fact;
2. the account fence records the event identity, complete bounded receive-time-free semantic value,
   attempted admission ordinal, and `CriticalAdmissionLoss`;
3. later exposure-increasing submissions for that account fail closed; and
4. the account becomes `Quarantined` and authoritative reconciliation is required before normal
   processing resumes.

If the fence is already active, its checked loss count increments and its earliest ordinal/value
remain canonical. Loss-counter exhaustion fails the runtime closed. An unconfigured or
unattributable failed input cannot fence an arbitrary account; its source receives failure and the
runtime globally refuses later private consumption until restart/reconciliation resolves the source.

`CopiedAndAdmitted` acknowledges only that the immutable value owns a queue slot. `EconomicallyConsumed`
is returned only after the owner commits one `Applied`, duplicate, projection-only, safety-contained,
or forbidden disposition with evidence. If an admitted value cannot preflight domain or evidence
capacity, its complete value remains in the owner/fence recovery state and the source receives
`RetainedForReconciliation`, never a false consumption acknowledgement. No private execution or
reconciliation fact is silently dropped.

A producer-side fence is visible only through the ingress coordinator's closed API. An older active
owner turn may finish according to its lower ordinal; the merge consumes the fence before any later
turn for that account. Consequently no later callback can reach the account submission gate before
the fence state is owner-local. M10 owns sustained priority, fairness, and backlog qualification;
M4 proves bounded correctness and explicit failure.

### Atomic reconciliation plan

One immutable plan contains:

- event and correlation disposition;
- OMS before/after projection;
- reservation before/after projection and exact closure cause;
- inventory deltas and all seven-scope replacement cells;
- unknown exposure and account-safety changes;
- event/trade/map index additions;
- exact journal, audit, diagnostic, and callback reservations.

Plan construction is side-effect free. It uses fixed scratch storage and checked arithmetic. The
owner commits only after every capacity and evidence preflight succeeds. The no-fail commit first
publishes the prepared typed recovery input reserved by the plan, then applies OMS, then residual
reservation/exposure and inventory, then account safety and audit. The prepared journal record does
not claim those later phases already happened; replay reclassifies it idempotently. Nothing outside
the serialized owner can observe an intermediate write.

Re-entry, wrong-owner access, missing provenance, stale metadata, inconsistent policy, arithmetic
failure, or capacity exhaustion is detected before business mutation. Callback failure occurs after
commit and cannot undo the economic state.

### Inventory and audit evidence

Every inventory transition and unknown-exposure observation records the originating normalized
event or reconciliation row, order/exchange/trade identities, complete known attribution,
configuration and organization provenance, metadata and risk-policy provenance, signed before/delta/
after values, all affected scope keys, reservation conversion, account-safety state, and audit
ordinal in the separate M4 audit schema.

ADR-0013 owns every capacity, policy fingerprint, validation relationship, and semantic evidence
record. ADR-0014 must fix canonical evidence bytes before an emitter exists. There is no default
permissive capacity and no overwriting ring for safety-critical evidence.

## Consequences

The inventory decision provides the following exposure guarantees and requires conservative
treatment of incomplete attribution.

- A fill cannot disappear between reservation and confirmed inventory.
- Cumulative fill partitioning cannot change final approved face notional.
- All seven known-order risk scopes reconcile from one confirmed source of truth.
- Partial and overlapping replacement exposure remain conservative.
- Unknown ownership is represented explicitly rather than guessed.
- Capacity failure blocks the affected account and preserves an explicit recovery obligation.
- Execution price remains available for evidence without prematurely introducing P&L semantics.

## Deferred

M5 owns dynamic hierarchical limits, authority epochs, freshness, market readiness, and risk modes.
M7 owns authenticated account facts and live private observation. M8 owns venue-native cancel and
transmission behavior. M9 owns durable inventory/journal/audit storage and venue-backed recovery.
M10 owns load/backlog qualification, M11 the operator quarantine-resolution workflow, and M12 P&L,
fees, settlement, and reporting. Cross-currency and cross-firm aggregation require later decisions.
