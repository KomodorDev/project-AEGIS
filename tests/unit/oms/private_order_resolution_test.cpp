// Purpose: prove stable private-event outcomes, timestamp-free origin-tagged registry keys, and
// the caller-unforgeable first-resolution and trade-comparison construction boundary.

#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/runtime/private_order_event_factory.hpp"
#include "m4_private_event_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace aegis;

// ########################################################################
// Stable numeric assignments for dispositions, safety states, and safety reasons are compatibility
// boundaries before ADR-0014 defines their exact encoded bytes.
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::Applied) == 1U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::BufferedGap) == 2U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::AppliedFromBuffer) == 3U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::ProjectionOnly) == 4U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::ExactEventDuplicate) == 5U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::ExactTradeDuplicate) == 6U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::SafetyContained) == 7U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventDisposition::ForbiddenRejected) == 8U);

static_assert(static_cast<std::uint8_t>(risk::AccountSafetyState::Synchronized) == 1U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyState::ReconciliationRequired) == 2U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyState::Quarantined) == 3U);

static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::SubmissionUnknown) == 1U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::TimeoutObserved) == 2U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::DisconnectObserved) == 3U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::IncompleteReconciliation) == 4U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::RecoveryGap) == 5U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::UnknownOrder) == 6U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::UnknownTrade) == 7U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::UnexplainedPosition) == 8U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::PermissionMismatch) == 9U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::MarginModeMismatch) == 10U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::OutOfScopeInstrument) == 11U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::CorrelationConflict) == 12U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::EventIdentityConflict) == 13U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::TradeIdentityConflict) == 14U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::AuthoritativeContradiction) ==
              15U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::CriticalAdmissionLoss) == 16U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::EvidenceCapacityExhausted) ==
              17U);
static_assert(
    static_cast<std::uint8_t>(risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure) == 18U);
static_assert(static_cast<std::uint8_t>(risk::AccountSafetyReason::ProvenanceMismatch) == 19U);

// ########################################################################

// ########################################################################
// Registry alternatives remain nominal; the sealed resolution and trade wrappers have no public
// construction authority.
static_assert(std::variant_size_v<oms::PrivateEventRegistryKeyValue> == 3U);
static_assert(std::same_as<std::variant_alternative_t<0U, oms::PrivateEventRegistryKeyValue>,
                           oms::LocalOrderEventId>);
static_assert(std::same_as<std::variant_alternative_t<1U, oms::PrivateEventRegistryKeyValue>,
                           oms::VenuePrivateEventKey>);
static_assert(std::same_as<std::variant_alternative_t<2U, oms::PrivateEventRegistryKeyValue>,
                           oms::ReconciliationPrivateEventKey>);
static_assert(!std::same_as<oms::LocalOrderEventId, oms::VenuePrivateEventKey>);
static_assert(!std::same_as<oms::VenuePrivateEventKey, oms::ReconciliationPrivateEventKey>);
static_assert(!std::same_as<oms::ReconciliationPrivateEventKey, oms::LocalOrderEventId>);

static_assert(std::variant_size_v<oms::PrivateEventResolutionValue> == 4U);
static_assert(std::same_as<std::variant_alternative_t<0U, oms::PrivateEventResolutionValue>,
                           oms::KnownPrivateEventResolution>);
static_assert(std::same_as<std::variant_alternative_t<1U, oms::PrivateEventResolutionValue>,
                           oms::UnknownPrivateEventResolution>);
static_assert(std::same_as<std::variant_alternative_t<2U, oms::PrivateEventResolutionValue>,
                           oms::ConflictPrivateEventResolution>);
static_assert(std::same_as<std::variant_alternative_t<3U, oms::PrivateEventResolutionValue>,
                           oms::NotOrderScopedPrivateEventResolution>);
static_assert(std::variant_size_v<oms::PrivateTradeResolutionValue> == 2U);
static_assert(std::same_as<std::variant_alternative_t<0U, oms::PrivateTradeResolutionValue>,
                           oms::KnownPrivateTradeResolution>);
static_assert(std::same_as<std::variant_alternative_t<1U, oms::PrivateTradeResolutionValue>,
                           oms::UnknownPrivateTradeResolution>);

// Interesting syntax: the requires-expressions make access control a compile-time fact without
// invoking a factory or adding a test-only friend that would weaken the production boundary.
template <typename Value>
concept HasPublicKnownOrderResolutionFactory =
    requires(model::OrderId order_id, model::M4Provenance provenance) {
      Value::create_known_order_resolution(std::move(order_id), std::move(provenance));
    };

template <typename Value>
concept HasPublicUnknownResolutionFactory = requires { Value::create_unknown_resolution(); };

template <typename Value>
concept HasPublicProvenanceConflictResolutionFactory =
    requires { Value::create_provenance_conflict_resolution(); };

template <typename Value>
concept HasPublicCorrelationConflictResolutionFactory =
    requires { Value::create_correlation_conflict_resolution(); };

template <typename Value>
concept HasPublicNotOrderScopedResolutionFactory =
    requires { Value::create_not_order_scoped_resolution(); };

template <typename Value>
concept HasPublicKnownTradeSemanticFactory =
    requires(const oms::ExecutionPayload& execution, const model::OrderId& order_id) {
      Value::create_known_trade_semantic_value(execution, order_id, execution::OrderSide::Buy);
    };

template <typename Value>
concept HasPublicUnknownTradeSemanticFactory = requires(const oms::ExecutionPayload& execution) {
  Value::create_unknown_trade_semantic_value(execution);
};

static_assert(!std::is_default_constructible_v<oms::PrivateEventRegistryKey>);
static_assert(!std::is_aggregate_v<oms::PrivateEventRegistryKey>);
static_assert(
    !std::is_constructible_v<oms::PrivateEventRegistryKey, oms::PrivateEventRegistryKeyValue>);
static_assert(!std::is_default_constructible_v<oms::PrivateEventResolution>);
static_assert(!std::is_aggregate_v<oms::PrivateEventResolution>);
static_assert(
    !std::is_constructible_v<oms::PrivateEventResolution, oms::PrivateEventResolutionValue>);
static_assert(!std::is_default_constructible_v<oms::PrivateTradeSemanticValue>);
static_assert(!std::is_aggregate_v<oms::PrivateTradeSemanticValue>);
static_assert(
    !std::is_constructible_v<oms::PrivateTradeSemanticValue, model::InstrumentId,
                             model::InstrumentMetadataRevision, model::Quantity, model::Quantity,
                             model::Price, oms::PrivateTradeResolutionValue>);
static_assert(!HasPublicKnownOrderResolutionFactory<oms::PrivateEventResolution>);
static_assert(!HasPublicUnknownResolutionFactory<oms::PrivateEventResolution>);
static_assert(!HasPublicProvenanceConflictResolutionFactory<oms::PrivateEventResolution>);
static_assert(!HasPublicCorrelationConflictResolutionFactory<oms::PrivateEventResolution>);
static_assert(!HasPublicNotOrderScopedResolutionFactory<oms::PrivateEventResolution>);
static_assert(!HasPublicKnownTradeSemanticFactory<oms::PrivateTradeSemanticValue>);
static_assert(!HasPublicUnknownTradeSemanticFactory<oms::PrivateTradeSemanticValue>);

// ########################################################################

// --------------------------------------------------------
// Move one successful normalized value out of a real source factory or fail on a fixture defect.
[[nodiscard]] oms::NormalizedPrivateOrderInput take_normalized_private_order_input_or_throw(
    model::Result<oms::NormalizedPrivateOrderInput> result) {
  if (!result) {
    throw std::logic_error{"invalid normalized input in private-resolution fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Derive one registry key through the receive-time-free semantic value used by event comparison.
[[nodiscard]] oms::PrivateEventRegistryKey
derive_private_event_registry_key(const oms::NormalizedPrivateOrderInput& input) {
  const auto semantic = oms::PrivateEventIngressSemanticValue::from_normalized_input(input);
  return oms::PrivateEventRegistryKey::from_ingress_semantic_value(semantic);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// A venue registry key discards both source/receive timestamps and payload while retaining the
// exact account, venue, source epoch, and event identity.
TEST_CASE("private venue registry key retains only complete source identity",
          "[m4][oms][private-resolution]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.private_event_factory();
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto other_exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x62U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Receive time disappears at semantic projection, while registry identity also excludes source
  // time and acknowledgement payload.
  const auto baseline =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), exchange_order_id,
          fixture.outbound_order_record().order_id()));
  const auto changed_receive_time =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U, 100U, 900U), exchange_order_id,
          fixture.outbound_order_record().order_id()));
  const auto changed_source_time =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U, 700U, 200U), exchange_order_id,
          fixture.outbound_order_record().order_id()));
  const auto changed_payload =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U),
          other_exchange_order_id, fixture.outbound_order_record().order_id()));
  const auto baseline_semantic =
      oms::PrivateEventIngressSemanticValue::from_normalized_input(baseline);
  const auto receive_semantic =
      oms::PrivateEventIngressSemanticValue::from_normalized_input(changed_receive_time);
  const auto source_semantic =
      oms::PrivateEventIngressSemanticValue::from_normalized_input(changed_source_time);
  CHECK(baseline_semantic == receive_semantic);
  CHECK_FALSE(baseline_semantic == source_semantic);

  const auto baseline_key =
      oms::PrivateEventRegistryKey::from_ingress_semantic_value(baseline_semantic);
  CHECK(baseline_key == derive_private_event_registry_key(changed_receive_time));
  CHECK(baseline_key == derive_private_event_registry_key(changed_source_time));
  CHECK(baseline_key == derive_private_event_registry_key(changed_payload));
  CHECK(baseline_key.origin() == oms::PrivateEventOrigin::Venue);

  // ++++++++++++++++++++++++++++++++++++++++
  // The active nominal alternative equals the complete original venue key field for field.
  const auto* const retained_key =
      std::get_if<oms::VenuePrivateEventKey>(&baseline_key.registry_key_value());
  REQUIRE(retained_key != nullptr);
  const auto& normalized_origin = std::get<oms::VenuePrivateEventOrigin>(baseline.origin_value());
  CHECK(*retained_key == normalized_origin.event_key);
  CHECK(retained_key->venue_id == fixture.venue_id());
  CHECK(retained_key->logical_account_id == fixture.account_id());

  // ++++++++++++++++++++++++++++++++++++++++
  // Each source-owned component remains significant, and same-domain order follows identity order.
  const auto changed_event_identity =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(2U, 100U, 200U), exchange_order_id,
          fixture.outbound_order_record().order_id()));
  auto changed_epoch_origin = fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U);
  changed_epoch_origin.event_key.source_epoch_id =
      test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x42U);
  const auto changed_source_epoch = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_acknowledgement(std::move(changed_epoch_origin), exchange_order_id,
                                              fixture.outbound_order_record().order_id()));
  const auto changed_event_key = derive_private_event_registry_key(changed_event_identity);
  CHECK_FALSE(baseline_key == changed_event_key);
  CHECK(baseline_key < changed_event_key);
  CHECK_FALSE(baseline_key == derive_private_event_registry_key(changed_source_epoch));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Local and reconciliation registry keys retain their exact nominal identity alternatives while
// excluding source/cut and receive timestamps.
TEST_CASE("private registry keys keep local and reconciliation domains distinct",
          "[m4][oms][private-resolution]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.private_event_factory();

  // ++++++++++++++++++++++++++++++++++++++++
  // Local keys retain the complete locally minted identity but neither authored timestamp.
  const auto local_baseline =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(1U, 100U, 200U), fixture.account_id(),
          fixture.venue_id()));
  const auto local_changed_times =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(1U, 700U, 900U), fixture.account_id(),
          fixture.venue_id()));
  const auto local_changed_identity =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(2U, 100U, 200U), fixture.account_id(),
          fixture.venue_id()));
  const auto local_key = derive_private_event_registry_key(local_baseline);
  CHECK(local_key == derive_private_event_registry_key(local_changed_times));
  CHECK_FALSE(local_key == derive_private_event_registry_key(local_changed_identity));
  CHECK(local_key.origin() == oms::PrivateEventOrigin::Local);
  const auto* const retained_local_identity =
      std::get_if<oms::LocalOrderEventId>(&local_key.registry_key_value());
  REQUIRE(retained_local_identity != nullptr);
  CHECK(*retained_local_identity ==
        std::get<oms::LocalPrivateEventOrigin>(local_baseline.origin_value()).event_id);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reconciliation keys retain epoch, cut, and row but exclude cut and receive timestamps.
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto create_reconciliation_input_or_throw =
      [&](oms::ReconciliationPrivateEventOrigin origin) {
        return take_normalized_private_order_input_or_throw(
            factory.normalize_reconciliation_acknowledgement(
                std::move(origin), fixture.account_id(), fixture.venue_id(), exchange_order_id,
                fixture.outbound_order_record().order_id(), fixture.instrument_id()));
      };
  const auto reconciliation_baseline = create_reconciliation_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U));
  const auto reconciliation_changed_times = create_reconciliation_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 700U, 900U));
  auto changed_epoch_origin =
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U);
  changed_epoch_origin.reconciliation_epoch_id =
      test_support::create_m4_reconciliation_epoch_or_throw(2U);
  const auto reconciliation_changed_epoch =
      create_reconciliation_input_or_throw(std::move(changed_epoch_origin));
  auto changed_cut_origin =
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U);
  changed_cut_origin.authoritative_cut_id =
      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x52U);
  const auto reconciliation_changed_cut =
      create_reconciliation_input_or_throw(std::move(changed_cut_origin));
  const auto reconciliation_changed_row = create_reconciliation_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(2U, 100U, 200U));
  const auto reconciliation_key = derive_private_event_registry_key(reconciliation_baseline);
  CHECK(reconciliation_key == derive_private_event_registry_key(reconciliation_changed_times));
  CHECK_FALSE(reconciliation_key ==
              derive_private_event_registry_key(reconciliation_changed_epoch));
  CHECK_FALSE(reconciliation_key == derive_private_event_registry_key(reconciliation_changed_cut));
  CHECK_FALSE(reconciliation_key == derive_private_event_registry_key(reconciliation_changed_row));
  CHECK(reconciliation_key.origin() == oms::PrivateEventOrigin::Reconciliation);

  const auto* const retained_reconciliation_identity =
      std::get_if<oms::ReconciliationPrivateEventKey>(&reconciliation_key.registry_key_value());
  REQUIRE(retained_reconciliation_identity != nullptr);
  const auto& normalized_reconciliation_origin =
      std::get<oms::ReconciliationPrivateEventOrigin>(reconciliation_baseline.origin_value());
  CHECK(retained_reconciliation_identity->reconciliation_epoch_id ==
        normalized_reconciliation_origin.reconciliation_epoch_id);
  CHECK(retained_reconciliation_identity->authoritative_cut_id ==
        normalized_reconciliation_origin.authoritative_cut_id);
  CHECK(retained_reconciliation_identity->row_ordinal ==
        normalized_reconciliation_origin.row_ordinal);

  // ++++++++++++++++++++++++++++++++++++++++
  // Origin tagging prevents conflation and fixes Local before Venue before Reconciliation.
  CHECK(local_key != reconciliation_key);
  const auto venue_key = derive_private_event_registry_key(
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U), exchange_order_id,
          fixture.outbound_order_record().order_id())));
  CHECK(venue_key != local_key);
  CHECK(venue_key != reconciliation_key);
  CHECK(local_key < venue_key);
  CHECK(venue_key < reconciliation_key);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
