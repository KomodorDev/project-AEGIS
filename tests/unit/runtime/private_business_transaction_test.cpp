// Purpose: qualify complete owner-sealed initial business preparations, real journal reservations,
// lease lifetime and freshness while proving no private publication or economic consumption occurs.

#include "aegis/runtime/private_business_evidence_store.hpp"
#include "aegis/runtime/private_business_transaction.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "private_business_transaction_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;
using test_support::BusinessEvidenceInput;
using test_support::PrivateBusinessTransactionFixture;

// --------------------------------------------------------
// Authored replay numbers are immutable semantic candidates, independent from real admission.
[[nodiscard]] recovery::JournalReplayProvenance create_transaction_replay_or_throw() {
  return recovery::JournalReplayProvenance{
      test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(7U),
      test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(9U)};
}

// --------------------------------------------------------
// Use one independently authored callback identity for each single-callback initial scenario.
[[nodiscard]] model::CallbackOrdinal create_transaction_callback_or_throw() {
  return test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
}

// --------------------------------------------------------
// Construct the exact literal seven-scope state for the integer Q=2/N=20 reference order.
[[nodiscard]] risk::RiskScopeExposure create_transaction_scope_or_throw(execution::OrderSide side,
                                                                        std::int64_t remaining,
                                                                        std::int64_t confirmed) {
  const bool buy = side == execution::OrderSide::Buy;
  const auto sign = buy ? 1 : -1;
  return risk::RiskScopeExposure{
      remaining > 0 ? 1U : 0U,
      test_support::create_m4_decimal_or_throw<model::Notional>(remaining * 10),
      test_support::create_m4_decimal_or_throw<model::Quantity>(buy ? remaining : 0),
      test_support::create_m4_decimal_or_throw<model::Quantity>(buy ? 0 : remaining),
      test_support::create_m4_decimal_or_throw<model::Quantity>(remaining + confirmed),
      test_support::create_m4_decimal_or_throw<model::Notional>(buy ? remaining * 10 : 0),
      test_support::create_m4_decimal_or_throw<model::Notional>(buy ? 0 : remaining * 10),
      test_support::create_m4_decimal_or_throw<model::Notional>((remaining + confirmed) * 10),
      test_support::create_m4_decimal_or_throw<model::Notional>((remaining + confirmed) * 10),
      test_support::create_m4_decimal_or_throw<model::Quantity>(sign * confirmed),
      test_support::create_m4_decimal_or_throw<model::Notional>(sign * confirmed * 10)};
}

// --------------------------------------------------------
// Compare all existing business state against its genuine initial baseline; prospective records
// must not masquerade as published rows, consumed inputs, or applied economics.
void check_transaction_owner_unchanged(
    const PrivateBusinessTransactionFixture& fixture,
    const oms::PrivateOrderProjection& projection_before,
    const risk::ReservationEvidence& reservation_before,
    const std::vector<risk::RiskScopeExposureEvidence>& scopes_before) {
  const auto& reservations = fixture.authority.submission->reservations();
  CHECK(fixture.order().private_projection() == projection_before);
  const auto* held = reservations.find_reservation(fixture.order().reservation_id());
  REQUIRE(held != nullptr);
  CHECK(*held == reservation_before);
  CHECK(reservations.held_reservation_count() == 1U);
  CHECK(test_support::collect_evidence_scope_rows_or_throw(reservations) == scopes_before);
  const auto* inventory = risk::InventoryLedger::installed_inventory(reservations);
  REQUIRE(inventory != nullptr);
  CHECK(inventory->source_row_count() == 0U);
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
  CHECK(fixture.owner().retained_identity_turn_count() == 0U);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 0U);
  CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 0U);
  const auto& store = fixture.owner().business_evidence_store();
  CHECK(store.accepted_private_event_record_count() == 0U);
  CHECK(store.accepted_private_audit_record_count() == 0U);
  CHECK(store.next_audit_ordinal().value() == 1U);
  CHECK(fixture.owner().account_safety_state(fixture.order().provenance().logical_account_id) ==
        risk::AccountSafetyState::Synchronized);
  CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());
  CHECK_FALSE(fixture.owner().find_committed_private_event_disposition(
      create_transaction_replay_or_throw().admission_ordinal));
  CHECK_FALSE(fixture.owner().find_committed_reconciliation_event_disposition(
      create_transaction_replay_or_throw().admission_ordinal));
}

// --------------------------------------------------------
// An abandoned real reservation leaves exactly the acknowledged namespace input in cold media.
void check_transaction_cold_namespace_prefix(
    const recovery::DeterministicFakeRecoveryMedium& medium) {
  const auto published = medium.published_journal_record_count();
  const auto acknowledged = medium.acknowledged_journal_record_count();
  const auto namespaces = medium.registered_namespace_count();
  REQUIRE(published);
  REQUIRE(acknowledged);
  REQUIRE(namespaces);
  CHECK(published.value() == 1U);
  CHECK(acknowledged.value() == 1U);
  CHECK(namespaces.value() == 1U);
  const auto row = medium.published_journal_record_at(0U);
  REQUIRE(row);
  CHECK(row.value().kind() == recovery::JournalRecordKind::NamespaceRegistered);
  CHECK(row.value().sequence().value() == 1U);
  CHECK_FALSE(row.value().predecessor());
  CHECK_FALSE(row.value().audit_span());
  CHECK_FALSE(medium.published_journal_record_at(1U));
}

// --------------------------------------------------------

// ########################################################################
// Literal contract outcomes are authored separately from the production transition/economics
// algorithms. Seven inventory scope rows remain one primary audit record, never seven ordinals.
struct ExpectedTransactionCase {
  BusinessEvidenceInput input;
  oms::PrivateEventDisposition disposition;
  oms::OutboundOrderState after_state;
  risk::ReservationState reservation_state;
  risk::ReservationClosureCause closure;
  std::int64_t remaining;
  std::int64_t confirmed;
  bool callback;
};

// ########################################################################
// Interesting syntax: requires-expressions prove that const preparations expose no command
// capable of publishing inputs, acknowledging durability, or extracting mutable economics.
template <typename Value>
concept HasTransactionPublication =
    requires(Value& value, runtime::PreparedInitialKnownPrivateBusinessTransaction& transaction) {
      value.publish_private_business_transaction(transaction);
    };

// ########################################################################
// Ordinary callers cannot acknowledge a prepared input through the closed fake-medium surface.
template <typename Value>
concept HasPreparedInputAcknowledgement =
    requires(Value& value, recovery::JournalSequence sequence) {
      value.acknowledge_private_event_input(sequence);
    };

// ########################################################################

// --------------------------------------------------------
// The capsule is owner-minted and move-only; every component getter is immutable and therefore
// cannot convey its economic component's existing commit capability.
TEST_CASE("initial business transaction exposes only closed immutable preparation authority",
          "[private-business-transaction][authority]") {
  using Capsule = runtime::PreparedInitialKnownPrivateBusinessTransaction;
  STATIC_REQUIRE_FALSE(std::is_default_constructible_v<Capsule>);
  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Capsule>);
  STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<Capsule>);
  STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Capsule>);
  STATIC_REQUIRE(std::is_nothrow_move_assignable_v<Capsule>);
  STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const Capsule&>().identity_plan()),
                                const runtime::FirstSeenAuthoritativePrivateIdentityPlan*>);
  STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const Capsule&>().business_proposal()),
                                const runtime::InitialKnownPrivateBusinessProposal*>);
  STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const Capsule&>().business_evidence()),
                                const runtime::PreparedPrivateBusinessEvidence*>);
  STATIC_REQUIRE(std::is_same_v<decltype(std::declval<const Capsule&>().journal_record()),
                                const recovery::JournalRecord*>);
  STATIC_REQUIRE_FALSE(std::is_constructible_v<risk::ReservationInventoryPlan,
                                               const risk::ReservationInventoryPlan&&>);
  STATIC_REQUIRE_FALSE(HasTransactionPublication<runtime::PrivateOrderReconciler>);
  STATIC_REQUIRE_FALSE(HasTransactionPublication<recovery::DeterministicFakeRecoveryMedium>);
  STATIC_REQUIRE_FALSE(HasPreparedInputAcknowledgement<recovery::DeterministicFakeRecoveryMedium>);
}

// --------------------------------------------------------
// Both source origins and Buy/Sell economics produce the full medium-assigned causal envelope
// and exact literal prospective effects while all live business and accepted evidence stays empty.
TEST_CASE(
    "initial business transaction retains exact literal source economics and real journal envelope",
    "[private-business-transaction]") {
  const std::array cases{
      ExpectedTransactionCase{BusinessEvidenceInput::Acknowledgement,
                              oms::PrivateEventDisposition::Applied,
                              oms::OutboundOrderState::Working, risk::ReservationState::Held,
                              risk::ReservationClosureCause::Unassigned, 2, 0, true},
      ExpectedTransactionCase{
          BusinessEvidenceInput::PartialFill, oms::PrivateEventDisposition::Applied,
          oms::OutboundOrderState::PartiallyFilled, risk::ReservationState::Held,
          risk::ReservationClosureCause::Unassigned, 1, 1, true},
      ExpectedTransactionCase{
          BusinessEvidenceInput::FullFill, oms::PrivateEventDisposition::Applied,
          oms::OutboundOrderState::Filled, risk::ReservationState::ConsumedByFill,
          risk::ReservationClosureCause::FullFill, 0, 2, true},
      ExpectedTransactionCase{
          BusinessEvidenceInput::ExchangeRejection, oms::PrivateEventDisposition::Applied,
          oms::OutboundOrderState::ExchangeRejected, risk::ReservationState::Released,
          risk::ReservationClosureCause::ExchangeRejected, 0, 0, true},
      ExpectedTransactionCase{BusinessEvidenceInput::CancellationAtZero,
                              oms::PrivateEventDisposition::Applied,
                              oms::OutboundOrderState::Cancelled, risk::ReservationState::Released,
                              risk::ReservationClosureCause::DefinitiveCancellation, 0, 0, true},
      ExpectedTransactionCase{BusinessEvidenceInput::BufferedGap,
                              oms::PrivateEventDisposition::BufferedGap,
                              oms::OutboundOrderState::WriteInitiated, risk::ReservationState::Held,
                              risk::ReservationClosureCause::Unassigned, 2, 0, false},
      ExpectedTransactionCase{BusinessEvidenceInput::ContradictorySide,
                              oms::PrivateEventDisposition::SafetyContained,
                              oms::OutboundOrderState::WriteInitiated, risk::ReservationState::Held,
                              risk::ReservationClosureCause::Unassigned, 2, 0, true}};
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    for (const bool reconciliation : {false, true}) {
      for (const auto& expected : cases) {
        CAPTURE(side, reconciliation, expected.input);
        const PrivateBusinessTransactionFixture fixture{side};
        const auto& reservations = fixture.authority.submission->reservations();
        const auto before = fixture.order().private_projection();
        const auto* held = reservations.find_reservation(fixture.order().reservation_id());
        REQUIRE(held != nullptr);
        const auto held_before = *held;
        const auto scopes_before = test_support::collect_evidence_scope_rows_or_throw(reservations);
        const auto input = fixture.normalize_input_or_throw(expected.input, reconciliation);
        const auto callback = expected.callback
                                  ? std::optional{create_transaction_callback_or_throw()}
                                  : std::optional<model::CallbackOrdinal>{};
        const auto diagnostic =
            test_support::create_m4_ordinal_or_throw<recovery::DiagnosticOrdinal>(17U);
        auto result = fixture.owner().prepare_initial_known_authoritative_business_transaction(
            input, create_transaction_replay_or_throw(), callback, diagnostic);
        REQUIRE(result);
        const auto& transaction = result.value();
        REQUIRE(transaction.has_preparation());
        REQUIRE(transaction.identity_plan() != nullptr);
        REQUIRE(transaction.business_proposal() != nullptr);
        REQUIRE(transaction.business_evidence() != nullptr);
        REQUIRE(transaction.journal_record() != nullptr);
        REQUIRE(fixture.owner().validate_prepared_initial_known_business_transaction(transaction));
        const auto& identity = *transaction.identity_plan();
        const auto& business = *transaction.business_proposal();
        const auto& evidence = *transaction.business_evidence();
        const auto& journal = *transaction.journal_record();
        const auto* event = evidence.private_event_evidence();
        REQUIRE(event != nullptr);
        REQUIRE(evidence.audit_span() != nullptr);
        const auto* primary = evidence.primary_audit_record_at(0U);
        REQUIRE(primary != nullptr);
        REQUIRE(primary->effect_count() == 1U);
        const auto* effect = primary->effect_at(0U);
        REQUIRE(effect != nullptr);
        const auto* known = event->first_admission_resolution().known_resolution();
        REQUIRE(known != nullptr);
        const auto* correlation = std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(
            &identity.correlation_plan());
        REQUIRE(correlation != nullptr);
        CHECK(correlation->resolution == event->first_admission_resolution());
        CHECK(identity.ingress_semantic_value() ==
              oms::PrivateEventIngressSemanticValue::from_normalized_input(input));
        CHECK(business.input() == input);
        CHECK(business.first_admission_resolution() == event->first_admission_resolution());
        CHECK(business.proposed_audit_span() == *evidence.audit_span());
        CHECK(business.reservation_before() == held_before);
        CHECK(event->input() == input);
        CHECK(event->disposition() == expected.disposition);
        CHECK(event->subject_provenance() == input.provenance().subject());
        CHECK(event->origin() == (reconciliation ? oms::PrivateEventOrigin::Reconciliation
                                                 : oms::PrivateEventOrigin::Venue));
        CHECK(event->journal_sequence() == journal.sequence());
        CHECK(event->diagnostic_ordinal() == diagnostic);
        CHECK(journal.kind() == recovery::JournalRecordKind::PrivateEventInput);
        CHECK(journal.lineage_id() == fixture.medium->lineage_id());
        CHECK(journal.sequence().value() == 2U);
        REQUIRE(journal.predecessor());
        CHECK(journal.predecessor()->value() == 1U);
        CHECK(journal.runtime_epoch_id() == fixture.owner().runtime_epoch_id());
        CHECK(journal.root_provenance() == fixture.authority.m4_policy.root_provenance());
        CHECK(journal.subject_provenance() == known->provenance.subject());
        CHECK(journal.replay_provenance() == create_transaction_replay_or_throw());
        REQUIRE(journal.audit_span());
        CHECK(*journal.audit_span() == *evidence.audit_span());
        CHECK(journal.audit_span()->first_audit_ordinal().value() == 1U);
        CHECK(journal.audit_span()->audit_record_count() == (expected.callback ? 3U : 1U));
        const auto* payload =
            std::get_if<recovery::PrivateEventInputJournalPayload>(&journal.payload());
        REQUIRE(payload != nullptr);
        CHECK(payload->input() == input);
        CHECK(payload->first_admission_resolution() == event->first_admission_resolution());
        CHECK(*payload == *evidence.journal_payload());
        CHECK(primary->journal_sequence() == journal.sequence());
        CHECK(primary->subject_provenance() == known->provenance.subject());
        CHECK(primary->source_input() == input);
        CHECK(primary->disposition() == expected.disposition);
        CHECK(effect->order_id == fixture.order().order_id());
        CHECK(effect->provenance == known->provenance);
        CHECK(effect->oms_before == before);
        auto expected_projection = before;
        expected_projection.state = expected.after_state;
        expected_projection.exchange_acknowledged =
            expected.input == BusinessEvidenceInput::Acknowledgement;
        const bool has_execution = expected.input == BusinessEvidenceInput::PartialFill ||
                                   expected.input == BusinessEvidenceInput::FullFill ||
                                   expected.input == BusinessEvidenceInput::BufferedGap ||
                                   expected.input == BusinessEvidenceInput::ContradictorySide;
        expected_projection.execution_evidence_observed = has_execution;
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
        CHECK(effect->reservation_before == held_before);
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
        CHECK(effect->reservation_after == expected_reservation);
        const bool has_economics = expected.remaining != 2 || expected.confirmed != 0;
        CHECK((business.economics_plan() != nullptr) == has_economics);
        CHECK(effect->inventory_effects.has_value() == has_economics);
        if (business.economics_plan() != nullptr) {
          CHECK(business.economics_plan()->reservation_before() == held_before);
          CHECK(business.economics_plan()->reservation_after() == expected_reservation);
          CHECK(business.economics_plan()->execution_effect_count() ==
                (expected.confirmed != 0 ? 1U : 0U));
          const auto* execution = business.economics_plan()->execution_effect_at(0U);
          if (expected.confirmed != 0) {
            REQUIRE(execution != nullptr);
            CHECK(execution->execution == input);
            CHECK(execution->audit_ordinal.value() == 1U);
            CHECK(execution->reservation_before == held_before);
            CHECK(execution->reservation_after == expected_reservation);
          } else {
            CHECK(execution == nullptr);
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
        const auto* planned_trade =
            std::get_if<runtime::FirstSeenPrivateTradeIdentityPlan>(&identity.trade_plan());
        CHECK((planned_trade != nullptr) ==
              (has_execution && expected.input != BusinessEvidenceInput::ContradictorySide));
        CHECK(event->trade_semantic_value().has_value() == (planned_trade != nullptr));
        if (planned_trade != nullptr) {
          CHECK(event->trade_semantic_value() == planned_trade->semantic_value);
        }
        if (effect->inventory_effects) {
          const auto& provenance = fixture.order().provenance();
          const std::array<trace::RiskScopeAuditSubject, 7U> subjects{
              provenance.bot_id,   provenance.desk_id,
              provenance.firm_id,  provenance.logical_account_id,
              provenance.route_id, provenance.instrument_id,
              provenance.venue_id};
          const auto sign = side == execution::OrderSide::Buy ? 1 : -1;
          for (std::size_t index = 0U; index < subjects.size(); ++index) {
            const auto& row = (*effect->inventory_effects)[index];
            CHECK(row.subject == subjects[index]);
            CHECK(row.firm_id == provenance.firm_id);
            CHECK(row.instrument_id == provenance.instrument_id);
            CHECK(row.quote_currency.value() == "USD");
            CHECK(row.before == create_transaction_scope_or_throw(side, 2, 0));
            CHECK(row.after ==
                  create_transaction_scope_or_throw(side, expected.remaining, expected.confirmed));
            CHECK(row.signed_quantity_delta ==
                  test_support::create_m4_decimal_or_throw<model::Quantity>(sign *
                                                                            expected.confirmed));
            CHECK(row.signed_notional_delta ==
                  test_support::create_m4_decimal_or_throw<model::Notional>(
                      sign * expected.confirmed * 10));
          }
        }
        CHECK(evidence.order_callback_count() == (expected.callback ? 1U : 0U));
        CHECK(evidence.reserved_terminal_audit_ordinal().has_value() == expected.callback);
        const auto* planned = evidence.planned_callback_record();
        CHECK((planned != nullptr) == expected.callback);
        if (planned != nullptr) {
          CHECK(planned->callback_decision() == trace::CallbackDecision::Planned);
          CHECK(planned->journal_sequence() == journal.sequence());
          CHECK(planned->audit_ordinal().value() == 2U);
          CHECK(evidence.reserved_terminal_audit_ordinal()->value() == 3U);
          REQUIRE(planned->callback_at(0U) != nullptr);
          CHECK(planned->callback_at(0U)->order_id == fixture.order().order_id());
          CHECK(planned->callback_at(0U)->originating_event == event->originating_event());
          CHECK(planned->callback_at(0U)->provenance == known->provenance);
          CHECK(planned->callback_at(0U)->trade_id.has_value() == has_execution);
          CHECK(planned->callback_at(0U)->applied_cumulative_quantity.has_value() ==
                (expected.confirmed != 0));
        }
        CHECK(evidence.primary_audit_record_at(1U) == nullptr);
        CHECK_FALSE(fixture.medium->published_journal_record_count());
        check_transaction_owner_unchanged(fixture, before, held_before, scopes_before);
      }
    }
  }
}

// --------------------------------------------------------
// The execution's original economic lease must stay busy for the capsule's whole life, rather
// than disappear after the evidence copy. Moving transfers all leases; abandonment reuses both
// the actual journal sequence and prospective audit ordinal without changing accepted prefixes.
TEST_CASE("initial business transaction keeps all leases through moves and releases them together",
          "[private-business-transaction][lifetime]") {
  const PrivateBusinessTransactionFixture fixture;
  const auto input = fixture.normalize_input_or_throw(BusinessEvidenceInput::PartialFill);
  const auto* inventory =
      risk::InventoryLedger::installed_inventory(fixture.authority.submission->reservations());
  REQUIRE(inventory != nullptr);
  const std::array executions{risk::ReservationInventoryExecutionInput{
      input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)}};
  std::optional<recovery::JournalRecord> first_journal;
  std::optional<trace::PrivateEventEvidence> first_event;
  {
    auto result = fixture.owner().prepare_initial_known_authoritative_business_transaction(
        input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
    REQUIRE(result);
    first_journal = *result.value().journal_record();
    first_event = *result.value().business_evidence()->private_event_evidence();
    const auto busy = inventory->plan_cumulative_fill_batch(fixture.order(), executions);
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == model::DomainErrorCode::InvalidReservationConversion);
    auto moved = std::move(result).value();
    CHECK_FALSE(result.value().has_preparation());
    CHECK(result.value().identity_plan() == nullptr);
    CHECK(result.value().business_proposal() == nullptr);
    CHECK(result.value().business_evidence() == nullptr);
    CHECK(result.value().journal_record() == nullptr);
    const auto moved_validation =
        fixture.owner().validate_prepared_initial_known_business_transaction(result.value());
    REQUIRE_FALSE(moved_validation);
    CHECK(moved_validation.error().context.field == "private_business_transaction.owner");
    REQUIRE(fixture.owner().validate_prepared_initial_known_business_transaction(moved));
    CHECK_FALSE(inventory->plan_cumulative_fill_batch(fixture.order(), executions));
    CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_transaction(
        input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw()));
    CHECK(fixture.owner().business_evidence_store().has_outstanding_preparation());
  }
  CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
  {
    const auto available = inventory->plan_cumulative_fill_batch(fixture.order(), executions);
    REQUIRE(available);
  }
  const auto repeated = fixture.owner().prepare_initial_known_authoritative_business_transaction(
      input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(repeated);
  CHECK(*repeated.value().journal_record() == *first_journal);
  CHECK(*repeated.value().business_evidence()->private_event_evidence() == *first_event);
  CHECK(repeated.value().journal_record()->sequence().value() == 2U);
  CHECK(repeated.value().business_evidence()->audit_span()->first_audit_ordinal().value() == 1U);
}

// --------------------------------------------------------
// Move assignment first abandons the destination's complete earlier reservation, then transfers
// the source's exact owner identity. Releasing either owner must not clear the other owner's lease.
TEST_CASE("initial business transaction move assignment releases the prior owner reservation",
          "[private-business-transaction][lifetime]") {
  const PrivateBusinessTransactionFixture first;
  const PrivateBusinessTransactionFixture second;
  const auto first_input = first.normalize_input_or_throw(BusinessEvidenceInput::PartialFill);
  const auto second_input = second.normalize_input_or_throw(BusinessEvidenceInput::PartialFill);
  auto first_result = first.owner().prepare_initial_known_authoritative_business_transaction(
      first_input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  auto second_result = second.owner().prepare_initial_known_authoritative_business_transaction(
      second_input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(first_result);
  REQUIRE(second_result);
  auto destination = std::move(second_result).value();
  destination = std::move(first_result).value();
  CHECK_FALSE(first_result.value().has_preparation());
  REQUIRE(first.owner().validate_prepared_initial_known_business_transaction(destination));
  CHECK_FALSE(second.owner().validate_prepared_initial_known_business_transaction(destination));
  CHECK(first.owner().business_evidence_store().has_outstanding_preparation());
  CHECK_FALSE(second.owner().business_evidence_store().has_outstanding_preparation());
  const auto* second_inventory =
      risk::InventoryLedger::installed_inventory(second.authority.submission->reservations());
  REQUIRE(second_inventory != nullptr);
  {
    const std::array executions{risk::ReservationInventoryExecutionInput{
        second_input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)}};
    REQUIRE(second_inventory->plan_cumulative_fill_batch(second.order(), executions));
  }
  const auto second_reprepared =
      second.owner().prepare_initial_known_authoritative_business_transaction(
          second_input, create_transaction_replay_or_throw(),
          create_transaction_callback_or_throw());
  REQUIRE(second_reprepared);
  CHECK(second_reprepared.value().journal_record()->sequence().value() == 2U);
  REQUIRE(second.owner().validate_prepared_initial_known_business_transaction(
      second_reprepared.value()));
  REQUIRE(first.owner().validate_prepared_initial_known_business_transaction(destination));
}

// --------------------------------------------------------
// A callback-shape rejection occurs after genuine economics and real journal reservation can be
// prepared. Its failure must release every temporary lease, allowing the same complete retry.
TEST_CASE(
    "initial business transaction rolls back late callback failure without consuming capacity",
    "[private-business-transaction][rollback]") {
  const PrivateBusinessTransactionFixture fixture;
  const auto input = fixture.normalize_input_or_throw(BusinessEvidenceInput::FullFill);
  const auto before = fixture.order().private_projection();
  const auto* held = fixture.authority.submission->reservations().find_reservation(
      fixture.order().reservation_id());
  REQUIRE(held != nullptr);
  const auto held_before = *held;
  const auto scopes_before = test_support::collect_evidence_scope_rows_or_throw(
      fixture.authority.submission->reservations());
  for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
    const auto failed = fixture.owner().prepare_initial_known_authoritative_business_transaction(
        input, create_transaction_replay_or_throw());
    REQUIRE_FALSE(failed);
    CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
    check_transaction_owner_unchanged(fixture, before, held_before, scopes_before);
  }
  const auto* inventory =
      risk::InventoryLedger::installed_inventory(fixture.authority.submission->reservations());
  REQUIRE(inventory != nullptr);
  {
    const std::array executions{risk::ReservationInventoryExecutionInput{
        input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)}};
    REQUIRE(inventory->plan_cumulative_fill_batch(fixture.order(), executions));
  }
  const auto prepared = fixture.owner().prepare_initial_known_authoritative_business_transaction(
      input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(prepared);
  CHECK(prepared.value().journal_record()->sequence().value() == 2U);
  REQUIRE(fixture.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
}

// --------------------------------------------------------
// Namespace registration itself consumes one real slot. Capacity one cannot reserve a business
// input; capacity two can, and abandoning either outcome leaves the same namespace-only history.
TEST_CASE("initial business transaction preflights exact remaining real journal capacity",
          "[private-business-transaction][capacity]") {
  for (const std::uint32_t capacity : {1U, 2U}) {
    CAPTURE(capacity);
    auto capacities = test_support::create_ordinary_m4_policy_capacities();
    capacities.max_journal_records = capacity;
    PrivateBusinessTransactionFixture fixture{execution::OrderSide::Buy, capacities};
    const auto input = fixture.normalize_input_or_throw(BusinessEvidenceInput::FullFill);
    {
      const auto prepared =
          fixture.owner().prepare_initial_known_authoritative_business_transaction(
              input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
      if (capacity == 1U) {
        REQUIRE_FALSE(prepared);
        CHECK(prepared.error().code == model::DomainErrorCode::JournalCapacityExceeded);
        CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
        const auto* inventory = risk::InventoryLedger::installed_inventory(
            fixture.authority.submission->reservations());
        REQUIRE(inventory != nullptr);
        const std::array executions{risk::ReservationInventoryExecutionInput{
            input, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)}};
        CHECK(inventory->plan_cumulative_fill_batch(fixture.order(), executions));
      } else {
        REQUIRE(prepared);
        CHECK(prepared.value().journal_record()->sequence().value() == 2U);
        REQUIRE(
            fixture.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
      }
    }
    fixture.authority.submission.reset();
    check_transaction_cold_namespace_prefix(*fixture.medium);
  }
}

// --------------------------------------------------------
// Equal policy, client namespace, order identity and copied source cannot substitute for the
// original owner incarnation. Movement revokes every source getter before validation runs.
TEST_CASE("initial business transaction rejects equal-valued foreign owners and moved values",
          "[private-business-transaction][ownership]") {
  const PrivateBusinessTransactionFixture first;
  const PrivateBusinessTransactionFixture foreign;
  CHECK(first.order().order_id() == foreign.order().order_id());
  CHECK(first.owner().runtime_epoch_id() == foreign.owner().runtime_epoch_id());
  CHECK(first.authority.m4_policy == foreign.authority.m4_policy);
  const auto input = first.normalize_input_or_throw(BusinessEvidenceInput::Acknowledgement);
  auto prepared = first.owner().prepare_initial_known_authoritative_business_transaction(
      input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(prepared);
  const auto rejected =
      foreign.owner().validate_prepared_initial_known_business_transaction(prepared.value());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().context.field == "private_business_transaction.owner");
  REQUIRE(first.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
  auto moved = std::move(prepared).value();
  CHECK_FALSE(first.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
  REQUIRE(first.owner().validate_prepared_initial_known_business_transaction(moved));
  CHECK_FALSE(foreign.owner().business_evidence_store().has_outstanding_preparation());
}

// --------------------------------------------------------
// Even acknowledgement and buffered proposals without economics capture the economic generation.
// A genuine later reservation invalidates every earlier capsule while preserving its copied facts.
TEST_CASE(
    "initial business transaction detects genuine new submissions for every proposal category",
    "[private-business-transaction][freshness]") {
  for (const auto kind :
       {BusinessEvidenceInput::Acknowledgement, BusinessEvidenceInput::PartialFill,
        BusinessEvidenceInput::FullFill, BusinessEvidenceInput::ExchangeRejection,
        BusinessEvidenceInput::CancellationAtZero, BusinessEvidenceInput::BufferedGap,
        BusinessEvidenceInput::ContradictorySide}) {
    CAPTURE(kind);
    PrivateBusinessTransactionFixture fixture;
    const auto input = fixture.normalize_input_or_throw(kind);
    const auto callback = kind == BusinessEvidenceInput::BufferedGap
                              ? std::optional<model::CallbackOrdinal>{}
                              : std::optional{create_transaction_callback_or_throw()};
    const auto prepared = fixture.owner().prepare_initial_known_authoritative_business_transaction(
        input, create_transaction_replay_or_throw(), callback);
    REQUIRE(prepared);
    REQUIRE(fixture.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
    const auto immutable_journal = *prepared.value().journal_record();
    const auto immutable_effect =
        *prepared.value().business_evidence()->primary_audit_record_at(0U)->effect_at(0U);
    const auto second = test_support::submit_m4_order_or_throw(
        fixture.authority, test_support::create_m4_reference_order_request_or_throw());
    REQUIRE(second.disposition() == execution::SubmitDisposition::WriteInitiated);
    REQUIRE(second.order_id());
    CHECK(*second.order_id() != fixture.order().order_id());
    CHECK(fixture.authority.submission->reservations().held_reservation_count() == 2U);
    const auto stale =
        fixture.owner().validate_prepared_initial_known_business_transaction(prepared.value());
    REQUIRE_FALSE(stale);
    CHECK(*prepared.value().journal_record() == immutable_journal);
    CHECK(*prepared.value().business_evidence()->primary_audit_record_at(0U)->effect_at(0U) ==
          immutable_effect);
    CHECK(fixture.order().state() == oms::OutboundOrderState::WriteInitiated);
    CHECK(fixture.owner().business_evidence_store().accepted_private_audit_record_count() == 0U);
  }
}

// --------------------------------------------------------
// Reconciliation identity is qualified by the installed runtime. A normalized foreign epoch,
// unknown source or local observation cannot enter this initial known authoritative transaction.
TEST_CASE("initial business transaction rejects unsupported or foreign source shapes before leases "
          "escape",
          "[private-business-transaction][fail-closed]") {
  const PrivateBusinessTransactionFixture fixture;
  const auto& provenance = fixture.order().provenance();
  const auto foreign_reconciliation = fixture.factory.normalize_reconciliation_acknowledgement(
      oms::ReconciliationPrivateEventOrigin{
          test_support::create_m4_reconciliation_epoch_or_throw(),
          test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
          test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(1U),
          model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
      provenance.logical_account_id, provenance.venue_id,
      fixture.create_exchange_order_id_or_throw(), fixture.order().order_id(),
      provenance.instrument_id);
  REQUIRE(foreign_reconciliation);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_transaction(
      foreign_reconciliation.value(), create_transaction_replay_or_throw(),
      create_transaction_callback_or_throw()));
  const auto local = fixture.factory.normalize_order_timeout(
      oms::LocalPrivateEventOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                   model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
      fixture.authority.submission->outbound_oms(), fixture.order());
  REQUIRE(local);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_transaction(
      local.value(), create_transaction_replay_or_throw(), create_transaction_callback_or_throw()));
  const auto unknown = fixture.factory.normalize_venue_acknowledgement(
      oms::VenuePrivateEventOrigin{
          oms::VenuePrivateEventKey{
              provenance.venue_id, provenance.logical_account_id,
              test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
              test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(1U)},
          model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}},
      fixture.create_exchange_order_id_or_throw(), std::nullopt);
  REQUIRE(unknown);
  CHECK_FALSE(fixture.owner().prepare_initial_known_authoritative_business_transaction(
      unknown.value(), create_transaction_replay_or_throw(),
      create_transaction_callback_or_throw()));
  CHECK_FALSE(fixture.owner().business_evidence_store().has_outstanding_preparation());
  const auto valid = fixture.owner().prepare_initial_known_authoritative_business_transaction(
      fixture.normalize_input_or_throw(BusinessEvidenceInput::PartialFill, true),
      create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(valid);
  CHECK(valid.value().journal_record()->sequence().value() == 2U);
}

// --------------------------------------------------------
// Owner and caller source destruction cannot invalidate any copied component. This includes the
// retained execution-batch scratch and complete medium record when its external handle also dies.
TEST_CASE(
    "initial business transaction copied inspection survives owner source and medium destruction",
    "[private-business-transaction][lifetime]") {
  auto fixture = std::make_unique<PrivateBusinessTransactionFixture>(execution::OrderSide::Sell);
  auto input = fixture->normalize_input_or_throw(BusinessEvidenceInput::PartialFill, true);
  auto result = fixture->owner().prepare_initial_known_authoritative_business_transaction(
      input, create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
  REQUIRE(result);
  auto prepared = std::move(result).value();
  const auto identity_before = *prepared.identity_plan();
  const auto journal_before = *prepared.journal_record();
  const auto event_before = *prepared.business_evidence()->private_event_evidence();
  const auto effect_before =
      *prepared.business_evidence()->primary_audit_record_at(0U)->effect_at(0U);
  const auto callback_before =
      *prepared.business_evidence()->planned_callback_record()->callback_at(0U);
  REQUIRE(prepared.business_proposal()->economics_plan() != nullptr);
  const auto execution_before =
      *prepared.business_proposal()->economics_plan()->execution_effect_at(0U);
  input = fixture->normalize_input_or_throw(BusinessEvidenceInput::Acknowledgement);
  CHECK(prepared.business_proposal()->input() != input);
  fixture->medium.reset();
  fixture.reset();
  REQUIRE(prepared.has_preparation());
  CHECK(*prepared.identity_plan() == identity_before);
  CHECK(*prepared.journal_record() == journal_before);
  CHECK(*prepared.business_evidence()->private_event_evidence() == event_before);
  CHECK(*prepared.business_evidence()->primary_audit_record_at(0U)->effect_at(0U) == effect_before);
  CHECK(*prepared.business_evidence()->planned_callback_record()->callback_at(0U) ==
        callback_before);
  const auto* retained_execution =
      prepared.business_proposal()->economics_plan()->execution_effect_at(0U);
  REQUIRE(retained_execution != nullptr);
  CHECK(retained_execution->execution == execution_before.execution);
  CHECK(retained_execution->audit_ordinal == execution_before.audit_ordinal);
  CHECK(retained_execution->reservation_before == execution_before.reservation_before);
  CHECK(retained_execution->reservation_after == execution_before.reservation_after);
  CHECK(retained_execution->signed_quantity_delta == execution_before.signed_quantity_delta);
  CHECK(retained_execution->signed_notional_delta == execution_before.signed_notional_delta);
  for (std::size_t index = 0U; index < execution_before.scopes.size(); ++index) {
    CHECK(retained_execution->scopes[index].exposure == execution_before.scopes[index].exposure);
  }
  REQUIRE(effect_before.inventory_effects);
  for (const auto& effect : *effect_before.inventory_effects) {
    CHECK(effect.signed_quantity_delta ==
          test_support::create_m4_decimal_or_throw<model::Quantity>(-1));
    CHECK(effect.signed_notional_delta ==
          test_support::create_m4_decimal_or_throw<model::Notional>(-10));
  }
}

// --------------------------------------------------------
// A detached journal reservation extends the live incarnation fence after its runtime dies. Cold
// inspection and restart become available only after abandonment, and no candidate becomes history.
TEST_CASE(
    "initial business transaction holds cold reopening fence until complete capsule abandonment",
    "[private-business-transaction][recovery-lease]") {
  PrivateBusinessTransactionFixture fixture;
  const auto policy = fixture.authority.m4_policy;
  model::OrderNamespace::Bytes bytes{};
  bytes.fill(0x90U);
  const model::OrderNamespace next_namespace{bytes};
  {
    const auto prepared = fixture.owner().prepare_initial_known_authoritative_business_transaction(
        fixture.normalize_input_or_throw(BusinessEvidenceInput::PartialFill),
        create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
    REQUIRE(prepared);
    fixture.authority.submission.reset();
    const auto cold_read = fixture.medium->published_journal_record_count();
    REQUIRE_FALSE(cold_read);
    CHECK(cold_read.error().context.field == "journal.read_lease");
    CHECK_FALSE(fixture.medium->acknowledged_journal_record_count());
    CHECK_FALSE(fixture.medium->registered_namespace_count());
    CHECK_FALSE(fixture.medium->discard_unacknowledged_journal_suffix());
    const auto reopen = fixture.medium->bootstrap_recovery_from_namespace(policy, next_namespace);
    REQUIRE_FALSE(reopen);
    CHECK(prepared.value().journal_record()->sequence().value() == 2U);
  }
  check_transaction_cold_namespace_prefix(*fixture.medium);
  {
    const auto reopened = fixture.medium->bootstrap_recovery_from_namespace(policy, next_namespace);
    REQUIRE(reopened);
    CHECK(reopened.value().registered_order_namespace() == next_namespace);
  }
  const auto published = fixture.medium->published_journal_record_count();
  const auto acknowledged = fixture.medium->acknowledged_journal_record_count();
  REQUIRE(published);
  REQUIRE(acknowledged);
  CHECK(published.value() == 2U);
  CHECK(acknowledged.value() == 2U);
  const auto namespace_record = fixture.medium->published_journal_record_at(1U);
  REQUIRE(namespace_record);
  CHECK(namespace_record.value().kind() == recovery::JournalRecordKind::NamespaceRegistered);
  CHECK(namespace_record.value().sequence().value() == 2U);
  CHECK_FALSE(namespace_record.value().audit_span());
}

// --------------------------------------------------------
// Real executor fences can change containment while a detached preparation exists. Freshness
// validation must reject both the account transition and a reasonless global block before use.
TEST_CASE("initial business transaction becomes stale after authentic account and global fences",
          "[private-business-transaction][containment]") {
  for (const bool global : {false, true}) {
    CAPTURE(global);
    auto capacities = test_support::create_ordinary_m4_policy_capacities();
    capacities.max_private_admissions = 1U;
    PrivateBusinessTransactionFixture fixture{execution::OrderSide::Buy, capacities};
    const auto prepared = fixture.owner().prepare_initial_known_authoritative_business_transaction(
        fixture.normalize_input_or_throw(BusinessEvidenceInput::Acknowledgement),
        create_transaction_replay_or_throw(), create_transaction_callback_or_throw());
    REQUIRE(prepared);
    const auto copied_journal = *prepared.value().journal_record();
    model::DeterministicClockProvider clock{100U};
    runtime::SerializedExecutor executor{
        0U, clock,
        test_support::extract_evidence_value_or_throw(
            runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                fixture.authority.configuration, fixture.authority.m4_policy)),
        *fixture.authority.submission->private_admission_owner()};
    if (global) {
      const auto attempted = fixture.create_acknowledgement_attempt_or_throw(31U, true);
      const auto rejected = executor.try_admit_private(attempted);
      REQUIRE(rejected);
      REQUIRE(rejected.value().outcome != runtime::AdmissionOutcome::Accepted);
    } else {
      const auto first_attempt = fixture.create_acknowledgement_attempt_or_throw(31U);
      const auto first = executor.try_admit_private(first_attempt);
      REQUIRE(first);
      REQUIRE(first.value().outcome == runtime::AdmissionOutcome::Accepted);
      const auto rejected_attempt = fixture.create_acknowledgement_attempt_or_throw(32U);
      const auto rejected = executor.try_admit_private(rejected_attempt);
      REQUIRE(rejected);
      REQUIRE(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
    }
    REQUIRE(executor.bind_to_current_thread());
    REQUIRE(executor.execute_pending_turns(128U));
    REQUIRE(executor.release_from_current_thread());
    if (global) {
      REQUIRE(fixture.owner().is_private_consumption_globally_blocked());
      CHECK(fixture.owner().account_safety_state(fixture.order().provenance().logical_account_id) ==
            risk::AccountSafetyState::Synchronized);
    } else {
      REQUIRE(
          fixture.owner().account_safety_state(fixture.order().provenance().logical_account_id) ==
          risk::AccountSafetyState::Quarantined);
    }
    CHECK_FALSE(
        fixture.owner().validate_prepared_initial_known_business_transaction(prepared.value()));
    CHECK(*prepared.value().journal_record() == copied_journal);
    CHECK(fixture.owner().business_evidence_store().accepted_private_event_record_count() == 0U);
    CHECK(fixture.owner().business_evidence_store().accepted_private_audit_record_count() == 0U);
  }
}

// --------------------------------------------------------

} // namespace
