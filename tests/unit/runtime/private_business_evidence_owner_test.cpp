// Purpose: qualify the genuine runtime owner's complete typed evidence preparation against literal
// lifecycle and seven-scope economic expectations while no event or business state is published.

#include "aegis/runtime/private_business_evidence_store.hpp"
#include "private_business_evidence_fixture.hpp"
#include "reference_configuration.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;
using test_support::BusinessEvidenceFixture;
using test_support::BusinessEvidenceInput;

// --------------------------------------------------------
// Author independent literal executor identities without executing or completing an owner turn.
[[nodiscard]] recovery::JournalReplayProvenance create_evidence_replay_provenance_or_throw() {
  return recovery::JournalReplayProvenance{
      test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(7U),
      test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(9U)};
}

// --------------------------------------------------------
// Author the exact integer scope tuple for a Q=2, N=20 reference order at each proposed prefix.
[[nodiscard]] risk::RiskScopeExposure create_evidence_scope_or_throw(execution::OrderSide side,
                                                                     std::int64_t remaining,
                                                                     std::int64_t confirmed) {
  const auto create_quantity_or_throw = [](std::int64_t value) {
    return test_support::create_m4_decimal_or_throw<model::Quantity>(value);
  };
  const auto create_notional_or_throw = [](std::int64_t value) {
    return test_support::create_m4_decimal_or_throw<model::Notional>(value);
  };
  const auto buy = side == execution::OrderSide::Buy;
  const auto sign = buy ? 1 : -1;
  return risk::RiskScopeExposure{remaining > 0 ? 1U : 0U,
                                 create_notional_or_throw(remaining * 10),
                                 create_quantity_or_throw(buy ? remaining : 0),
                                 create_quantity_or_throw(buy ? 0 : remaining),
                                 create_quantity_or_throw(remaining + confirmed),
                                 create_notional_or_throw(buy ? remaining * 10 : 0),
                                 create_notional_or_throw(buy ? 0 : remaining * 10),
                                 create_notional_or_throw((remaining + confirmed) * 10),
                                 create_notional_or_throw((remaining + confirmed) * 10),
                                 create_quantity_or_throw(sign * confirmed),
                                 create_notional_or_throw(sign * confirmed * 10)};
}

// --------------------------------------------------------
// Read-only evidence must preserve every canonical state, source row, identity table and oracle.
void check_evidence_owner_unchanged(
    const BusinessEvidenceFixture& fixture, const oms::PrivateOrderProjection& projection_before,
    const risk::ReservationEvidence& reservation_before,
    const std::vector<risk::RiskScopeExposureEvidence>& scopes_before) {
  const auto& reservations = fixture.authority.submission->reservations();
  CHECK(fixture.order().private_projection() == projection_before);
  const auto* reservation = reservations.find_reservation(fixture.order().reservation_id());
  REQUIRE(reservation != nullptr);
  CHECK(*reservation == reservation_before);
  CHECK(reservations.held_reservation_count() == 1U);
  CHECK(test_support::collect_evidence_scope_rows_or_throw(reservations) == scopes_before);
  const auto* inventory = risk::InventoryLedger::installed_inventory(reservations);
  REQUIRE(inventory != nullptr);
  CHECK(inventory->source_row_count() == 0U);
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
  CHECK(fixture.owner().retained_identity_turn_count() == 0U);
  CHECK(fixture.owner().account_safety_state(fixture.order().provenance().logical_account_id) ==
        risk::AccountSafetyState::Synchronized);
  CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());
  const auto ordinal = test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(7U);
  CHECK_FALSE(fixture.owner().find_committed_private_event_disposition(ordinal));
  CHECK_FALSE(fixture.owner().find_committed_reconciliation_event_disposition(ordinal));
  CHECK(fixture.owner().find_committed_retained_private_event_error(ordinal) == nullptr);
  CHECK(fixture.owner().find_committed_retained_reconciliation_event_error(ordinal) == nullptr);
}

// --------------------------------------------------------

// ########################################################################
// Each expected row is a literal contract consequence; no production planner supplies this oracle.
struct ExpectedEvidenceCase {
  BusinessEvidenceInput input;
  oms::PrivateEventDisposition disposition;
  oms::OutboundOrderState after_state;
  risk::ReservationState reservation_state;
  risk::ReservationClosureCause closure;
  std::int64_t remaining;
  std::int64_t confirmed;
  std::uint32_t callback_count;
};

// ########################################################################

// --------------------------------------------------------
// Both authoritative origins and economic sides retain complete proposed facts, with identical
// integer economics and distinct immutable originating identities, before canonical application.
TEST_CASE("initial business evidence preserves complete literal lifecycle and economics",
          "[private-business-evidence-owner]") {
  const std::array cases{
      ExpectedEvidenceCase{BusinessEvidenceInput::Acknowledgement,
                           oms::PrivateEventDisposition::Applied, oms::OutboundOrderState::Working,
                           risk::ReservationState::Held, risk::ReservationClosureCause::Unassigned,
                           2, 0, 1U},
      ExpectedEvidenceCase{BusinessEvidenceInput::PartialFill,
                           oms::PrivateEventDisposition::Applied,
                           oms::OutboundOrderState::PartiallyFilled, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 1, 1, 1U},
      ExpectedEvidenceCase{BusinessEvidenceInput::FullFill, oms::PrivateEventDisposition::Applied,
                           oms::OutboundOrderState::Filled, risk::ReservationState::ConsumedByFill,
                           risk::ReservationClosureCause::FullFill, 0, 2, 1U},
      ExpectedEvidenceCase{
          BusinessEvidenceInput::ExchangeRejection, oms::PrivateEventDisposition::Applied,
          oms::OutboundOrderState::ExchangeRejected, risk::ReservationState::Released,
          risk::ReservationClosureCause::ExchangeRejected, 0, 0, 1U},
      ExpectedEvidenceCase{BusinessEvidenceInput::CancellationAtZero,
                           oms::PrivateEventDisposition::Applied,
                           oms::OutboundOrderState::Cancelled, risk::ReservationState::Released,
                           risk::ReservationClosureCause::DefinitiveCancellation, 0, 0, 1U},
      ExpectedEvidenceCase{BusinessEvidenceInput::BufferedGap,
                           oms::PrivateEventDisposition::BufferedGap,
                           oms::OutboundOrderState::WriteInitiated, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 2, 0, 0U},
      ExpectedEvidenceCase{BusinessEvidenceInput::ContradictorySide,
                           oms::PrivateEventDisposition::SafetyContained,
                           oms::OutboundOrderState::WriteInitiated, risk::ReservationState::Held,
                           risk::ReservationClosureCause::Unassigned, 2, 0, 1U}};
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    for (const bool reconciliation : {false, true}) {
      for (const auto& expected : cases) {
        CAPTURE(side, reconciliation, expected.input);
        const BusinessEvidenceFixture fixture{side};
        const auto& reservations = fixture.authority.submission->reservations();
        const auto before = fixture.order().private_projection();
        const auto* held = reservations.find_reservation(fixture.order().reservation_id());
        REQUIRE(held != nullptr);
        const auto held_before = *held;
        const auto scopes_before = test_support::collect_evidence_scope_rows_or_throw(reservations);
        const auto input =
            fixture.normalize_business_input_or_throw(expected.input, reconciliation);
        const auto first_callback =
            expected.callback_count == 0U
                ? std::optional<model::CallbackOrdinal>{}
                : std::optional{
                      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U)};
        auto result = fixture.owner().prepare_initial_known_authoritative_business_evidence(
            input, create_evidence_replay_provenance_or_throw(), first_callback);
        REQUIRE(result);
        const auto& prepared = result.value();
        REQUIRE(prepared.has_preparation());
        REQUIRE(prepared.private_event_evidence() != nullptr);
        REQUIRE(prepared.journal_payload() != nullptr);
        REQUIRE(prepared.audit_span() != nullptr);
        CHECK(prepared.primary_audit_record_count() == 1U);
        CHECK(prepared.audit_span()->first_audit_ordinal().value() == 1U);
        CHECK(prepared.audit_span()->audit_record_count() == (expected.callback_count ? 3U : 1U));
        const auto& event = *prepared.private_event_evidence();
        const auto& payload = *prepared.journal_payload();
        const auto* primary = prepared.primary_audit_record_at(0U);
        REQUIRE(primary != nullptr);
        CHECK(prepared.primary_audit_record_at(1U) == nullptr);
        CHECK(event.input() == input);
        CHECK(event.ingress_semantic_value() ==
              oms::PrivateEventIngressSemanticValue::from_normalized_input(input));
        CHECK(event.subject_provenance() == input.provenance().subject());
        REQUIRE(event.subject_provenance());
        CHECK_FALSE(event.subject_provenance()->bot_id());
        CHECK_FALSE(event.subject_provenance()->desk_id());
        CHECK_FALSE(event.subject_provenance()->strategy_id());
        CHECK_FALSE(event.subject_provenance()->route());
        CHECK(event.root_provenance() == fixture.authority.m4_policy.root_provenance());
        CHECK(event.runtime_epoch_id() == fixture.owner().runtime_epoch_id());
        CHECK(event.replay_provenance() == create_evidence_replay_provenance_or_throw());
        CHECK(event.receive_time() == model::ReceiveTimestamp{20U});
        CHECK(event.disposition() == expected.disposition);
        CHECK_FALSE(event.journal_sequence());
        CHECK_FALSE(event.diagnostic_ordinal());
        CHECK(event.origin() == (reconciliation ? oms::PrivateEventOrigin::Reconciliation
                                                : oms::PrivateEventOrigin::Venue));
        CHECK(event.reconciliation_epoch_id().has_value() == reconciliation);
        REQUIRE(event.first_admission_resolution().known_resolution() != nullptr);
        const auto& known = *event.first_admission_resolution().known_resolution();
        CHECK(known.order_id == fixture.order().order_id());
        REQUIRE(known.provenance.subject());
        CHECK(known.provenance.subject()->bot_id() == fixture.order().provenance().bot_id);
        CHECK(known.provenance.subject()->desk_id() == fixture.order().provenance().desk_id);
        CHECK(known.provenance.subject()->strategy_id() ==
              fixture.order().provenance().strategy_id);
        CHECK(known.provenance.subject()->route()->route_id ==
              fixture.order().provenance().route_id);
        CHECK(payload.input() == input);
        CHECK(payload.first_admission_resolution() == event.first_admission_resolution());
        CHECK(primary->source_input() == input);
        CHECK(primary->originating_event() == event.originating_event());
        CHECK(primary->audit_ordinal().value() == 1U);
        CHECK(primary->kind() == trace::M4AuditKind::EventDisposition);
        CHECK(primary->root_provenance() == fixture.authority.m4_policy.root_provenance());
        CHECK(primary->runtime_epoch_id() == fixture.owner().runtime_epoch_id());
        CHECK(primary->subject_provenance() == known.provenance.subject());
        CHECK(primary->disposition() == expected.disposition);
        CHECK(primary->callback_count() == 0U);
        CHECK_FALSE(primary->callback_range());
        CHECK(primary->effect_count() == 1U);
        CHECK(primary->effect_at(1U) == nullptr);
        const auto* effect = primary->effect_at(0U);
        REQUIRE(effect != nullptr);
        CHECK(effect->order_id == fixture.order().order_id());
        CHECK(effect->provenance == known.provenance);
        CHECK(effect->oms_before == before);
        auto expected_projection = before;
        expected_projection.state = expected.after_state;
        if (expected.input == BusinessEvidenceInput::Acknowledgement) {
          expected_projection.exchange_acknowledged = true;
        }
        const auto has_execution = expected.input == BusinessEvidenceInput::PartialFill ||
                                   expected.input == BusinessEvidenceInput::FullFill ||
                                   expected.input == BusinessEvidenceInput::BufferedGap ||
                                   expected.input == BusinessEvidenceInput::ContradictorySide;
        if (has_execution) {
          expected_projection.execution_evidence_observed = true;
        }
        if (expected.input != BusinessEvidenceInput::ContradictorySide) {
          expected_projection.exchange_order_id = fixture.create_exchange_order_id_or_throw();
          expected_projection.exchange_mapping_established_by_execution = has_execution;
        }
        expected_projection.cumulative_filled_quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(expected.confirmed);
        if (expected.input == BusinessEvidenceInput::BufferedGap) {
          expected_projection.pending_fill_count = 1U;
        }
        if (expected.input == BusinessEvidenceInput::CancellationAtZero) {
          expected_projection.authoritative_terminal_cumulative_quantity =
              test_support::create_m4_decimal_or_throw<model::Quantity>(0);
          expected_projection.cancellation_state = oms::CancellationState::Confirmed;
        }
        CHECK(effect->oms_after == expected_projection);
        auto expected_reservation = held_before;
        expected_reservation.state = expected.reservation_state;
        expected_reservation.closure_cause = expected.closure;
        expected_reservation.remaining_exposure.quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(expected.remaining);
        expected_reservation.remaining_exposure.quote_notional =
            test_support::create_m4_decimal_or_throw<model::Notional>(expected.remaining * 10);
        expected_reservation.cumulative_confirmed_exposure.quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(expected.confirmed);
        expected_reservation.cumulative_confirmed_exposure.quote_notional =
            test_support::create_m4_decimal_or_throw<model::Notional>(expected.confirmed * 10);
        CHECK(effect->reservation_before == held_before);
        CHECK(effect->reservation_after == expected_reservation);
        const auto economics_expected = expected.remaining != 2 || expected.confirmed != 0;
        CHECK(effect->inventory_effects.has_value() == economics_expected);
        if (effect->inventory_effects) {
          const auto& provenance = fixture.order().provenance();
          const std::array<trace::RiskScopeAuditSubject, 7U> expected_subjects{
              provenance.bot_id,   provenance.desk_id,
              provenance.firm_id,  provenance.logical_account_id,
              provenance.route_id, provenance.instrument_id,
              provenance.venue_id};
          const auto before_scope = create_evidence_scope_or_throw(side, 2, 0);
          const auto after_scope =
              create_evidence_scope_or_throw(side, expected.remaining, expected.confirmed);
          const auto sign = side == execution::OrderSide::Buy ? 1 : -1;
          for (std::size_t index = 0U; index < expected_subjects.size(); ++index) {
            const auto& inventory_effect = (*effect->inventory_effects)[index];
            CHECK(inventory_effect.firm_id == provenance.firm_id);
            CHECK(inventory_effect.subject == expected_subjects[index]);
            CHECK(inventory_effect.instrument_id == provenance.instrument_id);
            CHECK(inventory_effect.quote_currency.value() == "USD");
            CHECK(inventory_effect.before == before_scope);
            CHECK(inventory_effect.after == after_scope);
            CHECK(inventory_effect.signed_quantity_delta ==
                  test_support::create_m4_decimal_or_throw<model::Quantity>(sign *
                                                                            expected.confirmed));
            CHECK(inventory_effect.signed_notional_delta ==
                  test_support::create_m4_decimal_or_throw<model::Notional>(
                      sign * expected.confirmed * 10));
          }
        }
        CHECK(effect->account_safety_transition.has_value() ==
              (expected.input == BusinessEvidenceInput::ContradictorySide));
        if (effect->account_safety_transition) {
          CHECK(effect->account_safety_transition->before ==
                risk::AccountSafetyState::Synchronized);
          CHECK(effect->account_safety_transition->after == risk::AccountSafetyState::Quarantined);
          CHECK(effect->account_safety_transition->reason ==
                risk::AccountSafetyReason::AuthoritativeContradiction);
        }
        CHECK(event.trade_semantic_value().has_value() ==
              (has_execution && expected.input != BusinessEvidenceInput::ContradictorySide));
        if (event.trade_semantic_value()) {
          const auto& trade = *event.trade_semantic_value();
          CHECK(trade.instrument_id() == fixture.order().provenance().instrument_id);
          CHECK(trade.metadata_revision() == fixture.order().provenance().metadata_revision);
          CHECK(trade.incremental_quantity() ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(
                    expected.input == BusinessEvidenceInput::FullFill ? 2 : 1));
          CHECK(trade.cumulative_quantity() ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(
                    expected.input == BusinessEvidenceInput::FullFill ||
                            expected.input == BusinessEvidenceInput::BufferedGap
                        ? 2
                        : 1));
          CHECK(trade.execution_price() == fixture.order().economics().price);
          const auto* resolved_trade =
              std::get_if<oms::KnownPrivateTradeResolution>(&trade.resolution_value());
          REQUIRE(resolved_trade != nullptr);
          CHECK(resolved_trade->order_id == fixture.order().order_id());
          CHECK(resolved_trade->canonical_side == side);
          const auto* source_execution =
              std::get_if<oms::ExecutionPayload>(&event.input().payload());
          REQUIRE(source_execution != nullptr);
          CHECK(source_execution->source_side == side);
        }
        CHECK(prepared.order_callback_count() == expected.callback_count);
        const auto* planned = prepared.planned_callback_record();
        CHECK((planned != nullptr) == (expected.callback_count != 0U));
        CHECK(prepared.reserved_terminal_audit_ordinal().has_value() ==
              (expected.callback_count != 0U));
        if (planned != nullptr) {
          CHECK(planned->kind() == trace::M4AuditKind::OrderCallbackDecision);
          CHECK(planned->callback_decision() == trace::CallbackDecision::Planned);
          CHECK(planned->audit_ordinal().value() == 2U);
          CHECK(prepared.reserved_terminal_audit_ordinal()->value() == 3U);
          CHECK(planned->effect_count() == 0U);
          CHECK(planned->callback_count() == 1U);
          CHECK(planned->callback_at(1U) == nullptr);
          REQUIRE(planned->callback_range());
          CHECK(planned->callback_range()->first_callback_ordinal().value() == 11U);
          CHECK(planned->callback_range()->last_callback_ordinal().value() == 11U);
          const auto* callback = planned->callback_at(0U);
          REQUIRE(callback != nullptr);
          CHECK(callback->order_id == fixture.order().order_id());
          CHECK(callback->provenance == known.provenance);
          CHECK(callback->originating_event == event.originating_event());
          CHECK(callback->trade_id.has_value() == has_execution);
          CHECK(callback->applied_cumulative_quantity.has_value() ==
                (expected.input == BusinessEvidenceInput::PartialFill ||
                 expected.input == BusinessEvidenceInput::FullFill));
          if (callback->trade_id) {
            CHECK(callback->trade_id == std::get<oms::ExecutionPayload>(input.payload()).trade_id);
            if (callback->applied_cumulative_quantity) {
              CHECK(callback->applied_cumulative_quantity ==
                    effect->oms_after.cumulative_filled_quantity);
            }
          }
        }
        const auto& store = fixture.owner().business_evidence_store();
        CHECK(store.accepted_private_event_record_count() == 0U);
        CHECK(store.accepted_private_audit_record_count() == 0U);
        CHECK(store.next_audit_ordinal().value() == 1U);
        CHECK(store.has_outstanding_preparation());
        check_evidence_owner_unchanged(fixture, before, held_before, scopes_before);
      }
    }
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Preparing and abandoning an identical event reuses prospective ordinals while immutable source
// identity and exact complete evidence values remain deterministic; a live lease blocks reuse.
TEST_CASE("runtime business evidence leases transfer without consuming evidence or ordinals",
          "[private-business-evidence-owner]") {
  const BusinessEvidenceFixture fixture;
  const auto input = fixture.normalize_business_input_or_throw(BusinessEvidenceInput::PartialFill);
  const auto callback = test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  const auto replay = create_evidence_replay_provenance_or_throw();
  const auto journal = test_support::create_m4_ordinal_or_throw<recovery::JournalSequence>(13U);
  const auto diagnostic =
      test_support::create_m4_ordinal_or_throw<recovery::DiagnosticOrdinal>(17U);
  std::optional<trace::PrivateEventEvidence> first_event;
  std::optional<trace::PrivateOrderAuditEffect> first_effect;
  {
    auto first = fixture.owner().prepare_initial_known_authoritative_business_evidence(
        input, replay, callback, journal, diagnostic);
    REQUIRE(first);
    first_event = *first.value().private_event_evidence();
    first_effect = *first.value().primary_audit_record_at(0U)->effect_at(0U);
    auto moved = std::move(first).value();
    CHECK_FALSE(first.value().has_preparation());
    CHECK(first.value().private_event_evidence() == nullptr);
    CHECK(first.value().journal_payload() == nullptr);
    CHECK(first.value().audit_span() == nullptr);
    CHECK(first.value().primary_audit_record_count() == 0U);
    CHECK(first.value().primary_audit_record_at(0U) == nullptr);
    CHECK(first.value().planned_callback_record() == nullptr);
    CHECK_FALSE(first.value().reserved_terminal_audit_ordinal());
    CHECK(first.value().order_callback_count() == 0U);
    const auto blocked = fixture.owner().prepare_initial_known_authoritative_business_evidence(
        input, replay, callback, journal, diagnostic);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().code == model::DomainErrorCode::PrivateEvidenceExhausted);
    CHECK(moved.private_event_evidence()->journal_sequence() == journal);
    CHECK(moved.private_event_evidence()->diagnostic_ordinal() == diagnostic);
    CHECK(moved.primary_audit_record_at(0U)->journal_sequence() == journal);
    CHECK(moved.planned_callback_record()->journal_sequence() == journal);
    CHECK(fixture.owner().business_evidence_store().accepted_private_event_record_count() == 0U);
    CHECK(fixture.owner().business_evidence_store().accepted_private_audit_record_count() == 0U);
    CHECK(fixture.owner().business_evidence_store().next_audit_ordinal().value() == 1U);
  }
  CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
  auto repeated = fixture.owner().prepare_initial_known_authoritative_business_evidence(
      input, replay, callback, journal, diagnostic);
  REQUIRE(repeated);
  CHECK(*repeated.value().private_event_evidence() == *first_event);
  CHECK(*repeated.value().primary_audit_record_at(0U)->effect_at(0U) == *first_effect);
  CHECK(repeated.value().audit_span()->first_audit_ordinal().value() == 1U);
}

// --------------------------------------------------------
// Callback-bearing proposals require a complete prospective callback identity, while buffered
// proposals forbid one; invalid shape must release economic scratch before a subsequent valid read.
TEST_CASE("runtime business evidence rejects callback shape and foreign or local inputs atomically",
          "[private-business-evidence-owner]") {
  const BusinessEvidenceFixture fixture;
  const auto replay = create_evidence_replay_provenance_or_throw();
  const auto callback = test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  const auto fill = fixture.normalize_business_input_or_throw(BusinessEvidenceInput::FullFill);
  const auto missing_callback =
      fixture.owner().prepare_initial_known_authoritative_business_evidence(fill, replay);
  CHECK_FALSE(missing_callback);
  const auto gap = fixture.normalize_business_input_or_throw(BusinessEvidenceInput::BufferedGap);
  CHECK_FALSE(
      fixture.owner().prepare_initial_known_authoritative_business_evidence(gap, replay, callback));
  const auto local = fixture.factory.normalize_order_timeout(
      oms::LocalPrivateEventOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                   model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
      fixture.authority.submission->outbound_oms(), fixture.order());
  REQUIRE(local);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_evidence(
      local.value(), replay, callback));
  const auto unknown = fixture.factory.normalize_venue_acknowledgement(
      fixture.create_venue_origin_or_throw(), fixture.create_exchange_order_id_or_throw(),
      std::nullopt);
  REQUIRE(unknown);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_evidence(
      unknown.value(), replay, callback));
  auto foreign_capacities = test_support::create_ordinary_m4_policy_capacities();
  ++foreign_capacities.max_private_event_records;
  const BusinessEvidenceFixture foreign{execution::OrderSide::Buy, foreign_capacities};
  const auto foreign_input =
      foreign.normalize_business_input_or_throw(BusinessEvidenceInput::Acknowledgement);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_evidence(
      foreign_input, replay, callback));
  CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
  CHECK(fixture.owner().business_evidence_store().accepted_private_audit_record_count() == 0U);
  CHECK(fixture.owner().business_evidence_store().next_audit_ordinal().value() == 1U);
  const auto valid =
      fixture.owner().prepare_initial_known_authoritative_business_evidence(fill, replay, callback);
  REQUIRE(valid);
  CHECK(valid.value().private_event_evidence()->input() == fill);
}

// --------------------------------------------------------
// The complete prepared record and nested seven-scope/callback values survive destruction of the
// coordinator and every source object; their immutable copied facts never dereference old owners.
TEST_CASE("runtime prepared business evidence survives complete owner destruction",
          "[private-business-evidence-owner]") {
  auto fixture = std::make_unique<BusinessEvidenceFixture>(execution::OrderSide::Sell);
  const auto input =
      fixture->normalize_business_input_or_throw(BusinessEvidenceInput::PartialFill, true);
  const auto callback = test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  auto result = fixture->owner().prepare_initial_known_authoritative_business_evidence(
      input, create_evidence_replay_provenance_or_throw(), callback);
  REQUIRE(result);
  auto prepared = std::move(result).value();
  const auto event_before = *prepared.private_event_evidence();
  const auto effect_before = *prepared.primary_audit_record_at(0U)->effect_at(0U);
  const auto callback_before = *prepared.planned_callback_record()->callback_at(0U);
  fixture.reset();
  REQUIRE(prepared.has_preparation());
  CHECK(*prepared.private_event_evidence() == event_before);
  CHECK(prepared.journal_payload()->input() == input);
  CHECK(*prepared.primary_audit_record_at(0U)->effect_at(0U) == effect_before);
  CHECK(*prepared.planned_callback_record()->callback_at(0U) == callback_before);
  REQUIRE(effect_before.inventory_effects);
  for (const auto& effect : *effect_before.inventory_effects) {
    CHECK(effect.signed_quantity_delta ==
          test_support::create_m4_decimal_or_throw<model::Quantity>(-1));
    CHECK(effect.signed_notional_delta ==
          test_support::create_m4_decimal_or_throw<model::Notional>(-10));
  }
  CHECK(prepared.audit_span()->audit_record_count() == 3U);
  CHECK(prepared.reserved_terminal_audit_ordinal()->value() == 3U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// A supported source instrument can contradict a known local instrument without losing its own
// independent source provenance. Containment proposals retain both subjects and no economics.
TEST_CASE("runtime business evidence contains supported source instrument contradictions",
          "[private-business-evidence-owner]") {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  REQUIRE_FALSE(params.instrument_metadata.empty());
  auto second_metadata = params.instrument_metadata.front();
  second_metadata.instrument_id = test_support::extract_evidence_value_or_throw(
      model::InstrumentId::parse_identifier("ETH-USD-PERPETUAL"));
  second_metadata.venue_instrument_id = test_support::extract_evidence_value_or_throw(
      model::VenueInstrumentId::parse_identifier("ETH-PERPETUAL"));
  second_metadata.base_currency = "ETH";
  second_metadata.settlement_currency = "ETH";
  const auto source_instrument = second_metadata.instrument_id;
  const auto source_revision = second_metadata.revision;
  params.instrument_metadata.push_back(std::move(second_metadata));
  auto authority = test_support::create_m4_owner_test_authority_or_throw(std::move(params));
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  const auto factory = test_support::create_evidence_event_factory_or_throw(authority);
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  const auto* order = authority.submission->outbound_oms().find_order(*submitted.order_id());
  const auto* owner = authority.submission->private_order_reconciler();
  REQUIRE(order != nullptr);
  REQUIRE(owner != nullptr);
  const auto& provenance = order->provenance();
  REQUIRE(provenance.instrument_id != source_instrument);
  const auto before = order->private_projection();
  const auto* held = authority.submission->reservations().find_reservation(order->reservation_id());
  REQUIRE(held != nullptr);
  const auto held_before = *held;
  const auto scopes_before =
      test_support::collect_evidence_scope_rows_or_throw(authority.submission->reservations());
  const auto exchange =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x51U);
  const auto locator = test_support::extract_evidence_value_or_throw(
      oms::PrivateOrderLocator::create_private_order_locator(order->order_id(), exchange));
  const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x61U);
  const auto quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto callback = test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  for (const bool reconciliation : {false, true}) {
    CAPTURE(reconciliation);
    const auto input = test_support::extract_evidence_value_or_throw(
        reconciliation
            ? factory.normalize_reconciliation_execution(
                  oms::ReconciliationPrivateEventOrigin{
                      test_support::create_m4_reconciliation_epoch_or_throw(),
                      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(
                          0x41U),
                      test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(
                          1U),
                      model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
                  provenance.logical_account_id, provenance.venue_id, locator, trade,
                  source_instrument, source_revision, quantity, quantity, order->economics().price,
                  order->economics().side)
            : factory.normalize_venue_execution(
                  oms::VenuePrivateEventOrigin{
                      oms::VenuePrivateEventKey{
                          provenance.venue_id, provenance.logical_account_id,
                          test_support::create_m4_opaque_identity_or_throw<
                              oms::PrivateSourceEpochId>(0x31U),
                          test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(
                              0x71U)},
                      model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
                  locator, trade, source_instrument, source_revision, quantity, quantity,
                  order->economics().price, order->economics().side));
    REQUIRE(input.provenance().subject());
    REQUIRE(input.provenance().subject()->instrument());
    CHECK(input.provenance().subject()->instrument()->instrument_id == source_instrument);
    auto result = owner->prepare_initial_known_authoritative_business_evidence(
        input, create_evidence_replay_provenance_or_throw(), callback);
    REQUIRE(result);
    const auto& prepared = result.value();
    const auto* event = prepared.private_event_evidence();
    REQUIRE(event != nullptr);
    CHECK(event->input() == input);
    CHECK(event->disposition() == oms::PrivateEventDisposition::SafetyContained);
    CHECK(event->subject_provenance() == input.provenance().subject());
    const auto* known = event->first_admission_resolution().known_resolution();
    REQUIRE(known != nullptr);
    REQUIRE(known->provenance.subject());
    CHECK(known->provenance.subject()->instrument()->instrument_id == provenance.instrument_id);
    CHECK(event->subject_provenance()->instrument()->instrument_id !=
          known->provenance.subject()->instrument()->instrument_id);
    const auto* effect = prepared.primary_audit_record_at(0U)->effect_at(0U);
    REQUIRE(effect != nullptr);
    CHECK(effect->provenance == known->provenance);
    CHECK(effect->reservation_before == held_before);
    CHECK(effect->reservation_after == held_before);
    CHECK_FALSE(effect->inventory_effects);
    auto expected_after = before;
    expected_after.execution_evidence_observed = true;
    CHECK(effect->oms_before == before);
    CHECK(effect->oms_after == expected_after);
    REQUIRE(effect->account_safety_transition);
    CHECK(effect->account_safety_transition->reason ==
          risk::AccountSafetyReason::AuthoritativeContradiction);
    const auto* notification = prepared.planned_callback_record()->callback_at(0U);
    REQUIRE(notification != nullptr);
    CHECK(notification->trade_id == trade);
    CHECK_FALSE(notification->applied_cumulative_quantity);
    CHECK(notification->provenance == known->provenance);
    CHECK(order->private_projection() == before);
    CHECK(*authority.submission->reservations().find_reservation(order->reservation_id()) ==
          held_before);
    CHECK(test_support::collect_evidence_scope_rows_or_throw(
              authority.submission->reservations()) == scopes_before);
    CHECK(owner->account_safety_state(provenance.logical_account_id) ==
          risk::AccountSafetyState::Synchronized);
    CHECK(owner->business_evidence_store().accepted_private_event_record_count() == 0U);
    CHECK(owner->business_evidence_store().accepted_private_audit_record_count() == 0U);
    CHECK(owner->business_evidence_store().next_audit_ordinal().value() == 1U);
  }
}

// --------------------------------------------------------

} // namespace
