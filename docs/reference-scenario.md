# M0 Reference Scenario

> **Purpose:** Pin one small, deterministic Deribit scenario that every later milestone can build,
> replay, and test against without silently widening trading authority.

**Status:** Accepted baseline for M1–M12. Changes require an explicit roadmap or decision update.

## Safety boundary

The reference scenario keeps the first implementation narrow enough to explain, replay, and test. It
is a delivery fixture, not a production strategy and not authorization to trade real money.
Its single firm is one valid M1 firm root; it does not restrict a startup configuration from holding
additional independently attributed firm roots for subsidiaries.

The requested “Deribit BTCUSD-Perp” product is interpreted as Deribit native instrument
`BTC-PERPETUAL`: the inverse, USD-quoted, BTC-settled perpetual. It is not the linear USDC-settled
`BTC_USDC-PERPETUAL` product.

## Fixed scenario

The values below are fixed fixture identities and scenario constraints. They are not venue payload
fields, live credentials or permission to substitute a similarly named instrument or account.

| Concept | Reference value |
|---|---|
| Firm | `firm.aegis-lab` |
| Desk | `desk.digital-assets` |
| Bot | `bot.deribit-btc-perpetual-reference` |
| Strategy | `strategy.deterministic-reference` |
| Venue | `deribit` |
| Environment | Deribit testnet only |
| Logical account | `account.deribit-testnet-aegis` |
| Normalized instrument | `BTC-USD-PERPETUAL` |
| Venue instrument | `BTC-PERPETUAL` |
| Subscription | One order-book subscription for the venue instrument |
| Execution route | `route.deribit-testnet-btc-perpetual`, unavailable by default |

The public test profile uses `https://test.deribit.com/api/v2` for HTTP JSON-RPC and
`wss://test.deribit.com/ws/api/v2` for WebSocket JSON-RPC. Those endpoints are not contacted by the
M0 default build or tests. Production hosts, accounts, keys, and routes are forbidden in this
scenario.

Instrument IDs, tick sizes, minimum amounts, contract values, fee settings, leverage limits, rate
limits, and subscription behavior are venue metadata, not constants in this document. Each relevant
milestone must obtain revisioned authoritative metadata and validate the contract properties it uses.

## Strategy behavior by milestone

The strategy is intentionally deterministic and contains no alpha logic.

Its default mode is observe-only. Every callback produces one runtime-owned canonical trace record
containing the callback ordinal, readiness state, normalized best bid/ask fields when coherent, and
relevant metadata/book revisions. Non-ready input never creates an order intent. M4's longitudinal
fake-lifecycle replay adds one `SubmitReferenceIntent`: the strategy submits the
fixed M3 request below on the next `Ready` callback exactly once and records all later callbacks
without submitting again. The intent is consumed for local rejection, local initiation or
uncertainty. Recovery marks an ambiguous consumption/call crash window outcome unknown and never
retries it. ADR-0012 owns this command; M3's request vocabulary and direct path remain unchanged.

| One-shot request field | Fixed value |
|---|---|
| Route | `route.deribit-testnet-btc-perpetual` |
| Instrument | `BTC-USD-PERPETUAL` |
| Side | Buy |
| Type | Limit |
| Time in force | GTC |
| Price | `100.0` |
| Quantity | `2` |

| Stage | Behavior |
|---|---|
| M1 | Its immutable configuration and organizational attribution validate or fail as a unit. |
| M2 | Credential-free recorded snapshots/deltas produce deterministic state/market callbacks and `AEGISRTS`; non-ready state exposes no book or decision. |
| M3 | The canonical fake-backed route/risk/OMS path is implemented and proved by the closeout matrix below. |
| M4 | The fake lifecycle/recovery replay adds one longitudinal one-shot intent driver while it proves exchange-event OMS and reservation transitions. |
| M5 | The same path is exercised under dynamic risk allocation, modes, freshness, and market-readiness policy. |
| M6–M7 | Live public and private testnet facts may be observed, but the constructed runtime has no transmission capability. |
| M8 | One explicitly armed testnet route may exercise a bounded submit/cancel lifecycle. |
| M9 | Restart and reconnect qualification re-establishes authoritative orders, positions, and durable audit state before the route can resume. |
| M10 | The same single-bot scenario runs under its named sustained, burst, backlog, and injected-fault workloads. |
| M11 | Authorized operators can observe, halt, and explicitly recover this route without widening its account or instrument boundary. |
| M12 | The unchanged scenario completes the defined testnet shadow/paper qualification period; production remains forbidden. |

No stage silently retries an ambiguous submission. A market-data subscription never implies
execution permission. This table fixes externally visible scenario behavior, including the
implemented M3 contract, the integrated bounded M4 private/recovery/evidence foundations, and the
remaining accepted venue-neutral M4 lifecycle/recovery contract. Native private protocol, dynamic
risk, durable recovery and operator-interface designs remain owned by M7, M5, M9 and M11
respectively.

## M2 deterministic replay

The implemented M2 scenario drives the normalized Deribit source through snapshot, delta, exact
duplicate, older input, gap, same-sequence conflict, checksum failure, metadata mismatch, malformed
input, staleness, session reset, capacity loss, and snapshot-only recovery. Two manual runs and one
dedicated-owner run reproduce the same 28 complete callback observations and 71 canonical runtime
trace records. The schema-one stream is 29,610 bytes with digest
`e63b89b8c3a826fd1104f9e9949364606be5efbeedc112cb980260ccb70fda0b`.

The configured scenario remains single-firm. Its extra unrelated registered bot receives no
callback, while separate BotRuntime tests preserve canonical multi-firm dispatch support.

## M3 deterministic replay

The implemented M3 closeout scenario is a contract matrix, not an implementation of the conceptual
M4 longitudinal one-shot behavior above. It deliberately submits ten requests during one Ready
callback so every ordinary local outcome is observable in one deterministic evidence bundle:

1. a peer-firm route returns `RouteNotOwned`;
2. a misaligned price returns `PriceTickMismatch`;
3. invalid quantity precision/lot economics returns `QuantityScaleExceeded` at the canonical first
   failing quantity check;
4. an otherwise valid request exceeds the bot `SingleOrderQuantity` limit;
5. the scripted encoder returns `EncodingFailed` and the reservation releases once;
6. fake initiation fails before accepted-slot copy and releases once;
7. fake bytes are copied but the local outcome is lost, producing `SubmissionUnknown` with retained
   exposure and no retry;
8. fake bytes are copied and initiation succeeds locally, producing `WriteInitiated` with retained
   exposure but no exchange acknowledgement;
9. a repeated admitted local identity returns `DuplicateOrderIdentity` and releases the new
   reservation; and
10. the bounded OMS returns `OmsCapacityExceeded` and releases the new reservation.

Two manual-owner replays and one dedicated-owner replay compare all callbacks, results, canonical
runtime/submission traces, diagnostics, OMS rows, reservations, seven-scope aggregates, fake writes,
identities, and injected measurement durations. The three runs execute 997 assertions and pin:

- M3 scenario `AEGISRTS`: 6 records, 2,494 bytes, SHA-256
  `4d62d2c42bce7eeea8901b1c28baf4059190a21a46747f7fd70ef8fc67d71ed7`;
- `AEGISSTS`: 67 records, 35,616 bytes, SHA-256
  `ace82ff42d02074d512f743f9c3b5d8dc040911e57031e1d438db2715780943d`;
- two accepted 442-byte `AEGISFOE` fake writes with SHA-256
  `4e9eae436a7db7e779a560ab2242e63c7816080376d8a59affbd21b2be789674` and
  `b19ac0749c2aaa1a55ee14c5b088e282cf83aa24a5379f3c270533af2ad79165`;
- exactly two held reservations after replay: uncertain quantity 3/notional 30 and locally initiated
  quantity 4/notional 40; and
- open count 2, worst/gross quote notional 70, and worst quantity 7 at each of the baseline firm's
  seven scopes, with every peer-firm scope remaining zero.

The accepted M1/M2 fixture deliberately keeps its execution route disabled and its fingerprint
`e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310` unchanged. The M3 matrix uses a
separate enabled two-firm fixture with fingerprint
`442dbeb26f2a1251f8badb9cff75e020940ad63d743e8b29175b50749793e908` solely to prove route authority
and firm isolation. It grants no live capability: the encoder and initiator are fixed in-memory
fakes. Complete revision-specific proof is in the [M3 exit-evidence
record](milestones/m3-exit-evidence.md).

## Account boundary

The account begins flat with no open orders and uses Segregated Standard Margin
(`segregated_sm`). It is dedicated to AEGIS, and manual or external activity is prohibited. Authority,
permission staging, mismatch checks, and quarantine behavior are normative in
[ADR-0003](decisions/0003-dedicated-testnet-account.md).

## M0 acceptance

The scenario is sufficiently bounded when every later test or benchmark can name the firm, desk,
bot, strategy, venue, account alias, instrument mapping, subscription, and route above; when the
default route cannot transmit; and when no credential or production endpoint is needed to configure,
build, test, or benchmark the repository.
