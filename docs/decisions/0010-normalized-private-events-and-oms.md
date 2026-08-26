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
ingress shape and assigned values; exact event duplicate/conflict over the correlation-independent
ingress semantic value; independently provable provenance/account/venue consistency; correlation;
exact trade duplicate/conflict over the first-admission resolved order/economics value; OMS
transition; checked reservation/inventory plan; domain and evidence capacity; commit. Event lookup
before owner-provenance consistency is safe because an exact duplicate never mutates economics and
reuses the retained first-admission resolution. It also gives a first-seen provenance failure one
stable replay identity. Exact duplicate disposition precedes ordinary OMS capacity, just as M3
duplicate order identity precedes OMS capacity. A safety conflict follows its accepted quarantine
plan rather than returning an economic partial failure.

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

An ordinary producer supplies a `PrivateOrderIngressAttempt`: one complete bounded source fact whose
typed `PrivateIngressOriginValue` contains the source identity and source time but cannot represent
local receive time or executor admission. The trusted runtime receipt boundary later attaches its
own observed receive timestamp to form `NormalizedPrivateOrderInput`; the attempt contributes no
receive timestamp to that decision. Compatibility normalization APIs may attach an explicitly
supplied receive timestamp for value-level tests and pre-admission callers, but the returned
normalized value grants neither executor admission nor consumption authority. Reconciliation
remains a separate trusted normalization path and exposes no producer-facing reconciliation attempt
API.

`PrivateEventSubjectScope` assigns `Order = 1`, `Account = 2`, and `PrivateSource = 3`. The closed
origin envelope is:

| Origin | Required identity | Forbidden identity | Required scope/time |
|---|---|---|---|
| `Local` | `LocalOrderEventId` | venue event key and reconciliation row key | exact subject scope, local source time, and receive time |
| `Venue` | venue event key | local event and reconciliation row keys | order scope, source time, and receive time |
| `Reconciliation` | `ReconciliationEpochId`, `AuthoritativeCutId`, and one-based canonical row ordinal | local and venue event keys | row-declared order/account scope, authoritative cut time, and receive time |

`PrivateEventResolutionKind : u8` assigns `Known = 1`, `Unknown = 2`, `Conflict = 3`, and
`NotOrderScoped = 4`. The immutable first-admission resolution is a closed tagged value: `Known`
carries the exact local `OrderId` and complete retained-order subject; `Unknown` carries no extra
subject because the source event already owns the maximal independently proved subject; and
`Conflict` carries exactly one assigned pre-trade `AccountSafetyReason`: `ProvenanceMismatch` or
`CorrelationConflict`. No other account-safety reason is a resolution payload. `NotOrderScoped`
carries no extra subject and is required only for an account-scoped timeout or private-source
disconnect, whose deterministic account-wide projection is not one-order correlation. A conflict
stops before trade lookup and economic mutation. Zero and unassigned resolution values reject.

Every origin carries logical account, venue, and the complete applicable `M4Provenance` fixed by
ADR-0013. Provenance in the normalized ingress value is correlation-independent and origin-based.
A local known-order event copies the complete retained OMS subject because AEGIS minted that event
from the row. A local account/source observation carries configured firm/account/venue and forbids
desk, bot, strategy, route, and instrument. A venue or reconciliation input retains only the maximal
provenance independently proved from its raw account/venue and sealed configuration: configured
firm when proved and supported instrument/metadata when proved. It never gains desk, bot, strategy,
route, or local ownership from a supplied local locator or the current exchange mapping.

After duplicate classification, correlation produces a separate resolved subject projection for
the transition plan, OMS/reservation/inventory effects, audit record, and callback. A known local
order resolution copies every subject field from the retained OMS row. An unknown authoritative
resolution remains limited to the normalized source subject. Correlation never rewrites the stored
normalized event or either duplicate registry. Reconciliation does not manufacture a missing field.

The closed kind-specific payload shapes are:

| Kind | Required semantic fields | Forbidden semantic fields |
|---|---|---|
| `ExchangeAcknowledged` | venue or reconciliation origin; `ExchangeOrderId`; optional syntactically valid local `OrderId` locator | trade, fill, cancel-attempt, local-failure and observation payloads |
| `ExchangeRejected` | venue or reconciliation origin; at least one syntactically valid local or exchange locator; assigned rejection category and zero-through-256 opaque detail bytes | trade, fill, cancel-attempt, cancellation and observation payloads |
| `Execution` | venue or reconciliation origin; at least one raw local/exchange locator; `TradeId`; raw instrument and claimed metadata revision; incremental/cumulative quantity; execution price; source side optional for venue ingress and required for reconciliation ingress | cancel-attempt, local-failure and observation payloads |
| `CancelRequested` | local origin; known local `OrderId`; `CancelAttemptId` | exchange event, trade, fill, result and observation payloads |
| `CancelWriteOutcome` | local origin; known local `OrderId`; matching `CancelAttemptId`; assigned outcome | trade, fill, venue terminal and observation payloads |
| `CancellationResult` | venue or reconciliation origin; at least one raw local/exchange locator; assigned result; terminal cumulative quantity when result is `Cancelled`; optional causal `CancelAttemptId` only for a venue-origin `CancelRejected` whose producer retained exact request/response evidence | trade execution, local-failure and observation payloads; causal cancel-attempt identity on `Cancelled` or reconciliation origin |
| `LocalFailure` | local origin; local `OrderId`, submission attempt and assigned certainty | trade, fill, cancel and observation payloads |
| `TimeoutObserved` | local origin; order or account scope and its exact locator | exchange result, trade, fill and cancel payloads |
| `DisconnectObserved` | local origin; private-source scope; account and affected source epoch | order economics, exchange result, trade, fill and cancel payloads |

Fields not named for a shape are absent rather than populated with zero or sentinel values. For a
known local order, cancelled terminal cumulative may equal the already applied fill and cannot be
below it or above original approved quantity. Unknown-terminal validation follows the retained
authoritative unknown-open-order rule below. A cancel rejection has no terminal cumulative
authority. A venue `CancelRejected` may carry one causal `CancelAttemptId` only when its trusted
producer retained the exact request/response binding before transmission or received a lossless
authenticated echo. It never infers the identity from timing, the sole unresolved attempt, an order
locator, or an exchange mapping. The identity is correlation evidence rather than ownership
authority: the owner first resolves the ordinary order locator, then requires the embedded `OrderId`
and retained attempt to match exactly. Reconciliation-origin results and `Cancelled` omit it.
`Cancelled` terminal authority may supersede and close the sole unresolved local attempt without
causal attribution because it ends the order. Bounded reason detail is retained as opaque evidence
and never used to infer ownership.

`ExchangeRejectionCategory : u16` assigns `Unspecified = 1`, `InvalidOrder = 2`,
`InsufficientAuthority = 3`, `InsufficientFunds = 4`, `PostOnlyWouldCross = 5`, and
`VenueRiskRejected = 6`. Zero and unassigned categories reject. Rejection detail is zero through
256 opaque bytes; a larger value rejects before admission.

An affected `PrivateSourceEpochId` is a valid nonempty typed identity only. M4 assigns no
current/stale or order-ownership meaning to its opaque bytes, and replay classification remains keyed
by the complete event identity. Adapter-specific epoch lifecycle and freshness semantics remain M6/M7
integration work.

Source-side presence is fixed before correlation: it is optional on venue ingress and required on a
reconciliation execution row. After correlation, a known execution derives its economic side only
from retained local OMS ownership. Any supplied source side must equal that retained side and is
then omitted from the resolved transition projection; a mismatch is a safety conflict. An unknown
execution retains a supplied authoritative Buy or Sell as evidence, making provisional unknown
trade economics quantifiable, while absent venue side deterministically creates `Unquantifiable`.
A supplied side never proves local ownership. M7 must prove any native side normalization.

The immutable ingress semantic value covers every origin-specific identity, independently proved
subject field, source time, root provenance field, raw locator, result, reason, and economic value.
Its only excluded observation fields are receive time, admission ordinal, audit ordinal, journal
sequence, and diagnostic linkage. The event-identity registry stores this complete typed ingress
value together with its complete immutable tagged first-admission resolution before any later
exchange mapping can affect attribution. The same event key with a field-for-field equal ingress
semantic value is an exact duplicate; the same key with any other semantic field changed is a
conflict. For a venue cancel rejection, causal-attempt absence, presence, and all identity bytes are
part of that comparison. A replay found in
this registry reuses the retained original ingress and resolution for comparison, emits a new
`ExactEventDuplicate` disposition, and never re-runs correlation, adopts ownership, applies
economics, or rewrites the resolved subject merely because a mapping now exists.

For a first-seen event key, correlation derives the separate resolved order and subject used by the
trade registry, OMS, transition plan, callback, and audit projection. An execution's trade semantic
value is one closed tuple containing instrument and claimed metadata revision, incremental and
cumulative quantity, execution price, canonical economic side, and one resolution tag. The tag is
either `Known(OrderId)` or `Unknown(raw local locator presence/value, raw exchange locator
presence/value)`. Known canonical side comes from the retained OMS row after any supplied source
side has been checked for equality; unknown canonical side is the supplied side or typed absence. A
known source-side mismatch is safety-contained as `AuthoritativeContradiction` before trade lookup.
It retains the first-seen `Known` event resolution but publishes no candidate mapping. Account,
venue, and trade ID belong to the `TradeKey`, not the value. Ordinary event identity, source time,
root/source provenance, receive time, evidence ordinals, and known source-side presence are
excluded.

The same trade key and equal complete tuple is an exact trade duplicate; any included field or
resolution difference is a trade conflict. A formerly unknown trade that appears under a distinct
event identity after a mapping change therefore conflicts rather than being silently adopted or
economically reapplied. ADR-0014 must define exact digest preimages before digest storage or
encoding exists; earlier semantic implementation compares the complete bounded typed values field
by field.

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

Every local row resolved by either locator must have retained `LogicalAccountId` and `VenueId`
exactly equal to the normalized source. This guard applies before every row in the table below; a
source mismatch is `Conflict(CorrelationConflict)` with no candidate mapping or trade lookup. It
prevents a globally valid `OrderId` or a corrupted exchange mapping from crossing account, venue,
or firm ownership.

The complete raw-locator truth table is finite. `Bound same` means the exchange key already maps to
the local row named by the local locator; `bound other` means it maps to a different retained row.
An unbound exchange locator is not itself a contradiction, and two unproved locators cannot prove
each other:

| Local locator | Exchange locator | Resolution |
|---|---|---|
| absent | bound | `Known` using the permanent mapping |
| absent | unbound | `Unknown` |
| known | absent | `Known` |
| unknown | absent | `Unknown` |
| known | bound same | `Known` |
| known, no existing reverse mapping | unbound | `Known` plus a candidate mapping |
| known, already reverse-bound to another exchange key | unbound | `Conflict(CorrelationConflict)` |
| known | bound other | `Conflict(CorrelationConflict)` |
| unknown | bound | `Conflict(CorrelationConflict)` |
| unknown | unbound | `Unknown` |

The empty-locator combination is rejected by shape validation before this table. A candidate mapping
is only a prepared effect: an exact event/trade duplicate, source-side mismatch, trade conflict,
forbidden local transition, or any later safety-only classification suppresses it. Only a fully
accepted first-seen economic/projection transition may publish the candidate mapping.

An execution before acknowledgement is valid when these rules uniquely identify a retained local
order. It may establish the exchange-order mapping without setting the acknowledgement flag.

An unknown or conflicting event is not silently adopted, retried, cancelled, or flattened. It is
handled through the account-safety contract in ADR-0011.

### Event and trade idempotency

The reconciler checks the event key before correlation and the OMS transition:

- the same key and correlation-independent ingress semantic value is an exact event duplicate;
- the same key with a different ingress semantic value is an event-identity conflict.

This lookup also precedes owner-provenance consistency. A first-seen normalized input whose root,
account, venue, or independently proved source subject conflicts with the bound owner is retained as
`SafetyContained` with `Conflict(ProvenanceMismatch)`. Its field-for-field identical replay is
`ExactEventDuplicate`; the same key with any changed ingress-semantic field is
`EventIdentityConflict`.

Only a first-seen event reaches correlation and an execution's independent trade check:

- the same trade key and identical closed execution/resolution tuple is an exact trade duplicate;
- the same trade key with any different included economic or resolution field is a trade-identity
  conflict.

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
- one raw `InstrumentId` and claimed `InstrumentMetadataRevision`, retained independently from any
  configured/proved source-subject metadata;
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

`execution_evidence_observed` is a derived monotonic OMS cache: it is true exactly when the retained
event registry contains any `Execution` row whose immutable first-admission resolution is `Known`
for that `OrderId`. `Applied`, `BufferedGap`, a known execution that reaches trade classification
before becoming `SafetyContained`, and a known source-side mismatch all set it. A retained known
trade row therefore implies the cache, but is not required for it. Provenance failure and
correlation conflict stop before a `Known` resolution and leave it false. Recovery validates the
cache against the event registry. This exact meaning supplies the rejection guard's “no execution
evidence” predicate without mistaking a zero applied cumulative quantity or a pre-trade safety
disposition for absence of execution evidence.

`exchange_mapping_established_by_execution` is a derived monotonic OMS cache. It becomes true only
when a known `Execution` first commits the order's immutable exchange-key mapping; it stays false
when acknowledgement, cancellation, or reconciliation established that mapping before an
execution. Once true it never clears, including after a late acknowledgement. True requires both a
retained exchange order identity and `execution_evidence_observed == true`. Recovery validates the
cache against the retained mapping-creation cause and event evidence.

`CancellationState` assigns `Unassigned = 0`, `None = 1`, `Requested = 2`,
`WriteInitiated = 3`, `OutcomeUnknown = 4`, `DefinitelyFailed = 5`, `Rejected = 6`, and
`Confirmed = 7`.

The per-order reconciliation-required flag represents unresolved live-order uncertainty rather than
the independently latched account-safety state. `SubmissionUnknown`, an order/account timeout, a
source disconnect, or an authoritative cancellation target above the applied cumulative quantity
sets it. Acknowledgement, partial fill, cancel request/write outcome, cancel rejection, and other
nonterminal facts do not clear it. It clears only when the primary state reaches `Filled`, an
accepted pre-fill `ExchangeRejected`, a fully reconciled `Cancelled`, or `ReconciledAbsent`.
ADR-0011 account safety remains latched until ADR-0012's explicit recovery decision even when this
one order no longer has live uncertainty.

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

Every contiguous-execution guard uses the final post-drain cumulative quantity: the owner first
applies the incoming contiguous interval virtually, then drains every newly contiguous retained
interval in ascending endpoint order. Original-quantity and terminal-target comparisons use that
final candidate, never an intermediate prefix. A noncontiguous incoming interval starts no drain
and is evaluated only by the retained-gap row.

| Source state | Stimulus and complete guard | Target/effect |
|---|---|---|
| `WriteInitiated`, `SubmissionUnknown` | consistent acknowledgement | `Working`; bind exchange ID; reservation unchanged |
| `Working`, `PartiallyFilled`, `Filled`, `Cancelled` | consistent late acknowledgement | primary unchanged; acknowledgement/correlation only |
| `WriteInitiated`, `SubmissionUnknown` | rejection with no acknowledgement, `execution_evidence_observed == false` (no known execution evidence), and no retained authoritative terminal cumulative quantity | `ExchangeRejected`; release residual once |
| `ExchangeRejected` | consistent late rejection under a distinct event identity | projection-only; no economics |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | contiguous execution leaves the final post-drain cumulative below original quantity and, when retained, below the authoritative terminal target | `PartiallyFilled`; convert the incoming and every drained cumulative delta atomically |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | no authoritative terminal target is retained and a contiguous execution makes the final post-drain cumulative reach original quantity | convert the incoming and every drained cumulative delta atomically; `Filled`; consume the reservation fully by fill |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | valid noncontiguous execution endpoint is at or below original quantity and, when present, the authoritative terminal target | retain gap; primary/economics unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authorized cancel request; cancellation state is `None`, `DefinitelyFailed`, or `Rejected`; neither an unresolved attempt nor an authoritative terminal target is retained | primary unchanged; append attempt; `Requested` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | first matching cancel-write outcome for the exact `Requested` attempt, with no retained write outcome | primary/economics unchanged; `DefiniteFailureBeforeAcceptance` closes the attempt as `DefinitelyFailed`; `AcceptedAndInitiated` and `AcceptedThenOutcomeLost` retain it unresolved as `WriteInitiated` and `OutcomeUnknown` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | first matching late `AcceptedAndInitiated` or `AcceptedThenOutcomeLost` cancel-write outcome for an exact attempt already closed as `Rejected` by matching causal evidence before any write outcome | retain the write-outcome evidence only on that exact closed attempt; primary/economics, any newer attempt, and the current order-level cancellation state remain unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | first matching late cancel-write outcome for an exact attempt already closed non-causally as `Confirmed` by a terminal cancellation before any write outcome | retain the write-outcome evidence only on that exact closed attempt; primary/economics, any newer attempt, and the current order-level cancellation state remain unchanged |
| `Filled`, `ExchangeRejected`, `Cancelled`, `ReconciledAbsent` | first matching late `AcceptedAndInitiated` or `AcceptedThenOutcomeLost` cancel-write outcome for an exact attempt with no retained write outcome that is closed as `Rejected` when the outcome arrives | retain the write-outcome evidence only on that exact closed attempt; primary/economics and the current order-level cancellation state remain unchanged |
| `Filled`, `Cancelled` | first matching late cancel-write outcome for an exact attempt with no retained write outcome that is closed non-causally as `Confirmed` when the outcome arrives | retain the write-outcome evidence only on that exact closed attempt; primary/economics and the current order-level cancellation state remain unchanged |
| `Filled`, `ExchangeRejected`, `Cancelled`, `ReconciledAbsent` | first matching late cancel-write outcome for the exact attempt with no retained write outcome that remains current and `Requested` when the outcome arrives | retain the first write outcome/evidence; apply the `DefinitelyFailed`, `WriteInitiated`, or `OutcomeUnknown` mapping from the ordinary first-outcome row; primary/economics unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | venue cancel rejection whose causal attempt identity exactly matches the current unresolved attempt and resolved known `OrderId`, with no retained authoritative terminal target | close only that exact attempt as `Rejected`; set current cancellation state to `Rejected`; primary/economics unchanged; a later explicit retry becomes eligible only after this atomic close |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | venue cancel rejection with no retained authoritative terminal target whose causal identity names a retained attempt already closed as `Rejected`, whether or not a newer attempt is current | retain rejection history only on the named closed attempt; primary/economics, any newer attempt, and current cancellation state remain unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | cancel rejection with no causal attempt identity, no retained authoritative terminal target, and no unresolved local attempt | primary/economics unchanged; set order-level cancellation state to `Rejected` |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | cancel rejection with no causal attempt identity and no retained authoritative terminal target while a local attempt is unresolved | projection-only; retain the uncorrelated rejection as history; primary/economics and current cancellation state remain unchanged; the unresolved attempt continues to make a new explicit cancel ineligible |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | cancel rejection after a retained cancellation target set `Confirmed`, with no causal identity or a causal identity naming a retained attempt already closed as `Rejected` or non-causally as `Confirmed` | projection-only; primary/economics unchanged; `Confirmed` dominates and is not downgraded |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authoritative cancelled cumulative equals applied cumulative below original quantity, no authoritative terminal cumulative quantity is retained, and the target bounds every pending interval | retain the authoritative terminal cumulative quantity; `Cancelled`; release residual once; set `Confirmed`; close the sole unresolved attempt as confirmed when present |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | authoritative cancelled cumulative is above applied, at or below original, bounds every pending interval, and either establishes the first terminal target or exactly equals the retained target | retain target; set reconciliation required; release nothing; set `Confirmed`; close the sole unresolved attempt as confirmed when present |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | a contiguous execution, including any newly unlocked pending-interval drain, makes the final post-drain cumulative reach the retained target below original quantity | convert the incoming and every drained cumulative delta atomically; `Cancelled`; release only the post-conversion residual once |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | a contiguous execution, including any newly unlocked pending-interval drain, makes the final post-drain cumulative reach the retained target equal to original quantity | convert the incoming and every drained cumulative delta atomically; `Filled`; consume the reservation fully by fill |
| `Filled` | late cancelled result at original quantity | retain the authoritative terminal cumulative quantity; primary/economics unchanged; set `Confirmed` and close the sole unresolved attempt as confirmed when present |
| `Filled`, `ExchangeRejected`, `ReconciledAbsent` | late cancel rejection whose causal identity exactly matches the sole unresolved attempt | close only that exact attempt as `Rejected`; set current cancellation state to `Rejected`; primary/economics unchanged; terminal primary state still forbids another cancel |
| `Filled`, `ExchangeRejected`, `ReconciledAbsent` | late cancel rejection with no causal identity, with a causal identity naming a retained attempt already closed as `Rejected`, or, for `Filled`, one closed non-causally as `Confirmed` | retain rejection history only; primary/economics, every unresolved or newer attempt, and current cancellation state remain unchanged |
| `Cancelled` | repeated cancelled result at the same target | projection-only; retain `Confirmed`; primary/economics unchanged |
| `Cancelled` | late cancel rejection with no causal identity or a causal identity naming a retained attempt already closed as `Rejected` or non-causally as `Confirmed` | projection-only; `Confirmed` dominates and is not downgraded; primary/economics unchanged |
| `LocallyFailed` | local failure with `ProvenBeforeAcceptance` matching retained M3 evidence | projection-only; no release reapplication |
| `SubmissionUnknown` | local failure with `AcceptanceCouldHaveOccurred` matching retained M3 evidence | projection-only; exposure retained |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | order-scoped timeout | primary/economics unchanged; set reconciliation required |
| `Filled`, `ExchangeRejected`, `Cancelled`, `ReconciledAbsent` | order-scoped timeout | stale projection-only evidence; safety/economics unchanged |
| `WriteInitiated`, `SubmissionUnknown`, `Working`, `PartiallyFilled` | complete-negative recovery action satisfying ADR-0012 with no retained authoritative terminal cumulative target | `ReconciledAbsent`; release residual once |
| `ReconciledAbsent` | same complete-negative proof replay | projection-only; no economics |

An authoritative cancellation may be unsolicited for a known order; a local cancel attempt is not
required. An exact causal identity closes only the matching current unresolved attempt and never a
newer one. An absent identity cannot close any attempt. A causal identity whose embedded order
disagrees with the resolved row, names no retained attempt, or names an attempt closed as
`DefinitelyFailed` is a safety-contained correlation contradiction. An identity for any retained
attempt already closed as `Rejected` retains history only. An identity for an attempt closed
non-causally as `Confirmed` by a terminal cancellation also retains history because the later exact
rejection can coexist with that order-level terminal fact. With no identity and no unresolved
attempt, an order-level rejection may still set `Rejected`; while an attempt is unresolved, the
uncorrelated rejection cannot change it, the current cancellation state, or retry eligibility. A
terminal `Cancelled` result may close the sole unresolved attempt as `Confirmed` because it
authoritatively ends the order rather than attributing a rejection to one request. A
`CancelWriteOutcome` always requires the exact `CancelAttemptId`, no retained write outcome for that
attempt, and one of two states when the outcome arrives: the attempt remains current and `Requested`,
or authoritative evidence has already closed it as `Rejected` or non-causally as `Confirmed`. The
first case applies the ordinary outcome mapping. The second retains history only, but a causally
Rejected attempt accepts only a late outcome proving that the request reached the adapter; a later
`DefiniteFailureBeforeAcceptance` contradicts that causal rejection and belongs to the forbidden
local remainder. A non-causally Confirmed attempt accepts every assigned first write outcome. Only
that first write outcome is accepted, and a late outcome for an older attempt never changes a newer
attempt.
`PendingEncoding` and `PendingInitiation` local-failure changes remain inherited M3 direct OMS
operations inside the synchronous submit turn; they are not normalized later-turn transitions.

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
| event-identity or provenance conflict from any origin, or authoritative correlation or trade-identity conflict | retain conflict value; apply its exact ADR-0011 quarantine reason; no economics |
| shape-valid authoritative acknowledgement, rejection, execution, or cancellation result for a known row that is not accepted by the economic/projection table | retain keys and fact as `SafetyContained`; apply `AuthoritativeContradiction`; no order economics |

The last row includes every authoritative fact against `PreInitiation` or `LocallyFailed`, execution
after `ExchangeRejected` or `ReconciledAbsent`, any new execution after `Filled` or completed
`Cancelled`, exchange rejection after any execution evidence or retained authoritative terminal
cumulative quantity, terminal target below applied cumulative, overfill, a second different cancellation
target, a causal cancel-attempt identity that is unknown, belongs to another order, or contradicts a
retained `DefinitelyFailed` attempt, and any other terminal-target violation. It makes the
authoritative remainder finite and testable.

A safety-contained first-seen fact retains its event identity, immutable first-admission
resolution, and, for an execution that reaches trade classification, its trade identity. It never
commits a candidate new exchange-order mapping; an already committed mapping remains unchanged.
Event-identity conflict retains the existing event-registry row, and trade-identity conflict retains
the existing trade-registry row. The contradictory fact remains explicit evidence without becoming
new correlation authority. Its complete prepared journal input carries the retained first-admission
resolution only as replay-comparison evidence; the event-identity-conflict branch, not that
resolution, selects containment and cannot select an order mutation or callback.

Reusing a local `LocalOrderEventId` with changed ingress semantics is the same
`SafetyContained`/`EventIdentityConflict` fault as changing an authoritative event key; it is not
the forbidden local state remainder. It changes no economics or mapping and emits no order callback
because the conflicting local input cannot select a trusted originating order. A field-for-field
equal local replay remains `ExactEventDuplicate` and likewise emits no callback.

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
quarantined. The OMS accepts a new explicit attempt only when cancellation state is `None`,
`DefinitelyFailed`, or `Rejected`, no authoritative terminal target is retained, and no attempt is
unresolved. `Confirmed` never permits another explicit attempt; timeout, disconnect, write
initiation, or outcome loss never schedules an automatic retry.

### Replace is cancel plus new

M4 defines no replace API, replacement identity, or venue replace event. A strategy expresses the
operation as a cancel request for the original `OrderId` plus an ordinary new M3 submission with a
new `OrderId`. The original reservation remains until a definitive terminal fact or complete
authoritative proof releases it. Both reservations therefore coexist while both orders may be live.

### Joint classify, plan, and commit

The private-event owner first validates the ingress value and resolves exact event duplicates or
conflicts. Only a first-seen event is correlated, after which its resolved trade value is checked
for a duplicate or conflict before the owner builds an immutable bounded transition plan. It
performs all checked arithmetic and preflights OMS,
mapping, pending-fill, reservation, inventory, audit, journal, diagnostic, and callback capacity.
The complete prepared `PrivateEventInput` journal record retains the unchanged source-normalized
event and its complete immutable tagged first-admission resolution. The record and its sequence are
part of the plan. Only after every fallible step succeeds may one no-fail owner-local commit publish
that prepared input, then update OMS, reservation, exposure, inventory, account safety, and audit.

Within the commit, the OMS transition is applied before its reservation and inventory effects. No
other owner turn can observe the intermediate writes. A failure before commit changes no business
state. A safety conflict commits only the fully preflighted quarantine/audit plan and no economics.

### Bot order-event observation

The strategy contract gains a compatibility-preserving default no-op order-event method; it does
not add a new pure virtual requirement to every M1-M3 strategy. Order callbacks use an order-specific
cause context and never invent a market-data `SubscriptionId`.

All newly contiguous facts in one owner turn commit before the first resulting callback. Every
accepted newly correlated known-owned `Applied`, `ProjectionOnly`, or `SafetyContained` input emits
one callback, except the pre-correlation event-identity conflict fixed above;
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
