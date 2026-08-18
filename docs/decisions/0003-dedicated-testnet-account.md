# ADR-0003: Isolate the Reference Scenario in a Dedicated Testnet Account

> **Purpose:** Fix the account, authority, permission, and fail-closed safety boundary that every
> Deribit milestone must obey.

- **Status:** Accepted
- **Date:** 2026-08-18
- **Scope:** Deribit account ownership, authority, permissions, and unexpected activity
- **Related:** [Reference scenario](../reference-scenario.md),
  [implementation roadmap](../implementation-roadmap.md)

Names in backticks are exact account settings, permission scopes, or AEGIS identifiers. Words such
as “must” and “never” state requirements rather than recommendations.

## Context

AEGIS cannot reconcile orders, fills, and positions safely if a person or another program can mutate
the same account without an explicit ownership protocol. Guessing that unknown activity belongs to a
bot would understate exposure and corrupt attribution. M0 must establish the operating boundary even
though authenticated observation does not begin until M7 and sandbox transmission does not begin
until M8.

## Decision

The reference scenario uses one dedicated Deribit testnet subaccount used only by AEGIS. Its logical
configuration name is `account.deribit-testnet-aegis`; the actual account identifier and credentials
must remain outside source control.

The required account baseline is:

- Deribit margin model `segregated_sm` (Segregated Standard Margin).
- BTC collateral only for the inverse `BTC-PERPETUAL` scenario.
- Deribit's single net position per instrument is used; there is no dual-side/hedge-mode choice. The
  `BTC-PERPETUAL` position is initially zero and there are no open orders.
- Manual/UI orders, other bots, other API clients, and other instruments are prohibited.
- Separate subaccount login is disabled.
- Runtime API keys are created by the dedicated subaccount, not inherited from the main account.
- Runtime API keys have read-only account access and never receive wallet, Block Trade, Block RFQ,
  custody, account-write, or other unlisted functional permission.
- The subaccount's enabled Deribit product classes contain only `perpetual`; `futures`, `options`,
  `future_combos`, `option_combos`, and `spots` are disabled by provisioning.
- Main-account self-match prevention uses `reject_taker` and is extended across subaccounts. Block
  RFQ self-match prevention is also enabled even though AEGIS has no Block RFQ capability.

Those self-match settings apply to the parent main account and every sibling subaccount. Therefore,
the preferred topology is a parent testnet main account dedicated to the AEGIS reference environment.
If the parent has other users or subaccounts, its owner must explicitly approve and record that the
required account-wide settings are compatible with all of them before AEGIS is armed. The AEGIS
runtime never changes parent-account settings; a human-controlled provisioning process owns them.

Permissions are staged by capability:

| Milestone | Maximum provisioned functional scopes |
|---|---|
| M0–M6 | No private credential; public data only when M6 begins |
| M7 | `account:read trade:read wallet:none`; no Block Trade, Block RFQ, custody, or other functional scope |
| M8 onward | `account:read trade:read_write wallet:none`; no Block Trade, Block RFQ, custody, or other functional scope, and only when separately enabled for sandbox transmission |

Provisioning must record, outside source control, the expected subaccount ID, runtime API client ID,
and its maximum scope. This out-of-band check matters because an authentication response proves only
the effective token scope, not that the stored key is incapable of minting a broader token. At
runtime, AEGIS requests only the milestone's exact functional scope and validates the effective scope
returned by authentication.

An authenticated session must also prove that Deribit reports account type `subaccount` and the
expected subaccount ID. Main-account keys are not acceptable because their authority can extend
across subaccounts. A session reporting account type `main`, an unexpected subaccount, excessive or
unrecognized functional token scope, or a mismatch with the provisioned key record enters
quarantine. A separately controlled setup credential may configure account settings and inspect
maximum key scopes, but it is never loaded by the AEGIS runtime.

Deribit product restrictions operate at a broader product-class level and do not make the credential
specific to `BTC-PERPETUAL`: allowing `perpetual` can still permit other perpetual instruments. The
AEGIS route and encoder allowlists enforce the exact one-instrument boundary; reconciliation
quarantines any activity outside it. Venue restriction and application allowlisting are independent
layers, and neither is documentation-only authority to trade.

Deribit is authoritative for venue facts: accepted exchange orders, order status, trades, balances,
positions, permissions, and configured margin model. AEGIS is authoritative for local intent,
organizational ownership, configuration/risk provenance, and the mapping between its client identity
and a venue order. Reconciliation compares the two; neither side may silently invent facts owned by
the other.

The entire account and its route enter a fail-closed quarantine when AEGIS observes any of the
following:

- an unknown order or trade;
- an unexplained non-zero position or balance effect;
- a margin-model or permission mismatch;
- an order in an instrument outside the reference scenario;
- a venue fact that cannot be correlated without guessing.

Quarantine blocks all exposure-increasing submissions and preserves conservative exposure. Unknown
activity is not silently adopted, attributed, retried, cancelled, or flattened. Resolution requires
an authoritative resnapshot and an explicit recovery decision. Exact recovery, cancel-all, and
live-catch-up mechanics remain M4–M9 decisions.

Cancel-on-disconnect, when introduced, is defense in depth only. Deribit can configure an
account-level default, but a disconnect cancels only orders created through the affected connection;
it cannot prove that every account order was cancelled and does not replace reconciliation.

## Testability

M7 must validate at startup and after reconnect that the authenticated account is the expected
subaccount; the effective token scope exactly matches the milestone allowlist; the margin model is
`segregated_sm`; account-level self-match prevention is enabled with `reject_taker`; the reported
`trading_products_details` enables only the required product class; and balances, collateral, open
orders, venue-reported net positions, and instrument scope match this decision. The operational
preflight must also attest that provisioning set the available `trading_products` allowlist to exactly
`["perpetual"]` and received a successful result, the parent-account owner approved the account-wide
settings, separate login remains disabled, no unauthorized key exists, the runtime key's provisioned
maximum scope still matches its record, and Block RFQ self-match prevention remains enabled.
Deterministic venue contract tests must inject each observable mismatch and prove that the account
remains quarantined and unable to transmit.

## Consequences

- Shared or manually operated accounts are not supported by the first-venue path.
- Test activity must use a separate account or subaccount if a human needs to trade manually.
- A clean account boundary makes unknown activity exceptional and permits conservative failure
  semantics without guessed ownership.
- Supporting shared accounts later requires a separate ownership and reconciliation decision; it is
  not an incremental configuration change.

## Venue references

- [Access scopes and effective token permissions](https://docs.deribit.com/articles/access-scope)
- [Creating API keys and maximum scopes](https://docs.deribit.com/articles/creating-api-key)
- [Subaccount ownership and isolation](https://support.deribit.com/hc/en-us/articles/25944616386973-Subaccounts)
- [Account-level self-match prevention](https://support.deribit.com/hc/en-us/articles/25944634289693-Account-settings-page)
- [Subaccount product-class restrictions](https://docs.deribit.com/api-reference/account-management/private-set_disabled_trading_products)
- [Observable product availability in account details](https://docs.deribit.com/api-reference/account-management/private-get_account_summary)
