// Purpose: prove fixed-capacity collision-safe OMS admission, provenance retention, duplicate
// precedence, and every allowed or forbidden M3 outbound state transition.

#include "aegis/oms/outbound_oms.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace aegis;

// ########################################################################
// Persisted state bytes are compatibility assignments and must never drift with declaration order.
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::PendingEncoding) == 1U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::PendingInitiation) == 2U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::WriteInitiated) == 3U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::SubmissionUnknown) == 4U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::LocallyFailed) == 5U);

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture defects rather than OMS behavior under test.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in outbound OMS fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build exact positive nominal decimals without allowing binary floating-point input.
template <typename Decimal>
[[nodiscard]] Decimal decimal(std::int64_t coefficient, std::uint8_t scale) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid decimal in outbound OMS fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Construct one exact positive ordinal/revision or fail fast on a broken fixture value.
template <typename Identity> [[nodiscard]] Identity identity(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid ordinal in outbound OMS fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Generate one canonical local identity from a fixed namespace and caller-selected counter.
[[nodiscard]] model::OrderId order_id(std::uint64_t counter) {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(index);
  }
  auto provider =
      model::DeterministicOrderIdProvider::create(model::OrderNamespace{bytes}, counter);
  if (!provider) {
    throw std::logic_error{"invalid order ID provider in outbound OMS fixture"};
  }
  auto generated = provider.value().next();
  if (!generated) {
    throw std::logic_error{"order ID generation failed in outbound OMS fixture"};
  }
  return generated.value();
}

// --------------------------------------------------------
// Populate distinguishable raw fingerprints without introducing wrapper/runtime dependencies.
[[nodiscard]] model::Sha256Digest digest(std::uint8_t first) noexcept {
  model::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = std::byte{static_cast<std::uint8_t>(first + index)};
  }
  return value;
}

// --------------------------------------------------------
// Build the complete immutable provenance projection retained by every admitted row.
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
      identity<model::ConfigurationRevision>(1U),
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
// Couple exact validated economics and once-rounded exposure to one attempt-derived reservation.
[[nodiscard]] oms::OutboundOrderAdmission admission(std::uint64_t attempt_value,
                                                    std::uint64_t order_counter) {
  const auto quantity = decimal<model::Quantity>(3, 0U);
  return oms::OutboundOrderAdmission{
      identity<model::SubmissionAttemptId>(attempt_value),
      order_id(order_counter),
      identity<model::ReservationId>(attempt_value),
      execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                         execution::TimeInForce::GoodTilCancelled,
                                         decimal<model::Price>(12'345, 2U), quantity},
      risk::OrderExposure{quantity, decimal<model::Notional>(30, 0U)},
      provenance(),
  };
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Startup rejects an unusable zero capacity before an owner-local table can be published.
TEST_CASE("outbound OMS requires positive fixed capacity", "[oms][outbound]") {
  const auto result = oms::OutboundOms::create(0U);
  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.oms_capacity"));
}

// --------------------------------------------------------
// A complete admitted row retains every exact identity, economic, exposure, and provenance value.
TEST_CASE("outbound OMS retains one provenance-rich risk-approved order", "[oms][outbound]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Admit one internally consistent order into preallocated owner-local storage.
  auto oms_result = oms::OutboundOms::create(2U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto expected = admission(1U, 1U);
  const auto admitted = outbound.admit(expected);

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify the stable row, initial state, complete retained value, and exact table accounting.
  REQUIRE(admitted);
  REQUIRE(admitted.value().admitted());
  REQUIRE(admitted.value().record() != nullptr);
  CHECK_FALSE(admitted.value().reason().has_value());
  CHECK(admitted.value().record()->state() == oms::OutboundOrderState::PendingEncoding);
  CHECK(admitted.value().record()->admission() == expected);
  CHECK(outbound.find(expected.order_id) == admitted.value().record());
  CHECK(outbound.size() == 1U);
  CHECK(outbound.capacity() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Exact OrderId equality remains authoritative across collisions and wins before full capacity.
TEST_CASE("outbound OMS is collision-safe and duplicate identity precedes capacity",
          "[oms][outbound]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Counters one and three share the same modulo-two FNV probe start but remain distinct
  // identities.
  auto oms_result = oms::OutboundOms::create(2U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto first = admission(1U, 1U);
  const auto colliding = admission(2U, 3U);
  REQUIRE(outbound.admit(first));
  REQUIRE(outbound.admit(colliding));
  CHECK(outbound.find(first.order_id) != nullptr);
  CHECK(outbound.find(colliding.order_id) != nullptr);
  CHECK(outbound.size() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical inspection follows admission order rather than collision-dependent table position.
  REQUIRE(outbound.record_at(0U) != nullptr);
  REQUIRE(outbound.record_at(1U) != nullptr);
  CHECK(outbound.record_at(0U)->admission() == first);
  CHECK(outbound.record_at(1U)->admission() == colliding);
  CHECK(outbound.record_at(2U) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
  // A repeated complete identity reports duplicate even though the table is already full.
  const auto duplicate = outbound.admit(first);
  REQUIRE(duplicate);
  CHECK_FALSE(duplicate.value().admitted());
  REQUIRE(duplicate.value().reason().has_value());
  CHECK(*duplicate.value().reason() == execution::SubmissionReason::DuplicateOrderIdentity);
  CHECK(outbound.size() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Only a distinct identity receives fixed-capacity non-admission, and neither result adds a row.
  const auto full = outbound.admit(admission(3U, 2U));
  REQUIRE(full);
  CHECK_FALSE(full.value().admitted());
  REQUIRE(full.value().reason().has_value());
  CHECK(*full.value().reason() == execution::SubmissionReason::OmsCapacityExceeded);
  CHECK(outbound.size() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Contradictory attempt/reservation or quantity/exposure evidence is an invariant fault, not an
// ordinary non-admission reason.
TEST_CASE("outbound OMS rejects internally inconsistent admission without a row",
          "[oms][outbound]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Break the attempt-derived reservation identity before the first admission.
  auto oms_result = oms::OutboundOms::create(2U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  auto inconsistent = admission(1U, 1U);
  inconsistent.reservation_id = identity<model::ReservationId>(2U);
  const auto reservation_result = outbound.admit(inconsistent);
  REQUIRE_FALSE(reservation_result);
  CHECK(reservation_result.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidOmsState,
                                     "outbound_oms.admission"));
  CHECK(outbound.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Break the once-approved quantity relationship and prove the same no-mutation invariant.
  inconsistent = admission(1U, 1U);
  inconsistent.exposure.quantity = decimal<model::Quantity>(4, 0U);
  const auto quantity_result = outbound.admit(inconsistent);
  REQUIRE_FALSE(quantity_result);
  CHECK(quantity_result.error().code == model::DomainErrorCode::InvalidOmsState);
  CHECK(outbound.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every allowed transition reaches exactly its assigned state, including retained local failure.
TEST_CASE("outbound OMS implements the complete M3 transition table", "[oms][outbound]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Create independent records for encoding failure, definite initiation failure, success, and
  // uncertainty so no terminal state needs an outgoing transition.
  auto oms_result = oms::OutboundOms::create(4U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto encoding_failure = admission(1U, 1U);
  const auto initiation_failure = admission(2U, 2U);
  const auto initiated = admission(3U, 3U);
  const auto uncertain = admission(4U, 4U);
  REQUIRE(outbound.admit(encoding_failure));
  REQUIRE(outbound.admit(initiation_failure));
  REQUIRE(outbound.admit(initiated));
  REQUIRE(outbound.admit(uncertain));

  // ++++++++++++++++++++++++++++++++++++++++
  // PendingEncoding may fail terminally or advance exactly once into PendingInitiation.
  REQUIRE(outbound.mark_encoding_failed(encoding_failure.order_id));
  CHECK(outbound.find(encoding_failure.order_id)->state() ==
        oms::OutboundOrderState::LocallyFailed);
  REQUIRE(outbound.mark_encoding_succeeded(initiation_failure.order_id));
  REQUIRE(outbound.mark_encoding_succeeded(initiated.order_id));
  REQUIRE(outbound.mark_encoding_succeeded(uncertain.order_id));

  // ++++++++++++++++++++++++++++++++++++++++
  // PendingInitiation maps the three fake boundary outcomes to their exact terminal states.
  REQUIRE(outbound.mark_initiation_definitely_failed(initiation_failure.order_id));
  REQUIRE(outbound.mark_write_initiated(initiated.order_id));
  REQUIRE(outbound.mark_submission_unknown(uncertain.order_id));
  CHECK(outbound.find(initiation_failure.order_id)->state() ==
        oms::OutboundOrderState::LocallyFailed);
  CHECK(outbound.find(initiated.order_id)->state() == oms::OutboundOrderState::WriteInitiated);
  CHECK(outbound.find(uncertain.order_id)->state() == oms::OutboundOrderState::SubmissionUnknown);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A latched post-acceptance internal fault has one exceptional conservative downgrade without
// changing the retained admission, enabling retry, or opening any other terminal transition.
TEST_CASE("outbound OMS contains post-acceptance internal fault as submission unknown",
          "[oms][outbound][internal-fault][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one WriteInitiated row and preserve its complete immutable admission for comparison.
  auto oms_result = oms::OutboundOms::create(6U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto initiated = admission(1U, 1U);
  REQUIRE(outbound.admit(initiated));
  REQUIRE(outbound.mark_encoding_succeeded(initiated.order_id));
  REQUIRE(outbound.mark_write_initiated(initiated.order_id));
  REQUIRE(outbound.mark_submission_unknown_after_internal_fault(initiated.order_id));
  REQUIRE(outbound.find(initiated.order_id) != nullptr);
  CHECK(outbound.find(initiated.order_id)->state() == oms::OutboundOrderState::SubmissionUnknown);
  CHECK(outbound.find(initiated.order_id)->admission() == initiated);

  // ++++++++++++++++++++++++++++++++++++++++
  // The containment transition is single-use and cannot be applied to any other source state.
  const auto repeated = outbound.mark_submission_unknown_after_internal_fault(initiated.order_id);
  REQUIRE_FALSE(repeated);
  CHECK(repeated.error().code == model::DomainErrorCode::InvalidOmsState);

  const auto pending_encoding = admission(2U, 2U);
  const auto pending_initiation = admission(3U, 3U);
  const auto locally_failed = admission(4U, 4U);
  const auto ordinary_unknown = admission(5U, 5U);
  REQUIRE(outbound.admit(pending_encoding));
  REQUIRE(outbound.admit(pending_initiation));
  REQUIRE(outbound.admit(locally_failed));
  REQUIRE(outbound.admit(ordinary_unknown));
  REQUIRE(outbound.mark_encoding_succeeded(pending_initiation.order_id));
  REQUIRE(outbound.mark_encoding_failed(locally_failed.order_id));
  REQUIRE(outbound.mark_encoding_succeeded(ordinary_unknown.order_id));
  REQUIRE(outbound.mark_submission_unknown(ordinary_unknown.order_id));

  for (const auto* const rejected :
       {&pending_encoding, &pending_initiation, &locally_failed, &ordinary_unknown}) {
    const auto before = outbound.find(rejected->order_id)->state();
    const auto result = outbound.mark_submission_unknown_after_internal_fault(rejected->order_id);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidOmsState);
    CHECK(outbound.find(rejected->order_id)->state() == before);
  }
  const auto missing = outbound.mark_submission_unknown_after_internal_fault(order_id(6U));
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == model::DomainErrorCode::InvalidOmsState);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Wrong-source, repeated terminal, and missing-identity operations return InvalidOmsState without
// changing any retained row or permitting local identity reuse.
TEST_CASE("outbound OMS rejects every transition outside the closed table", "[oms][outbound]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A PendingEncoding row cannot skip encoding or transition under an unknown identity.
  auto oms_result = oms::OutboundOms::create(1U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto retained = admission(1U, 1U);
  REQUIRE(outbound.admit(retained));
  const auto wrong_source = outbound.mark_write_initiated(retained.order_id);
  REQUIRE_FALSE(wrong_source);
  CHECK(wrong_source.error().code == model::DomainErrorCode::InvalidOmsState);
  CHECK(outbound.find(retained.order_id)->state() == oms::OutboundOrderState::PendingEncoding);
  const auto missing = outbound.mark_encoding_failed(order_id(2U));
  REQUIRE_FALSE(missing);
  CHECK(outbound.find(retained.order_id)->state() == oms::OutboundOrderState::PendingEncoding);

  // ++++++++++++++++++++++++++++++++++++++++
  // LocallyFailed is terminal but retained, so repetition fails and later admission stays
  // duplicate.
  REQUIRE(outbound.mark_encoding_failed(retained.order_id));
  const auto repeated = outbound.mark_encoding_failed(retained.order_id);
  REQUIRE_FALSE(repeated);
  CHECK(outbound.find(retained.order_id)->state() == oms::OutboundOrderState::LocallyFailed);
  const auto duplicate = outbound.admit(retained);
  REQUIRE(duplicate);
  CHECK(*duplicate.value().reason() == execution::SubmissionReason::DuplicateOrderIdentity);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
