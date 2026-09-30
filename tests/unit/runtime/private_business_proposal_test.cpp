// Purpose: qualify owner-bound initial business proposals against independent economics and
// evidence-count expectations without granting canonical application or changing runtime state.

#include "aegis/risk/inventory_ledger.hpp"
#include "aegis/runtime/private_business_proposal.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Each literal scenario begins with the same genuine M3 order and a distinct authoritative fact;
// the integer expectations are authored separately from the production planning algorithms.
enum class InitialBusinessInput : std::uint8_t {
  Acknowledgement = 1,
  PartialFill = 2,
  FullFill = 3,
  ExchangeRejection = 4,
  CancellationAtZero = 5,
  BufferedGap = 6,
  ContradictorySide = 7,
};

// ########################################################################

constexpr std::array all_scopes{risk::RiskScopeKind::Bot,   risk::RiskScopeKind::Desk,
                                risk::RiskScopeKind::Firm,  risk::RiskScopeKind::Account,
                                risk::RiskScopeKind::Route, risk::RiskScopeKind::Instrument,
                                risk::RiskScopeKind::Venue};

// --------------------------------------------------------
// Stop fixture setup at the precise invalid value rather than exposing partial test authority.
template <typename Value>
[[nodiscard]] Value extract_business_value_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid private business fixture: " + result.error().context.field};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Install the production M4 owner before the first genuine submission acquires callback authority.
[[nodiscard]] test_support::M4OwnerTestAuthority create_business_authority_or_throw() {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  return authority;
}

// --------------------------------------------------------
// Bind authoritative normalization to the same complete policy root as the owning runtime.
[[nodiscard]] runtime::PrivateOrderEventFactory
create_business_event_factory_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  return runtime::PrivateOrderEventFactory{
      extract_business_value_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
          authority.configuration, authority.m4_policy))};
}

// --------------------------------------------------------
// Copy every canonical risk cell so read-only planning cannot conceal an unrelated partial update.
[[nodiscard]] std::vector<risk::RiskScopeExposureEvidence>
collect_business_scope_evidence_or_throw(const risk::ReservationLedger& reservations) {
  std::vector<risk::RiskScopeExposureEvidence> result;
  for (std::size_t index = 0U; index < reservations.scope_evidence_count(); ++index) {
    const auto row = reservations.scope_evidence_at(index);
    if (!row) {
      throw std::logic_error{"missing private business scope evidence"};
    }
    result.push_back(*row);
  }
  return result;
}

// --------------------------------------------------------

// ########################################################################
// Own one real admitted reference order and its exact sealed normalization source. The coordinator
// outlives all borrowed rows; tests obtain only const observations and detached proposals from it.
class InitialBusinessProposalFixture final {
public:

  // --------------------------------------------------------
  // Submit exactly Q=2 at limit100 with quote-face multiplier10, yielding independently known N=20.
  explicit InitialBusinessProposalFixture(execution::OrderSide side = execution::OrderSide::Buy)
      : authority{create_business_authority_or_throw()},
        factory{create_business_event_factory_or_throw(authority)} {
    auto request = test_support::create_m4_reference_order_request_or_throw();
    request.side = side;
    const auto submitted = test_support::submit_m4_order_or_throw(authority, request);
    if (!submitted.order_id()) {
      throw std::logic_error{"private business fixture failed to submit its order"};
    }
    order_ = authority.submission->outbound_oms().find_order(*submitted.order_id());
    owner_ = authority.submission->private_order_reconciler();
    if (order_ == nullptr || owner_ == nullptr ||
        order_->state() != oms::OutboundOrderState::WriteInitiated) {
      throw std::logic_error{"private business fixture lacks its genuine initial owner row"};
    }
  }

  // --------------------------------------------------------
  // Keep the coordinator and every borrowed observation at stable addresses for the fixture life.
  InitialBusinessProposalFixture(const InitialBusinessProposalFixture&) = delete;
  InitialBusinessProposalFixture& operator=(const InitialBusinessProposalFixture&) = delete;
  InitialBusinessProposalFixture(InitialBusinessProposalFixture&&) = delete;
  InitialBusinessProposalFixture& operator=(InitialBusinessProposalFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the exact retained order established by the real submission path.
  [[nodiscard]] const oms::OutboundOrderRecord& order() const noexcept { return *order_; }

  // --------------------------------------------------------
  // Borrow the private owner solely for its const planning and completion-observation interfaces.
  [[nodiscard]] const runtime::PrivateOrderReconciler& owner() const noexcept { return *owner_; }

  // --------------------------------------------------------
  // Author one fixed exchange identity, independent from any candidate mapping in a proposal.
  [[nodiscard]] oms::ExchangeOrderId create_exchange_order_id_or_throw() const {
    return test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x51U);
  }

  // --------------------------------------------------------
  // Preserve the genuine local locator and a possible first exchange mapping without publishing it.
  [[nodiscard]] oms::PrivateOrderLocator create_business_locator_or_throw() const {
    return extract_business_value_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
        order().order_id(), create_exchange_order_id_or_throw()));
  }

  // --------------------------------------------------------
  // Bind ordinary source identity and times to the genuine retained account and venue.
  [[nodiscard]] oms::VenuePrivateEventOrigin
  create_venue_origin_or_throw(std::uint8_t event = 1U) const {
    return oms::VenuePrivateEventOrigin{
        oms::VenuePrivateEventKey{
            order().provenance().venue_id, order().provenance().logical_account_id,
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
  }

  // --------------------------------------------------------
  // Keep reconciliation origin nominally distinct while preserving equal economic source facts.
  [[nodiscard]] oms::ReconciliationPrivateEventOrigin
  create_reconciliation_origin_or_throw(std::uint8_t event = 1U) const {
    return oms::ReconciliationPrivateEventOrigin{
        test_support::create_m4_reconciliation_epoch_or_throw(),
        test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
        test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(event),
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
  }

  // --------------------------------------------------------
  // Normalize literal reference inputs through trusted factories; none advances live lifecycle.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_business_input_or_throw(InitialBusinessInput kind, bool reconciliation = false) const {
    const auto event = static_cast<std::uint8_t>(kind);
    const auto origin = create_venue_origin_or_throw(event);
    const auto reconciliation_origin = create_reconciliation_origin_or_throw(event);
    const auto& provenance = order().provenance();
    if (kind == InitialBusinessInput::Acknowledgement) {
      return extract_business_value_or_throw(
          reconciliation ? factory.normalize_reconciliation_acknowledgement(
                               reconciliation_origin, provenance.logical_account_id,
                               provenance.venue_id, create_exchange_order_id_or_throw(),
                               order().order_id(), provenance.instrument_id)
                         : factory.normalize_venue_acknowledgement(
                               origin, create_exchange_order_id_or_throw(), order().order_id()));
    }
    if (kind == InitialBusinessInput::ExchangeRejection) {
      return extract_business_value_or_throw(
          reconciliation ? factory.normalize_reconciliation_rejection(
                               reconciliation_origin, provenance.logical_account_id,
                               provenance.venue_id, create_business_locator_or_throw(),
                               oms::ExchangeRejectionCategory::InvalidOrder, {})
                         : factory.normalize_venue_rejection(
                               origin, create_business_locator_or_throw(),
                               oms::ExchangeRejectionCategory::InvalidOrder, {}));
    }
    if (kind == InitialBusinessInput::CancellationAtZero) {
      const auto zero = test_support::create_m4_decimal_or_throw<model::Quantity>(0);
      return extract_business_value_or_throw(
          reconciliation
              ? factory.normalize_reconciliation_cancellation_result(
                    reconciliation_origin, provenance.logical_account_id, provenance.venue_id,
                    create_business_locator_or_throw(), oms::CancellationResult::Cancelled, zero)
              : factory.normalize_venue_cancellation_result(
                    origin, create_business_locator_or_throw(), oms::CancellationResult::Cancelled,
                    zero));
    }
    const auto increment = test_support::create_m4_decimal_or_throw<model::Quantity>(
        kind == InitialBusinessInput::FullFill ? 2 : 1);
    const auto cumulative = test_support::create_m4_decimal_or_throw<model::Quantity>(
        kind == InitialBusinessInput::FullFill || kind == InitialBusinessInput::BufferedGap ? 2
                                                                                            : 1);
    const auto side =
        kind == InitialBusinessInput::ContradictorySide
            ? (order().economics().side == execution::OrderSide::Buy ? execution::OrderSide::Sell
                                                                     : execution::OrderSide::Buy)
            : order().economics().side;
    const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(event);
    return extract_business_value_or_throw(
        reconciliation ? factory.normalize_reconciliation_execution(
                             reconciliation_origin, provenance.logical_account_id,
                             provenance.venue_id, create_business_locator_or_throw(), trade,
                             provenance.instrument_id, provenance.metadata_revision, increment,
                             cumulative, order().economics().price, side)
                       : factory.normalize_venue_execution(
                             origin, create_business_locator_or_throw(), trade,
                             provenance.instrument_id, provenance.metadata_revision, increment,
                             cumulative, order().economics().price, side));
  }

  // --------------------------------------------------------
  // Public fixture values permit ordinary source normalization and cold owner destruction only;
  // the coordinator still exposes no mutable reservation or inventory seam to these tests.
  test_support::M4OwnerTestAuthority authority;
  runtime::PrivateOrderEventFactory factory;

private:
  const oms::OutboundOrderRecord* order_{nullptr};
  const runtime::PrivateOrderReconciler* owner_{nullptr};
};

// ########################################################################

// --------------------------------------------------------
// Retain all confirmed aggregates, including unrelated firm cells, before a read-only proposal.
[[nodiscard]] std::vector<risk::InventoryAggregateCell>
collect_business_inventory_or_throw(const risk::ReservationLedger& reservations) {
  const auto* inventory = risk::InventoryLedger::installed_inventory(reservations);
  if (inventory == nullptr) {
    throw std::logic_error{"missing private business inventory"};
  }
  std::vector<risk::InventoryAggregateCell> result;
  for (std::size_t index = 0U; index < inventory->aggregate_cell_count(); ++index) {
    const auto* aggregate = inventory->aggregate_at(index);
    if (aggregate == nullptr) {
      throw std::logic_error{"missing private business inventory aggregate"};
    }
    result.push_back(*aggregate);
  }
  return result;
}

// --------------------------------------------------------
// Check every canonical and preparation-only boundary independently from the proposed result.
void check_business_runtime_unchanged(
    const InitialBusinessProposalFixture& fixture, const oms::PrivateOrderProjection& before,
    const risk::ReservationEvidence& reservation_before,
    const std::vector<risk::RiskScopeExposureEvidence>& scopes_before,
    const std::vector<risk::InventoryAggregateCell>& inventory_before) {
  const auto& reservations = fixture.authority.submission->reservations();
  const auto& owner = fixture.owner();
  CHECK(fixture.order().private_projection() == before);
  const auto* retained = reservations.find_reservation(fixture.order().reservation_id());
  REQUIRE(retained != nullptr);
  CHECK(*retained == reservation_before);
  CHECK(reservations.held_reservation_count() == 1U);
  CHECK(collect_business_scope_evidence_or_throw(reservations) == scopes_before);
  CHECK(collect_business_inventory_or_throw(reservations) == inventory_before);
  const auto* inventory = risk::InventoryLedger::installed_inventory(reservations);
  REQUIRE(inventory != nullptr);
  CHECK(inventory->source_row_count() == 0U);
  CHECK(inventory->find_source(fixture.order().order_id()) == nullptr);
  CHECK(owner.event_identity_record_count() == 0U);
  CHECK(owner.trade_identity_record_count() == 0U);
  CHECK(owner.exchange_order_mapping_count() == 0U);
  CHECK(owner.identity_preparations().event_record_count() == 0U);
  CHECK(owner.identity_preparations().trade_record_count() == 0U);
  CHECK(owner.identity_preparations().mapping_candidate_count() == 0U);
  CHECK(owner.retained_identity_turn_count() == 0U);
  CHECK(owner.account_safety_state(fixture.order().provenance().logical_account_id) ==
        risk::AccountSafetyState::Synchronized);
  CHECK_FALSE(owner.is_private_consumption_globally_blocked());
  for (const auto ordinal_value : {1U, 7U}) {
    const auto ordinal =
        test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(ordinal_value);
    CHECK_FALSE(owner.find_committed_private_event_disposition(ordinal));
    CHECK_FALSE(owner.find_committed_reconciliation_event_disposition(ordinal));
    CHECK(owner.find_committed_retained_private_event_error(ordinal) == nullptr);
    CHECK(owner.find_committed_retained_reconciliation_event_error(ordinal) == nullptr);
    CHECK(owner.find_retained_identity_turn(ordinal, false) == nullptr);
    CHECK(owner.find_retained_identity_turn(ordinal, true) == nullptr);
  }
}

// --------------------------------------------------------
// Construct an exact literal expected quantity and reject a malformed test oracle immediately.
[[nodiscard]] model::Quantity create_expected_quantity_or_throw(std::int64_t value) {
  return test_support::create_m4_decimal_or_throw<model::Quantity>(value);
}

// --------------------------------------------------------
// Construct an exact literal expected notional independently from production economics arithmetic.
[[nodiscard]] model::Notional create_expected_notional_or_throw(std::int64_t value) {
  return test_support::create_m4_decimal_or_throw<model::Notional>(value);
}

// --------------------------------------------------------
// Author the complete small-integer seven-scope oracle without invoking production conversions.
[[nodiscard]] risk::RiskScopeExposure
create_expected_business_scope_or_throw(execution::OrderSide side, std::int64_t remaining,
                                        std::int64_t confirmed) {
  const auto buy = side == execution::OrderSide::Buy;
  const auto sign = buy ? 1 : -1;
  return risk::RiskScopeExposure{remaining > 0 ? 1U : 0U,
                                 create_expected_notional_or_throw(remaining * 10),
                                 create_expected_quantity_or_throw(buy ? remaining : 0),
                                 create_expected_quantity_or_throw(buy ? 0 : remaining),
                                 create_expected_quantity_or_throw(remaining + confirmed),
                                 create_expected_notional_or_throw(buy ? remaining * 10 : 0),
                                 create_expected_notional_or_throw(buy ? 0 : remaining * 10),
                                 create_expected_notional_or_throw((remaining + confirmed) * 10),
                                 create_expected_notional_or_throw((remaining + confirmed) * 10),
                                 create_expected_quantity_or_throw(sign * confirmed),
                                 create_expected_notional_or_throw(sign * confirmed * 10)};
}

// --------------------------------------------------------
// Check the complete final risk tuple and prove that exactly one cell from every scope is covered.
void check_business_scope_replacements(
    const InitialBusinessProposalFixture& fixture,
    std::span<const risk::ReservationInventoryScopeReplacement, 7U> replacements,
    const risk::RiskScopeExposure& expected) {
  const auto* inventory =
      risk::InventoryLedger::installed_inventory(fixture.authority.submission->reservations());
  REQUIRE(inventory != nullptr);
  std::array<std::uint32_t, 7U> observed{};
  for (const auto& replacement : replacements) {
    CHECK(replacement.exposure == expected);
    const auto* aggregate = inventory->aggregate_at(replacement.inventory_index);
    REQUIRE(aggregate != nullptr);
    CHECK(aggregate->firm_id == fixture.order().provenance().firm_id);
    CHECK(aggregate->instrument_id == fixture.order().provenance().instrument_id);
    CHECK(aggregate->quote_currency == "USD");
    for (std::size_t index = 0U; index < all_scopes.size(); ++index) {
      if (aggregate->scope == all_scopes[index]) {
        ++observed[index];
      }
    }
  }
  for (const auto count : observed) {
    CHECK(count == 1U);
  }
}

// --------------------------------------------------------
// Check copied resolution against every genuine admission attribution field, not input locators.
void check_business_known_resolution(const InitialBusinessProposalFixture& fixture,
                                     const oms::PrivateEventResolution& resolution) {
  const auto* known = resolution.known_resolution();
  REQUIRE(known != nullptr);
  CHECK(known->order_id == fixture.order().order_id());
  CHECK(known->provenance.root() == fixture.authority.m4_policy.root_provenance());
  REQUIRE(known->provenance.subject());
  const auto& subject = *known->provenance.subject();
  const auto& admission = fixture.order().provenance();
  CHECK(subject.logical_account_id() == admission.logical_account_id);
  CHECK(subject.venue_id() == admission.venue_id);
  CHECK(subject.firm_id() == admission.firm_id);
  CHECK(subject.desk_id() == admission.desk_id);
  CHECK(subject.bot_id() == admission.bot_id);
  CHECK(subject.strategy_id() == admission.strategy_id);
  REQUIRE(subject.route());
  CHECK(subject.route()->route_id == admission.route_id);
  CHECK(subject.route()->route_revision == admission.route_revision);
  REQUIRE(subject.instrument());
  CHECK(subject.instrument()->instrument_id == admission.instrument_id);
  CHECK(subject.instrument()->metadata_revision == admission.metadata_revision);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// The result cannot be forged, copied, or converted into mutable economic commit authority.
TEST_CASE("initial private business proposals expose immutable move-only authority",
          "[private-business-proposal]") {
  STATIC_REQUIRE_FALSE(
      std::is_default_constructible_v<runtime::InitialKnownPrivateBusinessProposal>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<runtime::InitialKnownPrivateBusinessProposal>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<runtime::InitialKnownPrivateBusinessProposal>);
  STATIC_REQUIRE(
      std::is_nothrow_move_constructible_v<runtime::InitialKnownPrivateBusinessProposal>);
  STATIC_REQUIRE(std::is_nothrow_move_assignable_v<runtime::InitialKnownPrivateBusinessProposal>);
  STATIC_REQUIRE(
      std::is_same_v<
          decltype(std::declval<runtime::InitialKnownPrivateBusinessProposal&>().economics_plan()),
          const risk::ReservationInventoryPlan*>);
  STATIC_REQUIRE(
      std::is_same_v<
          decltype(std::declval<runtime::InitialKnownPrivateBusinessProposal&>().oms_transition()),
          const oms::PrivateOmsTransitionPlan&>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<risk::ReservationInventoryPlan,
                                               const risk::ReservationInventoryPlan&&>);
}

// --------------------------------------------------------
// Independent Q=2/N=20 cases join source identity, lifecycle, unsigned reservations, signed
// inventory, exact seven-scope exposure, and evidence counts across both authoritative origins.
TEST_CASE("initial private business proposals join literal lifecycle and economics cases",
          "[private-business-proposal]") {

  // ########################################################################
  // Literal expected outcomes intentionally do not invoke either component planner as an oracle.
  struct ExpectedBusinessCase {
    InitialBusinessInput input;
    oms::OutboundOrderState state;
    oms::ProposedPrivateOmsClassification classification;
    oms::ProposedPrivateEconomicsAction economics;
    risk::ReservationState reservation_state;
    risk::ReservationClosureCause closure;
    std::int64_t remaining;
    std::int64_t confirmed;
    std::uint32_t execution_effect_count;
    std::uint32_t callback_count;
    std::uint32_t audit_count;
  };

  // ########################################################################

  const std::array cases{
      ExpectedBusinessCase{InitialBusinessInput::Acknowledgement, oms::OutboundOrderState::Working,
                           oms::ProposedPrivateOmsClassification::Applied,
                           oms::ProposedPrivateEconomicsAction::None, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 2, 0, 0U, 1U, 3U},
      ExpectedBusinessCase{
          InitialBusinessInput::PartialFill, oms::OutboundOrderState::PartiallyFilled,
          oms::ProposedPrivateOmsClassification::Applied,
          oms::ProposedPrivateEconomicsAction::ApplyExecutions, risk::ReservationState::Held,
          risk::ReservationClosureCause::Unassigned, 1, 1, 1U, 1U, 3U},
      ExpectedBusinessCase{InitialBusinessInput::FullFill, oms::OutboundOrderState::Filled,
                           oms::ProposedPrivateOmsClassification::Applied,
                           oms::ProposedPrivateEconomicsAction::ApplyExecutions,
                           risk::ReservationState::ConsumedByFill,
                           risk::ReservationClosureCause::FullFill, 0, 2, 1U, 1U, 3U},
      ExpectedBusinessCase{
          InitialBusinessInput::ExchangeRejection, oms::OutboundOrderState::ExchangeRejected,
          oms::ProposedPrivateOmsClassification::Applied,
          oms::ProposedPrivateEconomicsAction::ReleaseResidual, risk::ReservationState::Released,
          risk::ReservationClosureCause::ExchangeRejected, 0, 0, 0U, 1U, 3U},
      ExpectedBusinessCase{
          InitialBusinessInput::CancellationAtZero, oms::OutboundOrderState::Cancelled,
          oms::ProposedPrivateOmsClassification::Applied,
          oms::ProposedPrivateEconomicsAction::ReleaseResidual, risk::ReservationState::Released,
          risk::ReservationClosureCause::DefinitiveCancellation, 0, 0, 0U, 1U, 3U},
      ExpectedBusinessCase{InitialBusinessInput::BufferedGap,
                           oms::OutboundOrderState::WriteInitiated,
                           oms::ProposedPrivateOmsClassification::BufferedGap,
                           oms::ProposedPrivateEconomicsAction::None, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 2, 0, 0U, 0U, 1U},
      ExpectedBusinessCase{InitialBusinessInput::ContradictorySide,
                           oms::OutboundOrderState::WriteInitiated,
                           oms::ProposedPrivateOmsClassification::SafetyContained,
                           oms::ProposedPrivateEconomicsAction::None, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 2, 0, 0U, 1U, 3U}};
  const auto first_audit = test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(7U);
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    for (const bool reconciliation : {false, true}) {
      for (const auto& expected : cases) {
        CAPTURE(side, reconciliation, expected.input);
        const InitialBusinessProposalFixture fixture{side};
        const auto& reservations = fixture.authority.submission->reservations();
        const auto before = fixture.order().private_projection();
        const auto* held = reservations.find_reservation(fixture.order().reservation_id());
        REQUIRE(held != nullptr);
        const auto held_before = *held;
        const auto scopes_before = collect_business_scope_evidence_or_throw(reservations);
        const auto inventory_before = collect_business_inventory_or_throw(reservations);
        const auto input =
            fixture.normalize_business_input_or_throw(expected.input, reconciliation);
        auto result = fixture.owner().derive_initial_known_authoritative_business_proposal(
            input, first_audit);
        REQUIRE(result);
        const auto& proposal = result.value();
        CHECK(proposal.input() == input);
        CHECK(proposal.reservation_before() == held_before);
        check_business_known_resolution(fixture, proposal.first_admission_resolution());
        const auto& transition = proposal.oms_transition();
        CHECK(transition.before == before);
        CHECK(transition.after.state == expected.state);
        CHECK(transition.after.cumulative_filled_quantity ==
              create_expected_quantity_or_throw(expected.confirmed));
        CHECK(transition.classification == expected.classification);
        CHECK(transition.economics_action == expected.economics);
        CHECK(transition.transition_effect_count == 1U);
        CHECK(transition.order_callback_count == expected.callback_count);
        CHECK(transition.drained_pending_prefix_count == 0U);
        CHECK_FALSE(transition.cancel_history_change);
        CHECK_FALSE(transition.rejection_code);
        const auto& evidence = proposal.evidence_requirements();
        CHECK(evidence.primary_audit_record_count == 1U);
        CHECK(evidence.order_callback_count == expected.callback_count);
        CHECK(evidence.audit_record_count == expected.audit_count);
        CHECK(proposal.proposed_audit_span().first_audit_ordinal() == first_audit);
        CHECK(proposal.proposed_audit_span().audit_record_count() == expected.audit_count);
        CHECK(proposal.proposed_audit_span().last_audit_ordinal().value() ==
              7U + expected.audit_count - 1U);
        if (expected.input == InitialBusinessInput::BufferedGap) {
          REQUIRE(transition.gap_insertion);
          CHECK(transition.gap_insertion->execution == input);
          CHECK(transition.after.pending_fill_count == 1U);
        } else {
          CHECK_FALSE(transition.gap_insertion);
          CHECK(transition.after.pending_fill_count == 0U);
        }
        if (expected.input == InitialBusinessInput::ContradictorySide) {
          CHECK(transition.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
          CHECK(transition.after.execution_evidence_observed);
        } else {
          CHECK_FALSE(transition.safety_reason);
        }
        const auto* economic = proposal.economics_plan();
        if (expected.economics == oms::ProposedPrivateEconomicsAction::None) {
          CHECK(economic == nullptr);
        } else {
          REQUIRE(economic != nullptr);
          CHECK(economic->reservation_before() == held_before);
          const auto& after = economic->reservation_after();
          CHECK(after.reservation_id == fixture.order().reservation_id());
          CHECK(after.side == side);
          CHECK(after.exposure == held_before.exposure);
          CHECK(after.exposure.quantity == create_expected_quantity_or_throw(2));
          CHECK(after.exposure.quote_notional == create_expected_notional_or_throw(20));
          CHECK(after.remaining_exposure.quantity ==
                create_expected_quantity_or_throw(expected.remaining));
          CHECK(after.remaining_exposure.quote_notional ==
                create_expected_notional_or_throw(expected.remaining * 10));
          CHECK(after.cumulative_confirmed_exposure.quantity ==
                create_expected_quantity_or_throw(expected.confirmed));
          CHECK(after.cumulative_confirmed_exposure.quote_notional ==
                create_expected_notional_or_throw(expected.confirmed * 10));
          CHECK(after.state == expected.reservation_state);
          CHECK(after.closure_cause == expected.closure);
          const auto expected_scope =
              create_expected_business_scope_or_throw(side, expected.remaining, expected.confirmed);
          check_business_scope_replacements(fixture, economic->scope_replacements(),
                                            expected_scope);
          CHECK(economic->execution_effect_count() == expected.execution_effect_count);
          CHECK(economic->execution_effect_at(expected.execution_effect_count) == nullptr);
          if (expected.execution_effect_count == 0U) {
            CHECK_FALSE(economic->source_after());
          } else {
            REQUIRE(economic->source_after());
            const auto sign = side == execution::OrderSide::Buy ? 1 : -1;
            const auto& source = *economic->source_after();
            CHECK(source.admission == fixture.order().admission());
            CHECK(source.confirmed_quantity ==
                  create_expected_quantity_or_throw(sign * expected.confirmed));
            CHECK(source.confirmed_quote_notional ==
                  create_expected_notional_or_throw(sign * expected.confirmed * 10));
            CHECK(source.latest_execution == input);
            CHECK(source.latest_audit_ordinal == first_audit);
            const auto* effect = economic->execution_effect_at(0U);
            REQUIRE(effect != nullptr);
            CHECK(effect->execution == input);
            CHECK(effect->audit_ordinal == first_audit);
            CHECK(effect->reservation_before == held_before);
            CHECK(effect->reservation_after == after);
            CHECK(effect->signed_quantity_delta ==
                  create_expected_quantity_or_throw(sign * expected.confirmed));
            CHECK(effect->signed_notional_delta ==
                  create_expected_notional_or_throw(sign * expected.confirmed * 10));
            check_business_scope_replacements(fixture, effect->scopes, expected_scope);
          }
        }
        check_business_runtime_unchanged(fixture, before, held_before, scopes_before,
                                         inventory_before);
      }
    }
  }
}

// --------------------------------------------------------
// Two prospective reads have no ordinal allocation or admission effect; ordinary and reconciliation
// inputs retain their distinct origin even when equal source facts imply equal business proposals.
TEST_CASE("initial private business queries preserve origin and deterministic prospective evidence",
          "[private-business-proposal]") {
  const InitialBusinessProposalFixture fixture;
  const auto first = test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(7U);
  const auto venue =
      fixture.normalize_business_input_or_throw(InitialBusinessInput::Acknowledgement);
  const auto reconciliation =
      fixture.normalize_business_input_or_throw(InitialBusinessInput::Acknowledgement, true);
  const auto ordinary =
      fixture.owner().derive_initial_known_authoritative_business_proposal(venue, first);
  const auto repeated =
      fixture.owner().derive_initial_known_authoritative_business_proposal(venue, first);
  const auto reconciled =
      fixture.owner().derive_initial_known_authoritative_business_proposal(reconciliation, first);
  REQUIRE(ordinary);
  REQUIRE(repeated);
  REQUIRE(reconciled);
  CHECK(ordinary.value().input() == repeated.value().input());
  CHECK_FALSE(ordinary.value().input() == reconciled.value().input());
  CHECK(ordinary.value().oms_transition().after == repeated.value().oms_transition().after);
  CHECK(ordinary.value().oms_transition().after == reconciled.value().oms_transition().after);
  CHECK(ordinary.value().first_admission_resolution() ==
        reconciled.value().first_admission_resolution());
  CHECK(ordinary.value().proposed_audit_span() == repeated.value().proposed_audit_span());
  CHECK(ordinary.value().proposed_audit_span() == reconciled.value().proposed_audit_span());
  CHECK(ordinary.value().evidence_requirements() == reconciled.value().evidence_requirements());
  CHECK(fixture.owner().retained_identity_turn_count() == 0U);
}

// --------------------------------------------------------
// One fill holds the only bounded batch lease, while independent non-fill proposals remain valid.
// Moving and destroying the lease releases scratch without publishing its prepared economics.
TEST_CASE("initial private fill proposals lease scratch until the last moved result is destroyed",
          "[private-business-proposal]") {
  const InitialBusinessProposalFixture fixture;
  const auto first = test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(7U);
  const auto input = fixture.normalize_business_input_or_throw(InitialBusinessInput::PartialFill);
  const auto& reservations = fixture.authority.submission->reservations();
  const auto before = fixture.order().private_projection();
  const auto held_before = *reservations.find_reservation(fixture.order().reservation_id());
  const auto scopes_before = collect_business_scope_evidence_or_throw(reservations);
  const auto inventory_before = collect_business_inventory_or_throw(reservations);
  {
    auto original =
        fixture.owner().derive_initial_known_authoritative_business_proposal(input, first);
    REQUIRE(original);
    auto moved = std::move(original).value();
    REQUIRE(moved.economics_plan() != nullptr);
    CHECK(moved.economics_plan()->execution_effect_count() == 1U);
    const auto blocked =
        fixture.owner().derive_initial_known_authoritative_business_proposal(input, first);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == model::DomainErrorCode::InvalidReservationConversion);
    for (const auto independent :
         {InitialBusinessInput::Acknowledgement, InitialBusinessInput::ExchangeRejection,
          InitialBusinessInput::BufferedGap}) {
      const auto other = fixture.owner().derive_initial_known_authoritative_business_proposal(
          fixture.normalize_business_input_or_throw(independent), first);
      REQUIRE(other);
    }
    CHECK(moved.economics_plan()->execution_effect_at(0U)->execution == input);
    check_business_runtime_unchanged(fixture, before, held_before, scopes_before, inventory_before);
  }
  const auto repeated =
      fixture.owner().derive_initial_known_authoritative_business_proposal(input, first);
  REQUIRE(repeated);
  REQUIRE(repeated.value().economics_plan() != nullptr);
  CHECK(repeated.value().economics_plan()->reservation_before() == held_before);
  CHECK(repeated.value().economics_plan()->execution_effect_count() == 1U);
  CHECK(repeated.value().proposed_audit_span().first_audit_ordinal() == first);
  check_business_runtime_unchanged(fixture, before, held_before, scopes_before, inventory_before);
}

// --------------------------------------------------------
// Audit positions are prospective checked arithmetic: callbacks require their whole three-record
// span, and a rejected overflow must not leave the batch lease or any economic reservation held.
TEST_CASE("initial private business audit spans reject overflow before returning partial proposals",
          "[private-business-proposal]") {
  const InitialBusinessProposalFixture fixture;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto input = fixture.normalize_business_input_or_throw(InitialBusinessInput::FullFill);
  for (const auto start : {maximum, maximum - 1U}) {
    const auto rejected = fixture.owner().derive_initial_known_authoritative_business_proposal(
        input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(start));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::RecoveryCounterExhausted);
  }
  {
    const auto exact = fixture.owner().derive_initial_known_authoritative_business_proposal(
        input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(maximum - 2U));
    REQUIRE(exact);
    CHECK(exact.value().proposed_audit_span().last_audit_ordinal().value() == maximum);
    CHECK(exact.value().evidence_requirements().audit_record_count == 3U);
  }
  const auto gap = fixture.owner().derive_initial_known_authoritative_business_proposal(
      fixture.normalize_business_input_or_throw(InitialBusinessInput::BufferedGap),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(maximum));
  REQUIRE(gap);
  CHECK(gap.value().proposed_audit_span().first_audit_ordinal().value() == maximum);
  CHECK(gap.value().proposed_audit_span().last_audit_ordinal().value() == maximum);
  CHECK(gap.value().evidence_requirements().audit_record_count == 1U);
  CHECK(fixture.order().state() == oms::OutboundOrderState::WriteInitiated);
  CHECK(fixture.authority.submission->reservations().held_reservation_count() == 1U);
}

// --------------------------------------------------------
// Proposals retain copied identities and leased effects beyond the original owner lifetime;
// successful inspection never dereferences the obsolete owner pointer hidden in the economic plan.
TEST_CASE("initial private business proposals retain readonly facts after owner destruction",
          "[private-business-proposal]") {
  auto fixture = std::make_unique<InitialBusinessProposalFixture>(execution::OrderSide::Sell);
  const auto input = fixture->normalize_business_input_or_throw(InitialBusinessInput::PartialFill);
  const auto admission = fixture->order().admission();
  const auto first = test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(7U);
  auto result = fixture->owner().derive_initial_known_authoritative_business_proposal(input, first);
  REQUIRE(result);
  auto proposal = std::move(result).value();
  REQUIRE(proposal.economics_plan() != nullptr);
  const auto* effect = proposal.economics_plan()->execution_effect_at(0U);
  REQUIRE(effect != nullptr);
  const auto copied_effect = *effect;
  const auto resolution = proposal.first_admission_resolution();
  fixture.reset();
  CHECK(proposal.input() == input);
  CHECK(proposal.first_admission_resolution() == resolution);
  CHECK(proposal.oms_transition().before.state == oms::OutboundOrderState::WriteInitiated);
  CHECK(proposal.oms_transition().after.state == oms::OutboundOrderState::PartiallyFilled);
  CHECK(proposal.evidence_requirements().audit_record_count == 3U);
  CHECK(proposal.proposed_audit_span().first_audit_ordinal() == first);
  CHECK(proposal.reservation_before() == copied_effect.reservation_before);
  REQUIRE(proposal.economics_plan()->source_after());
  CHECK(proposal.economics_plan()->source_after()->admission == admission);
  CHECK(proposal.economics_plan()->source_after()->latest_execution == input);
  CHECK(effect->execution == copied_effect.execution);
  CHECK(effect->reservation_after == copied_effect.reservation_after);
  CHECK(effect->signed_quantity_delta ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(-1));
  CHECK(effect->signed_notional_delta ==
        test_support::create_m4_decimal_or_throw<model::Notional>(-10));
  for (std::size_t index = 0U; index < effect->scopes.size(); ++index) {
    CHECK(effect->scopes[index].exposure == copied_effect.scopes[index].exposure);
    CHECK(proposal.economics_plan()->scope_replacements()[index].exposure ==
          copied_effect.scopes[index].exposure);
  }
}

// --------------------------------------------------------
// A proposed exchange mapping confers no live correlation. Missing/foreign authority and local
// observations cannot cross the deliberately authoritative, genuine-initial-order boundary.
TEST_CASE("initial private business proposals reject absent foreign and local correlation",
          "[private-business-proposal]") {
  const InitialBusinessProposalFixture fixture;
  const auto first = test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(7U);
  const auto& reservations = fixture.authority.submission->reservations();
  const auto before = fixture.order().private_projection();
  const auto held_before = *reservations.find_reservation(fixture.order().reservation_id());
  const auto scopes_before = collect_business_scope_evidence_or_throw(reservations);
  const auto inventory_before = collect_business_inventory_or_throw(reservations);
  const auto possible_mapping =
      fixture.owner().derive_initial_known_authoritative_business_proposal(
          fixture.normalize_business_input_or_throw(InitialBusinessInput::Acknowledgement), first);
  REQUIRE(possible_mapping);
  for (const auto& locator : {std::optional<model::OrderId>{},
                              std::optional{test_support::create_m4_order_id_or_throw(99U)}}) {
    const auto missing = fixture.factory.normalize_venue_acknowledgement(
        fixture.create_venue_origin_or_throw(), fixture.create_exchange_order_id_or_throw(),
        locator);
    REQUIRE(missing);
    const auto rejected = fixture.owner().derive_initial_known_authoritative_business_proposal(
        missing.value(), first);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::PrivateCorrelationFailed);
  }
  const auto local = fixture.factory.normalize_order_timeout(
      oms::LocalPrivateEventOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                   model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
      fixture.authority.submission->outbound_oms(), fixture.order());
  REQUIRE(local);
  CHECK_FALSE(
      fixture.owner().derive_initial_known_authoritative_business_proposal(local.value(), first));
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  ++capacities.max_event_identity_records;
  const auto foreign_authority = test_support::create_m4_test_authority_or_throw(capacities);
  const auto foreign_factory = runtime::PrivateOrderEventFactory{
      extract_business_value_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
          foreign_authority.configuration, foreign_authority.m4_policy))};
  const auto foreign = foreign_factory.normalize_venue_acknowledgement(
      fixture.create_venue_origin_or_throw(), fixture.create_exchange_order_id_or_throw(),
      fixture.order().order_id());
  REQUIRE(foreign);
  CHECK_FALSE(
      fixture.owner().derive_initial_known_authoritative_business_proposal(foreign.value(), first));
  check_business_runtime_unchanged(fixture, before, held_before, scopes_before, inventory_before);
}

// --------------------------------------------------------
