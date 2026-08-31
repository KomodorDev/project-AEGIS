# ADR-0005: Seal Startup Configuration and Deterministic Provenance

> **Purpose:** Define the M1 organizational roots, immutable startup configuration identity, and
> bounded deterministic trace contract.

- **Status:** Accepted
- **Date:** 2026-08-18
- **Scope:** M1 startup configuration, organizational attribution, provenance, and traces
- **Related:** [Domain value contracts](0004-domain-value-contracts.md),
  [components and ownership](../architecture/components.md),
  [M0 reference scenario](../reference-scenario.md)

## Context

Later market, order, and audit events must identify exactly which organizational attribution,
instrument metadata, strategy settings, subscription permissions, and route permissions produced
them. Source-file order, unordered-container iteration, partially valid configuration, or ad hoc
logging would make that evidence ambiguous. The organization must also allow independently
attributed subsidiaries without inventing a parent-company model in M1.

## Decision

The subsections progress from authority, through atomic validation, to canonical evidence. That
ordering is deliberate: the hash and trace are meaningful only after the complete rulebook and its
ownership relationships are validated.

### Organization and permissions

One startup configuration contains one or more `Firm` roots. Each `Desk` references exactly one
existing firm, and each `Bot` references exactly one existing desk; the bot's firm is derived through
that immutable registration. IDs are unique within their type, and empty firm sets, duplicate IDs,
dangling references, and firms or desks with no registered descendants are invalid. A bot cannot
replace its desk or firm after configuration construction.

There is no parent `Company` entity, cross-firm aggregation, or implicit shared risk authority in M1.
Firm roots are peers and cannot contain one another. The reference scenario remains one firm;
additional firms represent independent roots such as legal subsidiaries. Any later cross-firm policy
requires an explicit decision.

Subscriptions and execution routes are separate records. A subscription grants one bot access to a
venue/instrument market-data stream. A route grants one bot a specific venue/logical-account/
instrument execution permission and has an explicit enabled state. Duplicate records and dangling
bot, venue, account, or instrument references are invalid. A matching subscription never creates,
enables, or implies a route. An enabled configured route is necessary but never sufficient to arm or
transmit an order. Route selection, native-account reconciliation, and actual submission begin in M3
and later.

### Atomic immutable startup configuration

The builder validates organization, instrument metadata, bot/strategy settings, subscriptions, and
routes as one unit. It returns either one complete immutable value or a deterministic error; no
partially usable object is published. After construction, consumers receive const views or owned
values, never mutable references into the builder.

The complete configuration carries a non-zero `ConfigurationRevision`. Its sections carry distinct
non-zero `OrganizationRevision`, `InstrumentMetadataRevision`, `StrategyConfigurationRevision`,
`SubscriptionRevision`, and `RouteRevision` values. These types cannot be interchanged. All relevant
later admission and audit values can therefore carry the complete configuration identity plus the
specific section revisions they used. M1 validates a single startup snapshot only; monotonic dynamic
adoption, patching, and rollback remain deferred.

### Canonical identity

Configuration identity is SHA-256 over a canonical binary encoding, not over source JSON, object
memory, or a platform hash. The encoding begins with the fixed `AEGISCFG` magic and an explicitly
assigned 16-bit schema version. Every field has a 16-bit tag and 32-bit byte length. Unsigned domain
scalars use fixed-width big-endian bytes; a decimal uses a two's-complement 64-bit coefficient and an
8-bit scale; enums use assigned integer values; strings use their validated ASCII bytes; sequences
include a 32-bit count and length-prefixed elements; optional values include an explicit zero-or-one
presence byte. All size and count overflows fail encoding.

Collections are sorted by their typed canonical ID bytes, and nested unordered data is likewise
sorted by its declared key. Input order, map implementation, padding, endianness, process state, and
locale cannot affect the result. Duplicates are rejected rather than discarded during sorting. The
encoding includes the schema version, all revisions, and every decision-relevant configuration value,
but never credentials or out-of-band venue account identifiers.

`ConfigurationFingerprint` stores the 32 digest bytes; lowercase hexadecimal is display-only.
Canonical encoding and SHA-256 have published golden vectors, including the reference configuration,
so implementations on different supported platforms must produce identical bytes and fingerprints.
A schema or field-semantics change requires a new schema version and updated vectors.

### Bounded deterministic traces

The M1 trace contract is a canonical in-memory sequence of structured `TraceRecord` values. Each
record contains a schema version, monotonically increasing ordinal, assigned event kind, applicable
typed subject IDs, configuration fingerprint, applicable section or metadata revisions, and a
bounded event-specific value payload. Records contain no pointer values, thread IDs, ambient wall
clock reads, unordered iteration output, or free-form diagnostic prose.

A trace sink has a fixed capacity supplied at construction. Appending beyond that capacity returns a
stable `TraceCapacityExceeded` error; it never silently drops, overwrites, or evicts a record. Tests
compare canonical records and may hash their length-prefixed canonical encodings with SHA-256. Given
the same configuration, ordered inputs, injected clock values, and generated IDs, record bytes and
their digest are identical.

File, network, database, formatting, and rich reporting I/O are not part of this sink and never occur
on the data-plane path. A later milestone may copy immutable trace batches to an off-path consumer,
but must define ownership and backpressure before doing so.

## Consequences

The immutable-provenance decision yields the following attribution guarantees and implementation
obligations.

- Multiple subsidiaries can coexist without weakening bot-to-firm attribution.
- Startup either publishes one sealed, provenance-carrying rulebook or fails closed.
- Semantically identical configuration has one cross-platform identity independent of authoring
  order or format.
- Canonical encoding, ordering, revision propagation, capacity failure, and reference fingerprints
  require unit and golden-vector tests.
- Dynamic configuration adoption, cross-firm risk, market-event dispatch, route/account selection,
  venue transmission, OMS state, and durable trace storage remain M2/M3 or later concerns.
