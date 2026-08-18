// Purpose: prove clocks, timestamps, sequences, sessions, and revisions remain deterministic.

#include "aegis/model/time.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace {

using namespace aegis::model;

template <typename Value>
concept HasRevisionFactory = requires(Value value) { ConfigurationRevision::from_value(value); };

template <typename Value>
concept HasClockAdvance =
    requires(DeterministicClockProvider clock, Value value) { clock.advance(value); };

static_assert(!std::is_same_v<SourceTimestamp, ReceiveTimestamp>);
static_assert(!std::is_same_v<ReceiveTimestamp, ProcessingTimestamp>);
static_assert(!std::is_convertible_v<SourceTimestamp, ReceiveTimestamp>);
static_assert(!std::is_same_v<ConfigurationRevision, OrganizationRevision>);
static_assert(!std::is_same_v<InstrumentMetadataRevision, RouteRevision>);
static_assert(!std::is_convertible_v<ConfigurationRevision, OrganizationRevision>);
static_assert(!std::is_same_v<SessionEpoch, SequenceNumber>);
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

TEST_CASE("a deterministic clock returns explicitly advanced monotonic values", "[model][time]") {
  DeterministicClockProvider clock{7U};

  CHECK(clock.receive_now() == ReceiveTimestamp{7U});
  REQUIRE(clock.advance(5U));
  CHECK(clock.processing_now() == ProcessingTimestamp{12U});
}

TEST_CASE("a deterministic clock fails rather than wrapping", "[model][time]") {
  DeterministicClockProvider clock{std::numeric_limits<std::uint64_t>::max()};

  const auto result = clock.advance(1U);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(result.error().context.field == "clock_nanoseconds");

  DeterministicClockProvider ordinary_clock;
  const auto negative = ordinary_clock.advance(-1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() ==
        DomainError::at_field(DomainErrorCode::ArithmeticOverflow, "clock_nanoseconds"));
}

TEST_CASE("processing delay is the only named receive-to-processing subtraction", "[model][time]") {
  const auto delay = processing_delay(ProcessingTimestamp{19U}, ReceiveTimestamp{11U});
  REQUIRE(delay);
  CHECK(delay.value() == ElapsedNanoseconds{8U});

  const auto reversed = processing_delay(ProcessingTimestamp{10U}, ReceiveTimestamp{11U});
  REQUIRE_FALSE(reversed);
  CHECK(reversed.error().code == DomainErrorCode::InvalidTimestampOrder);
}

TEST_CASE("session and sequence increments are checked", "[model][time]") {
  const auto next_epoch = SessionEpoch{4U}.next();
  REQUIRE(next_epoch);
  CHECK(next_epoch.value().value() == 5U);

  const auto exhausted = SequenceNumber{std::numeric_limits<std::uint64_t>::max()}.next();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::ArithmeticOverflow);
}

TEST_CASE("installed revisions reject zero and fail on increment overflow", "[model][time]") {
  const auto absent = ConfigurationRevision::from_value(0U);
  REQUIRE_FALSE(absent);
  CHECK(absent.error().code == DomainErrorCode::InvalidRevision);

  const auto negative = ConfigurationRevision::from_value(-1);
  REQUIRE_FALSE(negative);
  CHECK(negative.error() ==
        DomainError::at_field(DomainErrorCode::InvalidRevision, "configuration_revision"));

  const auto narrow_revision = ConfigurationRevision::from_value(static_cast<unsigned char>(2));
  REQUIRE(narrow_revision);
  CHECK(narrow_revision.value().value() == 2U);

  CHECK(OrganizationRevision::initial().value() == 1U);

  const auto maximum = RouteRevision::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum);
  const auto exhausted = maximum.value().next();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == DomainErrorCode::ArithmeticOverflow);
  CHECK(exhausted.error().context.field == "route_revision");
}

} // namespace
