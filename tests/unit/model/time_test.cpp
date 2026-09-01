// Purpose: prove time-domain separation, checked counters/revisions, and injected monotonic clocks
// remain deterministic across valid and unsafe integral inputs.

#include "aegis/model/time.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
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
    requires(DeterministicClockProvider clock, Value value) { clock.advance_nanoseconds(value); };

template <typename Value>
concept HasAdmissionOrdinalFactory = requires(Value value) { AdmissionOrdinal::from_value(value); };

// --------------------------------------------------------
// Exercise one nominal ordinal's terminal value without duplicating unchecked fixture arithmetic.
template <typename Ordinal>
void check_ordinal_exhaustion(DomainErrorCode code, std::string_view field) {
  const auto maximum = Ordinal::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum);
  const auto exhausted = maximum.value().derive_next_ordinal();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error() == DomainError::create_at_field(code, std::string{field}));
}

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
static_assert(!std::is_same_v<AdmissionOrdinal, ReceiveSequence>);
static_assert(!std::is_same_v<TurnOrdinal, CallbackOrdinal>);
static_assert(!std::is_same_v<BookGeneration, BookRevision>);

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
static_assert(!HasAdmissionOrdinalFactory<char>);
static_assert(HasAdmissionOrdinalFactory<signed char>);

// ########################################################################

// --------------------------------------------------------
// An injected clock changes only when the test advances it and returns distinct receive/processing
// timestamp types over the same controlled monotonic counter.
TEST_CASE("a deterministic clock returns explicitly advanced monotonic values", "[model][time]") {
  DeterministicClockProvider clock{7U};

  CHECK(clock.receive_timestamp_now() == ReceiveTimestamp{7U});
  REQUIRE(clock.advance_nanoseconds(5U));
  CHECK(clock.processing_timestamp_now() == ProcessingTimestamp{12U});
}

// --------------------------------------------------------
// Advancing is atomic on both state overflow and negative signed input, and both failures retain
// the clock_nanoseconds field.
TEST_CASE("a deterministic clock fails rather than wrapping", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove maximum-state overflow reports failure without wrapping the controlled clock.
  DeterministicClockProvider clock{std::numeric_limits<std::uint64_t>::max()};

  const auto result = clock.advance_nanoseconds(1U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(result.error().context.field == "clock_nanoseconds");

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove signed negative input reaches the same stable field-level failure boundary.
  DeterministicClockProvider ordinary_clock;
  const auto negative = ordinary_clock.advance_nanoseconds(-1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() ==
        DomainError::create_at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The named cross-type operation returns elapsed time only for a valid receive-before-processing
// ordering, avoiding generic timestamp arithmetic.
TEST_CASE("processing delay is the only named receive-to-processing subtraction", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept a forward interval and expose its exact elapsed duration.
  const auto delay = calculate_processing_delay(ProcessingTimestamp{19U}, ReceiveTimestamp{11U});
  REQUIRE(delay);
  CHECK(delay.value() == ElapsedNanoseconds{8U});

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject reversed timestamps instead of permitting unsigned subtraction wraparound.
  const auto reversed = calculate_processing_delay(ProcessingTimestamp{10U}, ReceiveTimestamp{11U});
  REQUIRE_FALSE(reversed);
  CHECK(reversed.error().code == DomainErrorCode::InvalidTimestampOrder);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Protocol counters increment normally but report an error instead of wrapping at UINT64_MAX.
TEST_CASE("session and sequence increments are checked", "[model][time]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin ordinary advancement for one nominal counter type.
  const auto next_epoch = SessionEpoch{4U}.derive_next_value();
  REQUIRE(next_epoch);
  CHECK(next_epoch.value().value() == 5U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin exhaustion for the other nominal counter type at the shared storage boundary.
  const auto exhausted =
      SequenceNumber{std::numeric_limits<std::uint64_t>::max()}.derive_next_value();
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
        DomainError::create_at_field(DomainErrorCode::InvalidRevision, "configuration_revision"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Unsigned char must remain a usable numeric revision source after the portable concept filter.
  const auto narrow_revision = ConfigurationRevision::from_value(static_cast<unsigned char>(2));
  REQUIRE(narrow_revision);
  CHECK(narrow_revision.value().value() == 2U);

  CHECK(OrganizationRevision::create_initial().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject incrementing the final representable revision instead of wrapping to absence.
  const auto maximum = RouteRevision::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum);
  const auto exhausted = maximum.value().derive_next_revision();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(exhausted.error().context.field == "route_revision");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// M2 ordinals reject the zero absence sentinel, remain nominally distinct, and report their owning
// subsystem's exhaustion code rather than wrapping.
TEST_CASE("runtime and market ordinals are one based and checked", "[model][time][m2]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absent and signed-negative admission positions before narrowing.
  const auto zero = AdmissionOrdinal::from_value(0U);
  const auto negative = AdmissionOrdinal::from_value(-1);
  REQUIRE_FALSE(zero);
  REQUIRE_FALSE(negative);
  CHECK(zero.error() ==
        DomainError::create_at_field(DomainErrorCode::InvalidValue, "admission_ordinal"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin initial and ordinary advancement across independent runtime and market domains.
  CHECK(ReceiveSequence::create_initial().value() == 1U);
  REQUIRE(TurnOrdinal::from_value(8U));
  CHECK(TurnOrdinal::from_value(8U).value().derive_next_ordinal().value().value() == 9U);
  CHECK(BookGeneration::create_initial().value() == 1U);
  CHECK(BookRevision::create_initial().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Exhaustion reports every owning domain and stable field while preserving the maximum ordinal.
  check_ordinal_exhaustion<MarketSourceOrdinal>(DomainErrorCode::ExecutorCounterExhausted,
                                                "market_source_ordinal");
  check_ordinal_exhaustion<AdmissionOrdinal>(DomainErrorCode::ExecutorCounterExhausted,
                                             "admission_ordinal");
  check_ordinal_exhaustion<ReceiveSequence>(DomainErrorCode::ExecutorCounterExhausted,
                                            "receive_sequence");
  check_ordinal_exhaustion<TurnOrdinal>(DomainErrorCode::ExecutorCounterExhausted, "turn_ordinal");
  check_ordinal_exhaustion<CallbackOrdinal>(DomainErrorCode::CallbackCounterExhausted,
                                            "callback_ordinal");
  check_ordinal_exhaustion<BookGeneration>(DomainErrorCode::MarketCounterExhausted,
                                           "book_generation");
  check_ordinal_exhaustion<BookRevision>(DomainErrorCode::MarketCounterExhausted, "book_revision");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
