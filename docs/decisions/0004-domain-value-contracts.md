# ADR-0004: Adopt Deterministic Domain Value Contracts

> **Purpose:** Fix the identity, numeric, time, revision, error, clock, and generated-identifier
> contracts on which the M1 domain kernel depends.

- **Status:** Accepted
- **Date:** 2026-08-18
- **Scope:** M1 dependency-light domain values
- **Related:** [Implementation roadmap](../implementation-roadmap.md),
  [M0 reference scenario](../reference-scenario.md),
  [ADR-0002](0002-delivery-toolchain.md)

## Context

Venue strings, primitive integers, floating-point values, wall clocks, and exceptions do not provide
the type safety or repeatability required by later admission and audit decisions. M1 must establish
portable C++20 value contracts without selecting a runtime framework or weakening production order
identity to make tests deterministic.

## Decision

The following value families form one compatibility boundary: identities prevent category mixing,
exact numbers prevent silent precision loss, and typed time/revision/identity providers make replay
deterministic without weakening production behavior.

### Typed identifiers and deterministic errors

Every identity is a distinct nominal type. In particular, `FirmId`, `DeskId`, `BotId`, `StrategyId`,
`VenueId`, `LogicalAccountId`, `InstrumentId`, `VenueInstrumentId`, `SubscriptionId`, `RouteId`, and
`VenueAccountId` and `OrderId` are not aliases of one string type and do not convert implicitly
between one another.
Configuration-owned textual IDs are created only by validating factories; an invalid value cannot be
constructed as a valid ID.

Configuration-owned organizational IDs use their exact lowercase kind prefix (`firm.`, `desk.`,
`bot.`, `strategy.`, `account.`, `subscription.`, or `route.`), followed by lowercase ASCII
alphanumeric slug segments separated by `.` or `-`, with no empty segment. They are at most 64 bytes.
`VenueId` is a 1–32 byte lowercase ASCII slug with `-` separators. Normalized `InstrumentId` is a
1–64 byte uppercase ASCII token made of alphanumeric segments separated by `-`. A
`VenueInstrumentId` or `VenueAccountId` is a distinct 1–128 byte printable ASCII adapter value;
venue-specific validation may narrow that grammar further.

`LogicalAccountId` is the stable configuration identity used by routes and provenance.
`VenueAccountId` is a separate adapter-owned identity reported by an authenticated venue. Neither can
convert to the other; their M7 reconciliation mapping is explicit and credentials or native account
identifiers remain outside M1 configuration.

Fallible domain operations return a small standard-library-only `Result<T>` containing either a value
or a `DomainError`. `DomainErrorCode` has stable explicitly assigned values, and structured context
uses stable field and collection positions rather than human message text. Validators examine
sections, entities, and fields in their specified canonical order and return the first failure. Thus
the same invalid value yields the same error code and location. Domain validation does not use
exceptions for expected failures; resource exhaustion and programming faults are not disguised as
domain errors.

### Exact numeric values

The common decimal representation is a signed 64-bit coefficient plus a decimal scale from 0 through
18, representing `coefficient * 10^-scale`. Equivalent values compare numerically, and canonical
storage removes redundant trailing zeroes (zero has scale zero). Decimal text parsing is exact and
accepts only ordinary base-10 notation; exponent notation, `NaN`, infinities, locale syntax, and
values outside the representation are rejected. Authored text may contain at most 18 fractional
digits before canonical trailing-zero removal, so the accepted grammar has the same explicit scale
bound as the representation.

Authored integer entry preserves the source type until validation: Boolean values are not numeric
domain inputs, coefficients must fit the signed 64-bit representation, and signed-negative or wide
scale values must fail before conversion to an unsigned or stored scale. These gates return the same
stable field-specific errors as textual validation instead of allowing implicit narrowing to change
the accepted value or its failure identity.

`Price`, `Quantity`, and `Notional` are distinct wrappers over that representation. They have no
implicit conversions between one another and no construction or decision-path conversion from
`float`, `double`, or `long double`. Zero is representable so state can be initialized, while the
owning metadata or order validator imposes positivity where required.

Addition, subtraction, rescaling, multiplication, division, and unit conversion are checked before
changing a coefficient. They return an error instead of overflowing, wrapping, saturating, or
discarding precision. An operation that can lose precision must name both its target scale and one of
these rounding modes: toward zero, away from zero, floor, ceiling, or nearest with ties to even.
There is no default rounding mode.

Instrument tick size, minimum/step quantity, and contract multiplier are positive exact decimals and
carry an `InstrumentMetadataRevision`. Tick and quantity-step validation uses exact integer multiple
tests at a common checked scale. Contract conversion is an explicit dimensional operation using the
metadata's declared contract model and multiplier; no generic `price * quantity` assumption is made
for inverse contracts. The precise M3 order vocabulary and risk economics remain deferred.

### Time, sequences, revisions, and generated identities

Source time, receive time, and processing time are different types. `SourceTimestamp` is normalized
venue time in UTC nanoseconds since the Unix epoch. `ReceiveTimestamp` and `ProcessingTimestamp` are
nanoseconds from an injected monotonic clock's process-local origin; they cannot be compared with
source time. A named checked operation may derive a duration from processing and receive timestamps;
there is no generic cross-type arithmetic. `SessionEpoch`, `SequenceNumber`, and every revision kind
are also distinct checked unsigned types. Revision zero means absent and is invalid for an installed
value; increment overflow is an explicit error.

The same before-narrowing rule applies to provider state. Non-fallible timestamp, sequence, session,
elapsed-time, and deterministic-clock construction accepts only representable unsigned inputs.
Fallible revision, clock-advance, and deterministic order-counter factories retain signedness long
enough to reject negatives through `configuration_revision`, `clock_nanoseconds`, or `order_counter`
errors, and their maximum values fail on the next checked transition rather than wrapping.

Clock and identifier generation are injected capabilities. Tests use a deterministic clock and an
order-ID provider initialized with a fixed namespace and counter. The production provider obtains a
fresh 128-bit namespace from the operating system's cryptographically secure random source at
startup and combines it with a checked 64-bit counter beginning at one. Startup fails if the
namespace cannot be obtained, and exhaustion fails rather than wrapping. This makes domain
`OrderId`s collision-resistant across restart without depending on wall time, host names, or process
IDs. M3 owns any constrained venue client-order-ID encoding and its correlation rules.

## Consequences

- Incorrect identity and unit mixing becomes a compile-time error instead of a convention.
- Replay tests control every clock value and generated identifier without changing production policy.
- Exact arithmetic is more verbose because every lossy boundary requires an explicit policy.
- Stable error codes, canonical parsing, and portable checked arithmetic become compatibility
  contracts and require compile-time input-surface checks plus runtime negative, wide-integer, and
  overflow boundary tests.
- Networking, event dispatch, order semantics, risk, OMS behavior, and venue-native identifier
  encoding remain outside M1.
