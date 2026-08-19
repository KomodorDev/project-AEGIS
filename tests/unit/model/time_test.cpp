// Purpose: prove time-domain separation, checked counters/revisions, and injected monotonic clocks
// remain deterministic across valid and unsafe integral inputs.

#include "aegis/model/time.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using namespace aegis::model;

// ########################################################################
// Interesting syntax: requires-expressions verify fallible factories reject bool and plain
// character sources at overload resolution. Runtime narrow-integer cases instantiate accepted
// bodies, keeping concept membership aligned with std::in_range compatibility.
template <typename Value>
concept HasRevisionFactory = requires(Value value) { ConfigurationRevision::from_value(value); };

template <typename Value>
concept HasClockAdvance =
    requires(DeterministicClockProvider clock, Value value) { clock.advance(value); };

// ########################################################################
// Nominal time and revision domains must never become interchangeable even though their storage is
// the same unsigned width.
static_assert(!std::is_same_v<SourceTimestamp, ReceiveTimestamp>);
static_assert(!std::is_same_v<ReceiveTimestamp, ProcessingTimestamp>);
static_assert(!std::is_convertible_v<SourceTimestamp, ReceiveTimestamp>);
static_assert(!std::is_same_v<ConfigurationRevision, OrganizationRevision>);
static_assert(!std::is_same_v<InstrumentMetadataRevision, RouteRevision>);
static_assert(!std::is_convertible_v<ConfigurationRevision, OrganizationRevision>);
static_assert(!std::is_same_v<SessionEpoch, SequenceNumber>);

// ########################################################################
// Non-fallible constructors accept unsigned char but reject all signed, bool, enum, floating, and
// plain/wide/Unicode character sources. Fallible factories also admit signed char so negatives can
// become stable DomainError values instead of narrowing.
static_assert(std::is_constructible_v<SourceTimestamp, std::uint64_t>);
static_assert(!std::is_constructible_v<SourceTimestamp, std::int64_t>);
static_assert(!std::is_constructible_v<SourceTimestamp, char>);
static_assert(!std::is_constructible_v<SessionEpoch, int>);
static_assert(!std::is_constructible_v<SessionEpoch, char>);
static_assert(!std::is_constructible_v<ElapsedNanoseconds, double>);
static_assert(!std::is_constructible_v<DeterministicClockProvider, int>);
static_assert(!std::is_constructible_v<DeterministicClockProvider, char>);
static_assert(!HasRevisionFactory<char>);
static_assert(!HasRevisionFactory<bool>);
static_assert(HasRevisionFactory<signed char>);
static_assert(!HasClockAdvance<char>);
static_assert(!HasClockAdvance<bool>);
static_assert(HasClockAdvance<unsigned char>);

// ########################################################################

// --------------------------------------------------------
// An injected clock changes only when the test advances it and returns distinct receive/processing
// timestamp types over the same controlled monotonic counter.
TEST_CASE("a deterministic clock returns explicitly advanced monotonic values", "[model][time]") {
  DeterministicClockProvider clock{7U};

  CHECK(clock.receive_now() == ReceiveTimestamp{7U});
  REQUIRE(clock.advance(5U));
  CHECK(clock.processing_now() == ProcessingTimestamp{12U});
}

// --------------------------------------------------------
// Advancing is atomic on both state overflow and negative signed input, and both failures retain
// the clock_nanoseconds field.
TEST_CASE("a deterministic clock fails rather than wrapping", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove maximum-state overflow reports failure without wrapping the controlled clock.
  DeterministicClockProvider clock{std::numeric_limits<std::uint64_t>::max()};

  const auto result = clock.advance(1U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(result.error().context.field == "clock_nanoseconds");

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove signed negative input reaches the same stable field-level failure boundary.
  DeterministicClockProvider ordinary_clock;
  const auto negative = ordinary_clock.advance(-1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() ==
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The named cross-type operation returns elapsed time only for a valid receive-before-processing
// ordering, avoiding generic timestamp arithmetic.
TEST_CASE("processing delay is the only named receive-to-processing subtraction", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept a forward interval and expose its exact elapsed duration.
  const auto delay = processing_delay(ProcessingTimestamp{19U}, ReceiveTimestamp{11U});
  REQUIRE(delay);
  CHECK(delay.value() == ElapsedNanoseconds{8U});

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject reversed timestamps instead of permitting unsigned subtraction wraparound.
  const auto reversed = processing_delay(ProcessingTimestamp{10U}, ReceiveTimestamp{11U});
  REQUIRE_FALSE(reversed);
  CHECK(reversed.error().code == DomainErrorCode::InvalidTimestampOrder);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Protocol counters increment normally but report an error instead of wrapping at UINT64_MAX.
TEST_CASE("session and sequence increments are checked", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin ordinary advancement for one nominal counter type.
  const auto next_epoch = SessionEpoch{4U}.next();
  REQUIRE(next_epoch);
  CHECK(next_epoch.value().value() == 5U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin exhaustion for the other nominal counter type at the shared storage boundary.
  const auto exhausted = SequenceNumber{std::numeric_limits<std::uint64_t>::max()}.next();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::ArithmeticOverflow);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Installed revision zero and negative entry both report the nominal revision field; the final
// assigned revision cannot increment into wraparound.
TEST_CASE("installed revisions reject zero and fail on increment overflow", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absent and negative authored revisions through stable nominal errors.
  const auto absent = ConfigurationRevision::from_value(0U);
  REQUIRE_FALSE(absent);
  CHECK(absent.error().code == DomainErrorCode::InvalidRevision);

  const auto negative = ConfigurationRevision::from_value(-1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() ==
        DomainError::at_field(DomainErrorCode::InvalidRevision, "configuration_revision"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Unsigned char must remain a usable numeric revision source after the portable concept filter.
  const auto narrow_revision = ConfigurationRevision::from_value(static_cast<unsigned char>(2));
  REQUIRE(narrow_revision);
  CHECK(narrow_revision.value().value() == 2U);

  CHECK(OrganizationRevision::initial().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject incrementing the final representable revision instead of wrapping to absence.
  const auto maximum = RouteRevision::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum);
  const auto exhausted = maximum.value().next();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(exhausted.error().context.field == "route_revision");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
