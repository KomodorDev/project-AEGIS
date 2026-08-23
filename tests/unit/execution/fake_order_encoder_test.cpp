// Purpose: prove validated default-plus-override encoder scripts, exact no-rounding AEGISFOE bytes,
// bounded failure semantics, and the concrete fake encoder's lack of live capabilities.

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/oms/outbound_oms.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using namespace aegis;

// ########################################################################
// Compile-time probes make accidental transport-shaped fields or methods a structural regression.
template <typename Value>
concept HasEndpoint = requires(Value value) { value.endpoint; };

template <typename Value>
concept HasCredential = requires(Value value) { value.credential; };

template <typename Value>
concept HasSocket = requires(Value value) { value.socket; };

template <typename Value>
concept HasSend = requires(Value value) { value.send(); };

static_assert(std::is_final_v<execution::DeterministicFakeOrderEncoder>);
static_assert(!HasEndpoint<execution::DeterministicFakeOrderEncoder>);
static_assert(!HasCredential<execution::DeterministicFakeOrderEncoder>);
static_assert(!HasSocket<execution::DeterministicFakeOrderEncoder>);
static_assert(!HasSend<execution::DeterministicFakeOrderEncoder>);
static_assert(static_cast<std::uint8_t>(execution::FakeEncodingAction::Encode) == 1U);
static_assert(static_cast<std::uint8_t>(execution::FakeEncodingAction::Fail) == 2U);
static_assert(execution::canonical_fake_order_schema_version == 1U);
static_assert(execution::maximum_encoded_fake_order_bytes == 1'024U);

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture defects rather than encoder behavior under test.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in fake encoder fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build exact nominal decimal inputs without passing through binary floating point.
template <typename Decimal>
[[nodiscard]] Decimal decimal(std::int64_t coefficient, std::uint8_t scale) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid decimal in fake encoder fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Construct one exact positive ordinal or revision for deterministic fixtures.
template <typename Identity> [[nodiscard]] Identity identity(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid identity in fake encoder fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Generate one canonical 24-byte local identity with namespace bytes 00 through 0f.
[[nodiscard]] model::OrderId order_id(std::uint64_t counter = 1U) {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(index);
  }
  auto provider =
      model::DeterministicOrderIdProvider::create(model::OrderNamespace{bytes}, counter);
  if (!provider) {
    throw std::logic_error{"invalid order ID provider in fake encoder fixture"};
  }
  auto generated = provider.value().next();
  if (!generated) {
    throw std::logic_error{"order ID generation failed in fake encoder fixture"};
  }
  return generated.value();
}

// --------------------------------------------------------
// Populate raw fixed-width fingerprints with deterministic consecutive byte ranges.
[[nodiscard]] model::Sha256Digest digest(std::uint8_t first) noexcept {
  model::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = std::byte{static_cast<std::uint8_t>(first + index)};
  }
  return value;
}

// --------------------------------------------------------
// Author the exact provenance whose identifiers and revisions appear in the golden byte vector.
[[nodiscard]] oms::OutboundOrderProvenance provenance() {
  return oms::OutboundOrderProvenance{
      id<model::RouteId>("route.r"),
      id<model::VenueId>("deribit"),
      id<model::LogicalAccountId>("account.a"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      id<model::FirmId>("firm.f"),
      id<model::DeskId>("desk.d"),
      id<model::BotId>("bot.b"),
      id<model::StrategyId>("strategy.s"),
      digest(0x10U),
      identity<model::ConfigurationRevision>(9U),
      identity<model::OrganizationRevision>(2U),
      identity<model::RouteRevision>(3U),
      identity<model::InstrumentMetadataRevision>(4U),
      digest(0x30U),
      digest(0x50U),
      identity<model::RiskPolicyRevision>(5U),
      digest(0x70U),
  };
}

// --------------------------------------------------------
// Couple canonical economics and exposure into one admitted order fixture.
[[nodiscard]] oms::OutboundOrderAdmission admission() {
  const auto quantity = decimal<model::Quantity>(3, 0U);
  return oms::OutboundOrderAdmission{
      identity<model::SubmissionAttemptId>(1U),
      order_id(),
      identity<model::ReservationId>(1U),
      execution::CanonicalOrderEconomics{execution::OrderSide::Sell, execution::OrderType::Limit,
                                         execution::TimeInForce::GoodTilCancelled,
                                         decimal<model::Price>(12'345, 2U), quantity},
      risk::OrderExposure{quantity, decimal<model::Notional>(30, 0U)},
      provenance(),
  };
}

// --------------------------------------------------------
// Admit one record or fail immediately when fixture construction violates the OMS contract.
[[nodiscard]] const oms::OutboundOrderRecord& admit_one(oms::OutboundOms& outbound) {
  auto result = outbound.admit(admission());
  if (!result || !result.value().admitted()) {
    throw std::logic_error{"fake encoder fixture OMS admission failed"};
  }
  return *result.value().record();
}

// --------------------------------------------------------
// Render exact bytes as lowercase hexadecimal for a compact immutable golden assertion.
[[nodiscard]] std::string hex(std::span<const std::byte> bytes) {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    const auto value = std::to_integer<std::uint8_t>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

// --------------------------------------------------------
// Read one big-endian unsigned 16-bit value while advancing a bounded test cursor.
[[nodiscard]] std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t& cursor) {
  if (cursor + 2U > bytes.size()) {
    throw std::logic_error{"truncated u16 in fake encoder fixture"};
  }
  const auto first = std::to_integer<std::uint8_t>(bytes[cursor]);
  const auto second = std::to_integer<std::uint8_t>(bytes[cursor + 1U]);
  cursor += 2U;
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(first) << 8U) | second);
}

// --------------------------------------------------------
// Read one big-endian coefficient bit pattern while advancing a bounded test cursor.
[[nodiscard]] std::int64_t read_i64(std::span<const std::byte> bytes, std::size_t& cursor) {
  if (cursor + 8U > bytes.size()) {
    throw std::logic_error{"truncated i64 in fake encoder fixture"};
  }
  std::uint64_t value = 0U;
  for (std::size_t index = 0U; index < 8U; ++index) {
    value = (value << 8U) | std::to_integer<std::uint8_t>(bytes[cursor + index]);
  }
  cursor += 8U;
  return static_cast<std::int64_t>(value);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Script creation canonicalizes authored order and rejects every ambiguous action/ordinal shape.
TEST_CASE("fake encoder scripts validate and select canonical overrides", "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Sort two authored overrides and prove missing ordinals permanently select the default.
  const auto script = execution::FakeEncoderScript::create(
      execution::FakeEncodingAction::Encode, 3U,
      {{3U, execution::FakeEncodingAction::Fail}, {1U, execution::FakeEncodingAction::Fail}});
  REQUIRE(script);
  REQUIRE(script.value().overrides().size() == 2U);
  CHECK(script.value().overrides()[0U].invocation_ordinal == 1U);
  CHECK(script.value().overrides()[1U].invocation_ordinal == 3U);
  CHECK(script.value().action_for(identity<model::EncoderInvocationOrdinal>(1U)) ==
        execution::FakeEncodingAction::Fail);
  CHECK(script.value().action_for(identity<model::EncoderInvocationOrdinal>(2U)) ==
        execution::FakeEncodingAction::Encode);

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero maximum, unassigned values, zero/out-of-range ordinals, and duplicates all fail closed.
  CHECK_FALSE(execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 0U, {}));
  CHECK_FALSE(
      execution::FakeEncoderScript::create(static_cast<execution::FakeEncodingAction>(0U), 1U, {}));
  CHECK_FALSE(execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 2U,
                                                   {{0U, execution::FakeEncodingAction::Fail}}));
  CHECK_FALSE(execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 2U,
                                                   {{3U, execution::FakeEncodingAction::Fail}}));
  CHECK_FALSE(execution::FakeEncoderScript::create(
      execution::FakeEncodingAction::Encode, 2U,
      {{1U, execution::FakeEncodingAction::Fail}, {1U, execution::FakeEncodingAction::Encode}}));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The exact golden sequence fixes every listed AEGISFOE field, byte order, and omission.
TEST_CASE("fake encoder emits exact AEGISFOE schema-one bytes without rounding",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode one PendingEncoding row under the non-failing default action.
  auto outbound_result = oms::OutboundOms::create(1U);
  REQUIRE(outbound_result);
  const auto& record = admit_one(outbound_result.value());
  auto script = execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 1U, {});
  REQUIRE(script);
  auto encoder = execution::DeterministicFakeOrderEncoder::create(std::move(script).value(), 512U);
  REQUIRE(encoder);
  const auto result = encoder.value().encode(record);
  REQUIRE(result);
  REQUIRE(result.value().encoded());
  REQUIRE(result.value().encoded_order() != nullptr);
  const auto& encoded = *result.value().encoded_order();

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare the complete fixed positional sequence to one immutable lowercase golden vector.
  CHECK(hex(encoded.bytes()) ==
        "4145474953464f450001000102030405060708090a0b0c0d0e0f0000000000000001"
        "0007726f7574652e7200076465726962697400096163636f756e742e6100114254432d"
        "5553442d50455250455455414c000d4254432d50455250455455414c00066669726d"
        "2e6600066465736b2e640005626f742e62000a73747261746567792e730201010000"
        "00000000303902000000000000000300101112131415161718191a1b1c1d1e1f20"
        "2122232425262728292a2b2c2d2e2f000000000000000200000000000000030000"
        "000000000004303132333435363738393a3b3c3d3e3f404142434445464748494a"
        "4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b"
        "6c6d6e6f0000000000000005707172737475767778797a7b7c7d7e7f8081828384"
        "85868788898a8b8c8d8e8f0000000000000001");

  // ++++++++++++++++++++++++++++++++++++++++
  // Decode only the economics positions and prove coefficients/scales are exactly the OMS values.
  std::size_t cursor = 8U;
  CHECK(read_u16(encoded.bytes(), cursor) == execution::canonical_fake_order_schema_version);
  cursor += model::OrderId::byte_size;
  for (std::size_t identifier = 0U; identifier < 9U; ++identifier) {
    cursor += read_u16(encoded.bytes(), cursor);
  }
  REQUIRE(cursor + 21U <= encoded.bytes().size());
  CHECK(std::to_integer<std::uint8_t>(encoded.bytes()[cursor++]) ==
        static_cast<std::uint8_t>(execution::OrderSide::Sell));
  CHECK(std::to_integer<std::uint8_t>(encoded.bytes()[cursor++]) == 1U);
  CHECK(std::to_integer<std::uint8_t>(encoded.bytes()[cursor++]) == 1U);
  CHECK(read_i64(encoded.bytes(), cursor) == record.economics().price.coefficient());
  CHECK(std::to_integer<std::uint8_t>(encoded.bytes()[cursor++]) ==
        record.economics().price.scale());
  CHECK(read_i64(encoded.bytes(), cursor) == record.economics().quantity.coefficient());
  CHECK(std::to_integer<std::uint8_t>(encoded.bytes()[cursor]) ==
        record.economics().quantity.scale());
  CHECK(encoded.attempt_id() == record.attempt_id());
  CHECK(encoded.invocation_ordinal().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every reached call consumes its action; scripted failure creates no bytes and does not exhaust
// the permanent default before the validated maximum invocation count.
TEST_CASE("fake encoder consumes scripted failures exactly once", "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Override only invocation one to fail, leaving invocation two on the successful default.
  auto outbound_result = oms::OutboundOms::create(1U);
  REQUIRE(outbound_result);
  const auto& record = admit_one(outbound_result.value());
  auto script = execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 2U,
                                                     {{1U, execution::FakeEncodingAction::Fail}});
  REQUIRE(script);
  auto encoder = execution::DeterministicFakeOrderEncoder::create(std::move(script).value(), 512U);
  REQUIRE(encoder);

  // ++++++++++++++++++++++++++++++++++++++++
  // The first ordinary failure retains its invocation but has no encoded object.
  const auto failed = encoder.value().encode(record);
  REQUIRE(failed);
  CHECK(failed.value().action() == execution::FakeEncodingAction::Fail);
  CHECK(failed.value().invocation_ordinal().value() == 1U);
  CHECK_FALSE(failed.value().encoded());

  // ++++++++++++++++++++++++++++++++++++++++
  // The next reached call selects invocation two, proving no implicit retry of the first action.
  const auto succeeded = encoder.value().encode(record);
  REQUIRE(succeeded);
  CHECK(succeeded.value().action() == execution::FakeEncodingAction::Encode);
  CHECK(succeeded.value().invocation_ordinal().value() == 2U);
  CHECK(succeeded.value().encoded());
  CHECK(encoder.value().invocations_consumed() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Calls beyond the validated outer-attempt bound are impossible fake state, not script
  // exhaustion.
  const auto exhausted = encoder.value().encode(record);
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::InvalidFakeState);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Policy-invalid capacities fail at construction, while an undersized otherwise-positive capacity
// is treated as an internal composition fault and never as scripted EncodingFailed.
TEST_CASE("fake encoder bounds every byte result and separates policy from internal failure",
          "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero and values above the compiled 1,024-byte ceiling cannot construct the concrete fake.
  auto script = execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 1U, {});
  REQUIRE(script);
  CHECK_FALSE(execution::DeterministicFakeOrderEncoder::create(script.value(), 0U));
  CHECK_FALSE(execution::DeterministicFakeOrderEncoder::create(script.value(), 1'025U));

  // ++++++++++++++++++++++++++++++++++++++++
  // A submission policy should preclude this undersized case; direct misuse fails without bytes.
  auto outbound_result = oms::OutboundOms::create(1U);
  REQUIRE(outbound_result);
  const auto& record = admit_one(outbound_result.value());
  auto undersized = execution::DeterministicFakeOrderEncoder::create(std::move(script).value(), 1U);
  REQUIRE(undersized);
  const auto result = undersized.value().encode(record);
  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_field(model::DomainErrorCode::InvalidFakeState,
                                                       "fake_order_encoder.bytes"));
  CHECK(undersized.value().invocations_consumed() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Encoding accepts only PendingEncoding rows; a repeated operation consumes its selected action but
// cannot reinterpret or mutate an already-advanced OMS record.
TEST_CASE("fake encoder rejects wrong OMS source state without mutation", "[execution][fake]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Advance one successfully encoded row exactly as the coordinator would before initiation.
  auto outbound_result = oms::OutboundOms::create(1U);
  REQUIRE(outbound_result);
  const auto& record = admit_one(outbound_result.value());
  auto script = execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, 2U, {});
  REQUIRE(script);
  auto encoder = execution::DeterministicFakeOrderEncoder::create(std::move(script).value(), 512U);
  REQUIRE(encoder);
  REQUIRE(encoder.value().encode(record));
  REQUIRE(outbound_result.value().mark_encoding_succeeded(record.order_id()));

  // ++++++++++++++++++++++++++++++++++++++++
  // Repeating encode consumes invocation two, returns InvalidFakeState, and leaves OMS unchanged.
  const auto repeated = encoder.value().encode(record);
  REQUIRE_FALSE(repeated);
  CHECK(repeated.error().code == model::DomainErrorCode::InvalidFakeState);
  CHECK(encoder.value().invocations_consumed() == 2U);
  CHECK(record.state() == oms::OutboundOrderState::PendingInitiation);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
