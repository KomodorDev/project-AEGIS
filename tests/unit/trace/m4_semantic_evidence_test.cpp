// Purpose: prove M4 semantic identity tags and checked ordinal/range values retain exact typed
// meaning without adding evidence storage, canonical bytes, or runtime mutation authority.

#include "aegis/trace/m4_semantic_evidence.hpp"
#include "m4_private_event_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: requires-expressions prove checked factories preserve authored source types
// and reject Boolean, character, floating-point, and enum inputs before conversion.
template <typename Value>
concept AcceptsRecoveryActionOrdinalValue =
    requires(Value value) { trace::RecoveryActionOrdinal::from_value(value); };

template <typename Count>
concept AcceptsCallbackOrdinalCount =
    requires(model::CallbackOrdinal first_callback_ordinal, Count callback_count) {
      trace::CallbackOrdinalRange::create_callback_ordinal_range(first_callback_ordinal,
                                                                 callback_count);
    };

static_assert(std::is_final_v<trace::RecoveryActionOrdinal>);
static_assert(std::is_final_v<trace::OriginatingEventIdentity>);
static_assert(std::is_final_v<trace::CallbackOrdinalRange>);
static_assert(!std::is_default_constructible_v<trace::RecoveryActionOrdinal>);
static_assert(!std::is_default_constructible_v<trace::OriginatingEventIdentity>);
static_assert(!std::is_default_constructible_v<trace::CallbackOrdinalRange>);
static_assert(AcceptsRecoveryActionOrdinalValue<std::uint64_t>);
static_assert(!AcceptsRecoveryActionOrdinalValue<bool>);
static_assert(!AcceptsRecoveryActionOrdinalValue<double>);
static_assert(!AcceptsRecoveryActionOrdinalValue<char>);
static_assert(!AcceptsRecoveryActionOrdinalValue<trace::RecoveryGapKind>);
static_assert(AcceptsCallbackOrdinalCount<std::uint32_t>);
static_assert(!AcceptsCallbackOrdinalCount<bool>);
static_assert(!AcceptsCallbackOrdinalCount<double>);
static_assert(!AcceptsCallbackOrdinalCount<char>);
static_assert(!AcceptsCallbackOrdinalCount<trace::CallbackDecision>);
static_assert(static_cast<std::uint8_t>(trace::OriginatingEventIdentityKind::None) == 0U);
static_assert(static_cast<std::uint8_t>(trace::OriginatingEventIdentityKind::Local) == 1U);
static_assert(static_cast<std::uint8_t>(trace::OriginatingEventIdentityKind::Venue) == 2U);
static_assert(static_cast<std::uint8_t>(trace::OriginatingEventIdentityKind::Reconciliation) == 3U);
static_assert(static_cast<std::uint8_t>(trace::OriginatingEventIdentityKind::RecoveryAction) == 4U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::EventDisposition) == 1U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::OmsTransition) == 2U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::ReservationTransition) == 3U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::InventoryTransition) == 4U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::AccountSafetyTransition) == 5U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::OrderCallbackDecision) == 6U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::CallbackFault) == 7U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::RecoveryGap) == 8U);
static_assert(static_cast<std::uint8_t>(trace::M4AuditKind::RecoveryNotificationDecision) == 9U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::None) == 0U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::Planned) == 1U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::Delivered) == 2U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::SuppressedDuplicate) == 3U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::SuppressedBuffered) == 4U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::SuppressedReplay) == 5U);
static_assert(static_cast<std::uint8_t>(trace::CallbackDecision::Faulted) == 6U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::None) == 0U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::PublishedNotAcknowledged) == 1U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::MissingJournalInput) == 2U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::InvalidSnapshot) == 3U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::LocalProvenanceMissing) == 4U);
static_assert(static_cast<std::uint8_t>(trace::RecoveryGapKind::CallbackDeliveryAmbiguous) == 5U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Admission) == 1U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Shape) == 2U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Provenance) == 3U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Identity) == 4U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Correlation) == 5U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Oms) == 6U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Economics) == 7U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Evidence) == 8U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Recovery) == 9U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Callback) == 10U);
static_assert(static_cast<std::uint8_t>(trace::M4DiagnosticStage::Internal) == 11U);
static_assert(static_cast<std::uint8_t>(trace::DiagnosticSafetyAction::None) == 0U);
static_assert(static_cast<std::uint8_t>(trace::DiagnosticSafetyAction::ReconciliationRequired) ==
              1U);
static_assert(static_cast<std::uint8_t>(trace::DiagnosticSafetyAction::Quarantined) == 2U);
static_assert(static_cast<std::uint8_t>(trace::DiagnosticSafetyAction::RuntimeFaulted) == 3U);

// ########################################################################

// --------------------------------------------------------
// Extract one expected successful Result or fail the fixture before the behavior assertion.
template <typename Value>
[[nodiscard]] Value take_semantic_evidence_result_value_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid semantic-evidence fixture value: " +
                           std::to_string(static_cast<std::uint16_t>(result.error().code)) + "/" +
                           result.error().context.field};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Require one exact stable error code and field from a checked semantic-evidence factory.
template <typename Value>
void check_semantic_evidence_error(const model::Result<Value>& result,
                                   model::DomainErrorCode expected_code,
                                   const char* expected_field) {
  REQUIRE_FALSE(result);
  CHECK(result.error().code == expected_code);
  CHECK(result.error().context.field == expected_field);
}

// --------------------------------------------------------
// Checked ordinal and range values reject empty, narrowing, and wrapping input.
TEST_CASE("M4 semantic evidence checks ordinals and callback ranges",
          "[trace][m4][semantic-evidence]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Recovery-action ordinals retain the complete nonzero u64 domain.
  const auto first_action =
      take_semantic_evidence_result_value_or_throw(trace::RecoveryActionOrdinal::from_value(1U));
  const auto last_action = take_semantic_evidence_result_value_or_throw(
      trace::RecoveryActionOrdinal::from_value(std::numeric_limits<std::uint64_t>::max()));
  CHECK(first_action.value() == 1U);
  CHECK(last_action.value() == std::numeric_limits<std::uint64_t>::max());
  CHECK(first_action < last_action);
  check_semantic_evidence_error(trace::RecoveryActionOrdinal::from_value(0),
                                model::DomainErrorCode::InvalidRecoveryPolicy,
                                "recovery_action_ordinal");
  check_semantic_evidence_error(trace::RecoveryActionOrdinal::from_value(-1),
                                model::DomainErrorCode::InvalidRecoveryPolicy,
                                "recovery_action_ordinal");

  // ++++++++++++++++++++++++++++++++++++++++
  // Callback ranges derive an inclusive final ordinal and reject empty, narrowing, or wrap.
  const auto first_callback_ordinal =
      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(7U);
  const auto callback_range = take_semantic_evidence_result_value_or_throw(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(first_callback_ordinal, 3));
  CHECK(callback_range.first_callback_ordinal().value() == 7U);
  CHECK(callback_range.callback_count() == 3U);
  CHECK(callback_range.last_callback_ordinal().value() == 9U);
  CHECK(callback_range == take_semantic_evidence_result_value_or_throw(
                              trace::CallbackOrdinalRange::create_callback_ordinal_range(
                                  first_callback_ordinal, std::uint64_t{3U})));
  const auto terminal_callback_ordinal =
      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(
          std::numeric_limits<std::uint64_t>::max());
  const auto terminal_singleton = take_semantic_evidence_result_value_or_throw(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(terminal_callback_ordinal, 1U));
  CHECK(terminal_singleton.first_callback_ordinal() == terminal_singleton.last_callback_ordinal());
  CHECK(terminal_singleton.callback_count() == 1U);
  check_semantic_evidence_error(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(first_callback_ordinal, 0),
      model::DomainErrorCode::InvalidPrivateEvent, "callback_ordinal_range.count");
  check_semantic_evidence_error(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(first_callback_ordinal, -1),
      model::DomainErrorCode::InvalidPrivateEvent, "callback_ordinal_range.count");
  check_semantic_evidence_error(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(
          first_callback_ordinal,
          static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U),
      model::DomainErrorCode::InvalidPrivateEvent, "callback_ordinal_range.count");
  check_semantic_evidence_error(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(terminal_callback_ordinal, 2U),
      model::DomainErrorCode::CallbackCounterExhausted, "callback_ordinal_range.last");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Named origin factories retain exact identity domains without accepting sentinel construction.
TEST_CASE("M4 semantic evidence retains every closed originating identity",
          "[trace][m4][semantic-evidence]") {
  test_support::M4PrivateEventFixture fixture;
  const auto reconciliation_epoch = test_support::create_m4_reconciliation_epoch_or_throw(3U);
  const auto authoritative_cut =
      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U);
  const auto row_ordinal =
      test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(5U);
  const auto action_ordinal =
      take_semantic_evidence_result_value_or_throw(trace::RecoveryActionOrdinal::from_value(7U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Each factory selects exactly its assigned tag and corresponding complete payload.
  const auto absent = trace::OriginatingEventIdentity::create_without_originating_event();
  const auto local = trace::OriginatingEventIdentity::create_from_local_event_id(
      test_support::create_m4_local_event_id_or_throw(2U));
  const auto venue_origin = fixture.create_venue_private_event_origin_or_throw();
  const auto venue =
      trace::OriginatingEventIdentity::create_from_venue_event_key(venue_origin.event_key);
  const auto reconciliation = trace::OriginatingEventIdentity::create_from_reconciliation_row(
      reconciliation_epoch, authoritative_cut, row_ordinal);
  const auto recovery_action = trace::OriginatingEventIdentity::create_from_recovery_action(
      reconciliation_epoch, action_ordinal);
  CHECK(absent.originating_event_identity_kind() == trace::OriginatingEventIdentityKind::None);
  CHECK(local.originating_event_identity_kind() == trace::OriginatingEventIdentityKind::Local);
  CHECK(venue.originating_event_identity_kind() == trace::OriginatingEventIdentityKind::Venue);
  CHECK(reconciliation.originating_event_identity_kind() ==
        trace::OriginatingEventIdentityKind::Reconciliation);
  CHECK(recovery_action.originating_event_identity_kind() ==
        trace::OriginatingEventIdentityKind::RecoveryAction);
  CHECK(std::holds_alternative<trace::NoOriginatingEventIdentity>(
      absent.originating_event_identity_value()));
  const auto& local_value =
      std::get<trace::LocalOriginatingEventIdentity>(local.originating_event_identity_value());
  CHECK(local_value.event_id == test_support::create_m4_local_event_id_or_throw(2U));
  const auto& venue_value =
      std::get<trace::VenueOriginatingEventIdentity>(venue.originating_event_identity_value());
  CHECK(venue_value.event_key == venue_origin.event_key);
  const auto& reconciliation_value = std::get<trace::ReconciliationOriginatingEventIdentity>(
      reconciliation.originating_event_identity_value());
  CHECK(reconciliation_value.reconciliation_epoch_id == reconciliation_epoch);
  CHECK(reconciliation_value.authoritative_cut_id == authoritative_cut);
  CHECK(reconciliation_value.row_ordinal == row_ordinal);
  const auto& recovery_action_value = std::get<trace::RecoveryActionOriginatingEventIdentity>(
      recovery_action.originating_event_identity_value());
  CHECK(recovery_action_value.reconciliation_epoch_id == reconciliation_epoch);
  CHECK(recovery_action_value.action_ordinal == action_ordinal);
  CHECK_FALSE(absent == local);
  CHECK_FALSE(local == venue);
  CHECK_FALSE(venue == reconciliation);
  CHECK_FALSE(reconciliation == recovery_action);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
