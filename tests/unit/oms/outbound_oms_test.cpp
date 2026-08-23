// Purpose: prove fixed-capacity collision-safe OMS admission, provenance retention, the complete M3
// transition table, and the initialized M4 order projection without granting M4 mutation authority.

#include "aegis/oms/outbound_oms.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace aegis;

// ########################################################################
// Stable semantic assignments must never drift: values 1-5 retain their M3 evidence bytes, while
// the appended M4 order and cancellation values are not encoded by the M3 submission-trace schema.
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::PendingEncoding) == 1U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::PendingInitiation) == 2U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::WriteInitiated) == 3U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::SubmissionUnknown) == 4U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::LocallyFailed) == 5U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::Working) == 6U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::PartiallyFilled) == 7U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::Filled) == 8U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::ExchangeRejected) == 9U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::Cancelled) == 10U);
static_assert(static_cast<std::uint8_t>(oms::OutboundOrderState::ReconciledAbsent) == 11U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::Unassigned) == 0U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::None) == 1U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::Requested) == 2U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::WriteInitiated) == 3U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::OutcomeUnknown) == 4U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::DefinitelyFailed) == 5U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::Rejected) == 6U);
static_assert(static_cast<std::uint8_t>(oms::CancellationState::Confirmed) == 7U);

// ########################################################################

// --------------------------------------------------------
// Parse one identifier literal or throw when the test fixture itself is malformed.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in outbound OMS fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Construct an exact nominal decimal or throw when a fixture value is unrepresentable.
template <typename Decimal>
[[nodiscard]] Decimal create_decimal_or_throw(std::int64_t coefficient, std::uint8_t scale) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid decimal in outbound OMS fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Construct one exact positive ordinal/revision or throw on a broken fixture value.
template <typename Identity> [[nodiscard]] Identity create_ordinal_or_throw(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid ordinal in outbound OMS fixture"};
  }
  return result.value();
}

// --------------------------------------------------------
// Generate one canonical local order identity or throw if its fixed provider cannot be constructed.
[[nodiscard]] model::OrderId create_order_id_or_throw(std::uint64_t counter) {
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
// Construct distinguishable raw fingerprint bytes without wrapper or runtime dependencies.
[[nodiscard]] model::Sha256Digest create_sha256_digest(std::uint8_t first) noexcept {
  model::Sha256Digest value{};
  for (std::size_t index = 0U; index < value.size(); ++index) {
    value[index] = std::byte{static_cast<std::uint8_t>(first + index)};
  }
  return value;
}

// --------------------------------------------------------
// Construct complete provenance or throw if any fixture identity is malformed.
[[nodiscard]] oms::OutboundOrderProvenance create_outbound_order_provenance_or_throw() {
  return oms::OutboundOrderProvenance{
      parse_identifier_or_throw<model::RouteId>("route.r"),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::LogicalAccountId>("account.a"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
      parse_identifier_or_throw<model::FirmId>("firm.f"),
      parse_identifier_or_throw<model::DeskId>("desk.d"),
      parse_identifier_or_throw<model::BotId>("bot.b"),
      parse_identifier_or_throw<model::StrategyId>("strategy.s"),
      create_sha256_digest(0x10U),
      create_ordinal_or_throw<model::ConfigurationRevision>(1U),
      create_ordinal_or_throw<model::OrganizationRevision>(2U),
      create_ordinal_or_throw<model::RouteRevision>(3U),
      create_ordinal_or_throw<model::InstrumentMetadataRevision>(4U),
      create_sha256_digest(0x30U),
      create_sha256_digest(0x50U),
      create_ordinal_or_throw<model::RiskPolicyRevision>(5U),
      create_sha256_digest(0x70U),
  };
}

// --------------------------------------------------------
// Construct one internally coherent admission or throw if any fixture value is malformed.
[[nodiscard]] oms::OutboundOrderAdmission
create_outbound_order_admission_or_throw(std::uint64_t attempt_value, std::uint64_t order_counter) {
  const auto quantity = create_decimal_or_throw<model::Quantity>(3, 0U);
  return oms::OutboundOrderAdmission{
      create_ordinal_or_throw<model::SubmissionAttemptId>(attempt_value),
      create_order_id_or_throw(order_counter),
      create_ordinal_or_throw<model::ReservationId>(attempt_value),
      execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                         execution::TimeInForce::GoodTilCancelled,
                                         create_decimal_or_throw<model::Price>(12'345, 2U),
                                         quantity},
      risk::OrderExposure{quantity, create_decimal_or_throw<model::Notional>(30, 0U)},
      create_outbound_order_provenance_or_throw(),
  };
}

// --------------------------------------------------------
// Construct the complete pre-execution projection expected after an M3 lifecycle transition.
[[nodiscard]] oms::PrivateOrderProjection
create_pre_execution_private_order_projection_or_throw(oms::OutboundOrderState state,
                                                       bool reconciliation_required = false) {
  return oms::PrivateOrderProjection{state,
                                     false,
                                     std::nullopt,
                                     create_decimal_or_throw<model::Quantity>(0, 0U),
                                     std::nullopt,
                                     oms::CancellationState::None,
                                     reconciliation_required,
                                     false,
                                     false,
                                     0U,
                                     0U};
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
  const auto expected = create_outbound_order_admission_or_throw(1U, 1U);
  const auto admitted = outbound.admit(expected);

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify the stable row, initial state, complete retained value, and exact table accounting.
  REQUIRE(admitted);
  REQUIRE(admitted.value().admitted());
  REQUIRE(admitted.value().record() != nullptr);
  CHECK_FALSE(admitted.value().reason().has_value());
  CHECK(admitted.value().record()->state() == oms::OutboundOrderState::PendingEncoding);
  CHECK(admitted.value().record()->admission() == expected);
  CHECK(admitted.value().record()->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::PendingEncoding));
  CHECK(outbound.find(expected.order_id) == admitted.value().record());
  CHECK(outbound.size() == 1U);
  CHECK(outbound.capacity() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A caller may alter its detached copy without changing any retained owner-local field.
  auto detached_projection = admitted.value().record()->private_projection();
  detached_projection.state = oms::OutboundOrderState::Filled;
  detached_projection.reconciliation_required = true;
  detached_projection.cancel_attempt_count = 7U;
  CHECK(detached_projection.state == oms::OutboundOrderState::Filled);
  CHECK(detached_projection.reconciliation_required);
  CHECK(detached_projection.cancel_attempt_count == 7U);
  CHECK(admitted.value().record()->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::PendingEncoding));

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
  const auto first = create_outbound_order_admission_or_throw(1U, 1U);
  const auto colliding = create_outbound_order_admission_or_throw(2U, 3U);
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
  const auto full = outbound.admit(create_outbound_order_admission_or_throw(3U, 2U));
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
  auto inconsistent = create_outbound_order_admission_or_throw(1U, 1U);
  inconsistent.reservation_id = create_ordinal_or_throw<model::ReservationId>(2U);
  const auto reservation_result = outbound.admit(inconsistent);
  REQUIRE_FALSE(reservation_result);
  CHECK(reservation_result.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidOmsState,
                                     "outbound_oms.admission"));
  CHECK(outbound.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Break the once-approved quantity relationship and prove the same no-mutation invariant.
  inconsistent = create_outbound_order_admission_or_throw(1U, 1U);
  inconsistent.exposure.quantity = create_decimal_or_throw<model::Quantity>(4, 0U);
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
  // uncertainty so each M3 handoff can be inspected without requiring an outgoing transition.
  auto oms_result = oms::OutboundOms::create(4U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto encoding_failure = create_outbound_order_admission_or_throw(1U, 1U);
  const auto initiation_failure = create_outbound_order_admission_or_throw(2U, 2U);
  const auto initiated = create_outbound_order_admission_or_throw(3U, 3U);
  const auto uncertain = create_outbound_order_admission_or_throw(4U, 4U);
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
  // PendingInitiation maps the three fake boundaries to exact M3 handoff states; only LocallyFailed
  // is terminal in the M4 OMS lifecycle.
  REQUIRE(outbound.mark_initiation_definitely_failed(initiation_failure.order_id));
  REQUIRE(outbound.mark_write_initiated(initiated.order_id));
  REQUIRE(outbound.mark_submission_unknown(uncertain.order_id));
  CHECK(outbound.find(initiation_failure.order_id)->state() ==
        oms::OutboundOrderState::LocallyFailed);
  CHECK(outbound.find(initiated.order_id)->state() == oms::OutboundOrderState::WriteInitiated);
  CHECK(outbound.find(uncertain.order_id)->state() == oms::OutboundOrderState::SubmissionUnknown);
  CHECK(outbound.find(encoding_failure.order_id)->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::LocallyFailed));
  CHECK(outbound.find(initiation_failure.order_id)->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::LocallyFailed));
  CHECK(outbound.find(initiated.order_id)->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::WriteInitiated));
  CHECK(outbound.find(uncertain.order_id)->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::SubmissionUnknown, true));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A latched post-acceptance internal fault has one exceptional conservative downgrade without
// changing the retained admission, enabling retry, or granting any other M3 transition.
TEST_CASE("outbound OMS contains post-acceptance internal fault as submission unknown",
          "[oms][outbound][internal-fault][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one WriteInitiated row and preserve its complete immutable admission for comparison.
  auto oms_result = oms::OutboundOms::create(6U);
  REQUIRE(oms_result);
  auto& outbound = oms_result.value();
  const auto initiated = create_outbound_order_admission_or_throw(1U, 1U);
  REQUIRE(outbound.admit(initiated));
  REQUIRE(outbound.mark_encoding_succeeded(initiated.order_id));
  REQUIRE(outbound.mark_write_initiated(initiated.order_id));
  REQUIRE(outbound.mark_submission_unknown_after_internal_fault(initiated.order_id));
  REQUIRE(outbound.find(initiated.order_id) != nullptr);
  CHECK(outbound.find(initiated.order_id)->state() == oms::OutboundOrderState::SubmissionUnknown);
  CHECK(outbound.find(initiated.order_id)->admission() == initiated);
  CHECK(outbound.find(initiated.order_id)->private_projection() ==
        create_pre_execution_private_order_projection_or_throw(
            oms::OutboundOrderState::SubmissionUnknown, true));

  // ++++++++++++++++++++++++++++++++++++++++
  // The containment transition is single-use and cannot be applied to any other source state.
  const auto unknown_projection = outbound.find(initiated.order_id)->private_projection();
  const auto repeated = outbound.mark_submission_unknown_after_internal_fault(initiated.order_id);
  REQUIRE_FALSE(repeated);
  CHECK(repeated.error().code == model::DomainErrorCode::InvalidOmsState);
  CHECK(outbound.find(initiated.order_id)->private_projection() == unknown_projection);

  const auto pending_encoding = create_outbound_order_admission_or_throw(2U, 2U);
  const auto pending_initiation = create_outbound_order_admission_or_throw(3U, 3U);
  const auto locally_failed = create_outbound_order_admission_or_throw(4U, 4U);
  const auto ordinary_unknown = create_outbound_order_admission_or_throw(5U, 5U);
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
    const auto before = outbound.find(rejected->order_id)->private_projection();
    const auto result = outbound.mark_submission_unknown_after_internal_fault(rejected->order_id);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidOmsState);
    CHECK(outbound.find(rejected->order_id)->private_projection() == before);
  }
  const auto missing =
      outbound.mark_submission_unknown_after_internal_fault(create_order_id_or_throw(6U));
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
  const auto retained = create_outbound_order_admission_or_throw(1U, 1U);
  REQUIRE(outbound.admit(retained));
  const auto initial_projection = outbound.find(retained.order_id)->private_projection();
  const auto wrong_source = outbound.mark_write_initiated(retained.order_id);
  REQUIRE_FALSE(wrong_source);
  CHECK(wrong_source.error().code == model::DomainErrorCode::InvalidOmsState);
  CHECK(outbound.find(retained.order_id)->private_projection() == initial_projection);
  const auto missing = outbound.mark_encoding_failed(create_order_id_or_throw(2U));
  REQUIRE_FALSE(missing);
  CHECK(outbound.find(retained.order_id)->private_projection() == initial_projection);

  // ++++++++++++++++++++++++++++++++++++++++
  // LocallyFailed is terminal but retained, so repetition fails and later admission stays
  // duplicate.
  REQUIRE(outbound.mark_encoding_failed(retained.order_id));
  const auto terminal_projection = outbound.find(retained.order_id)->private_projection();
  const auto repeated = outbound.mark_encoding_failed(retained.order_id);
  REQUIRE_FALSE(repeated);
  CHECK(outbound.find(retained.order_id)->private_projection() == terminal_projection);
  const auto duplicate = outbound.admit(retained);
  REQUIRE(duplicate);
  CHECK(*duplicate.value().reason() == execution::SubmissionReason::DuplicateOrderIdentity);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
