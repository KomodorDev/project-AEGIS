# ADR-0008: Canonical Bot-Bound Submission and Fixed M3 Risk

> **Purpose:** Fix the M3 order vocabulary, route authorization, immutable policy provenance,
> exact exposure economics, validation order, and atomic reservation rules.

- **Status:** Accepted
- **Date:** 2026-08-22
- **Scope:** M3 canonical requests, owner-local routes, fixed risk policy, exposure, and reservations
- **Related:** [Serialized data-plane execution](0001-serialized-data-plane-execution.md),
  [domain value contracts](0004-domain-value-contracts.md),
  [immutable configuration provenance](0005-immutable-configuration-provenance.md),
  [bounded deterministic runtime](0006-bounded-deterministic-runtime.md), and
  [market-state validity](0007-market-state-validity.md)

## Context

M2 ends inside a synchronous strategy callback with coherent market state but no order capability.
M3 must let that callback exercise the complete local submission decision without introducing a
queue, remote risk service, live venue session, or caller-authored organizational identity. A local
result must also say whether the request was certainly rejected before transport acceptance,
successfully handed to the deterministic fake initiator, or left uncertain after acceptance may
have occurred.

The existing M1 configuration already defines explicit bot-owned execution routes and exact
instrument metadata. It also provides a 24-byte namespace-plus-counter `OrderId`. Those accepted
contracts are reused; M3 does not replace them or alter the schema-one `AEGISCFG` compatibility
anchor.

## Decision

### Bot-bound authority and explicit route identity

`BotContext` gains one normalized `submit` operation. Only `BotRuntime` can mint the callback
context, and the context obtains firm, desk, bot, and strategy attribution from the sealed
organization. `OrderRequest` cannot carry any of those identities. It also cannot carry a venue,
account, client order identity, venue-native identifier, credential, endpoint, adapter, session, or
transport handle.

The context is neither copyable nor movable after it carries submission authority. Each bot owns one
persistent context whose lifetime is the enclosing `BotRuntime`; `BotRuntime` activates and
deactivates it around each callback. A post-callback call while the runtime remains alive therefore
has defined `ContextInactive` behavior rather than dereferencing destroyed stack storage. Strategy
code must still treat the reference as callback-borrowed and must not retain it: one reused object
cannot distinguish an old pointer from the current reference after a later activation. Use after
runtime destruction remains ordinary invalid C++ lifetime use.

One private callback token binds the context to the active bot, owner turn, callback ordinal, and
owner thread. The activation and owner-token gates are atomically observable so a wrong thread can
reject before reading or mutating owner-local state; successful submission remains single-owner and
uses no atomic risk fields. Submission outside the active callback, on another thread, or with a
mismatched private activation token rejects before attempt or order identity generation, risk
mutation, OMS mutation, encoding, or fake initiation. Sequential submissions in one callback are
valid. Recursive entry while one submission is active rejects.

Every request names one explicit `RouteId`. At startup, the M3 composition copies the complete
validated route catalog into a canonical `RouteId`-sorted owner-local projection. The projection
retains disabled routes so “missing,” “wrong bot,” and “disabled” remain distinct stable decisions.
Construction requires each route to resolve exactly one metadata record whose venue and normalized
instrument equal the sealed route; missing, duplicate, or mismatched metadata prevents a
submission-capable runtime from being created.
Authorization succeeds only when the route exists, belongs to the active bot, is enabled, and names
the same instrument as the request. The route supplies the venue and logical account. A market-data
subscription is never consulted for trading authority and never creates or enables a route.

The M1 `StrategyMode::ObserveOnly` and accepted reference `AEGISCFG` bytes remain unchanged. M3 adds
a separate submission-runtime capability with assigned byte value `DeterministicFakeOnly = 1`.
This capability permits
only the local fake path described by ADR-0009; it cannot authorize native encoding or live
communication. The existing M1/M2 reference fixture keeps its route disabled. M3 tests derive a
separate enabled-route fixture with separate fingerprints.

One fake-submission composition installs the normalized method on every registered `BotContext`;
it does not maintain a second caller-authored bot allow-list. Effective authority is still narrow:
the active context must resolve an enabled route owned by that exact bot, and that route must resolve
the complete fixed policy. A bot with no enabled route can call the method only to receive a stable
local route rejection.

### Smallest canonical order vocabulary

M3 assigns this closed vocabulary:

| Type | Assigned values | M3 support |
|---|---|---|
| `OrderSide` | `Buy = 1`, `Sell = 2` | Both values |
| `OrderType` | `Limit = 1` | Limit only |
| `TimeInForce` | `GoodTilCancelled = 1` | GTC only |

`OrderRequest` contains exactly `RouteId`, `InstrumentId`, `OrderSide`, `OrderType`,
`TimeInForce`, exact `Price`, and exact `Quantity`. There are no optional venue flags in M3. Market,
stop, trigger, post-only, reduce-only, immediate-or-cancel, fill-or-kill, hidden, iceberg, and
venue-specific fields are deferred.

M3 supports only the accepted inverse-contract metadata combination whose multiplier is quote
currency per contract. Linear-contract exposure is rejected as unsupported because M3 has no
accepted worst-case price collar. The request price is still mandatory and validated exactly even
though inverse face-value risk does not multiply by it.

The complete M3 venue-filter vocabulary is the sealed metadata's maximum price scale and exact tick
plus maximum quantity scale, minimum quantity, and exact quantity step. Canonical fixed-point values
may remove trailing zeroes, so an accepted value requires `value.scale() <= metadata scale`, not
scale equality, followed by exact cross-scale alignment. No maximum quantity, price band, minimum
notional, maximum notional, or venue-native flag exists in accepted metadata, so M3 neither invents
nor silently assumes one. Adding any such filter requires a later revisioned metadata decision.

### Canonical decision order and stable outcomes

The direct path applies this order and stops at the first failure:

1. installed capability, active callback, owner-thread, callback-token, latched submission-runtime,
   and submission re-entry checks;
2. one-based submission-attempt identity generation, then maximum submission-trace and possible
   re-entry evidence preflight;
3. route existence, ownership, enabled state, and request/route instrument agreement, resolving the
   exact venue-and-instrument metadata projection proven complete at runtime construction;
4. assigned side, limit type, and GTC values;
5. positive price, scale not above the metadata maximum, and exact tick alignment;
6. positive quantity, scale not above the metadata maximum, minimum quantity, and exact step
   alignment;
7. supported inverse quote-multiplier economics;
8. local `OrderId` generation;
9. exact exposure calculation, every fixed limit, and atomic reservation using the policy whose
   completeness and provenance were proven at construction;
10. OMS admission;
11. exact fake encoding; and
12. deterministic fake write initiation.

The ordinary return value is `SubmitResult`, not an exception or executor fault. It assigns these
dispositions:

| `SubmitDisposition` | Value | Meaning |
|---|---:|---|
| `LocallyRejected` | 1 | A known local rejection or failure occurred before transport acceptance. |
| `WriteInitiated` | 2 | The local fake accepted the bytes and initiated its asynchronous-write analogue. |
| `SubmissionUnknown` | 3 | Fake acceptance may have occurred, but the local outcome was lost. |

`WriteInitiated` is never named or represented as an exchange acknowledgement. M3 has no exchange
order identity and no acknowledgement state.

Each result carries one assigned stage and reason plus optional one-based `SubmissionAttemptId`, local
`OrderId`, failed risk scope, and risk measure with observed/limit values. `RiskMeasureKind` is
`None = 0`,
`Quantity = 1`, `QuoteNotional = 2`, or `OrderCount = 3`; decimal measures retain signed coefficient
and scale, while counts retain one unsigned integer. Hot-path results carry no free-form text.
`SubmissionReason::None = 0` is valid only for `WriteInitiated`; `SubmissionUnknown` uses
`InitiationOutcomeUnknown`. Non-success outcomes use this closed reason set and assigned values:

| Range | Assigned reasons |
|---|---|
| `1–6` context/evidence | `ContextInactive = 1`, `WrongOwner = 2`, `SubmissionReentry = 3`, `EvidenceCapacityExceeded = 4`, `SubmissionAttemptExhausted = 5`, `SubmissionCapabilityUnavailable = 6` |
| `10–13` route | `RouteNotFound = 10`, `RouteNotOwned = 11`, `RouteDisabled = 12`, `RouteInstrumentMismatch = 13` |
| `20–30` order/economics | `UnsupportedSide = 20`, `UnsupportedOrderType = 21`, `UnsupportedTimeInForce = 22`, `PriceNotPositive = 23`, `PriceScaleExceeded = 24`, `PriceTickMismatch = 25`, `QuantityNotPositive = 26`, `QuantityScaleExceeded = 27`, `QuantityBelowMinimum = 28`, `QuantityStepMismatch = 29`, `UnsupportedContractEconomics = 30` |
| `40` identity | `OrderIdentityExhausted = 40` |
| `50–57` risk | `RiskArithmeticFailure = 50`, `SingleOrderQuantityExceeded = 51`, `SingleOrderNotionalExceeded = 52`, `OpenOrderCountExceeded = 53`, `GrossReservedNotionalExceeded = 54`, `WorstCasePositionQuantityExceeded = 55`, `WorstCasePositionNotionalExceeded = 56`, `ReservationCapacityExceeded = 57` |
| `70–71` OMS | `DuplicateOrderIdentity = 70`, `OmsCapacityExceeded = 71` |
| `80` encoding | `EncodingFailed = 80` |
| `90–91` initiation | `InitiationDefinitelyFailed = 90`, `InitiationOutcomeUnknown = 91` |
| `100` internal | `SubmissionRuntimeFaulted = 100` |

Stages are assigned independently as `Context = 1`, `Evidence = 2`, `Route = 3`,
`CanonicalValidation = 4`, `Identity = 5`, `Policy = 6`, `Risk = 7`, `Oms = 8`,
`Encoding = 9`, `Initiation = 10`, and `Internal = 11`. `Policy` names construction-only evidence
and is not an ordinary result from a valid fake-submission composition. This separates where a
result occurred from why it
occurred and lets future stages add reasons without renumbering earlier evidence.

Optional presence is fixed. A successful or uncertain result requires both attempt and order IDs.
An ordinary local rejection after step 2 requires an attempt ID; capability, inactive, wrong-owner,
attempt-exhaustion, and an already-latched runtime result have none, while a re-entry result carries
the active outer attempt ID. A fault that latches during the current attempt carries every identity
already generated. Any rejection at or after order-ID generation requires that `OrderId`, including
risk rejection.
Risk scope and observed/limit measure are required only for the six limit-exceeded reasons and are
absent for every other reason. `WriteInitiated` has stage `Initiation`, reason `None`, and no risk
fields. `SubmissionUnknown` has stage `Initiation`, reason `InitiationOutcomeUnknown`, and no risk
fields.

Every result also has a noncanonical optional local-path duration in nanoseconds. A dedicated
thread-safe monotonic measurement clock—not the owner-local deterministic event clock—is read as the
first operation inside `BotContext::submit`; a wrong-thread call may read this clock but still
rejects before any owner-local read or mutation. For a local rejection the end is read after all
required rollback and evidence work, immediately before return. For accepted initiation or
post-acceptance uncertainty the end is read at the fake initiator outcome, before later local state
or evidence finalization. Regression or an unrepresentable duration leaves the value absent and
records exactly one `MeasurementUnavailable` diagnostic observation after the call has passed the
owner-valid gates and obtained attempt/evidence authority; it never changes the submission
disposition. A rejection before that point instead leaves duration absent without a diagnostic.
Qualification workloads fail rather than discard a sample with no duration. Durations never enter
canonical policy, order, OMS, fake-order, or trace bytes.

### Immutable policy artifacts and fail-closed startup

M3 adds two independent schema-one canonical artifacts rather than changing M1 or M2 bytes:

- `AEGISRSP` is the immutable risk-policy snapshot. It carries a nonzero `RiskPolicyRevision`, the
  configuration fingerprint, organization and route revisions, every referenced instrument
  metadata revision, the notional decision scale and rounding rule, the complete fixed scope-limit
  table, and its SHA-256 fingerprint.
- `AEGISSUP` is the deterministic submission-runtime policy. It carries
  `DeterministicFakeOnly`, the configuration, M2 runtime-policy, and risk-policy fingerprints,
  fixed capacities for submission attempts, reservations, OMS orders, encoded bytes, accepted fake
  writes, submission trace records, and submission diagnostics, plus the deterministic fake fault
  scripts and its SHA-256 fingerprint.

Both artifacts use one positional canonical profile. The first eight bytes are the displayed ASCII
magic and the next two are unsigned schema version one. Unsigned 16-, 32-, and 64-bit integers are
big-endian; signed 64-bit coefficients use their two's-complement bit pattern in big-endian order;
enums use their stated one- or two-byte width; SHA-256 values are 32 raw bytes; decimals are one
signed 64-bit coefficient followed by one unsigned scale byte; strings are an unsigned 16-bit byte
length followed by exact accepted ASCII; and vectors are an unsigned 32-bit count followed by their
canonical sorted elements. A policy fingerprint is SHA-256 over all its canonical bytes; the
fingerprint itself is not embedded in those bytes.

`RiskLimitKind` assigns `SingleOrderQuantity = 1`, `SingleOrderQuoteNotional = 2`,
`OpenOrderCount = 3`, `GrossReservedQuoteNotional = 4`,
`WorstCasePositionQuantity = 5`, and `WorstCasePositionQuoteNotional = 6` as one-byte values.
`AEGISRSP` then encodes, in order: policy revision; configuration fingerprint; configuration,
organization, and route revisions; notional scale; the existing one-byte
`RoundingMode::AwayFromZero = 2`; a metadata vector sorted by venue then instrument, each containing
venue, instrument, and metadata revision; and the canonical limit-set vector defined below. Each
limit set encodes firm, scope byte, scope-subject identifier, instrument, quote currency, then the
six positive limit values in `RiskLimitKind` order. Open-order count is unsigned 32-bit; the other
five values use the decimal form above.

`AEGISSUP` encodes, in order: `DeterministicFakeOnly` byte; configuration, M2 runtime-policy, and
risk-policy fingerprints; risk-policy revision; unsigned 64-bit maximum submission attempts;
unsigned 32-bit reservation and OMS capacities; unsigned 16-bit encoded-byte capacity; unsigned
32-bit accepted-write, submission-trace-record, and diagnostic capacities; then encoder and
initiator scripts. Each script is its one-byte default action, an unsigned 32-bit override count,
and sorted overrides of unsigned 64-bit invocation ordinal plus one-byte action.

Every capacity and maximum-attempt value is positive. Schema one assigns
`maximum_submission_attempts_supported = 1,000,000`; a larger authored value is invalid. All
capacity-to-byte calculations use checked arithmetic, every value must fit both its encoded width
and `std::size_t`, and inability to complete every startup allocation fails construction with
`InvalidSubmissionPolicy`. The relation
`accepted writes <= OMS orders <= reservations <= maximum submission attempts` is mandatory.
`EncodedFakeOrder::maximum_bytes` is 1,024. Policy construction calculates the exact longest
`AEGISFOE` record possible from every installed route and attribution and requires the encoded-byte
capacity to fall between that maximum and 1,024 inclusive. Thus ordinary `EncodingFailed` is the
explicit scripted outcome, never an undersized-policy accident. Script overrides must be unique,
one-based, and no greater than maximum submission attempts.

Canonical policy collections are sorted by their complete semantic keys; duplicate identities and
duplicate semantic keys reject. Every configured enabled route for a submission-capable bot must
resolve all seven required risk scopes and one metadata revision. Missing, incomplete, zero-valued,
stale, mismatched, unassigned, or arithmetically invalid policy fails construction closed. There is
no default policy and no permissive fallback.

For the immutable M3 startup snapshot, “stale” means that a configuration fingerprint,
organization revision, route revision, instrument metadata revision, or referenced scope identity
does not exactly equal the sealed startup authority. Time-based expiry and later-revision adoption
begin with dynamic policy publication in M5.

The existing observation-only M2 `MarketRuntime::create` remains valid without an M3 policy and
installs no submission authority. Its concrete `BotContext` still has the normalized method for API
compatibility, but every call returns `LocallyRejected` at stage `Context` with
`SubmissionCapabilityUnavailable`, no attempt or order identity, and no M3 trace or diagnostic
mutation. A separate named fake-submission composition requires both accepted M3 artifacts and a
move-only order-ID provider. Thus “missing policy fails closed” does not prevent an explicitly
observation-only runtime from starting; it prevents that runtime from submitting.

### Exact fixed-point exposure economics

All risk decisions use the accepted fixed-point kernel and nominal wrappers. No binary
floating-point value participates. For the supported inverse contract, positive quote face notional
is:

```text
order quantity in contracts × quote-currency contract multiplier
```

`InstrumentMetadata::contract_value` performs that privileged dimensional conversion at the
risk-policy notional scale with `RoundingMode::AwayFromZero`. The explicit rounding mode may
increase the positive decision value by less than one output quantum; invalid scale, overflow, or
another arithmetic failure rejects. Nothing saturates, wraps, truncates toward zero, or changes the
order's price or quantity. The encoder must later preserve the original approved request economics
exactly.

M3 tracks confirmed position as zero because acknowledgements, fills, inventory, and recovery begin
in M4. Let `q` be the order quantity and `n` its positive conservatively rounded quote face
notional. For each applicable scope `s`, instrument `i`, and quote currency `c`, the candidate state
is defined exactly as follows:

```text
open_count'[s] = open_count[s] + 1
gross_reserved_notional'[s,c] = gross_reserved_notional[s,c] + n

reserved_buy_quantity'[s,i]  = reserved_buy_quantity[s,i]  + (side == Buy  ? q : 0)
reserved_sell_quantity'[s,i] = reserved_sell_quantity[s,i] + (side == Sell ? q : 0)
worst_quantity'[s,i] = max(
    abs(confirmed_quantity[i] + reserved_buy_quantity'[s,i]),
    abs(confirmed_quantity[i] - reserved_sell_quantity'[s,i]))

reserved_buy_notional'[s,i,c]  = reserved_buy_notional[s,i,c]  + (side == Buy  ? n : 0)
reserved_sell_notional'[s,i,c] = reserved_sell_notional[s,i,c] + (side == Sell ? n : 0)
instrument_worst_notional'[s,i,c] = max(
    abs(confirmed_notional[i,c] + reserved_buy_notional'[s,i,c]),
    abs(confirmed_notional[i,c] - reserved_sell_notional'[s,i,c]))
worst_notional'[s,c] = worst_notional[s,c]
    - instrument_worst_notional[s,i,c]
    + instrument_worst_notional'[s,i,c]
```

Both confirmed values are exactly zero in M3 but remain explicit in the formula M4 must extend.
Open-order count includes only `Held` reservations, including `WriteInitiated` and
`SubmissionUnknown`; released definite failures are excluded even though their OMS row and
canonical release evidence remain. Gross notional adds buy and sell face values without netting.
Quantity is never
aggregated across instruments. Worst notional is quote-currency-qualified and sums the absolute
per-instrument directional maximum, so one instrument cannot offset another. The stored
`worst_notional[s,c]` is the sum of every current instrument contribution; replacing exactly the
touched instrument contribution makes the incremental formula identical to a full recomputation.
The conservative `n` is rounded once per order before any scope accumulator uses it. Every addition,
subtraction, absolute value, and aggregate sum is checked; overflow rejects rather than understating.

### Fixed limits and multi-firm scope keys

Every order checks the same six limit kinds at each of these assigned scopes, in this stable order:

1. `Bot = 1`
2. `Desk = 2`
3. `Firm = 3`
4. `Account = 4`
5. `Route = 5`
6. `Instrument = 6`
7. `Venue = 7`

The six limit kinds, checked in this order inside each scope, are maximum single-order quantity,
maximum single-order quote face notional, maximum open order count, maximum gross reserved quote
notional, maximum absolute worst-case position quantity, and maximum absolute worst-case position
quote notional. The first two compare `q` and `n`; the remaining four compare the primed formulas
above. All limits are positive and explicit. Equality admits: each comparison succeeds exactly when
`observed <= configured_limit` and rejects exactly when `observed > configured_limit`. A failure at
an earlier scope wins, and the result records that first scope, observed exact value, and configured
exact limit.

Bot, desk, and firm subjects come from `BotContext`. Account, route, venue, and instrument subjects
come from the authorized route and metadata. The complete keys are:

```text
CountKey       = (firm, scope kind, scope subject)
QuantityKey    = (CountKey, instrument)
NotionalKey    = (CountKey, quote currency)
DirectionalNotionalKey = (CountKey, instrument, quote currency)
LimitSetKey    = (CountKey, instrument, quote currency)
```

The scope subject is the corresponding bot, desk, firm, logical account, route, instrument, or venue
identifier. Every key begins with authoritative firm identity; venue and instrument scopes are
therefore firm-qualified. One `LimitSetKey` carries all six limits. Policy validation requires
quantity limits to agree across records with the same `QuantityKey`, notional limits to agree across
records with the same `NotionalKey`, and the open-count limit to agree across records with the same
`CountKey`. Therefore two peer firms never share a bucket, and collection order cannot choose a
limit.

### Atomic check-and-reserve and identity consumption

For each of seven scope projections, the serialized owner resolves five mutable cells: count; gross
notional by `NotionalKey`; buy/sell quantity by `QuantityKey`; buy/sell notional plus its current
instrument-worst contribution by `DirectionalNotionalKey`; and aggregate worst notional by
`NotionalKey`. These are mutable ledger cells, not seven overloaded policy rows. A decision
calculates all 35 candidate cells in fixed scratch storage, then checks all six limits scope-major
and limit-kind-minor in the order above. It next checks held-reservation capacity. Only after every
check succeeds does it commit every cell and one reservation. Capacity exhaustion mutates no cell.
No lock,
remote lookup, database, queue, coroutine, or executor handoff exists inside check-and-reserve; the
single owner supplies its atomicity.

`ReservationId` is a distinct one-based local type whose numeric value equals the already bounded
`SubmissionAttemptId` that created it. One attempt can create at most one reservation, so the
explicit maximum-attempt contract proves uniqueness and prevents wrap without a second ambient
counter. M3 reservation states are `Held = 1` and `Released = 2`. Release validates the reservation,
applies every inverse delta, and changes state exactly once; a second release is an
`InvalidRiskReservationState` invariant error and cannot decrement exposure again.

`AEGISSUP` reservation capacity limits concurrently `Held` records. Reaching it returns
`ReservationCapacityExceeded` with no partial mutation. The ledger preallocates exactly that many
slots. A released slot may be reused only for a different monotonically newer `ReservationId`; a
second release of the old identity cannot match the replacement and remains an
`InvalidRiskReservationState` invariant. Canonical release evidence and the retained
`LocallyFailed` OMS row preserve the historical exact-once proof without an unbounded reservation
tombstone table.

The existing 24-byte `OrderId` is the collision-safe canonical local/client identity. It is generated
after request validation and before risk. The request cannot provide it. The final unsigned counter
value is emitted once and exhaustion rejects before reservation. A risk rejection may consume an
identity, and consumed identities are never reused. OMS independently detects an injected duplicate
or collision before admission.

`SubmissionAttemptId` is also one-based and non-wrapping, but its final valid value is the explicit
`AEGISSUP` maximum rather than ambient process lifetime. It is assigned only after capability,
context, owner, and re-entry gates. That configured final value is emitted once; the next outer call
returns `SubmissionAttemptExhausted` without canonical or owner-local diagnostic mutation. Evidence
capacity rejection consumes its already assigned attempt ID. Encoder and initiator invocation
ordinals are independent one-based counters that advance exactly when their component is reached;
the outer maximum proves neither can wrap.

Ordinary submission outcomes never use `DomainError`. Construction or impossible invariant failures
use the separately assigned persisted codes `InvalidRiskPolicy = 700`,
`InvalidRiskReservationState = 701`, `InvalidSubmissionPolicy = 800`,
`SubmissionEvidenceExhausted = 801`, `InvalidOmsState = 802`, and
`InvalidFakeState = 803`.

## Consequences

- Caller-supplied data cannot forge strategy, bot, desk, firm, account, venue, or client-order
  identity.
- Every supported order has one deterministic validation precedence and exact risk interpretation.
- A configured route is necessary but cannot bypass canonical validation, fixed policy, risk, OMS,
  encoding, or fake initiation.
- All fixed limits are checked and reserved in the same owner turn, so a later callback sees the
  complete prior decision.
- Existing `AEGISCFG`, `AEGISTRS`, `AEGISRTP`, and `AEGISRTS` schema-one vectors remain unchanged.

## Deferred

Market and trigger orders, additional time-in-force values, venue flags, price collars, linear
contract risk, dynamic policy publication, risk modes, market-readiness authorization, fills,
confirmed positions, P&L, cross-currency aggregation, and cross-firm limits are outside M3. Dynamic
risk authority begins in M5; native venue behavior begins in M6–M8.
