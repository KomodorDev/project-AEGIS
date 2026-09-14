// Purpose: qualify trusted local normalization and owner-bound initial lifecycle proposals while
// proving that detached values cannot change live OMS, inventory, reservations, or identity stores.

#include "aegis/risk/inventory_ledger.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Stop fixture construction on an invalid authored value instead of inspecting partial authority.
template <typename Value>
[[nodiscard]] Value extract_transition_result_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid owner transition fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Bind normalization to exactly the same sealed root as the genuine submission owner.
[[nodiscard]] runtime::PrivateOrderEventFactory
create_transition_factory_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  return runtime::PrivateOrderEventFactory{extract_transition_result_or_throw(
      runtime::M4ProvenanceResolver::create_m4_provenance_resolver(authority.configuration,
                                                                   authority.m4_policy))};
}

// --------------------------------------------------------
// Supply a deterministic local observation without implying executor or cancel authorization.
[[nodiscard]] oms::LocalPrivateEventOrigin create_transition_local_origin_or_throw() {
  return oms::LocalPrivateEventOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                      model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
}

// --------------------------------------------------------
// Scope an authoritative source identity to the immutable account and venue of one genuine row.
[[nodiscard]] oms::VenuePrivateEventOrigin
create_transition_venue_origin_or_throw(const oms::OutboundOrderRecord& order,
                                        std::uint8_t event_value = 1U) {
  return oms::VenuePrivateEventOrigin{
      oms::VenuePrivateEventKey{
          order.provenance().venue_id, order.provenance().logical_account_id,
          test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(1U),
          test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event_value)},
      model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
}

// --------------------------------------------------------
// Capture every canonical scope so a planning-only regression cannot hide a partial risk update.
[[nodiscard]] std::vector<risk::RiskScopeExposureEvidence>
collect_transition_scope_evidence_or_throw(const risk::ReservationLedger& reservations) {
  std::vector<risk::RiskScopeExposureEvidence> result;
  for (std::size_t index = 0U; index < reservations.scope_evidence_count(); ++index) {
    auto evidence = reservations.scope_evidence_at(index);
    if (!evidence) {
      throw std::logic_error{"missing owner transition scope evidence"};
    }
    result.push_back(std::move(*evidence));
  }
  return result;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Local normalization requires exact storage membership even when a detached row has equal fields.
TEST_CASE("local order normalization requires genuine live OMS storage", "[private-oms-owner]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  const auto* order = authority.submission->outbound_oms().find_order(*submitted.order_id());
  REQUIRE(order != nullptr);
  const auto factory = create_transition_factory_or_throw(authority);
  auto table = extract_transition_result_or_throw(oms::OutboundOms::create_outbound_oms(1U));
  auto admitted = table.admit_outbound_order(order->admission());
  REQUIRE(admitted);
  REQUIRE(admitted.value().is_admitted());
  const auto& owned = *admitted.value().record();
  const auto detached = owned;
  CHECK(table.has_retained_order_record(owned));
  CHECK_FALSE(table.has_retained_order_record(detached));
  CHECK_FALSE(table.has_retained_order_record(*order));
  CHECK_FALSE(
      factory.normalize_order_timeout(create_transition_local_origin_or_throw(), table, detached));
  CHECK_FALSE(
      factory.normalize_order_timeout(create_transition_local_origin_or_throw(), table, *order));
  const auto normalized =
      factory.normalize_order_timeout(create_transition_local_origin_or_throw(), table, owned);
  REQUIRE(normalized);
  CHECK(normalized.value().subject_scope() == oms::PrivateEventSubjectScope::Order);
  CHECK(normalized.value().provenance().subject()->bot_id() == order->provenance().bot_id);
  const auto before = owned.private_projection();
  auto transferred = std::move(table);
  CHECK_FALSE(table.has_retained_order_record(owned));
  CHECK(transferred.has_retained_order_record(owned));
  CHECK_FALSE(
      factory.normalize_order_timeout(create_transition_local_origin_or_throw(), table, owned));
  CHECK(owned.private_projection() == before);
}

// --------------------------------------------------------
// Typed local facts retain exact source meaning but do not decide lifecycle eligibility or mutate
// it.
TEST_CASE("local order normalization validates closed cancel and failure shapes",
          "[private-oms-owner]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  const auto& orders = authority.submission->outbound_oms();
  const auto* order = orders.find_order(*submitted.order_id());
  REQUIRE(order != nullptr);
  const auto factory = create_transition_factory_or_throw(authority);
  const auto attempt =
      extract_transition_result_or_throw(oms::CancelAttemptId::cancel_attempt_id_from_components(
          test_support::create_m4_runtime_epoch_or_throw(), order->order_id(), 1U));
  const auto before = order->private_projection();
  auto request = factory.normalize_order_cancel_request(create_transition_local_origin_or_throw(),
                                                        orders, *order, attempt);
  REQUIRE(request);
  CHECK(request.value().kind() == oms::PrivateOrderEventKind::CancelRequested);
  for (const auto outcome : {oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance,
                             oms::CancelWriteOutcome::AcceptedAndInitiated,
                             oms::CancelWriteOutcome::AcceptedThenOutcomeLost}) {
    auto write = factory.normalize_order_cancel_write_outcome(
        create_transition_local_origin_or_throw(), orders, *order, attempt, outcome);
    REQUIRE(write);
    CHECK(std::get<oms::CancelWriteOutcomePayload>(write.value().payload()).outcome == outcome);
  }
  for (const auto certainty : {oms::LocalFailureCertainty::ProvenBeforeAcceptance,
                               oms::LocalFailureCertainty::AcceptanceCouldHaveOccurred}) {
    auto failure = factory.normalize_order_local_failure(create_transition_local_origin_or_throw(),
                                                         orders, *order, certainty);
    REQUIRE(failure);
    CHECK(std::get<oms::LocalFailurePayload>(failure.value().payload()).submission_attempt_id ==
          order->attempt_id());
  }
  CHECK_FALSE(factory.normalize_order_cancel_write_outcome(
      create_transition_local_origin_or_throw(), orders, *order, attempt,
      static_cast<oms::CancelWriteOutcome>(0U)));
  CHECK_FALSE(factory.normalize_order_local_failure(create_transition_local_origin_or_throw(),
                                                    orders, *order,
                                                    static_cast<oms::LocalFailureCertainty>(0U)));
  const auto other_attempt =
      extract_transition_result_or_throw(oms::CancelAttemptId::cancel_attempt_id_from_components(
          test_support::create_m4_runtime_epoch_or_throw(),
          test_support::create_m4_order_id_or_throw(99U), 1U));
  CHECK_FALSE(factory.normalize_order_cancel_request(create_transition_local_origin_or_throw(),
                                                     orders, *order, other_attempt));
  CHECK(order->private_projection() == before);
}

// --------------------------------------------------------
// A hypothetical acknowledgement or full fill never advances live ownership, economics, or identity
// stores. Repeating the query describes the same M3 baseline rather than a hidden shadow lifecycle.
TEST_CASE("owner-bound OMS transition proposals leave every live business component unchanged",
          "[private-oms-owner]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  const auto* order = authority.submission->outbound_oms().find_order(*submitted.order_id());
  REQUIRE(order != nullptr);
  const auto* owner = authority.submission->private_order_reconciler();
  REQUIRE(owner != nullptr);
  const auto factory = create_transition_factory_or_throw(authority);
  const auto exchange = test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(8U);
  auto acknowledgement = factory.normalize_venue_acknowledgement(
      create_transition_venue_origin_or_throw(*order), exchange, order->order_id());
  REQUIRE(acknowledgement);
  const auto before = order->private_projection();
  const auto reservation_before =
      *authority.submission->reservations().find_reservation(order->reservation_id());
  const auto scopes_before =
      collect_transition_scope_evidence_or_throw(authority.submission->reservations());
  auto proposal = owner->derive_initial_known_authoritative_oms_transition(acknowledgement.value());
  REQUIRE(proposal);
  CHECK(proposal.value().before == before);
  CHECK(proposal.value().after.state == oms::OutboundOrderState::Working);
  CHECK(proposal.value().after.exchange_acknowledged);
  CHECK(proposal.value().after.exchange_order_id == exchange);
  CHECK(proposal.value().economics_action == oms::ProposedPrivateEconomicsAction::None);
  const auto repeated =
      owner->derive_initial_known_authoritative_oms_transition(acknowledgement.value());
  REQUIRE(repeated);
  CHECK(repeated.value().after == proposal.value().after);
  const auto reconciliation_origin = oms::ReconciliationPrivateEventOrigin{
      test_support::create_m4_reconciliation_epoch_or_throw(),
      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(9U),
      test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(1U),
      model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
  const auto reconciliation = factory.normalize_reconciliation_acknowledgement(
      reconciliation_origin, order->provenance().logical_account_id, order->provenance().venue_id,
      exchange, order->order_id(), order->provenance().instrument_id);
  REQUIRE(reconciliation);
  const auto reconciled_proposal =
      owner->derive_initial_known_authoritative_oms_transition(reconciliation.value());
  REQUIRE(reconciled_proposal);
  CHECK(reconciled_proposal.value().after == proposal.value().after);
  proposal.value().after.state = oms::OutboundOrderState::Filled;
  CHECK(order->private_projection() == before);

  // A full-fill proposal owns its final state but cannot move either side of economic exposure.
  const auto locator = extract_transition_result_or_throw(
      oms::PrivateOrderLocator::create_private_order_locator(order->order_id(), exchange));
  auto execution = factory.normalize_venue_execution(
      create_transition_venue_origin_or_throw(*order, 2U), locator,
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(2U),
      order->provenance().instrument_id, order->provenance().metadata_revision,
      order->economics().quantity, order->economics().quantity, order->economics().price,
      order->economics().side);
  REQUIRE(execution);
  auto fill = owner->derive_initial_known_authoritative_oms_transition(execution.value());
  REQUIRE(fill);
  CHECK(fill.value().after.state == oms::OutboundOrderState::Filled);
  CHECK(fill.value().after.exchange_mapping_established_by_execution);
  CHECK(fill.value().economics_action == oms::ProposedPrivateEconomicsAction::ApplyExecutions);
  CHECK(order->private_projection() == before);
  CHECK(*authority.submission->reservations().find_reservation(order->reservation_id()) ==
        reservation_before);
  CHECK(collect_transition_scope_evidence_or_throw(authority.submission->reservations()) ==
        scopes_before);
  const auto* inventory =
      risk::InventoryLedger::installed_inventory(authority.submission->reservations());
  REQUIRE(inventory != nullptr);
  CHECK(inventory->source_row_count() == 0U);
  CHECK(owner->event_identity_record_count() == 0U);
  CHECK(owner->trade_identity_record_count() == 0U);
  CHECK(owner->exchange_order_mapping_count() == 0U);
}

// --------------------------------------------------------
// Unknown locators and local observations cannot select a row through the authoritative proposal
// API.
TEST_CASE("owner-bound OMS proposals reject missing correlation authority", "[private-oms-owner]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  const auto* order = authority.submission->outbound_oms().find_order(*submitted.order_id());
  REQUIRE(order != nullptr);
  const auto factory = create_transition_factory_or_throw(authority);
  const auto* owner = authority.submission->private_order_reconciler();
  REQUIRE(owner != nullptr);
  auto unknown = factory.normalize_venue_acknowledgement(
      create_transition_venue_origin_or_throw(*order),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(8U),
      test_support::create_m4_order_id_or_throw(99U));
  REQUIRE(unknown);
  auto result = owner->derive_initial_known_authoritative_oms_transition(unknown.value());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::PrivateCorrelationFailed);
  auto local = factory.normalize_order_timeout(create_transition_local_origin_or_throw(),
                                               authority.submission->outbound_oms(), *order);
  REQUIRE(local);
  CHECK_FALSE(owner->derive_initial_known_authoritative_oms_transition(local.value()));
  auto foreign_capacities = test_support::create_ordinary_m4_policy_capacities();
  ++foreign_capacities.max_event_identity_records;
  const auto foreign_authority =
      test_support::create_m4_test_authority_or_throw(foreign_capacities);
  const auto foreign_factory = runtime::PrivateOrderEventFactory{extract_transition_result_or_throw(
      runtime::M4ProvenanceResolver::create_m4_provenance_resolver(foreign_authority.configuration,
                                                                   foreign_authority.m4_policy))};
  const auto foreign_input = foreign_factory.normalize_venue_acknowledgement(
      create_transition_venue_origin_or_throw(*order),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(8U),
      order->order_id());
  REQUIRE(foreign_input);
  CHECK_FALSE(owner->derive_initial_known_authoritative_oms_transition(foreign_input.value()));
  CHECK(owner->event_identity_record_count() == 0U);
  CHECK(order->state() == oms::OutboundOrderState::WriteInitiated);
}

// --------------------------------------------------------
