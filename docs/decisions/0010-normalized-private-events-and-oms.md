# ADR-0010: Normalized Private Events and Extended OMS

> **Purpose:** Fix M4 private-order event identities, correlation, fill normalization, idempotency,
> cancellation, extended OMS transitions, and update-before-callback ordering.

- **Status:** Accepted
- **Date:** 2026-08-23
- **Scope:** M4 private-order vocabulary, exchange lifecycle, correlation, cancellation, and callbacks
- **Related:** [Outbound OMS and conservative fake initiation](0009-outbound-oms-and-fake-initiation.md),
  [canonical bot-bound submission and fixed risk](0008-canonical-submission-and-fixed-risk.md),
  [serialized data-plane execution](0001-serialized-data-plane-execution.md),
  [bounded deterministic runtime](0006-bounded-deterministic-runtime.md), and
  [M4 capacity policy and canonical evidence](0013-m4-policy-and-canonical-evidence.md)

## Context

M3 stops after local fake initiation. Its `WriteInitiated` state is not an exchange acknowledgement,
and `SubmissionUnknown` deliberately retains exposure because exchange acceptance may have
occurred. M4 must process later acknowledgements, rejections, executions, cancellations, timeouts,
disconnects, duplicates, and valid reordering without allowing any inbound fact to bypass the OMS.

Private facts may be repeated after reconnect, delivered out of order, or conflict with an earlier
identity. A fill can precede an acknowledgement. Cancellation initiation proves neither that a
venue received the cancel nor that the original order stopped trading. The event model therefore
must separate authoritative venue facts from local observations, correlate only through exact
identities, and make every accepted economic transition idempotent.

M4 remains venue-neutral and credential-free. It does not select a Deribit private protocol,
authenticated account mapping, venue-native order ID encoding, or live cancellation primitive.

## Decision

### M3 compatibility boundary

M4 preserves `PendingEncoding = 1`, `PendingInitiation = 2`, `WriteInitiated = 3`,
`SubmissionUnknown = 4`, and `LocallyFailed = 5`. It does not renumber or reinterpret those values
inside any M1-M3 schema-one artifact. `SubmissionUnknown` is terminal only for the local M3 submit
operation; M4 may later reconcile it with authoritative venue facts.

The M3 request vocabulary remains Buy/Sell, Limit, GTC, exact price, exact quantity, and no optional
flags. M4 private events cannot introduce a second order vocabulary or modify the request economics
that risk approved and the OMS retained.

Existing `AEGISTRS`, `AEGISRTS`, `AEGISSTS`, `AEGISFOE`, `AEGISRSP`, and `AEGISSUP` schema-one
bytes remain compatibility anchors. M4 uses separate versioned event and audit schemas.

M4 appends these stable `DomainErrorCode` values without changing earlier assignments:

| Value | Error |
|---:|---|
| 900 | `InvalidPrivateIdentity` |
| 901 | `InvalidPrivateEvent` |
| 902 | `PrivateEventConflict` |
| 903 | `PrivateEventCapacityExceeded` |
| 904 | `PrivateCorrelationFailed` |
| 905 | `InvalidPrivateOmsState` |
| 906 | `PrivateCounterExhausted` |

One-event classification uses this failure precedence: owner/capability/re-entry gate; normalized
shape and assigned values; provenance/account/venue consistency; exact event duplicate/conflict;
exact trade duplicate/conflict; correlation; OMS transition; checked reservation/inventory plan;
domain and evidence capacity; commit. Exact duplicate disposition precedes ordinary OMS capacity,
just as M3 duplicate order identity precedes OMS capacity. A safety conflict follows its accepted
quarantine plan rather than returning an economic partial failure.

### Bounded private identities

The existing 24-byte `OrderId` remains AEGIS's canonical local/client identity. M4 adds separate
nominal identities rather than aliases or strings with interchangeable meaning:

| Identity | Authority | Contract |
|---|---|---|
| `ExchangeOrderId` | authoritative fake venue | one through 128 opaque bytes |
| `TradeId` | authoritative fake venue | one through 128 opaque bytes |
| `PrivateEventId` | authoritative private source | one through 128 opaque bytes |
| `PrivateSourceEpochId` | private source/adapter | one through 128 opaque bytes |
| `AuthoritativeCutId` | reconciliation source | one through 128 opaque bytes |
| `LocalOrderEventId` | AEGIS | exactly 24 bytes: 16-byte restart namespace plus nonzero, non-wrapping unsigned 64-bit counter |
| `CancelAttemptId` | AEGIS | exactly 56 bytes: complete 24-byte `RuntimeEpochId`, complete 24-byte `OrderId`, and one nonzero, non-wrapping unsigned 64-bit cancel ordinal |

Opaque bytes are compared and canonically length-prefixed but not parsed as integers, UUIDs,
Deribit values, or authenticated account identifiers. Embedded zero bytes are data rather than
terminators. Empty and over-bound values reject. M7 owns authenticated logical-to-native account
mapping; M8 owns venue-native client-ID encoding.

Cancel ordinals are monotonic per `(RuntimeEpochId, OrderId)` during one runtime incarnation. A pair
begins at one; exhaustion rejects before a request event and never wraps. Recovery retains every
surviving unresolved old attempt byte-for-byte, but a new runtime uses its fresh, durability-fenced
ADR-0012 `RuntimeEpochId` and may begin its new per-order ordinal at one. The epoch component makes
the complete identity different even when a published-but-unacknowledged pre-crash ordinal is lost;
therefore no synchronous cancel-ordinal durability fence is required and a `CancelAttemptId` is
never reused within one recovery lineage.

A venue-originated event key is:

```text
(VenueId, LogicalAccountId, PrivateSourceEpochId, PrivateEventId)
```

A trade key is independently:

```text
(VenueId, LogicalAccountId, TradeId)
```

The source epoch scopes ordinary message identity across private-session generations. The trade key
does not include that epoch, so replaying the same execution after reconnect cannot apply it again.
Reconciliation rows additionally retain their authoritative cut and canonical row ordinal; they
still pass through the same order/trade deduplication indexes.

`PrivateEventOrigin` assigns `Local = 1`, `Venue = 2`, and `Reconciliation = 3`. A local event uses
one `LocalOrderEventId`; a venue event uses the complete venue event key; a reconciliation row uses
its `AuthoritativeCutId` plus a one-based canonical row ordinal. Exactly one origin-specific identity
shape must be populated. Replayed journal records preserve the original identity rather than minting
a new one.

### Smallest normalized event vocabulary

`PrivateOrderEventKind` has these assigned values:

| Value | Kind | Authority and meaning |
|---:|---|---|
| 1 | `ExchangeAcknowledged` | authoritative acceptance/correlation fact |
| 2 | `ExchangeRejected` | authoritative definitive rejection fact |
| 3 | `Execution` | one uniquely identified incremental fill with a cumulative endpoint |
| 4 | `CancelRequested` | authorized local request for one retained order |
| 5 | `CancelWriteOutcome` | local fake cancel-initiation boundary result |
| 6 | `CancellationResult` | authoritative cancelled or cancel-rejected result |
| 7 | `LocalFailure` | local submission fact with explicit acceptance certainty |
| 8 | `TimeoutObserved` | local lack-of-response observation |
| 9 | `DisconnectObserved` | local private-source loss observation |

Zero and every unassigned value reject. Authoritative reconciliation is a separate complete-batch
contract rather than a tenth ordinary event kind.

`CancelWriteOutcome` assigns `DefiniteFailureBeforeAcceptance = 1`,
`AcceptedAndInitiated = 2`, and `AcceptedThenOutcomeLost = 3`. `CancellationResult` assigns
`Cancelled = 1` and `CancelRejected = 2`. `LocalFailureCertainty` assigns
`ProvenBeforeAcceptance = 1` and `AcceptanceCouldHaveOccurred = 2`.

`PrivateEventSubjectScope` assigns `Order = 1`, `Account = 2`, and `PrivateSource = 3`. The closed
origin envelope is:

| Origin | Required identity | Forbidden identity | Required scope/time |
|---|---|---|---|
| `Local` | `LocalOrderEventId` | venue event key and reconciliation row key | exact subject scope, local source time, and receive time |
| `Venue` | venue event key | local event and reconciliation row keys | order scope, source time, and receive time |
| `Reconciliation` | `ReconciliationEpochId`, `AuthoritativeCutId`, and one-based canonical row ordinal | local and venue event keys | row-declared order/account scope, authoritative cut time, and receive time |

Every origin carries logical account, venue, and the complete applicable `M4Provenance` fixed by
ADR-0013. A known-order event requires local order, firm, desk, bot, strategy, route, instrument,
and metadata provenance copied from the retained OMS row. An account/source observation requires
the configured firm/account/venue but forbids desk, bot, strategy, route, and instrument. An
unknown authoritative subject retains firm only when sealed configuration independently proves the
account's firm; it retains instrument and metadata only when independently supported; local order,
bot, desk, strategy, and route remain absent. Reconciliation does not manufacture a missing field.

The closed kind-specific payload shapes are:

| Kind | Required semantic fields | Forbidden semantic fields |
|---|---|---|
| `ExchangeAcknowledged` | venue or reconciliation origin; `ExchangeOrderId`; optional syntactically valid local `OrderId` locator | trade, fill, cancel-attempt, local-failure and observation payloads |
| `ExchangeRejected` | venue or reconciliation origin; at least one syntactically valid local or exchange locator; assigned rejection category and zero-through-256 opaque detail bytes | trade, fill, cancel-attempt, cancellation and observation payloads |
| `Execution` | venue or reconciliation origin; known or unknown order locator; `TradeId`; incremental/cumulative quantity; execution price; side present only for an unknown locator | cancel-attempt, local-failure and observation payloads |
| `CancelRequested` | local origin; known local `OrderId`; `CancelAttemptId` | exchange event, trade, fill, result and observation payloads |
| `CancelWriteOutcome` | local origin; known local `OrderId`; matching `CancelAttemptId`; assigned outcome | trade, fill, venue terminal and observation payloads |
| `CancellationResult` | venue or reconciliation origin; known or unknown order locator; assigned result; terminal cumulative quantity when result is `Cancelled` | trade execution, local-failure and observation payloads |
| `LocalFailure` | local origin; local `OrderId`, submission attempt and assigned certainty | trade, fill, cancel and observation payloads |
| `TimeoutObserved` | local origin; order or account scope and its exact locator | exchange result, trade, fill and cancel payloads |
| `DisconnectObserved` | local origin; private-source scope; account and affected source epoch | order economics, exchange result, trade, fill and cancel payloads |

Fields not named for a shape are absent rather than populated with zero or sentinel values. For a
known local order, cancelled terminal cumulative may equal the already applied fill and cannot be
below it or above original approved quantity. Unknown-terminal validation follows the retained
authoritative unknown-open-order rule below. A cancel rejection has no terminal cumulative
authority. Bounded reason detail is retained as opaque evidence and never used to infer ownership.

`ExchangeRejectionCategory : u16` assigns `Unspecified = 1`, `InvalidOrder = 2`,
`InsufficientAuthority = 3`, `InsufficientFunds = 4`, `PostOnlyWouldCross = 5`, and
`VenueRiskRejected = 6`. Zero and unassigned categories reject. Rejection detail is zero through
256 opaque bytes; a larger value rejects before admission.

For a known execution, side is forbidden and is derived only from retained local OMS ownership. For
an unknown execution, authoritative side is optional: Buy or Sell makes the provisional unknown
trade quantifiable, while absence deterministically creates `Unquantifiable`. A supplied unknown
side is evidence only and never proves local ownership. M7 must prove any native side normalization.

The semantic digest covers every origin-specific identity, subject, source time, provenance field,
locator, result, reason, and economic value. Its only excluded observation fields are receive time,
admission ordinal, audit ordinal, journal sequence, and diagnostic linkage. A difference in any
other field is a conflict rather than an exact duplicate. Field-by-field equality of the complete
bounded typed value is normative. ADR-0014 must define the digest's exact canonical-byte preimage
before digest storage or encoding exists; earlier semantic implementation compares the full value.

### Exact correlation

Correlation uses complete identity equality and this precedence:

1. When local and exchange identities are both present, both must resolve to the same OMS row.
2. A known local `OrderId` may establish the first exchange-order mapping only when its retained
   account and venue exactly match the event source.
3. An already bound `ExchangeOrderId` may resolve its one local OMS row.
4. One local order cannot bind two exchange identities.
5. One exchange identity cannot bind two local orders.
6. Side, price, quantity, instrument, timestamps, and visual similarity are never ownership proof.
7. A syntactically valid but unknown client identity remains unknown.

An execution before acknowledgement is valid when these rules uniquely identify a retained local
order. It may establish the exchange-order mapping without setting the acknowledgement flag.

An unknown or conflicting event is not silently adopted, retried, cancelled, or flattened. It is
handled through the account-safety contract in ADR-0011.

### Event and trade idempotency

The reconciler checks the event key before the OMS transition:

- the same key and semantic digest is an exact event duplicate;
- the same key with a different digest is an event-identity conflict.

It checks an execution's trade key independently:

- the same trade key and identical order/economics digest is an exact trade duplicate;
- the same trade key with different correlation or economics is a trade-identity conflict.

An exact duplicate records one explicit duplicate disposition but causes no OMS, reservation,
inventory, exposure, or callback change. A conflicting duplicate performs one fully preflighted
account-safety transition with no economic mutation. Receive-time differences alone do not turn an
otherwise identical duplicate into a conflict.

Retained event/trade identity history is bounded and cannot be discarded merely because an order
became terminal. Capacity exhaustion fails closed before an event is accepted as economically
applied. Reclaiming active order capacity never permits a late duplicate to apply again.

### Canonical execution representation

A normalized `Execution` always requires:

- one `TradeId`;
- positive exact incremental quantity;
- positive exact cumulative quantity after this trade;
- positive exact execution price;
- exact account, venue, instrument, and origin provenance fields.

Common structural validation requires checked nonnegative `cumulative - incremental` and nominal
fixed-point values within their domain scale bounds. Correlation then selects exactly one branch:

- **Known local order:** quantity scale, step, approved-quantity bound, price scale/tick, metadata
  revision, and Buy/Sell limit guard are checked against the retained OMS admission. Failure is a
  safety-contained contradiction.
- **Unknown supported instrument:** independently configured account/venue/instrument metadata must
  match the event revision; quantity step and price tick are checked against that metadata. No local
  limit, local approved quantity, or local cumulative baseline is consulted. The positive
  incremental interval may be retained as unattributed exposure. Side absence makes it
  `Unquantifiable`.
- **Unknown unsupported or missing metadata:** when sealed configuration cannot prove that the
  account/venue/instrument tuple belongs to the supported scope, retain `Unquantifiable` with
  `OutOfScopeInstrument`. Once that tuple is proved supported, absent metadata, a different metadata
  revision, or a value that violates the proved scale/step/tick contract retains `Unquantifiable`
  with `ProvenanceMismatch`. Scope membership is tested first, so a combined unsupported-scope and
  metadata defect deterministically selects `OutOfScopeInstrument`. No numeric economics are
  guessed.

For an unknown cancellation terminal cumulative, an earlier retained authoritative unknown-open-
order row with the same account/venue/exchange key may supply its original-quantity bound. Without
that row, the cancellation remains retained `Unquantifiable`; it never borrows a local OMS baseline.
Execution price is retained in OMS/audit only and does not replace ADR-0011 contract-face math.

The execution owns the half-open cumulative interval:

```text
[cumulative_after - incremental_quantity, cumulative_after)
```

The following contiguous/gap rules apply only after exact correlation proves a known local OMS row.
An unknown execution never enters a local order's pending-gap set; ADR-0011 retains its separate
unattributed projection or unquantifiable fact.

An interval whose start equals the currently applied cumulative quantity is contiguous and may
apply. An interval whose start is greater is retained in a bounded pending-gap set. Event and trade
keys are committed when the gap is buffered with `BufferedGap`; no economics or callback occurs.
When the missing prefix arrives, all newly contiguous intervals drain in ascending cumulative
endpoint order. Each buffered registry row changes exactly once to `AppliedFromBuffer`, and audit
retains both its earlier buffer disposition and later application disposition. One callback is
planned per newly applied execution in the same ascending order, but the complete drain batch,
economics, and audit commit before the first callback. Distinct overlapping intervals, a cumulative
decrease, zero or negative values, overfill, competing intervals with the same start, or
inconsistent provenance are conflicts.

A retained authoritative terminal target bounds the pending set: every existing and new interval
must end at or below that target. Any interval beyond it is an authoritative contradiction. Reaching
a target below the original quantity produces `Cancelled`; reaching a target equal to the original
quantity produces `Filled`.

The interval notation is an ordering device; exact endpoint equality uses the fixed-point quantity
contract. A full fill ends exactly at the original approved quantity.

An adapter receiving native incremental facts may produce a cumulative endpoint only from
authoritative sequence/order state. It cannot sum arrival order after a gap. An adapter receiving
only a cumulative update may derive an increment only when a unique trade identity and preceding
cumulative point are known. Otherwise it requests reconciliation. M4 fakes provide both values
explicitly; M7 must prove a native normalization method later.

### Extended OMS projection

M4 appends these primary order states:

| Value | State | Meaning |
|---:|---|---|
| 6 | `Working` | acknowledged or otherwise authoritatively active with zero fills |
| 7 | `PartiallyFilled` | positive cumulative fill below approved quantity |
| 8 | `Filled` | cumulative fill equals approved quantity |
| 9 | `ExchangeRejected` | authoritative pre-fill rejection |
| 10 | `Cancelled` | authoritative terminal cancellation with all fills reconciled |
| 11 | `ReconciledAbsent` | a complete authoritative negative proves no remaining live order |

The retained row also owns orthogonal fields for acknowledgement, exchange correlation, cumulative
fill, pending intervals, cancellation, authoritative terminal cumulative quantity, and
reconciliation requirement. Connectivity and cancel progress do not replace the economic state.

`CancellationState` assigns `Unassigned = 0`, `None = 1`, `Requested = 2`,
`WriteInitiated = 3`, `OutcomeUnknown = 4`, `DefinitelyFailed = 5`, `Rejected = 6`, and
`Confirmed = 7`.

### Exhaustive transition partition

The literal state sets are:

```text
PreInitiation = {PendingEncoding, PendingInitiation}
OpenVenueRisk = {WriteInitiated, SubmissionUnknown, Working, PartiallyFilled}
TerminalVenue = {Filled, ExchangeRejected, Cancelled, ReconciledAbsent}
LocalTerminal = {LocallyFailed}
AllStates = PreInitiation union OpenVenueRisk union TerminalVenue union LocalTerminal
```

Exact event and trade duplicates are classified before the tables and leave every economic,
projection, safety, and callback value unchanged. The first table is the complete accepted
economic/projection partition:

| Source state | Stimulus and complete guard | Target/effect |
|---|---|---|
| `WriteInitiated`, `SubmissionUnknown` | consistent acknowledgement | `Working`; bind exchange ID; reservation unchanged |
| `Working`, `PartiallyFilled`, `Filled`, `Cancelled` | consistent late acknowledgement | primary unchanged; acknowledgement/correlation only |
| `WriteInitiated`, `SubmissionUnknown` | rejection with no acknowledgement, no applied fill, no retained pending interval/trade, and no execution-established exchange mapping | `ExchangeRejected`; release residual once |
| `ExchangeRejected` | consistent late rejection under a distinct event identity | projection-only; no economics |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | contiguous execution ends below original quantity and no terminal target is reached | `PartiallyFilled`; convert cumulative delta atomically |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | contiguous execution reaches original quantity | `Filled`; consume residual by fill |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | valid noncontiguous execution within original quantity and terminal target | retain gap; primary/economics unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authorized cancel request and no unresolved attempt | primary unchanged; append attempt; `Requested` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | matching unresolved cancel-write outcome | primary unchanged; close that attempt as `DefinitelyFailed`, `WriteInitiated`, or `OutcomeUnknown` |
| `Filled`, `ExchangeRejected`, `Cancelled`, `ReconciledAbsent` | matching late cancel-write outcome for an attempt that was unresolved when the terminal fact committed | consume attempt outcome/evidence; primary and economics unchanged; `Cancelled` keeps `Confirmed` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authoritative cancel rejection, with or without a local attempt | primary/economics unchanged; close the sole unresolved attempt as `Rejected` when present |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authoritative cancelled cumulative equals applied cumulative below original quantity | `Cancelled`; release residual once; `Confirmed` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authoritative cancelled cumulative is above applied, at or below original, and bounds every pending interval | retain target; set reconciliation required; release nothing |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | a later execution drain reaches retained target below original quantity | `Cancelled`; release residual once |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | a later execution drain reaches retained target equal to original quantity | `Filled`; consume residual by fill |
| `Filled` | late cancelled result at original quantity | primary/economics unchanged; set `Confirmed` and close the sole unresolved attempt as confirmed when present |
| `Filled` | late cancel rejection | primary/economics unchanged; close the sole unresolved attempt as `Rejected` when present, otherwise leave prior cancellation history unchanged |
| `Cancelled` | repeated cancelled result at the same target | projection-only; retain `Confirmed`; primary/economics unchanged |
| `Cancelled` | late cancel rejection | projection-only; `Confirmed` dominates and is not downgraded; primary/economics unchanged |
| `LocallyFailed` | local failure with `ProvenBeforeAcceptance` matching retained M3 evidence | projection-only; no release reapplication |
| `SubmissionUnknown` | local failure with `AcceptanceCouldHaveOccurred` matching retained M3 evidence | projection-only; exposure retained |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | order-scoped timeout | primary/economics unchanged; set reconciliation required |
| `Filled`, `ExchangeRejected`, `Cancelled`, `ReconciledAbsent` | order-scoped timeout | stale projection-only evidence; safety/economics unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | complete-negative recovery action satisfying ADR-0012 | `ReconciledAbsent`; release residual once |
| `ReconciledAbsent` | same complete-negative proof replay | projection-only; no economics |

An authoritative cancellation may be unsolicited for a known order; a local cancel attempt is not
required. A `CancelWriteOutcome` always requires the exact `CancelAttemptId` and an unresolved
matching attempt. `PendingEncoding` and `PendingInitiation` local-failure changes remain inherited
M3 direct OMS operations inside the synchronous submit turn; they are not normalized later-turn
transitions.

Account/source observations form a separate complete accepted table because they do not have one
source OMS state:

| Stimulus | Bounded account-wide effect |
|---|---|
| account timeout | mark every account row in `OpenVenueRisk` reconciliation-required in `OrderId` byte order |
| source disconnect | validate the affected epoch from the event, then mark every account row in `OpenVenueRisk` reconciliation-required in `OrderId` byte order |

Each account-wide plan preflights the complete row/audit/callback fan-out. It commits all affected
rows and account safety before one callback per affected known order in the same canonical order.
Terminal and local-terminal rows receive no projection change.

The complete safety-only accepted partition is:

| Input | Safety effect |
|---|---|
| shape-valid authoritative unknown acknowledgement, rejection, or cancellation | retain the event; add `UnknownOrder`; quarantine; no known-order economics |
| shape-valid authoritative unknown execution | retain the event; add `UnknownTrade` first and `UnknownOrder` second when no retained authoritative unknown-order row has the same exchange key; quarantine; no known-order economics |
| authoritative correlation, event-identity, trade-identity, or provenance conflict | retain conflict digest; apply its exact ADR-0011 quarantine reason; no economics |
| shape-valid authoritative acknowledgement, rejection, execution, or cancellation result for a known row that is not accepted by the economic/projection table | retain keys and fact as `SafetyContained`; apply `AuthoritativeContradiction`; no order economics |

The last row includes every authoritative fact against `PreInitiation` or `LocallyFailed`, execution
after `ExchangeRejected` or `ReconciledAbsent`, any new execution after `Filled` or completed
`Cancelled`, rejection after any execution evidence, terminal target below applied cumulative,
overfill, and terminal-target violation. It makes the authoritative remainder finite and testable.

Every remaining shape-valid order-scoped local source/state combination is forbidden and returns the stable
error without OMS, reservation, inventory, mapping, dedupe, safety, or callback mutation. Invalid
shape fails earlier. These three partitions—economic/projection, safety-only, and forbidden local
remainder—cover every shape-valid order-scoped `AllStates × PrivateOrderEventKind` combination
exactly once; the separate account/source table exhausts the other two scopes.

### Cancel request result

`CancelRequestDisposition : u8` assigns `LocallyRejected = 1`, `WriteInitiated = 2`, and
`SubmissionUnknown = 3`. `CancelRequestStage : u8` assigns `Context = 1`, `Authority = 2`,
`Evidence = 3`, `Oms = 4`, `Initiation = 5`, and `Internal = 6`.

`CancelRequestReason : u16` assigns `None = 0`, `ContextInactive = 1`, `WrongOwner = 2`,
`CancelReentry = 3`, `EvidenceCapacityExceeded = 4`, `UnknownOrder = 10`,
`OrderNotOwned = 11`, `OrderTerminal = 12`, `OutstandingCancelAttempt = 13`,
`InvalidCancelState = 14`, `FakeWriteDefinitelyFailed = 20`, `FakeWriteOutcomeUnknown = 21`, and
`InternalFailure = 30`. The order context, ownership, re-entry, evidence, OMS, and fake-initiation
checks use that precedence.

A pre-OMS local cancel rejection never admits `CancelRequested`. Once context, authority, evidence,
and OMS checks accept, `CancelRequested` and its attempt commit before fake initiation. The exact
returned triples are:

| Fake outcome | Result triple | Committed evidence |
|---|---|---|
| definite failure before fake acceptance | `LocallyRejected / Initiation / FakeWriteDefinitelyFailed` | request plus matching `DefiniteFailureBeforeAcceptance` outcome |
| accepted and initiated | `WriteInitiated / Initiation / None` | request plus matching `AcceptedAndInitiated` outcome |
| accepted then outcome lost | `SubmissionUnknown / Initiation / FakeWriteOutcomeUnknown` | request plus matching `AcceptedThenOutcomeLost` outcome |

A known locally owned open order may be cancelled while its account is reconciliation-required or
quarantined. A new explicit attempt is allowed only after the preceding attempt is
`DefinitelyFailed` or `Rejected`; timeout, disconnect, write initiation, or outcome loss never
schedules an automatic retry.

### Replace is cancel plus new

M4 defines no replace API, replacement identity, or venue replace event. A strategy expresses the
operation as a cancel request for the original `OrderId` plus an ordinary new M3 submission with a
new `OrderId`. The original reservation remains until a definitive terminal fact or complete
authoritative proof releases it. Both reservations therefore coexist while both orders may be live.

### Joint classify, plan, and commit

The private-event owner first validates, correlates, deduplicates, and builds an immutable bounded
transition plan. It then performs all checked arithmetic and preflights OMS, mapping, pending-fill,
reservation, inventory, audit, journal, diagnostic, and callback capacity. The complete prepared
`PrivateEventInput` journal record and its sequence are part of that plan. Only after every fallible
step succeeds may one no-fail owner-local commit publish that prepared input, then update OMS,
reservation, exposure, inventory, account safety, and audit.

Within the commit, the OMS transition is applied before its reservation and inventory effects. No
other owner turn can observe the intermediate writes. A failure before commit changes no business
state. A safety conflict commits only the fully preflighted quarantine/audit plan and no economics.

### Bot order-event observation

The strategy contract gains a compatibility-preserving default no-op order-event method; it does
not add a new pure virtual requirement to every M1-M3 strategy. Order callbacks use an order-specific
cause context and never invent a market-data `SubscriptionId`.

All newly contiguous facts in one owner turn commit before the first resulting callback. Every
accepted known-owned `Applied`, `ProjectionOnly`, or `SafetyContained` input emits one callback;
an execution drain emits one for each newly applied execution in ascending cumulative endpoint
order. Each notification names its event/trade and applied endpoint when present; every context
query sees the same final batch-committed owner state. Exact duplicates, newly buffered gaps,
forbidden local requests, complete-negative recovery actions, and unowned external events do not
invoke an order callback. Account/source observations use their explicit canonical per-order fan-out.
Historical callbacks are not redispatched during recovery; ADR-0012 defines the recovery-state
notification.

The existing global callback ordinal, wrong-owner checks, and re-entry protection apply. Callback
failure occurs after state commit, cannot roll it back, and produces bounded fault evidence.

### Critical admission and canonical evidence

An event is accepted only after its complete bounded value has been copied into private critical
admission storage. Ordinary public work cannot consume the private reserve. Capacity rejection is
explicit, leaves the source responsible for the unconsumed fact, and activates the preallocated
account safety fence defined by ADR-0011.

ADR-0013 fixes every stable semantic disposition and M4 capacity. ADR-0014 must fix the exact
`AEGISPEV`/`AEGISOAS` bytes before their encoders exist. Evidence saturation preserves the accepted
prefix, prevents unsafe economic mutation, and latches the affected account `Quarantined` with
`EvidenceCapacityExhausted` in its dedicated fence. Every M4-owned source, test, scenario, build
declaration, and fake path belongs to the fail-closed forbidden-capability manifest.

## Consequences

- Acknowledgement affects correlation and lifecycle but not economic reservation.
- Fill-before-ack is safe when exact identity proves ownership.
- Repeated events and trades cannot double-apply economics or callbacks.
- Cancellation, timeout, disconnect, and local write completion cannot release exposure.
- Out-of-order fills are buffered without arrival-order guessing.
- Bot code observes complete OMS, reservation, exposure, and inventory state.
- M1-M3 source and evidence compatibility remains intact.

## Deferred

M5 owns dynamic risk modes and market-readiness policy. M7 owns authenticated private streams,
native event normalization, and authoritative account mapping. M8 owns venue-native order/cancel
encoding, transmission, sequencing, rate limits, arming, and any later native replace decision. M9
owns durable storage and venue-backed recovery. P&L, fees, and settlement remain M12. Additional
order types, time-in-force values, flags, collars, and contract models require later ADRs.
