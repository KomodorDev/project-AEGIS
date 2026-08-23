# AEGIS Implementation Roadmap

> **Purpose:** Sequence AEGIS delivery by externally meaningful capability gates while preserving
> the accepted safety and ownership constraints at every intermediate milestone.

**Status: Proposed.** This roadmap turns the accepted [architecture](architecture.md) and [serialized data-plane decision](decisions/0001-serialized-data-plane-execution.md) into capability-based delivery milestones. Milestones are completion gates, not calendar estimates. Independent delivery-sequencing and trading-safety reviews have been incorporated.

## Delivery Strategy

The lowest-friction path is to prove one deterministic, fake-backed vertical slice, use public venue data to challenge the normalized contract in parallel, complete and qualify one venue before adding another, and make uncertain outcomes and recovery correct before considering live trading. Each milestone extends an executable behavior instead of building isolated horizontal subsystems.

```text
M0 -> M1 -> M2
M2 -> M3 -> M4 -> M5 --+
M2 -> M6 --------------+-> M7 -> M8 -> M9 -> M10 -> M11 -> M12 -> M13 -> M14
```

The milestone names for that dependency graph are shown in the table below.

M6 is the one intentional parallel track: it may start once M2 stabilizes the normalized market-data contract while M3–M5 complete the fake-backed order, inventory and risk path. The tracks rejoin before any private venue session is allowed.

The ordering deliberately:

- settles a decision only shortly before code depends on it;
- validates domain and lifecycle semantics with deterministic fakes before networking obscures failures;
- gets read-only protocol feedback early without coupling core correctness work to exchange availability;
- stabilizes the venue contract against one real integration before copying it;
- qualifies recovery, load behavior and minimum operations on one venue before multiplying the state space;
- keeps diagnostics early while postponing the full operator plane until its control-plane inputs are stable;
- treats real-money connectivity as a separate authorization gate, not an automatic consequence of a successful build.

## Milestones at a Glance

| Milestone | Shippable outcome | Depends on |
|---|---|---|
| M0 | Repeatable delivery plus bounded product, venue and account assumptions | Accepted architecture |
| M1 | Deterministic domain values and immutable configuration provenance | M0 |
| M2 | A bounded serialized runtime exposes only coherent market state to strategies | M1 |
| M3 | Canonical orders use fixed risk economics and conservative send-outcome semantics | M2 |
| M4 | OMS events reconcile into inventory and survive fake-backed crash/replay scenarios | M3 |
| M5 | Hierarchical policy and readiness are published and enforced without entering the hot path | M4 |
| M6 | One real venue supplies coherent normalized public data with transmission impossible | M2 |
| M7 | A private sandbox account can be observed and reconciled while transmission remains impossible | M4, M5, M6 |
| M8 | Explicitly enabled sandbox orders complete through the proven lifecycle | M7 |
| M9 | Venue-backed restart and reconnect preserve orders, exposure and durable audit evidence | M8 |
| M10 | The single-venue system remains safe under load, backlog and injected faults | M9 |
| M11 | Operators can safely observe, configure, halt, deploy and recover the single-venue system | M10 |
| M12 | The first venue passes a defined shadow/paper qualification period | M11 |
| M13 | A second venue supports coherent cross-venue behavior using proven contracts | M12 |
| M14 | Combined evidence supports a separately authorized, tightly limited live pilot | M13 |

---

## M0 — Delivery Baseline and Operating Assumptions

### Outcome

A fresh checkout can be built, tested and measured consistently, and the team shares one narrow reference scenario plus explicit venue/account operating assumptions.

### For dummies

Prepare the workshop before building the robot: choose the tools, make automatic tests work, choose one practice exchange/account and agree on one tiny demonstration.

### Scope

- Choose and record the supported C++ standard, compiler/platform matrix, build system, dependency approach and test framework.
- Add CI with warnings-as-errors, formatting checks, unit tests and appropriate sanitizer jobs.
- Establish a minimal benchmark harness before optimizing anything.
- Time-box a protocol spike and select the first venue, one instrument family and a sandbox/test environment. The other venue remains out of scope until M13.
- Define the reference scenario used through M1–M12: one firm, one desk, one bot, one instrument, one route and a trivial deterministic strategy.
- State whether the sandbox account is dedicated or shared, its position/margin mode, whether manual or external orders are prohibited, which venue state is authoritative during reconciliation, and how unknown activity will be quarantined.
- Define initial correctness and performance budgets, including submission latency, callback duration, executor queue age and sustained workload.
- Record blocking choices as ADRs; do not create empty subsystem directories.

### Exit Gate

- A clean checkout configures, builds and runs tests with one documented command locally and in CI.
- The reference scenario, first venue and supported environment are explicit.
- Account ownership, permissions, position mode and external-activity assumptions are explicit and testable.
- Correctness and latency measurements have named workloads and units, even if the initial targets are provisional.
- No credential or production endpoint is required by the default build or test path.

---

## M1 — Domain Kernel and Immutable Provenance

**Integration status (2026-08-20):** Implemented on `codex/m1-domain-kernel` and merged into `dev`
by PR #7 as `6287e01be33300e0e7ea24c76dd64a869c67612b`. The reviewed head remained the pinned
`796321825d701f3add83af104b7924eb2826fd07`, and all seven remote checks passed. [M1 exit
evidence](milestones/m1-exit-evidence.md) records the local and remote proof. GitHub records no
approving review, so M1 is integrated but is not claimed formally closed under an approval-based
closure gate.

### Outcome

AEGIS can represent its identities, units, organization, permissions and decision provenance without venue-native types or ambiguous arithmetic.

### For dummies

Give everything a clear name, label and measuring stick. The robot must never confuse a price with an amount, one account with another, or an old rule with the current rule.

### Scope

- Add strong, dependency-light identifiers for firms, desks, bots, venues, accounts, instruments and orders.
- Define fixed-point price, quantity and notional types, including overflow, rounding, tick-size, lot-size and contract-multiplier rules. Floating-point values must not silently enter order or risk decisions.
- Define source, receive and processing timestamps; sequence/session epochs; error/result types; and injected clock and identifier providers. Tests use deterministic providers, while production order identities must remain unique across restart.
- Implement one or more validated firm roots with firm → desk → bot registration and immutable
  organizational attribution; no parent-company model is introduced in M1.
- Model subscriptions separately from execution routes and reject invalid or duplicate configuration.
- Add a validated immutable startup configuration with a stable identity/hash and revisions for instrument metadata, organization, bot/strategy settings, subscriptions and routes. Dynamic adoption remains deferred.
- Add deterministic trace output suitable for scenario comparison; keep richer reporting off the data plane.

### Exit Gate

- Invalid identifiers, instrument metadata, unit conversions and hierarchy references fail deterministically.
- A bot cannot alter its desk/firm identity or infer trading permission from a market-data subscription.
- The same test configuration and ordered inputs produce identical identifiers and traces without weakening production identity uniqueness.
- Configuration and metadata revisions are available to every later admission and audit event.
- Unit tests cover arithmetic boundaries, rounding direction and overflow behavior.

---

## M2 — Deterministic Runtime and Market-State Validity

**Integration status (2026-08-21):** Implemented on `codex/m2-deterministic-runtime` through feature
head `4b5d89e834b45fef30fca87689937770d2c2ab35`, then merged unchanged into `dev` by
[PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8) as
`d7733bb16a52d5ec954338f861d798d8c6620dad`. The executor and market-validity contracts are accepted
in [ADR-0006](decisions/0006-bounded-deterministic-runtime.md) and
[ADR-0007](decisions/0007-market-state-validity.md). The [M2 exit-evidence
file](milestones/m2-exit-evidence.md) preserves the revision-specific local verification and smoke
benchmark evidence separately from the later integration commit; integration does not turn that
smoke timing into controlled-host qualification.

### Outcome

A credential-free recorded fixture travels through normalization and subscription dispatch into the correct strategy callback, but only when its market state is coherent and ready.

### For dummies

Teach the robot to listen to market messages one at a time and build an order-book puzzle in memory. If a piece is missing or stale, it says “not ready” instead of showing a broken book to a trading bot.

### Scope

- Implement the dedicated serialized executor, bounded ingress and a deterministic test driver for run-to-completion turns. Expose queue age/capacity and never hide an admission failure.
- Define the minimum normalized `MarketEvent` contract required by the reference strategy, including source/session identity, source and receive sequence/timestamps, metadata revision, snapshot/delta meaning and book generation where applicable.
- Maintain the current normalized order book in owner-local memory for each active venue/instrument subscription that needs it. Apply a complete snapshot or delta before dispatching the resulting event/read-only view; leave retained depth and historical storage configurable or deferred.
- Define parser/normalizer and fixture-source contracts without selecting final networking primitives.
- Implement subscription matching, bot runtime, strategy callbacks and a bot-bound context.
- Model market state as `Synchronizing`, `Ready`, `Stale` or `Invalid`. A gap, checksum failure or incompatible metadata revision invalidates readiness until a new snapshot establishes a coherent generation.
- Contain malformed or unsupported input and expose bounded structured diagnostics.
- Detect accidental callback re-entry and make blocking or unbounded callback work reviewable and measurable.

### Exit Gate

- A recorded fixture invokes the subscribed bot exactly as expected; unrelated bots receive nothing.
- Malformed input cannot reach a strategy or corrupt the following valid event.
- Duplicate, out-of-order and gapped update policies are deterministic; an invalid book cannot silently become tradable again without resynchronization.
- Strategies can distinguish coherent pricing input from synchronizing, stale or invalid state.
- A strategy never observes a half-applied order-book update.
- Replay of the same ordered input produces the same callback and trace sequence.
- All mutable data-plane state in the slice is owned by the one serialized executor.

---

## M3 — Canonical Submission, Inline Risk and OMS Contract

**Integrated status (2026-08-23):** The complete M3 order, route, fixed-risk,
reservation, outbound OMS, deterministic fake-initiation, result, and evidence contracts are
accepted in [ADR-0008](decisions/0008-canonical-submission-and-fixed-risk.md) and
[ADR-0009](decisions/0009-outbound-oms-and-fake-initiation.md). They were verified at clean producer
`27087d4da423546041295de43e7fa2fb31425b63`, with tree
`2e339a5d97bdc9b6bd4522e754c6052783cb01b4`, from M2 merge baseline
`d7733bb16a52d5ec954338f861d798d8c6620dad`. The [M3 exit-evidence
record](milestones/m3-exit-evidence.md) maps every gate and records local verification plus
uncontrolled smoke timing. [PR #10](https://github.com/KomodorDev/project-AEGIS/pull/10) merged
final feature head `2d5ea9e5b7fc28234789dc7c97ec4fc7bb71ef01`, tree
`f58410737c473bc3992d7cfd7c934d41db1d11cf`, unchanged into `dev` as merge commit
`962eb8602c13c1930a74c59232f96920482edb2b`. Integration status remains separate from the
revision-specific producer evidence and uncontrolled smoke results.

### Outcome

A strategy can submit a canonical order through route authorization, a fixed v1 risk model, OMS admission and fake write initiation while definitive and uncertain transmission outcomes remain distinct.

### For dummies

Before pressing “send,” check that the order is valid, allowed and within its safety allowance. If the robot cannot tell whether an order was sent, it assumes the risky answer—“maybe yes”—until it can check.

### Scope

- Implement the accepted explicit `RouteId` and owner-local route projection; the route selects the
  account, venue and instrument under bot-bound authority.
- Implement the accepted limit-only, GTC, no-flags vocabulary. Canonically validate positive exact
  price/quantity, metadata scale, tick, minimum quantity, lot step, instrument, and inverse-contract
  economics before risk; market orders and price collars are deferred.
- Implement the fixed v1 six-limit model at bot, desk, firm, account, route, instrument, and venue
  scopes using the exact conservative exposure calculation defined by ADR-0008.
- Implement the accepted `OrderRequest`, collision-safe client order identity, `SubmitResult`, and
  stable rejection reasons.
- Implement the accepted outbound OMS/reservation transition matrix. It distinguishes a failure
  proven before fake acceptance from `SubmissionUnknown` after acceptance may have occurred.
- Install an initial immutable risk snapshot and implement owner-local canonical validation, check-and-reserve and minimal outbound OMS states.
- Implement a fake encoder and fake asynchronous transport initiator. Encoding must preserve exactly the economics that risk approved; it may not silently round or reinterpret them.
- Attach configuration, instrument metadata, route/account and risk-policy provenance to each admitted order.
- Instrument the direct submission path for latency and allocation measurements.

### Exit Gate

- Deterministic scenarios cover unauthorized route, invalid order, risk rejection, duplicate local
  identity, OMS non-admission, encoding failure, definite pre-acceptance initiation failure,
  ambiguous post-acceptance `SubmissionUnknown`, and successful local fake initiation.
- A failure proven to occur before transport acceptance creates no reservation or releases it exactly once. An ambiguous post-initiation failure retains conservative exposure in an explicit reconciliation-required state.
- Missing or invalid policy starts fail closed; no default-constructed snapshot can authorize an order.
- Property tests show the fixed risk model never understates worst-case exposure for the supported order vocabulary.
- The strategy → route → risk → OMS → write-initiation path has no blocking I/O, database call, serialization boundary, general-purpose queue, remote call or executor hop.
- The successful local result is never represented as an exchange acknowledgement.
- `BENCH-M3-SUBMIT-001` reports `p50_us`, `p99_us`, `p99_9_us`, `sample_count`,
  `orders_per_second`, and `allocations_per_order`; its provisional controlled-host p99 limit is
  50 microseconds.
- `BENCH-M3-SUBMIT-002` reports `p50_us`, `p99_us`, `p99_9_us`, `sample_count`,
  `rejections_per_second`, and `allocations_per_request`; its provisional controlled-host p99 limit
  is 25 microseconds.
- Each benchmark starts at the bot-bound submit entry and ends at its required local rejection or
  fake initiation endpoint. Network and acknowledgement timing are excluded. Uncontrolled runs are
  labelled smoke and never presented as qualification evidence.

---

## M4 — OMS, Inventory and Recovery Contract

### Outcome

Normalized private-order events produce one coherent OMS, reservation and inventory state before the bot can react, and the same contracts define recovery before any private venue integration begins.

### For dummies

Give the robot a careful notebook for every order and everything it owns. A repeated message must not be counted twice, and after a restart the robot must be able to rebuild the notebook safely.

### Scope

- Implement the conceptual OMS transition matrix for acknowledgements, exchange rejections, fill-before-ack, partial and full fills, cancel requests and results, local failures, timeouts, duplicate trade IDs, unknown orders and valid out-of-order delivery.
- Define client/exchange identifier correlation, cumulative-versus-incremental fill handling and idempotency rules. Treat replace as cancel-plus-new unless a demonstrated venue requirement justifies another model, and reserve for both orders while both may coexist.
- Treat timeouts, disconnects, cancel requests and cancel-write completion as non-terminal. Release exposure only after a definitive terminal venue event or authoritative reconciliation.
- Apply every private event through OMS reconciliation before changing reservations or inventory.
- Attribute fills immediately to bot, desk and firm positions and to every non-organizational exposure scope enforced by risk, including account, route, instrument and venue where applicable.
- Notify the bot only after owner-local OMS, reservation, exposure and inventory updates complete.
- Add the longitudinal reference driver: one injected `SubmitReferenceIntent` causes exactly one
  fixed M3 request on the next Ready callback and never submits it again automatically.
- Decide recovery authority and crash consistency before networking: stable identities across restart, journal/snapshot ordering and durability guarantees, replay/live-catch-up boundary, local-versus-exchange source-of-truth rules and quarantine of unknown/external orders.
- Define audit events now, including order, route/account, configuration, metadata and risk-policy provenance; durable storage arrives in M9.
- Add fake journal/reconciliation adapters and inject a crash at every lifecycle transition.

### Exit Gate

- Table-driven tests exercise every accepted transition and reject every forbidden transition.
- Model/property tests prove total worst-case exposure is never understated through partial fills, rejection, cancellation, replacement, fill-before-ack, duplicates and ambiguous transmission outcomes.
- Position and exposure aggregates reconcile after every event across bot, desk, firm, account, route, instrument and venue scopes used by the v1 risk model.
- A bot callback caused by an order event observes the updated state and cannot double-apply the event.
- The longitudinal reference scenario submits its fixed intent exactly once and does not turn an
  ambiguous result, replay, reconnect, or later callback into an automatic retry.
- Fake-backed restart and live-catch-up scenarios converge at every injected crash point without reusing a client identity or releasing uncertain exposure.
- Unknown or externally created orders cannot be silently attributed; the affected account is conservatively quarantined pending reconciliation.

---

## M5 — Hierarchical Risk and Readiness Control Plane

### Outcome

The control plane can allocate and change hierarchical risk policy while the data plane enforces one coherent, fresh authority epoch/revision and can halt locally when readiness is lost.

### For dummies

Add a safety boss that gives each bot and desk an allowance. If the instructions become missing or too old, the robot stops instead of continuing with a guess.

### Scope

- Allocate the v1 limit vocabulary from M3 across bot, desk, firm and non-organizational scopes such as account, route, instrument and venue where applicable.
- Define hierarchy precedence and exact `Normal`, `ReduceOnly` and `Halted` semantics, including budget cuts below current exposure, cancellation permissions, existing-order treatment and cancel-all failure.
- Publish immutable order, reservation, exposure and inventory observations from the data plane.
- Bound the non-blocking observation handoff now. Define which reporting facts may be coalesced, which may never be silently dropped, and the fail-safe response when capacity is exhausted.
- Aggregate observations and publish complete risk snapshots with an authority epoch/generation plus monotonically increasing revision.
- Adopt newer structurally valid snapshots only between callbacks. Validate scope coverage and child allocations; reject incomplete, invalid, duplicate and stale authority.
- Start `Halted` until a valid snapshot is installed. Define validity/freshness deadlines, missed-heartbeat and clock-anomaly behavior, plus a bounded-latency local halt path independent of the reporting/UI route.
- Integrate market-data readiness from M2 so stale or invalid inputs cannot authorize exposure-increasing behavior under an undefined pricing basis.
- Provide a narrow non-UI control interface for tests and operator-command integration later.

### Exit Gate

- Organizational and applicable account/route/instrument/venue limits cannot be bypassed by interleaved submissions.
- A callback observes one complete authority epoch/revision for its full turn.
- Delayed, duplicated, reordered or post-restart snapshot delivery cannot reinstall stale authority.
- Slow aggregation/reporting cannot synchronously delay submission, but policy expiry or lost readiness transitions to the specified local fail-safe behavior rather than authorizing indefinitely.
- `ReduceOnly`, `Halted`, cancellation, budget reductions and existing-order behavior have explicit deterministic tests.
- Safety-critical observation backlog is never silently discarded; overflow produces an observable degraded or halted state.

---

## M6 — First Venue Public Data, Read-Only

### Outcome

One real venue challenges the M2 market-data contract with coherent public data while order transmission remains structurally impossible. This milestone may run in parallel with M3–M5.

### For dummies

Connect the robot’s ears to one real exchange, but do not give it hands. It can watch the real order book and learn how the exchange talks, but it cannot trade.

### Scope

- Implement public session lifecycle, subscription, parsing, symbol mapping and normalization for the selected venue and instrument family.
- Implement venue snapshot/delta, checksum, duplicate/out-of-order, reconnect-epoch, sequence-gap and resubscription rules. A gap or checksum failure immediately invalidates the book and suppresses readiness until resynchronization.
- Use revisioned authoritative venue metadata for all tick, lot, multiplier and timestamp conversions.
- Define bounded public-data ingress, coalescing/shedding and resynchronization behavior. Overload may reduce freshness; it may not leave a corrupt book marked ready.
- Build credential-free venue-contract tests and parser fuzz/property tests from sanitized captured fixtures.
- Run the existing reference strategy without enabling order transmission.
- Compare normalized output and message-rate behavior against the deterministic harness.

### Exit Gate

- Recorded fixtures and a live public endpoint both drive the same normalized contract.
- A strategy sees `Ready` only after a coherent generation; disconnect, gap, checksum, malformed-input and overload scenarios move through observable non-ready states and recover through resnapshot.
- Tick, lot, contract and timestamp conversions match authoritative venue metadata.
- Transmission code and credentials are absent or disabled by construction in this mode.

---

## M7 — First Venue Private Observe/Reconcile-Only

### Outcome

AEGIS can authenticate to the selected sandbox account, consume private state and reconcile it without having any capability to transmit an order.

### For dummies

Let the robot look inside a practice account without giving it a send button. It compares the exchange’s notebook with its own and locks the account if it finds activity it cannot explain.

### Scope

- Implement private authentication and session lifecycle with a secrets provider that keeps credentials out of source, logs and fixtures.
- Consume and normalize private order, execution, balance and position streams plus authoritative open-order/recent-execution queries needed by the M4 reconciliation contract.
- Validate account permissions, position/margin mode, clock/nonce behavior, reconnect epochs and client/exchange correlation.
- Introduce explicit admission/priority for private execution and reconciliation facts relative to public data. Private facts may not be silently dropped; exhausted safety capacity halts/quarantines the account.
- Reconcile an empty or seeded known account; quarantine unknown/external orders and represent conservative account-level exposure until ownership is resolved.
- Start and remain `Halted` throughout observe-only reconciliation.
- Keep native order encoding and write initiation outside the constructed runtime or behind a test-proven fail-closed capability boundary.

### Exit Gate

- Startup query results and private streams converge into the expected OMS, position and account state without double application.
- Unknown orders, unexpected permissions or account-mode mismatch halt/quarantine the account instead of being guessed or ignored.
- Reconnect, clock drift and permission-change scenarios are covered by deterministic venue-contract tests and controllable sandbox observations.
- Fixtures are sanitized, logs redact secret material, and an attempted transmit is rejected before encoding or I/O.

---

## M8 — First Venue Sandbox Transmission

### Outcome

An explicitly enabled sandbox route completes a gated order lifecycle using the already-proven public, private, OMS, risk and reconciliation contracts.

### For dummies

Turn on the send button only for the pretend-money exchange. Practice sending, cancelling, filling and rejecting orders—including cases where nobody is sure what happened.

### Scope

- Implement native order encoding as a lossless mapping from the canonical, risk-approved order.
- Give every session a hard bound on outstanding outbound work, even when its transport permits concurrent writes. Use a session-local sequencer when serialization or buffer lifetime requires it; capacity exhaustion follows an explicit OMS transition and retains conservative exposure unless non-submission is proven.
- Handle venue rate limits and retry semantics without blindly retrying an ambiguous submission.
- Enable transmission only for explicit sandbox endpoint/account/route allowlists and a separately granted runtime capability.
- Prioritize cancellation and reconciliation traffic; cancellation remains permitted in `ReduceOnly` and `Halted` according to M5 policy and never releases exposure before confirmation.
- Use deterministic venue-contract/fake-transport scenarios for every failure branch. Treat the live sandbox as a smoke test only for behavior the venue can reliably induce.

### Exit Gate

- Market event → strategy → route authorization → canonical validation → local identity → risk → OMS → native submission → private event → inventory succeeds end to end in the sandbox.
- Submit, acknowledgement and cancel smoke flows work; a fill is demonstrated when the sandbox can reliably provide one.
- Deterministic tests cover reject, fill-before-ack, partial/cumulative fills, client-ID collision, rate limit, write overflow, timeout, disconnect, late event and ambiguous submission.
- Ambiguous outcomes retain conservative exposure and enter reconciliation; no retry can create a duplicate economic order silently.
- The default configuration remains incapable of reaching a production trading endpoint.

---

## M9 — Venue-Backed Recovery and Durable Audit

### Outcome

The first-venue system can restart and reconnect without orphaning orders, applying a fill twice or silently releasing uncertain exposure, and it retains honest durable evidence about every decision.

### For dummies

Pull the plug and turn the robot back on. It must remember what it can, ask the exchange about the rest and keep an honest diary instead of inventing an answer.

### Scope

- Implement the journal, snapshot and audit design fixed in M4 without adding synchronous persistence to the latency-sensitive path unless a later accepted ADR explicitly changes that invariant.
- State the exact durability guarantee and the crash windows that require exchange reconstruction; do not promise uninterrupted local audit where asynchronous publication cannot provide it.
- Recover stable client-order identities, OMS state, reservations, positions and installed configuration/policy provenance across restart.
- Reconcile open orders, recent executions, positions and balances with the venue before returning an account to a ready state.
- Make replay/live-catch-up idempotent across duplicate, delayed, fill-before-ack and unknown/external activity.
- Inject crashes before and after reservation, OMS admission, write initiation, private event application, audit publication and snapshot rotation.

### Exit Gate

- Restart and reconnect converge with the venue without double-applied fills, orphan orders, reused client identities or prematurely released uncertain exposure.
- Audit evidence explains each accepted, rejected, cancelled and filled order, including configuration, metadata, route/account and risk authority epoch/revision.
- Any crash-created audit gap is either reconstructed from authoritative venue data or explicitly reported as unresolved; it is never silently presented as complete.
- Unknown/external orders keep the account quarantined with conservative exposure until an explicit resolution workflow succeeds.
- Recovery and reconciliation tests run automatically against fakes/fixtures, with separate controllable sandbox smoke tests.

---

## M10 — Resilience and Load Qualification

### Outcome

The qualified single-venue system remains bounded and fail-safe when market data floods, private traffic backs up, handlers fail or dependencies slow down.

### For dummies

Give the robot a terrible day: too many messages, slow connections and broken parts. It must slow down or stop safely, never throw away an important fill or emergency command.

### Scope

- Stress the bounded admission, public-data shedding/resync, observation handoff and outbound sequencing policies introduced in M2, M5, M6 and M8 rather than inventing them here.
- Finalize and stress the scheduling priorities introduced progressively so private execution events, halt/cancel commands and reconciliation work cannot be silently dropped or indefinitely starved by public market data.
- Add exception/fault containment, watchdogs and explicit healthy/degraded/halted states.
- Exercise slow consumers, full buffers, rate limits, socket churn, stale clocks, malformed bursts and long-running handler failures.
- Run representative latency/capacity benchmarks and long soaks against the M0 workload, recording callback duration, executor utilization, queue age, write backlog and submission tail latency.
- Optimize measured bottlenecks only. Revisit the single-thread ADR only when a documented trigger is reproducible.

### Exit Gate

- Saturation has a bounded, observable response and cannot bypass route authorization, risk or OMS.
- Public data may be coalesced or invalidated and resynchronized; private execution facts, halt commands and reconciliation requirements are never silently lost.
- A safety-critical backlog exceeding its bound transitions to the specified degraded/halted state within a measured limit.
- Latency, capacity and queue-age budgets pass with agreed single-venue headroom under normal and fault workloads.
- Repeated fault and soak runs end in a reconcilable state with no reservation/inventory drift.

---

## M11 — Minimum Operator Plane and Deployability

### Outcome

Operators can safely observe, configure, halt, deploy and recover the first-venue system before a long-running shadow qualification or second adapter adds more state.

### For dummies

Build the control room with alarms, status screens and a big stop button. Record who changed a rule, what they changed and whether it worked.

### Scope

- Implement authorized revisioned runtime configuration and execution-route adoption while preserving the immutable provenance established in M1–M4.
- Add the minimum monitoring, alerts, structured logs and control-plane status surfaces needed to operate safely. Every view exposes its age, source and relevant revision where staleness matters.
- Add operator workflows for risk-mode changes, local emergency halt, route/account disablement, reconciliation and quarantine resolution.
- Audit every operator command with authenticated actor, request identity, target scope, prior/new revision where applicable, result and timestamp.
- Define single-venue deployment, secrets rotation, backups, rollback, incident response and venue-specific runbooks.

### Exit Gate

- Operator/reporting surfaces use copied control-plane state and cannot read, mutate or block live data-plane objects.
- Alerts distinguish stale reporting, stale policy, invalid market state, private-session loss and unresolved reconciliation.
- Halt, cancel, credential-rotation, restart, rollback and reconciliation drills pass from versioned runbooks.
- Configuration adoption is validated, authorized, atomic at its owner and fully attributable in later order/audit events.
- Operator commands are authenticated, idempotent where required and attributable from request through resulting state revision.

---

## M12 — Single-Venue Shadow Qualification

### Outcome

The first venue passes a defined shadow/paper observation period with explainable positions, risk state, operational events and performance before second-venue work begins.

### For dummies

Let the robot practice for a long time without real money. Watch until its numbers, alarms and explanations are reliably boring and every surprise has an answer.

### Scope

- Decide P&L ownership and marking semantics before implementing reporting: realized/unrealized treatment, mark/index/reference-price authority and freshness, currency conversion, fees and late corrections.
- Add single-venue inventory/P&L reporting and the risk-monitoring views needed to evaluate the shadow run; cosmetic UI polish remains deferrable.
- Run shadow/paper operation and sandbox-only execution for an agreed period with realistic public traffic and recorded qualification criteria.
- Review every reconciliation discrepancy, halt, backlog excursion, stale state and alert; feed fixes back into the owning earlier contract rather than masking them in the UI.
- Freeze the first-venue contract only after recovery, capacity and operational evidence remains stable for the agreed window.

### Exit Gate

- The observation window completes without unexplained order, reservation, position or audit drift.
- P&L/reporting projections are deterministic for the same facts and expose their marking source, freshness and revision.
- Alert quality, reconciliation frequency, latency/capacity headroom and operator response times meet the documented criteria.
- All material shadow findings are resolved or explicitly accepted with an owner and follow-up gate.
- Real-money transmission remains disabled.

---

## M13 — Second Venue and Cross-Venue Behavior

### Outcome

A bot can consume and trade across both target venues while inventory, uncertain orders and risk remain coherent.

### For dummies

Only after the first exchange works safely, teach the robot about the second one. It must still know its total allowance and belongings even when the two exchanges behave differently.

### Scope

- Implement the second adapter against the proven market-data, execution, OMS, recovery, overload and operations contracts.
- Generalize shared abstractions only where the second implementation demonstrates a real commonality.
- Repeat public read-only, private observe-only and sandbox capability gates for the second venue; do not skip directly to transmission.
- Add a simple deterministic cross-venue reference strategy for pricing and hedging, not a production alpha strategy.
- Exercise one-venue degradation, asymmetric/ambiguous fills, shared-account assumptions and venue/account-specific limits.
- Re-run contract, recovery, audit and load suites against each adapter and the combined workload.

### Exit Gate

- Both adapters pass the same behavior-oriented contract suites while keeping native types inside venue code.
- A multi-venue bot can price on both venues and hedge only through authorized, ready routes.
- Cross-venue fills and uncertain submissions update one coherent bot, desk and firm exposure view before the next risk decision.
- Failure, quarantine or recovery of one venue cannot corrupt the other venue's state or hide aggregate exposure.
- Second-venue integration does not weaken any first-venue qualification gate.

---

## M14 — Combined Production Qualification

### Outcome

Measured combined-system evidence—not architectural optimism—determines whether AEGIS is ready to request authorization for a tightly limited live pilot.

### For dummies

Give the whole two-exchange robot its final safety exam. Passing the exam still does not turn on real money; a human must separately approve a very small, carefully watched trial.

### Scope

- Repeat representative benchmarks, long soaks and fault drills with both venues and the cross-venue workload.
- Qualify time synchronization, venue rate-limit headroom, account permissions, credential rotation, backups, deployment rollback and kill/halt paths.
- Exercise venue isolation, simultaneous reconnect, asymmetric fills, stale pricing, policy expiry and control-plane outage scenarios from runbooks.
- For every leveraged instrument eligible for a live pilot, accept and test its mark/index/reference-price authority and freshness, conservative conversion/rounding, collateral and margin utilization, liquidation proximity and loss/P&L policy. Otherwise mark that instrument family ineligible for live use.
- Complete the multi-venue risk-monitoring UI and reporting projections without adding a dependency from the data plane to those views.
- Verify capacity headroom and all single-thread ADR revisit triggers under the combined workload; propose partitioning only if measurements require it.
- Run both venues in shadow/paper mode before any request to enable production transmission.
- Define the scope, exposure cap, accounts, routes, observers, rollback criteria and explicit human authorization required for any live pilot.

### Exit Gate

- Combined latency, capacity, queue-age, recovery and audit targets pass with agreed headroom.
- Kill/halt, credential-rotation, restart, rollback, venue-isolation and reconciliation drills pass from versioned runbooks.
- Every live-eligible leveraged route has passing margin, collateral, liquidation-proximity and risk-pricing tests; unsupported routes remain disabled.
- UI and alerts expose current readiness, authority epoch/revision, configuration/metadata provenance and data age for both venues.
- No production credential, endpoint or route is enabled by the default build, CI or deployment configuration.
- Real-money connectivity remains a deliberate, audited authorization outside this roadmap's automatic execution.

## Execution Rules

- Treat a milestone as an aggregate capability gate, not one giant pull request. Deliver it through small, independently reviewable task branches created from `dev`, with every pull request targeting `dev`.
- Resolve a milestone's blocking ADR, transition table or failure policy before merging implementation that depends on it.
- M6 is the only planned parallel milestone track. Rejoin it with M4/M5 before private connectivity in M7.
- Close a milestone only when its exit gate is automated or has reproducible evidence attached.
- Detail issues for the next one or two milestones, not the entire roadmap; later breakdowns should use what earlier slices teach.
- Prefer one thin end-to-end behavior over broad scaffolding. Add a directory or abstraction when its first implementation or test needs it.
- Add bounded metrics and diagnostics with each behavior; do not wait for the operator-plane milestones to make safety state observable.
- Use deterministic venue-contract tests for failure branches; sandbox availability is not a substitute for reproducibility.
- Do not begin the second venue until the first venue passes M12 and its recovery/operations contracts are stable.
- Do not make live trading part of CI, a default configuration or an automatic milestone demonstration.

## Intentionally Deferred

M0–M8 do not require a graphical UI, full P&L accounting, dynamic runtime configuration, multiple data-plane executors, production deployment or sophisticated strategies. Those concerns should not reshape the deterministic core. Sharding remains deferred until M14 measurements reproduce an ADR revisit trigger.
