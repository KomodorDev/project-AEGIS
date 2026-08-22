// Purpose: prove deterministic fake initiation consumes each scripted action once, copies exact
// bytes at the acceptance boundary, and treats all pre-copy capacity failure as definitive.

#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/oms/outbound_oms.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Compile-time probes protect the concrete initiator from gaining any generic live capability.
template <typename Value>
concept HasEndpoint = requires(Value value) { value.endpoint; };

template <typename Value>
concept HasCredential = requires(Value value) { value.credential; };

template <typename Value>
concept HasSocket = requires(Value value) { value.socket; };

template <typename Value>
concept HasConnect = requires(Value value) { value.connect(); };

template <typename Value>
concept HasRetry = requires(Value value) { value.retry(); };

static_assert(std::is_final_v<execution::DeterministicFakeWriteInitiator>);
static_assert(!HasEndpoint<execution::DeterministicFakeWriteInitiator>);
static_assert(!HasCredential<execution::DeterministicFakeWriteInitiator>);
static_assert(!HasSocket<execution::DeterministicFakeWriteInitiator>);
static_assert(!HasConnect<execution::DeterministicFakeWriteInitiator>);
static_assert(!HasRetry<execution::DeterministicFakeWriteInitiator>);
static_assert(static_cast<std::uint8_t>(
                  execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance) == 1U);
static_assert(static_cast<std::uint8_t>(execution::FakeInitiationOutcome::AcceptedAndInitiated) ==
              2U);
static_assert(
    static_cast<std::uint8_t>(execution::FakeInitiationOutcome::AcceptedThenOutcomeLost) == 3U);

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture defects rather than fake-initiation behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in fake initiator fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build exact nominal decimal inputs without binary floating-point conversion.
template <typename Decimal>
[[nodiscard]] Decimal decimal(std::int64_t coefficient, std::uint8_t scale) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid decimal in fake initiator fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Construct one exact positive ordinal or revision for deterministic fixtures.
template <typename Identity> [[nodiscard]] Identity identity(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid identity in fake initiator fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Generate one canonical local identity with a stable namespace and selected counter.
[[nodiscard]] model::OrderId order_id(std::uint64_t counter) {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(0xa0U + index);
  }
  auto provider =
      model::DeterministicOrderIdProvider::create(model::OrderNamespace{bytes}, counter);
  if (!provider) {
    throw std::logic_error{"invalid order ID provider in fake initiator fixture"};
  }
  auto generated = provider.value().next();
  if (!generated) {
    throw std::logic_error{"order ID generation failed in fake initiator fixture"};
  }
  return generated.value();
}

// --------------------------------------------------------
// Fill one raw fingerprint deterministically while keeping the fake independent from wrappers.
[[nodiscard]] model::Sha256Digest digest(std::uint8_t value) noexcept {
  model::Sha256Digest result{};
  result.fill(std::byte{value});
  return result;
}

// --------------------------------------------------------
// Construct a complete admitted order for the exact fake encoder used by initiator tests.
[[nodiscard]] oms::OutboundOrderAdmission admission(std::uint64_t attempt_value,
                                                    std::uint64_t order_counter) {
  const auto quantity = decimal<model::Quantity>(2, 0U);
  return oms::OutboundOrderAdmission{
      identity<model::SubmissionAttemptId>(attempt_value),
      order_id(order_counter),
      identity<model::ReservationId>(attempt_value),
      execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                         execution::TimeInForce::GoodTilCancelled,
                                         decimal<model::Price>(25'050, 1U), quantity},
      risk::OrderExposure{quantity, decimal<model::Notional>(20, 0U)},
      oms::OutboundOrderProvenance{
          id<model::RouteId>("route.fake"),
          id<model::VenueId>("deribit"),
          id<model::LogicalAccountId>("account.fake"),
          id<model::InstrumentId>("BTC-USD-PERPETUAL"),
          id<model::VenueInstrumentId>("BTC-PERPETUAL"),
          id<model::FirmId>("firm.fake"),
          id<model::DeskId>("desk.fake"),
          id<model::BotId>("bot.fake"),
          id<model::StrategyId>("strategy.fake"),
          digest(0x11U),
          identity<model::ConfigurationRevision>(1U),
          identity<model::OrganizationRevision>(1U),
          identity<model::RouteRevision>(1U),
          identity<model::InstrumentMetadataRevision>(1U),
          digest(0x22U),
          digest(0x33U),
          identity<model::RiskPolicyRevision>(1U),
          digest(0x44U),
      },
  };
}

// --------------------------------------------------------
// Produce one exact EncodedFakeOrder using only the concrete offline OMS/encoder path.
[[nodiscard]] execution::EncodedFakeOrder encoded_order(std::uint64_t attempt_value = 1U,
                                                        std::uint64_t order_counter = 1U) {
  auto outbound = oms::OutboundOms::create(1U);
  if (!outbound) {
    throw std::logic_error{"failed to create OMS in fake initiator fixture"};
  }
  auto admitted = outbound.value().admit(admission(attempt_value, order_counter));
  if (!admitted || !admitted.value().admitted()) {
    throw std::logic_error{"failed to admit OMS row in fake initiator fixture"};
  }
  auto script = execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 1U, {});
  if (!script) {
    throw std::logic_error{"failed to create encoder script in fake initiator fixture"};
  }
  auto encoder = execution::DeterministicFakeOrderEncoder::create(std::move(script).value(), 512U);
  if (!encoder) {
    throw std::logic_error{"failed to create encoder in fake initiator fixture"};
  }
  auto encoded = encoder.value().encode(*admitted.value().record());
  if (!encoded || !encoded.value().encoded()) {
    throw std::logic_error{"failed to encode order in fake initiator fixture"};
  }
  return *encoded.value().encoded_order();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Script creation canonicalizes authored order and rejects every ambiguous outcome/ordinal shape.
TEST_CASE("fake initiator scripts validate and select canonical overrides", "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Sort authored overrides and prove an absent ordinal uses the permanent default outcome.
  const auto script = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, 3U,
      {{3U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost},
       {1U, execution::FakeInitiationOutcome::AcceptedAndInitiated}});
  REQUIRE(script);
  REQUIRE(script.value().overrides().size() == 2U);
  CHECK(script.value().overrides()[0U].invocation_ordinal == 1U);
  CHECK(script.value().outcome_for(identity<model::InitiatorInvocationOrdinal>(1U)) ==
        execution::FakeInitiationOutcome::AcceptedAndInitiated);
  CHECK(script.value().outcome_for(identity<model::InitiatorInvocationOrdinal>(2U)) ==
        execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance);

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero maximum, unassigned values, zero/out-of-range ordinals, and duplicates all fail closed.
  CHECK_FALSE(execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 0U, {}));
  CHECK_FALSE(execution::FakeInitiatorScript::create(
      static_cast<execution::FakeInitiationOutcome>(0U), 1U, {}));
  CHECK_FALSE(execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 2U,
      {{0U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}}));
  CHECK_FALSE(execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 2U,
      {{3U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}}));
  CHECK_FALSE(execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 2U,
      {{1U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost},
       {1U, execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance}}));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The three actions consume consecutive invocations, but only accepted actions cross the slot-copy
// boundary and receive consecutive accepted-write identities.
TEST_CASE("fake initiator distinguishes definite, initiated, and uncertain outcomes",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Script accepted success, accepted uncertainty, then a definite pre-copy failure.
  auto script = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, 3U,
      {{1U, execution::FakeInitiationOutcome::AcceptedAndInitiated},
       {2U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}});
  REQUIRE(script);
  auto initiator =
      execution::DeterministicFakeWriteInitiator::create(std::move(script).value(), 2U);
  REQUIRE(initiator);
  const auto encoded = encoded_order();
  execution::DeterministicSubmissionMeasurementClock measurement_clock{
      std::vector<std::optional<std::uint64_t>>{101U, 202U, 303U}};

  // ++++++++++++++++++++++++++++++++++++++++
  // Accepted-and-initiated copies first and receives accepted-write ordinal one.
  const auto initiated = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(initiated);
  CHECK(initiated.value().invocation_ordinal().value() == 1U);
  CHECK(initiated.value().outcome() == execution::FakeInitiationOutcome::AcceptedAndInitiated);
  REQUIRE(initiated.value().write_ordinal().has_value());
  CHECK(initiated.value().write_ordinal()->value() == 1U);
  CHECK(initiated.value().accepted_slot_endpoint_nanoseconds() == 101U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Accepted-then-lost also copies first and receives the next accepted-write identity.
  const auto uncertain = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(uncertain);
  CHECK(uncertain.value().invocation_ordinal().value() == 2U);
  CHECK(uncertain.value().outcome() == execution::FakeInitiationOutcome::AcceptedThenOutcomeLost);
  REQUIRE(uncertain.value().write_ordinal().has_value());
  CHECK(uncertain.value().write_ordinal()->value() == 2U);
  CHECK(uncertain.value().accepted_slot_endpoint_nanoseconds() == 202U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The definite action consumes invocation three but assigns no slot or accepted-write ordinal.
  const auto failed = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(failed);
  CHECK(failed.value().invocation_ordinal().value() == 3U);
  CHECK(failed.value().outcome() ==
        execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance);
  CHECK_FALSE(failed.value().write_ordinal().has_value());
  CHECK_FALSE(failed.value().accepted_slot_endpoint_nanoseconds().has_value());
  CHECK(initiator.value().invocations_consumed() == 3U);
  CHECK(measurement_clock.readings_consumed() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Accepted slots retain exact bytes and every local identity needed for deterministic inspection.
TEST_CASE("fake accepted-write slots retain exact immutable provenance and bytes",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Cross the accepted-copy boundary once under the successful default action.
  auto script = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 1U, {});
  REQUIRE(script);
  auto initiator =
      execution::DeterministicFakeWriteInitiator::create(std::move(script).value(), 1U);
  REQUIRE(initiator);
  const auto encoded = encoded_order(7U, 9U);
  execution::SteadySubmissionMeasurementClock measurement_clock;
  const auto result = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(result);
  REQUIRE(result.value().accepted());
  REQUIRE(result.value().accepted_slot_endpoint_nanoseconds().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Inspect the fixed slot and prove all identities and every byte equal the accepted input.
  const auto writes = initiator.value().accepted_writes();
  REQUIRE(writes.size() == 1U);
  CHECK(writes[0U].attempt_id() == encoded.attempt_id());
  CHECK(writes[0U].encoder_invocation_ordinal() == encoded.invocation_ordinal());
  CHECK(writes[0U].initiator_invocation_ordinal().value() == 1U);
  CHECK(writes[0U].write_ordinal().value() == 1U);
  CHECK(writes[0U].byte_length() == encoded.byte_length());
  CHECK(std::equal(writes[0U].bytes().begin(), writes[0U].bytes().end(), encoded.bytes().begin(),
                   encoded.bytes().end()));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Full slot capacity is checked before copying, converts any would-be accepted action to a definite
// failure, and still consumes that invocation/action without assigning a write ordinal.
TEST_CASE("fake initiator capacity failure is definitive and consumes its action",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Both invocations select accepted outcomes, but the fixed slot array can retain only the first.
  auto script = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 2U,
      {{2U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}});
  REQUIRE(script);
  auto initiator =
      execution::DeterministicFakeWriteInitiator::create(std::move(script).value(), 1U);
  REQUIRE(initiator);
  const auto encoded = encoded_order();
  execution::SteadySubmissionMeasurementClock measurement_clock;
  REQUIRE(initiator.value().initiate(encoded, measurement_clock));

  // ++++++++++++++++++++++++++++++++++++++++
  // Invocation two remains consumed but reports a definite pre-copy failure with no new slot.
  const auto full = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(full);
  CHECK(full.value().invocation_ordinal().value() == 2U);
  CHECK(full.value().outcome() ==
        execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance);
  CHECK_FALSE(full.value().accepted());
  CHECK(initiator.value().invocations_consumed() == 2U);
  CHECK(initiator.value().accepted_writes().size() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Invalid construction and out-of-bound invocation are explicit local fake errors; neither can
// copy bytes or masquerade as a post-acceptance unknown outcome.
TEST_CASE("fake initiator fails closed outside validated capacity and invocation bounds",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero accepted-write capacity is invalid AEGISSUP construction.
  auto script = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, 1U, {});
  REQUIRE(script);
  CHECK_FALSE(execution::DeterministicFakeWriteInitiator::create(script.value(), 0U));

  // ++++++++++++++++++++++++++++++++++++++++
  // After one definite scripted invocation, a second call is impossible and adds no accepted slot.
  auto initiator =
      execution::DeterministicFakeWriteInitiator::create(std::move(script).value(), 1U);
  REQUIRE(initiator);
  const auto encoded = encoded_order();
  execution::SteadySubmissionMeasurementClock measurement_clock;
  const auto first = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE(first);
  CHECK_FALSE(first.value().accepted());
  const auto beyond = initiator.value().initiate(encoded, measurement_clock);
  REQUIRE_FALSE(beyond);
  CHECK(beyond.error().code == model::DomainErrorCode::InvalidFakeState);
  CHECK(initiator.value().accepted_writes().empty());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
