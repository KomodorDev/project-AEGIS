# Deribit BTC Perpetual Public-Protocol Spike

- **Date:** 2026-08-18
- **Time box:** Half a working day
- **Result:** Select Deribit testnet and native instrument `BTC-PERPETUAL`
- **Transmission:** Not investigated or implemented

## Question

Can one credential-free Deribit product and test environment provide a sufficiently precise first
venue/instrument boundary without committing AEGIS to networking primitives or native data types?

## Finding

Yes. Deribit exposes public JSON-RPC metadata over HTTP and WebSocket. The requested BTC/USD perpetual
maps to `BTC-PERPETUAL`, which current metadata identifies as:

- `kind`: `future`
- `settlement_period`: `perpetual`
- `instrument_type`: `reversed` (the response's legacy `future_type` field is deprecated and is not
  part of the normalized contract)
- base currency: BTC
- quote and counter currency: USD
- settlement currency: BTC
- price index: `btc_usd`

The selected public test endpoints are:

- HTTP: `https://test.deribit.com/api/v2`
- WebSocket: `wss://test.deribit.com/ws/api/v2`

Testnet and production use separate accounts and credentials. Testnet may run a different release and
return different metadata, so passing against testnet is protocol evidence rather than proof of
production equivalence.

## Time-stamped observation

A credential-free request to
[`public/get_instrument`](https://test.deribit.com/api/v2/public/get_instrument?instrument_name=BTC-PERPETUAL)
on 2026-08-18 returned, among other fields, contract size 10 USD, minimum trade amount 10 USD,
tick size 0.5 USD, `lot_size` 1, and testnet instrument ID 124972. Deribit defines `lot_size` as the
unit for fee-lot counting, not the minimum order-quantity step; it must not be used as a lot-step
shortcut. These values are observations, not configuration constants. Numeric instrument IDs,
quantity filters, fees, rate limits, and contract metadata can differ by environment or change over
time; adapters must fetch and revision authoritative metadata before using them.

## Account findings relevant to later gates

Deribit exposes multiple margin models. The reference account selects `segregated_sm`, and a dedicated
subaccount provides the isolation boundary because Deribit does not offer a portable per-position
isolated-margin switch. M0 does not create or authenticate an account. The normative operating policy
is [ADR-0003](../decisions/0003-dedicated-testnet-account.md).

The public documentation does not establish a resumable private-stream cursor. The later recovery
decision must therefore prove how subscriptions, authoritative queries, any historical backfill, and
identifier-based deduplication close reconnect gaps. This is a requirement to investigate in M4, not
a recovery mechanism selected by this M0 spike.

## Official sources

- [Deribit JSON-RPC overview](https://docs.deribit.com/articles/json-rpc-overview)
- [`public/get_instruments` API](https://docs.deribit.com/api-reference/market-data/public-get_instruments)
- [Inverse perpetual contract description](https://support.deribit.com/hc/en-us/articles/31424954847133-Inverse-Perpetual)
- [Testnet features and differences](https://support.deribit.com/hc/en-us/articles/25944687755293-Features)
- [Margin types](https://support.deribit.com/hc/en-us/articles/25944811317149-Margin-types-and-usage)
- [Subaccounts](https://support.deribit.com/hc/en-us/articles/25944616386973-Subaccounts)
- [Account settings and self-match prevention](https://support.deribit.com/hc/en-us/articles/25944634289693-Account-settings-page)
- [API key permissions](https://support.deribit.com/hc/en-us/articles/26268257333661-Creating-new-API-key-on-Deribit)

## Deferred to M6 and later

- WebSocket implementation and reconnect lifecycle
- exact order-book channel, depth, interval, sequence, and checksum rules
- revisioned metadata ingestion and normalized units
- parser fixtures and fuzz/property tests
- private authentication, reconciliation, and order encoding

No networking library, credential format, asynchronous executor, or order capability is selected by
this spike.
