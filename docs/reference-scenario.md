# M0 Reference Scenario

**Status:** Accepted baseline for M1–M12. Changes require an explicit roadmap or decision update.

## Purpose

The reference scenario keeps the first implementation narrow enough to explain, replay, and test. It
is a delivery fixture, not a production strategy and not authorization to trade real money.

The requested “Deribit BTCUSD-Perp” product is interpreted as Deribit native instrument
`BTC-PERPETUAL`: the inverse, USD-quoted, BTC-settled perpetual. It is not the linear USDC-settled
`BTC_USDC-PERPETUAL` product.

## Fixed scenario

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

Its default mode is observe-only. Every callback appends one trace record containing the callback
ordinal, readiness state, normalized best bid/ask fields when coherent, and relevant metadata/book
revisions. Non-ready input never creates an order intent. In fake-backed submission tests, the test
driver may inject one `SubmitReferenceIntent` command: the strategy submits the fixed M3 reference
request on the next `Ready` callback exactly once and records all later callbacks without submitting
again. M3 owns the request's supported order vocabulary and economics.

| Stage | Behavior |
|---|---|
| M1 | Its immutable configuration and organizational attribution validate or fail as a unit. |
| M2 | A recorded coherent top-of-book event produces a deterministic digest; non-ready state produces no decision. |
| M3–M5 | The injected one-shot command requests one fixed fake-backed order intent through the canonical route/risk/OMS path. |
| M6–M7 | Live public and private testnet facts may be observed, but the constructed runtime has no transmission capability. |
| M8 | One explicitly armed testnet route may exercise a bounded submit/cancel lifecycle. |

No stage silently retries an ambiguous submission. A market-data subscription never implies
execution permission.

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
