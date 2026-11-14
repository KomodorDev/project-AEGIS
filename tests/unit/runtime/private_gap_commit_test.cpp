// Purpose: qualify genuine owner commits for callback-free execution gaps and exact event repeats,
// including fixed capacity, immutable disposition publication, and loss of the volatile journal.

#include "aegis/oms/private_oms_transition.hpp"
#include "aegis/runtime/private_business_evidence_store.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "private_business_transaction_fixture.hpp"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;
using test_support::create_m4_decimal_or_throw;
using test_support::create_m4_opaque_identity_or_throw;
using test_support::create_m4_order_id_or_throw;
using test_support::create_m4_ordinal_or_throw;
using test_support::extract_evidence_value_or_throw;

// ########################################################################
// Distinguish the public venue lane from the separately authorized reconciliation lane in tests.
enum class GapAdmissionLane { Venue, Reconciliation };

// ########################################################################
// Separate changes to event semantics from a different event conflicting with the same trade.
enum class GapIdentityConflict { ChangedPrice, ChangedKind, ChangedTrade };

// ########################################################################
// Select the single preflight resource exhausted while every other component still has room.
enum class GapCapacityResource {
  Journal,
  EventEvidence,
  Audit,
  EventIdentity,
  TradeIdentity,
  PendingInterval
};

// ########################################################################
// Describe one authored source execution independently of the owner's correlation or disposition.
struct GapSourceExecution {
  std::uint8_t event_seed;
  std::uint8_t trade_seed;
  std::int64_t cumulative;
  std::int64_t increment{1};
  bool include_local_locator{true};
  std::int64_t price{100};
  std::optional<recovery::RuntimeEpochId> reconciliation_runtime_epoch{};
};

// ########################################################################
// The executor borrows a stable installed runtime and deterministic clock. Test observations are
// quiescent except for the explicitly concurrent, immutable admission-oracle checks.
class PrivateGapCommitFixture final {
public:

  // --------------------------------------------------------
  // Use the ordinary production composition, with independently authored preinstallation limits.
  explicit PrivateGapCommitFixture(
      execution::OrderSide side = execution::OrderSide::Buy,
      runtime::M4PolicyCapacities capacities = test_support::create_ordinary_m4_policy_capacities())
      : transaction{side, capacities},
        executor{std::make_unique<runtime::SerializedExecutor>(
            0U, clock,
            extract_evidence_value_or_throw(
                runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                    transaction.authority.configuration, transaction.authority.m4_policy)),
            *transaction.authority.submission->private_admission_owner())} {}

  // --------------------------------------------------------
  // Borrow the live private owner before this fixture ends the runtime incarnation.
  [[nodiscard]] const runtime::PrivateOrderReconciler& owner() const { return transaction.owner(); }

  // --------------------------------------------------------
  // Create an additional genuine order with enough contracts to exercise disjoint gaps.
  [[nodiscard]] const oms::OutboundOrderRecord&
  submit_additional_order_or_throw(std::int64_t quantity) {
    auto request = test_support::create_m4_reference_order_request_or_throw();
    request.quantity = create_m4_decimal_or_throw<model::Quantity>(quantity);
    const auto submitted = test_support::submit_m4_order_or_throw(transaction.authority, request);
    if (submitted.disposition() != execution::SubmitDisposition::WriteInitiated ||
        !submitted.order_id()) {
      throw std::logic_error{"expected a genuine order for private gap qualification"};
    }
    const auto* const order =
        transaction.authority.submission->outbound_oms().find_order(*submitted.order_id());
    if (order == nullptr) {
      throw std::logic_error{"missing submitted order for private gap qualification"};
    }
    return *order;
  }

  // --------------------------------------------------------
  // Build a receive-time-free source fact in the requested genuine lane. The executor supplies the
  // receive stamp after copying; callers can vary the raw locator without changing the order.
  [[nodiscard]] runtime::CriticalPrivateEventAttempt
  create_execution_attempt_or_throw(GapAdmissionLane lane, const oms::OutboundOrderRecord& order,
                                    oms::ExchangeOrderId exchange_order,
                                    const GapSourceExecution& source) const {
    auto locator =
        extract_evidence_value_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
            source.include_local_locator ? std::optional{order.order_id()} : std::nullopt,
            exchange_order));
    return create_execution_attempt_or_throw(lane, order, std::move(locator), source);
  }

  // --------------------------------------------------------
  // Keep the provenance and source scope fixed while allowing an independently authored locator.
  [[nodiscard]] runtime::CriticalPrivateEventAttempt
  create_execution_attempt_or_throw(GapAdmissionLane lane, const oms::OutboundOrderRecord& order,
                                    oms::PrivateOrderLocator locator,
                                    const GapSourceExecution& source) const {
    const auto& provenance = order.provenance();
    const auto trade_id = create_m4_opaque_identity_or_throw<oms::TradeId>(source.trade_seed);
    const auto increment = create_m4_decimal_or_throw<model::Quantity>(source.increment);
    const auto cumulative = create_m4_decimal_or_throw<model::Quantity>(source.cumulative);
    const auto price = create_m4_decimal_or_throw<model::Price>(source.price);
    if (lane == GapAdmissionLane::Venue) {
      return extract_evidence_value_or_throw(transaction.factory.create_venue_execution_attempt(
          oms::VenuePrivateIngressOrigin{
              oms::VenuePrivateEventKey{
                  provenance.venue_id, provenance.logical_account_id,
                  create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
                  create_m4_opaque_identity_or_throw<oms::PrivateEventId>(source.event_seed)},
              model::SourceTimestamp{10U}},
          std::move(locator), trade_id, provenance.instrument_id, provenance.metadata_revision,
          increment, cumulative, price, std::nullopt));
    }
    return extract_evidence_value_or_throw(
        transaction.factory.create_reconciliation_execution_attempt(
            oms::ReconciliationPrivateIngressOrigin{
                extract_evidence_value_or_throw(
                    recovery::ReconciliationEpochId::
                        reconciliation_epoch_id_from_runtime_and_counter(
                            source.reconciliation_runtime_epoch.value_or(
                                owner().runtime_epoch_id()),
                            1U)),
                create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
                create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(source.event_seed),
                model::SourceTimestamp{10U}},
            provenance.logical_account_id, provenance.venue_id, std::move(locator), trade_id,
            provenance.instrument_id, provenance.metadata_revision, increment, cumulative, price,
            order.economics().side));
  }

  // --------------------------------------------------------
  // Build a valid acknowledgement with the same source key as an execution in either lane.
  [[nodiscard]] runtime::CriticalPrivateEventAttempt create_acknowledgement_attempt_or_throw(
      GapAdmissionLane lane, const oms::OutboundOrderRecord& order,
      oms::ExchangeOrderId exchange_order, std::uint8_t event) const {
    const auto& provenance = order.provenance();
    if (lane == GapAdmissionLane::Venue) {
      return extract_evidence_value_or_throw(
          transaction.factory.create_venue_acknowledgement_attempt(
              oms::VenuePrivateIngressOrigin{
                  oms::VenuePrivateEventKey{
                      provenance.venue_id, provenance.logical_account_id,
                      create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
                      create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
                  model::SourceTimestamp{10U}},
              exchange_order, order.order_id()));
    }
    return extract_evidence_value_or_throw(
        transaction.factory.create_reconciliation_acknowledgement_attempt(
            oms::ReconciliationPrivateIngressOrigin{
                extract_evidence_value_or_throw(
                    recovery::ReconciliationEpochId::
                        reconciliation_epoch_id_from_runtime_and_counter(owner().runtime_epoch_id(),
                                                                         1U)),
                create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
                create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(event),
                model::SourceTimestamp{10U}},
            provenance.logical_account_id, provenance.venue_id, exchange_order, order.order_id(),
            provenance.instrument_id));
  }

  // --------------------------------------------------------
  // Copy the real attempt into its executor reserve and retain the genuine receipt or throw.
  [[nodiscard]] runtime::AdmissionReceipt
  admit_or_throw(const runtime::CriticalPrivateEventAttempt& attempt) {
    return std::visit(
        [this](const auto& source) {
          const auto decision = extract_evidence_value_or_throw(admit_gap_source(source));
          if (decision.outcome != runtime::AdmissionOutcome::Accepted || !decision.receipt) {
            throw std::logic_error{"private gap fixture expected accepted admission"};
          }
          return *decision.receipt;
        },
        attempt);
  }

  // --------------------------------------------------------
  // Drain queued turns while genuinely bound to the executor, releasing it before inspection.
  void execute_admitted_turns_or_throw() {
    if (!executor->bind_to_current_thread()) {
      throw std::logic_error{"could not bind private gap fixture executor"};
    }
    const auto completed = executor->execute_pending_turns(128U);
    const auto released = executor->release_from_current_thread();
    if (!completed || !released) {
      throw std::logic_error{"could not execute private gap fixture turns"};
    }
  }

  // --------------------------------------------------------
  // Close the exact borrowing executor before destroying its owner; the external medium survives.
  void end_incarnation() noexcept {
    executor.reset();
    transaction.authority.submission.reset();
  }

  // --------------------------------------------------------
  test_support::PrivateBusinessTransactionFixture transaction;
  model::DeterministicClockProvider clock{100U};
  std::unique_ptr<runtime::SerializedExecutor> executor;

private:

  // --------------------------------------------------------
  // Preserve each nominal source alternative through its corresponding admission API.
  [[nodiscard]] model::Result<runtime::PrivateAdmissionDecision>
  admit_gap_source(const oms::PrivateOrderIngressAttempt& source) {
    return executor->try_admit_private(source);
  }

  // --------------------------------------------------------
  // Copy an authoritative row through its separately reserved reconciliation lane.
  [[nodiscard]] model::Result<runtime::ReconciliationAdmissionDecision>
  admit_gap_source(const oms::ReconciliationPrivateEventIngressAttempt& source) {
    return executor->try_admit_reconciliation_event(source);
  }

  // --------------------------------------------------------
};

// ########################################################################
// Snapshot the exact economic state an execution-gap turn is forbidden to change.
struct GapEconomicBaseline {
  risk::ReservationEvidence reservation;
  std::vector<risk::RiskScopeExposureEvidence> scopes;
  std::uint32_t held_count;
  std::uint64_t inventory_generation;
};

// ########################################################################

// --------------------------------------------------------
// Read the genuine held reservation, all risk scopes, and the installed inventory generation.
[[nodiscard]] GapEconomicBaseline capture_gap_economics(const PrivateGapCommitFixture& fixture,
                                                        const oms::OutboundOrderRecord& order) {
  const auto& reservations = fixture.transaction.authority.submission->reservations();
  const auto* const reservation = reservations.find_reservation(order.reservation_id());
  const auto* const inventory = risk::InventoryLedger::installed_inventory(reservations);
  REQUIRE(reservation != nullptr);
  REQUIRE(inventory != nullptr);
  CHECK(inventory->source_row_count() == 0U);
  return GapEconomicBaseline{
      *reservation, test_support::collect_evidence_scope_rows_or_throw(reservations),
      reservations.held_reservation_count(), inventory->planning_generation()};
}

// --------------------------------------------------------
// Compare all risk scopes as well as the named order's reservation; quantity alone is insufficient.
void check_gap_economics_unchanged(const PrivateGapCommitFixture& fixture,
                                   const oms::OutboundOrderRecord& order,
                                   const GapEconomicBaseline& before) {
  const auto after = capture_gap_economics(fixture, order);
  CHECK(after.reservation == before.reservation);
  CHECK(after.scopes == before.scopes);
  CHECK(after.held_count == before.held_count);
  CHECK(after.inventory_generation == before.inventory_generation);
}

// --------------------------------------------------------
// Read only the owner disposition oracle matching the accepted lane.
[[nodiscard]] std::optional<oms::PrivateEventDisposition>
find_gap_disposition(const runtime::PrivateOrderReconciler& owner, GapAdmissionLane lane,
                     model::AdmissionOrdinal attempt) noexcept {
  return lane == GapAdmissionLane::Venue
             ? owner.find_committed_private_event_disposition(attempt)
             : owner.find_committed_reconciliation_event_disposition(attempt);
}

// --------------------------------------------------------
// Check public observation separately: a completed buffer owns no active slot, while a duplicate
// is terminal and may report EconomicallyConsumed without applying any inventory or reservation.
void check_gap_completion(const PrivateGapCommitFixture& fixture, GapAdmissionLane lane,
                          const runtime::AdmissionReceipt& receipt,
                          oms::PrivateEventDisposition disposition) {
  const auto ordinal = receipt.attempt_ordinal;
  CHECK(find_gap_disposition(fixture.owner(), lane, ordinal) == disposition);
  CHECK_FALSE(find_gap_disposition(
      fixture.owner(),
      lane == GapAdmissionLane::Venue ? GapAdmissionLane::Reconciliation : GapAdmissionLane::Venue,
      ordinal));
  CHECK(fixture.owner().find_committed_retained_private_event_error(ordinal) == nullptr);
  CHECK(fixture.owner().find_committed_retained_reconciliation_event_error(ordinal) == nullptr);
  const auto observation = lane == GapAdmissionLane::Venue
                               ? fixture.executor->private_admission_observation(ordinal)
                               : fixture.executor->reconciliation_admission_observation(ordinal);
  if (disposition == oms::PrivateEventDisposition::BufferedGap) {
    CHECK_FALSE(observation);
  } else {
    REQUIRE(observation);
    CHECK(observation->state == runtime::CriticalPrivateAdmissionState::EconomicallyConsumed);
    CHECK(observation->disposition == disposition);
    CHECK_FALSE(observation->retention_error);
  }
}

// --------------------------------------------------------
// Verify exactly one primary disposition row covers the genuine receipt, full source, and bounded
// owner effects. A suppressed callback must allocate neither an ordinal range nor another row.
void check_gap_evidence_row(
    const PrivateGapCommitFixture& fixture, std::uint32_t index,
    const runtime::CriticalPrivateEventAttempt& attempt, const runtime::AdmissionReceipt& receipt,
    const oms::OutboundOrderRecord& order, oms::PrivateEventDisposition disposition,
    trace::CallbackDecision callback_decision, const oms::PrivateOrderProjection& before,
    const oms::PrivateOrderProjection& after, const risk::ReservationEvidence& held) {
  const auto& store = fixture.owner().business_evidence_store();
  const auto* const event = store.accepted_private_event_record_at(index);
  const auto* const audit = store.accepted_private_audit_record_at(index);
  REQUIRE(event != nullptr);
  REQUIRE(audit != nullptr);
  const auto expected_ingress =
      std::visit([](const auto& source) { return source.semantic_value(); }, attempt);
  CHECK(event->ingress_semantic_value() == expected_ingress);
  CHECK(event->admission_ordinal() == receipt.attempt_ordinal);
  CHECK(event->receive_sequence() == receipt.receive_sequence);
  CHECK(event->receive_time() == receipt.received_at);
  CHECK(event->runtime_epoch_id() == fixture.owner().runtime_epoch_id());
  CHECK(event->root_provenance() == store.root_provenance());
  CHECK(event->disposition() == disposition);
  REQUIRE(event->journal_sequence());
  CHECK(event->journal_sequence()->value() == static_cast<std::uint64_t>(index) + 2U);
  REQUIRE(event->first_admission_resolution().known_resolution() != nullptr);
  const auto& known = *event->first_admission_resolution().known_resolution();
  CHECK(known.order_id == order.order_id());
  CHECK(audit->kind() == trace::M4AuditKind::EventDisposition);
  CHECK(audit->audit_ordinal().value() == static_cast<std::uint64_t>(index) + 1U);
  CHECK(audit->runtime_epoch_id() == event->runtime_epoch_id());
  CHECK(audit->root_provenance() == event->root_provenance());
  CHECK(audit->subject_provenance() == known.provenance.subject());
  CHECK(audit->journal_sequence() == event->journal_sequence());
  CHECK(audit->disposition() == disposition);
  CHECK(audit->originating_event() == event->originating_event());
  REQUIRE(audit->source_input());
  CHECK(*audit->source_input() == event->input());
  CHECK(audit->callback_decision() == callback_decision);
  CHECK(audit->callback_count() == 0U);
  CHECK_FALSE(audit->callback_range());
  CHECK_FALSE(audit->error());
  REQUIRE(audit->effect_count() == 1U);
  const auto* const effect = audit->effect_at(0U);
  REQUIRE(effect != nullptr);
  CHECK(effect->order_id == order.order_id());
  CHECK(effect->provenance == known.provenance);
  CHECK(effect->oms_before == before);
  CHECK(effect->oms_after == after);
  CHECK(effect->reservation_before == held);
  CHECK(effect->reservation_after == held);
  CHECK_FALSE(effect->inventory_effects);
  CHECK_FALSE(effect->account_safety_transition);
}

// --------------------------------------------------------
// Pin a real append-only journal row after closing its live lease, with the exact event and span.
void check_cold_gap_journal_row(const recovery::DeterministicFakeRecoveryMedium& medium,
                                std::uint32_t index, const trace::PrivateEventEvidence& event) {
  const auto record = medium.published_journal_record_at(index);
  REQUIRE(record);
  const auto& row = record.value();
  CHECK(row.kind() == recovery::JournalRecordKind::PrivateEventInput);
  CHECK(row.sequence() == event.journal_sequence());
  REQUIRE(row.predecessor());
  CHECK(row.predecessor()->value() == index);
  CHECK(row.runtime_epoch_id() == event.runtime_epoch_id());
  CHECK(row.root_provenance() == event.root_provenance());
  REQUIRE(event.first_admission_resolution().known_resolution() != nullptr);
  CHECK(row.subject_provenance() ==
        event.first_admission_resolution().known_resolution()->provenance.subject());
  CHECK(row.replay_provenance() == event.replay_provenance());
  REQUIRE(row.audit_span());
  CHECK(row.audit_span()->first_audit_ordinal().value() == index);
  CHECK(row.audit_span()->audit_record_count() == 1U);
  const auto* const input = std::get_if<recovery::PrivateEventInputJournalPayload>(&row.payload());
  REQUIRE(input != nullptr);
  CHECK(input->input() == event.input());
  CHECK(input->first_admission_resolution() == event.first_admission_resolution());
}

// --------------------------------------------------------
// An authoritative interval [1, 2) leaves cumulative quantity zero. An exact repeat with a new
// local receipt emits its own journal and audit rows but changes no ownership or pending interval.
TEST_CASE("private gap commits exact evidence and suppresses repeat economics in both lanes",
          "[private-gap-commit][runtime][m4]") {
  for (const auto lane : {GapAdmissionLane::Venue, GapAdmissionLane::Reconciliation}) {
    for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
      CAPTURE(static_cast<int>(lane), static_cast<int>(side));
      auto capacities = test_support::create_ordinary_m4_policy_capacities();
      capacities.max_event_identity_records = 1U;
      capacities.max_trade_identity_records = 1U;
      capacities.max_pending_fill_intervals_per_order = 1U;
      PrivateGapCommitFixture fixture{side, capacities};
      const auto& order = fixture.transaction.order();
      const auto before = order.private_projection();
      const auto held = capture_gap_economics(fixture, order);
      const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
      const auto attempt = fixture.create_execution_attempt_or_throw(
          lane, order, exchange, GapSourceExecution{11U, 21U, 2});
      const auto first = fixture.admit_or_throw(attempt);
      fixture.execute_admitted_turns_or_throw();

      // ++++++++++++++++++++++++++++++++++++++++
      // Compare the entire projection after the first append and the exact retained identity rows.
      auto expected = before;
      expected.exchange_order_id = exchange;
      expected.execution_evidence_observed = true;
      expected.exchange_mapping_established_by_execution = true;
      expected.pending_fill_count = 1U;
      CHECK(order.private_projection() == expected);
      check_gap_economics_unchanged(fixture, order, held);
      check_gap_completion(fixture, lane, first, oms::PrivateEventDisposition::BufferedGap);
      check_gap_evidence_row(
          fixture, 0U, attempt, first, order, oms::PrivateEventDisposition::BufferedGap,
          trace::CallbackDecision::SuppressedBuffered, before, expected, held.reservation);
      const auto& store = fixture.owner().business_evidence_store();
      const auto first_event = *store.accepted_private_event_record_at(0U);
      const auto event_key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(
          first_event.ingress_semantic_value());
      const auto trade_key =
          oms::TradeKey{order.provenance().venue_id, order.provenance().logical_account_id,
                        create_m4_opaque_identity_or_throw<oms::TradeId>(21U)};
      const auto exchange_key = oms::ExchangeOrderKey{
          order.provenance().venue_id, order.provenance().logical_account_id, exchange};
      const auto* const event = fixture.owner().find_committed_event_identity(event_key);
      const auto* const trade = fixture.owner().find_committed_trade_identity(trade_key);
      const auto* const mapping = fixture.owner().find_committed_exchange_mapping(exchange_key);
      REQUIRE(event != nullptr);
      REQUIRE(trade != nullptr);
      REQUIRE(mapping != nullptr);
      CHECK(event->ingress_semantic_value == first_event.ingress_semantic_value());
      CHECK(event->resolution == first_event.first_admission_resolution());
      CHECK(event->original_disposition == oms::PrivateEventDisposition::BufferedGap);
      CHECK(trade->original_disposition == oms::PrivateEventDisposition::BufferedGap);
      CHECK(trade->current_disposition == oms::PrivateEventDisposition::BufferedGap);
      REQUIRE(first_event.trade_semantic_value());
      CHECK(trade->semantic_value == *first_event.trade_semantic_value());
      CHECK(mapping->order_id == order.order_id());
      const auto pending = fixture.owner().pending_private_executions(order.order_id());
      REQUIRE(pending.size() == 1U);
      CHECK(pending.front().execution == first_event.input());
      CHECK(store.accepted_private_event_record_count() == 1U);
      CHECK(store.accepted_private_audit_record_count() == 1U);
      CHECK(store.next_audit_ordinal().value() == 2U);

      // ++++++++++++++++++++++++++++++++++++++++
      // Force a different receive time while all identity and interval capacities are already full.
      REQUIRE(fixture.clock.advance_nanoseconds(9U));
      const auto repeat = fixture.admit_or_throw(attempt);
      fixture.execute_admitted_turns_or_throw();
      CHECK(repeat.received_at != first.received_at);
      CHECK(repeat.receive_sequence != first.receive_sequence);
      CHECK(order.private_projection() == expected);
      check_gap_economics_unchanged(fixture, order, held);
      check_gap_completion(fixture, lane, repeat,
                           oms::PrivateEventDisposition::ExactEventDuplicate);
      check_gap_completion(fixture, lane, first, oms::PrivateEventDisposition::BufferedGap);
      check_gap_evidence_row(
          fixture, 1U, attempt, repeat, order, oms::PrivateEventDisposition::ExactEventDuplicate,
          trace::CallbackDecision::SuppressedDuplicate, expected, expected, held.reservation);
      const auto repeated_event = *store.accepted_private_event_record_at(1U);
      CHECK(repeated_event.first_admission_resolution() ==
            first_event.first_admission_resolution());
      CHECK(repeated_event.ingress_semantic_value() == first_event.ingress_semantic_value());
      CHECK(repeated_event.input() != first_event.input());
      CHECK(*store.accepted_private_event_record_at(0U) == first_event);
      CHECK(fixture.owner().find_committed_event_identity(event_key) == event);
      CHECK(fixture.owner().find_committed_trade_identity(trade_key) == trade);
      CHECK(fixture.owner().find_committed_exchange_mapping(exchange_key) == mapping);
      CHECK(fixture.owner().pending_private_executions(order.order_id()).front().execution ==
            first_event.input());
      CHECK(fixture.owner().event_identity_record_count() == 1U);
      CHECK(fixture.owner().trade_identity_record_count() == 1U);
      CHECK(fixture.owner().exchange_order_mapping_count() == 1U);
      CHECK(fixture.owner().retained_identity_turn_count() == 0U);
      CHECK(fixture.owner().account_safety_state(order.provenance().logical_account_id) ==
            risk::AccountSafetyState::Synchronized);
      CHECK(store.accepted_private_event_record_count() == 2U);
      CHECK(store.accepted_private_audit_record_count() == 2U);
      CHECK(store.next_audit_ordinal().value() == 3U);
      CHECK(store.accepted_private_event_record_at(2U) == nullptr);
      CHECK(store.accepted_private_audit_record_at(2U) == nullptr);
      CHECK_FALSE(store.has_outstanding_preparation());

      // ++++++++++++++++++++++++++++++++++++++++
      // Publication is observable in cold media, but recovery cannot silently treat it as durable.
      auto& medium = *fixture.transaction.medium;
      CHECK_FALSE(medium.published_journal_record_count());
      CHECK_FALSE(medium.discard_unacknowledged_journal_suffix());
      const auto policy = fixture.transaction.authority.m4_policy;
      fixture.end_incarnation();
      CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 3U);
      CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 1U);
      CHECK(extract_evidence_value_or_throw(medium.registered_namespace_count()) == 1U);
      check_cold_gap_journal_row(medium, 1U, first_event);
      check_cold_gap_journal_row(medium, 2U, repeated_event);
      model::OrderNamespace::Bytes fresh_bytes{};
      fresh_bytes.fill(0x90U);
      const model::OrderNamespace fresh_namespace{fresh_bytes};
      const auto blocked = medium.bootstrap_recovery_from_namespace(policy, fresh_namespace);
      REQUIRE_FALSE(blocked);
      CHECK(blocked.error().context.field == "journal.unacknowledged_suffix");
      REQUIRE(medium.discard_unacknowledged_journal_suffix());
      CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 1U);
      CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 1U);
      CHECK_FALSE(medium.published_journal_record_at(1U));
      {
        const auto reopened = medium.bootstrap_recovery_from_namespace(policy, fresh_namespace);
        REQUIRE(reopened);
        CHECK(reopened.value().registered_order_namespace() == fresh_namespace);
      }
      const auto new_namespace_row = medium.published_journal_record_at(1U);
      REQUIRE(new_namespace_row);
      CHECK(new_namespace_row.value().kind() == recovery::JournalRecordKind::NamespaceRegistered);
      CHECK(new_namespace_row.value().sequence().value() == 2U);
      CHECK_FALSE(new_namespace_row.value().audit_span());
      CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 2U);

      // ++++++++++++++++++++++++++++++++++++++++
    }
  }
}

// --------------------------------------------------------
// Verify a fallback is independently visible in its exact executor lane and does not become an
// accepted disposition. The retained full source belongs to reconciliation instead.
[[nodiscard]] const runtime::RetainedPrivateIdentityTurn&
require_gap_fallback(const PrivateGapCommitFixture& fixture, GapAdmissionLane lane,
                     const runtime::CriticalPrivateEventAttempt& attempted,
                     const runtime::AdmissionReceipt& receipt) {
  const auto ordinal = receipt.attempt_ordinal;
  const bool reconciliation = lane == GapAdmissionLane::Reconciliation;
  CHECK_FALSE(find_gap_disposition(fixture.owner(), lane, ordinal));
  const auto* const retained = fixture.owner().find_retained_identity_turn(ordinal, reconciliation);
  REQUIRE(retained != nullptr);
  CHECK(fixture.owner().find_retained_identity_turn(ordinal, !reconciliation) == nullptr);
  CHECK(retained->receipt == receipt);
  REQUIRE(retained->attempt);
  CHECK(*retained->attempt == attempted);
  const auto observation = reconciliation
                               ? fixture.executor->reconciliation_admission_observation(ordinal)
                               : fixture.executor->private_admission_observation(ordinal);
  REQUIRE(observation);
  CHECK(observation->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(observation->disposition);
  REQUIRE(observation->retention_error);
  return *retained;
}

// --------------------------------------------------------
// Accept a second local order after a prior gap, and accept a later-arriving lower interval using
// only the established exchange key. The unsupported contiguous prefix cannot drain either gap.
TEST_CASE(
    "private gap commit sorts disjoint intervals and leaves a contiguous drain for reconciliation",
    "[private-gap-commit][runtime][m4]") {
  PrivateGapCommitFixture fixture;
  const auto& first_order = fixture.transaction.order();
  const auto first_exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto first_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, first_order, first_exchange, GapSourceExecution{11U, 21U, 2});
  const auto first = fixture.admit_or_throw(first_attempt);
  fixture.execute_admitted_turns_or_throw();
  check_gap_completion(fixture, GapAdmissionLane::Venue, first,
                       oms::PrivateEventDisposition::BufferedGap);
  CHECK(fixture.owner().account_safety_state(first_order.provenance().logical_account_id) ==
        risk::AccountSafetyState::Synchronized);

  const auto& second_order = fixture.submit_additional_order_or_throw(5);
  const auto second_before = second_order.private_projection();
  const auto held = capture_gap_economics(fixture, second_order);
  const auto second_exchange = create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x52U);
  const auto high_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, second_order, second_exchange, GapSourceExecution{12U, 22U, 3});
  const auto low_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, second_order, second_exchange,
      GapSourceExecution{13U, 23U, 2, 1, false});
  const auto high = fixture.admit_or_throw(high_attempt);
  const auto low = fixture.admit_or_throw(low_attempt);
  fixture.execute_admitted_turns_or_throw();
  check_gap_completion(fixture, GapAdmissionLane::Venue, high,
                       oms::PrivateEventDisposition::BufferedGap);
  check_gap_completion(fixture, GapAdmissionLane::Reconciliation, low,
                       oms::PrivateEventDisposition::BufferedGap);
  auto after_high = second_before;
  after_high.exchange_order_id = second_exchange;
  after_high.execution_evidence_observed = true;
  after_high.exchange_mapping_established_by_execution = true;
  after_high.pending_fill_count = 1U;
  auto after_low = after_high;
  after_low.pending_fill_count = 2U;
  CHECK(second_order.private_projection() == after_low);
  check_gap_evidence_row(
      fixture, 1U, high_attempt, high, second_order, oms::PrivateEventDisposition::BufferedGap,
      trace::CallbackDecision::SuppressedBuffered, second_before, after_high, held.reservation);
  check_gap_evidence_row(
      fixture, 2U, low_attempt, low, second_order, oms::PrivateEventDisposition::BufferedGap,
      trace::CallbackDecision::SuppressedBuffered, after_high, after_low, held.reservation);
  const auto& store = fixture.owner().business_evidence_store();
  const auto low_input = store.accepted_private_event_record_at(2U)->input();
  const auto high_input = store.accepted_private_event_record_at(1U)->input();
  const auto pending = fixture.owner().pending_private_executions(second_order.order_id());
  REQUIRE(pending.size() == 2U);
  CHECK(pending[0U].execution == low_input);
  CHECK(pending[1U].execution == high_input);
  CHECK(fixture.owner().pending_private_executions(first_order.order_id()).size() == 1U);
  CHECK(fixture.owner().event_identity_record_count() == 3U);
  CHECK(fixture.owner().trade_identity_record_count() == 3U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 2U);

  const auto prefix_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, second_order, second_exchange,
      GapSourceExecution{14U, 24U, 1, 1, false});
  const auto prefix = fixture.admit_or_throw(prefix_attempt);
  fixture.execute_admitted_turns_or_throw();
  static_cast<void>(require_gap_fallback(fixture, GapAdmissionLane::Venue, prefix_attempt, prefix));
  CHECK(second_order.private_projection() == after_low);
  const auto still_pending = fixture.owner().pending_private_executions(second_order.order_id());
  REQUIRE(still_pending.size() == 2U);
  CHECK(still_pending[0U].execution == low_input);
  CHECK(still_pending[1U].execution == high_input);
  CHECK(fixture.owner().event_identity_record_count() == 3U);
  CHECK(fixture.owner().trade_identity_record_count() == 3U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 2U);
  CHECK(store.accepted_private_event_record_count() == 3U);
  CHECK(store.accepted_private_audit_record_count() == 3U);
  CHECK(store.next_audit_ordinal().value() == 4U);
  CHECK_FALSE(store.has_outstanding_preparation());
  check_gap_economics_unchanged(fixture, second_order, held);
  auto& medium = *fixture.transaction.medium;
  fixture.end_incarnation();
  CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 4U);
  CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 1U);
  CHECK_FALSE(medium.published_journal_record_at(4U));
}

// --------------------------------------------------------
// Reuse of an accepted event key with a changed execution price or a different input kind must
// quarantine. A different event key can still conflict with the same accepted trade.
TEST_CASE("conflicting private event or trade identity leaves the accepted gap intact",
          "[private-gap-commit][runtime][m4][containment]") {
  for (const auto lane : {GapAdmissionLane::Venue, GapAdmissionLane::Reconciliation}) {
    for (const auto profile : {GapIdentityConflict::ChangedPrice, GapIdentityConflict::ChangedKind,
                               GapIdentityConflict::ChangedTrade}) {
      CAPTURE(static_cast<int>(lane), static_cast<int>(profile));
      PrivateGapCommitFixture fixture;
      const auto& order = fixture.transaction.order();
      const auto held = capture_gap_economics(fixture, order);
      const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
      const auto original = fixture.create_execution_attempt_or_throw(
          lane, order, exchange, GapSourceExecution{11U, 21U, 2});
      const auto first = fixture.admit_or_throw(original);
      fixture.execute_admitted_turns_or_throw();
      check_gap_completion(fixture, lane, first, oms::PrivateEventDisposition::BufferedGap);
      const auto projection = order.private_projection();
      const auto& store = fixture.owner().business_evidence_store();
      const auto event = *store.accepted_private_event_record_at(0U);
      const auto key =
          oms::PrivateEventRegistryKey::from_ingress_semantic_value(event.ingress_semantic_value());
      const auto* const identity = fixture.owner().find_committed_event_identity(key);
      REQUIRE(identity != nullptr);
      const auto retained_identity = *identity;

      const bool trade_conflict = profile == GapIdentityConflict::ChangedTrade;
      const auto changed =
          profile == GapIdentityConflict::ChangedKind
              ? fixture.create_acknowledgement_attempt_or_throw(lane, order, exchange, 11U)
              : fixture.create_execution_attempt_or_throw(
                    lane, order, exchange,
                    GapSourceExecution{static_cast<std::uint8_t>(trade_conflict ? 12U : 11U), 21U,
                                       2, 1, true, 99});
      const auto conflict = fixture.admit_or_throw(changed);
      fixture.execute_admitted_turns_or_throw();
      const auto& retained = require_gap_fallback(fixture, lane, changed, conflict);
      CHECK(retained.safety_reason == (trade_conflict
                                           ? risk::AccountSafetyReason::TradeIdentityConflict
                                           : risk::AccountSafetyReason::EventIdentityConflict));
      CHECK(fixture.owner().account_safety_state(order.provenance().logical_account_id) ==
            risk::AccountSafetyState::Quarantined);
      CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());
      CHECK(fixture.owner().find_committed_event_identity(key) == identity);
      CHECK(*identity == retained_identity);
      CHECK(*store.accepted_private_event_record_at(0U) == event);
      CHECK(fixture.owner().event_identity_record_count() == 1U);
      CHECK(fixture.owner().trade_identity_record_count() == 1U);
      CHECK(fixture.owner().exchange_order_mapping_count() == 1U);
      CHECK(order.private_projection() == projection);
      const auto pending = fixture.owner().pending_private_executions(order.order_id());
      REQUIRE(pending.size() == 1U);
      CHECK(pending.front().execution == event.input());
      CHECK(store.accepted_private_event_record_count() == 1U);
      CHECK(store.accepted_private_audit_record_count() == 1U);
      CHECK(store.next_audit_ordinal().value() == 2U);
      CHECK_FALSE(store.has_outstanding_preparation());
      check_gap_economics_unchanged(fixture, order, held);
      auto& medium = *fixture.transaction.medium;
      fixture.end_incarnation();
      CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 2U);
    }
  }
}

// --------------------------------------------------------
// A known canonical trade takes precedence over an unknown candidate locator without replacing
// that candidate's resolution or staging a second identity.
TEST_CASE("unknown local locator cannot reuse a committed known private trade",
          "[private-gap-commit][runtime][m4][containment]") {
  for (const auto lane : {GapAdmissionLane::Venue, GapAdmissionLane::Reconciliation}) {
    CAPTURE(static_cast<int>(lane));
    PrivateGapCommitFixture fixture;
    const auto& order = fixture.transaction.order();
    const auto held = capture_gap_economics(fixture, order);
    const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
    const auto other_lane = lane == GapAdmissionLane::Venue ? GapAdmissionLane::Reconciliation
                                                            : GapAdmissionLane::Venue;
    const auto original = fixture.create_execution_attempt_or_throw(
        other_lane, order, exchange, GapSourceExecution{11U, 21U, 2});
    const auto first = fixture.admit_or_throw(original);
    fixture.execute_admitted_turns_or_throw();
    check_gap_completion(fixture, other_lane, first, oms::PrivateEventDisposition::BufferedGap);
    const auto projection = order.private_projection();
    const auto trade_key =
        oms::TradeKey{order.provenance().venue_id, order.provenance().logical_account_id,
                      create_m4_opaque_identity_or_throw<oms::TradeId>(21U)};
    const auto* const trade = fixture.owner().find_committed_trade_identity(trade_key);
    REQUIRE(trade != nullptr);
    const auto canonical_trade = *trade;

    const auto unknown_order = create_m4_order_id_or_throw(91U);
    REQUIRE(fixture.transaction.authority.submission->outbound_oms().find_order(unknown_order) ==
            nullptr);
    auto locator = extract_evidence_value_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(unknown_order, std::nullopt));
    const auto reused_trade = fixture.create_execution_attempt_or_throw(
        lane, order, std::move(locator), GapSourceExecution{12U, 21U, 2});
    const auto reused = fixture.admit_or_throw(reused_trade);
    fixture.execute_admitted_turns_or_throw();
    const auto& retained = require_gap_fallback(fixture, lane, reused_trade, reused);
    CHECK(retained.safety_reason == risk::AccountSafetyReason::TradeIdentityConflict);
    REQUIRE(retained.first_admission_resolution);
    CHECK(retained.first_admission_resolution->resolution_kind() ==
          oms::PrivateEventResolutionKind::Unknown);
    CHECK_FALSE(retained.preparation);
    CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().trade_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 0U);
    CHECK(fixture.owner().account_safety_state(order.provenance().logical_account_id) ==
          risk::AccountSafetyState::Quarantined);
    CHECK(fixture.owner().find_committed_trade_identity(trade_key) == trade);
    CHECK(*trade == canonical_trade);
    CHECK(fixture.owner().event_identity_record_count() == 1U);
    CHECK(fixture.owner().trade_identity_record_count() == 1U);
    CHECK(fixture.owner().exchange_order_mapping_count() == 1U);
    CHECK(order.private_projection() == projection);
    const auto& store = fixture.owner().business_evidence_store();
    CHECK(store.accepted_private_event_record_count() == 1U);
    CHECK(store.accepted_private_audit_record_count() == 1U);
    CHECK_FALSE(store.has_outstanding_preparation());
    check_gap_economics_unchanged(fixture, order, held);
    auto& medium = *fixture.transaction.medium;
    fixture.end_incarnation();
    CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 2U);
  }
}

// --------------------------------------------------------
// A retained unknown input cannot acquire a canonical identity merely because another exact
// event subsequently proves the exchange-to-order mapping. Its original resolution still wins.
TEST_CASE("retained unknown execution is not retroactively promoted after an exchange mapping",
          "[private-gap-commit][runtime][m4][containment]") {
  PrivateGapCommitFixture fixture;
  const auto& order = fixture.submit_additional_order_or_throw(5);
  const auto held = capture_gap_economics(fixture, order);
  const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto unknown_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, order, exchange, GapSourceExecution{11U, 21U, 2, 1, false});
  const auto unknown = fixture.admit_or_throw(unknown_attempt);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_unknown =
      require_gap_fallback(fixture, GapAdmissionLane::Venue, unknown_attempt, unknown);
  REQUIRE(retained_unknown.preparation);
  CHECK(retained_unknown.preparation->resolution.resolution_kind() ==
        oms::PrivateEventResolutionKind::Unknown);
  const auto unknown_key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(
      std::get<oms::PrivateOrderIngressAttempt>(unknown_attempt).semantic_value());
  CHECK(fixture.owner().find_committed_event_identity(unknown_key) == nullptr);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);

  const auto mapping_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, order, exchange, GapSourceExecution{12U, 22U, 5});
  const auto mapping = fixture.admit_or_throw(mapping_attempt);
  fixture.execute_admitted_turns_or_throw();
  check_gap_completion(fixture, GapAdmissionLane::Reconciliation, mapping,
                       oms::PrivateEventDisposition::BufferedGap);
  REQUIRE(fixture.owner().exchange_order_mapping_count() == 1U);
  const auto mapped_projection = order.private_projection();
  REQUIRE(fixture.clock.advance_nanoseconds(7U));
  const auto repeated_unknown = fixture.admit_or_throw(unknown_attempt);
  fixture.execute_admitted_turns_or_throw();
  const auto& repeat =
      require_gap_fallback(fixture, GapAdmissionLane::Venue, unknown_attempt, repeated_unknown);
  REQUIRE(repeat.preparation);
  CHECK(repeat.preparation->resolution == retained_unknown.preparation->resolution);
  CHECK(fixture.owner().find_committed_event_identity(unknown_key) == nullptr);
  CHECK(fixture.owner().event_identity_record_count() == 1U);
  CHECK(fixture.owner().trade_identity_record_count() == 1U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 1U);
  CHECK(fixture.owner().pending_private_executions(order.order_id()).size() == 1U);
  CHECK(order.private_projection() == mapped_projection);
  CHECK(fixture.owner().business_evidence_store().accepted_private_event_record_count() == 1U);
  CHECK(fixture.owner().business_evidence_store().accepted_private_audit_record_count() == 1U);
  check_gap_economics_unchanged(fixture, order, held);
  auto& medium = *fixture.transaction.medium;
  fixture.end_incarnation();
  CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 2U);
}

// --------------------------------------------------------
// A foreign epoch retains its event identity for conflict comparison without claiming trade,
// interval, or mapping progress from another runtime incarnation.
TEST_CASE("foreign reconciliation epoch preserves event comparison without claiming ownership",
          "[private-gap-commit][runtime][m4][containment]") {
  PrivateGapCommitFixture fixture;
  const auto& order = fixture.transaction.order();
  const auto& other = fixture.submit_additional_order_or_throw(5);
  const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto lane = GapAdmissionLane::Reconciliation;
  const auto foreign_epoch =
      extract_evidence_value_or_throw(recovery::RuntimeEpochId::identity_from_namespace_and_counter(
          fixture.owner().runtime_epoch_id().order_namespace(), 2U));
  auto source = GapSourceExecution{11U, 21U, 2, 1, true, 100, foreign_epoch};
  const auto first = fixture.create_execution_attempt_or_throw(lane, order, exchange, source);
  const auto received = fixture.admit_or_throw(first);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained = require_gap_fallback(fixture, lane, first, received);
  CHECK(retained.safety_reason == risk::AccountSafetyReason::ProvenanceMismatch);
  REQUIRE(retained.first_admission_resolution);
  CHECK(retained.first_admission_resolution->resolution_kind() ==
        oms::PrivateEventResolutionKind::Conflict);
  CHECK_FALSE(retained.execution_progress.trade);
  CHECK_FALSE(retained.execution_progress.mapping);
  CHECK_FALSE(retained.execution_progress.interval);

  source.price = 99;
  const auto changed = fixture.create_execution_attempt_or_throw(lane, order, exchange, source);
  const auto changed_receipt = fixture.admit_or_throw(changed);
  fixture.execute_admitted_turns_or_throw();
  const auto& conflicting = require_gap_fallback(fixture, lane, changed, changed_receipt);
  CHECK(conflicting.safety_reason == risk::AccountSafetyReason::EventIdentityConflict);
  CHECK(conflicting.first_admission_resolution == retained.first_admission_resolution);

  source.reconciliation_runtime_epoch.reset();
  const auto current = fixture.create_execution_attempt_or_throw(lane, other, exchange, source);
  const auto current_receipt = fixture.admit_or_throw(current);
  fixture.execute_admitted_turns_or_throw();
  check_gap_completion(fixture, lane, current_receipt, oms::PrivateEventDisposition::BufferedGap);
  const auto key = oms::ExchangeOrderKey{order.provenance().venue_id,
                                         order.provenance().logical_account_id, exchange};
  const auto* mapping = fixture.owner().find_committed_exchange_mapping(key);
  REQUIRE(mapping != nullptr);
  CHECK(mapping->order_id == other.order_id());
  CHECK(fixture.owner().event_identity_record_count() == 1U);
  CHECK(fixture.owner().trade_identity_record_count() == 1U);
}

// --------------------------------------------------------
// An interval that cannot fit the order's full pending list still preserves its verified mapping
// claim for reconciliation. A different order cannot bind that exchange identity in the meantime.
TEST_CASE("pending interval capacity preserves an independently verified mapping claim",
          "[private-gap-commit][runtime][m4][containment]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_pending_fill_intervals_per_order = 1U;
  PrivateGapCommitFixture fixture{execution::OrderSide::Buy, capacities};
  const auto& first = fixture.submit_additional_order_or_throw(5);
  const auto& other = fixture.transaction.order();
  const auto lane = GapAdmissionLane::Venue;
  const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto locator = extract_evidence_value_or_throw(
      oms::PrivateOrderLocator::create_private_order_locator(first.order_id(), std::nullopt));
  const auto initial = fixture.create_execution_attempt_or_throw(lane, first, locator,
                                                                 GapSourceExecution{11U, 21U, 2});
  const auto initial_receipt = fixture.admit_or_throw(initial);
  fixture.execute_admitted_turns_or_throw();
  check_gap_completion(fixture, lane, initial_receipt, oms::PrivateEventDisposition::BufferedGap);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);

  const auto overflow = fixture.create_execution_attempt_or_throw(lane, first, exchange,
                                                                  GapSourceExecution{12U, 22U, 4});
  const auto overflow_receipt = fixture.admit_or_throw(overflow);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained = require_gap_fallback(fixture, lane, overflow, overflow_receipt);
  CHECK(retained.safety_reason == risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure);
  REQUIRE(retained.execution_progress.mapping);
  CHECK(retained.execution_progress.mapping->order_id == first.order_id());
  REQUIRE(retained.execution_progress.interval);

  const auto conflicting = fixture.create_execution_attempt_or_throw(
      lane, other, exchange, GapSourceExecution{13U, 23U, 2});
  const auto conflicting_receipt = fixture.admit_or_throw(conflicting);
  fixture.execute_admitted_turns_or_throw();
  const auto& conflict = require_gap_fallback(fixture, lane, conflicting, conflicting_receipt);
  CHECK(conflict.safety_reason == risk::AccountSafetyReason::CorrelationConflict);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
  CHECK(fixture.owner().event_identity_record_count() == 1U);
  CHECK(fixture.owner().trade_identity_record_count() == 1U);
  CHECK(fixture.owner().pending_private_executions(first.order_id()).size() == 1U);
  CHECK(fixture.owner().pending_private_executions(other.order_id()).empty());
}

// --------------------------------------------------------
// A failed owner turn retains its source even if an occupied preparation prevents allocating
// canonical identity. Releasing the unrelated lease cannot clear its event, trade, or exchange
// mapping conflict checks.
TEST_CASE("released evidence lease cannot promote a previously retained execution",
          "[private-gap-commit][runtime][m4][containment]") {
  PrivateGapCommitFixture fixture;
  const auto& order = fixture.transaction.order();
  const auto& competing_order = fixture.submit_additional_order_or_throw(5);
  REQUIRE(competing_order.order_id() != order.order_id());
  const auto before = order.private_projection();
  const auto competing_before = competing_order.private_projection();
  const auto held = capture_gap_economics(fixture, order);
  const auto competing_held = capture_gap_economics(fixture, competing_order);
  const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto gap = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, order, exchange, GapSourceExecution{11U, 21U, 2});
  const auto& store = fixture.owner().business_evidence_store();
  std::optional<oms::PrivateEventResolution> first_resolution;
  {
    const auto input = fixture.transaction.normalize_input_or_throw(
        test_support::BusinessEvidenceInput::BufferedGap);
    const auto lease = fixture.owner().prepare_initial_known_authoritative_business_evidence(
        input,
        recovery::JournalReplayProvenance{create_m4_ordinal_or_throw<model::AdmissionOrdinal>(91U),
                                          create_m4_ordinal_or_throw<model::ReceiveSequence>(92U)});
    REQUIRE(lease);
    REQUIRE(store.has_outstanding_preparation());
    const auto first = fixture.admit_or_throw(gap);
    fixture.execute_admitted_turns_or_throw();
    const auto& retained = require_gap_fallback(fixture, GapAdmissionLane::Venue, gap, first);
    CHECK(retained.safety_reason == risk::AccountSafetyReason::EvidenceCapacityExhausted);
    REQUIRE(retained.first_admission_resolution);
    first_resolution = retained.first_admission_resolution;
    CHECK(store.has_outstanding_preparation());
    CHECK(store.accepted_private_event_record_count() == 0U);
  }
  CHECK_FALSE(store.has_outstanding_preparation());

  const auto repeat = fixture.admit_or_throw(gap);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_repeat = require_gap_fallback(fixture, GapAdmissionLane::Venue, gap, repeat);
  CHECK(retained_repeat.first_admission_resolution == first_resolution);

  const auto changed_kind = fixture.create_acknowledgement_attempt_or_throw(GapAdmissionLane::Venue,
                                                                            order, exchange, 11U);
  const auto conflict = fixture.admit_or_throw(changed_kind);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_conflict =
      require_gap_fallback(fixture, GapAdmissionLane::Venue, changed_kind, conflict);
  CHECK(retained_conflict.safety_reason == risk::AccountSafetyReason::EventIdentityConflict);
  CHECK(retained_conflict.first_admission_resolution == first_resolution);

  const auto reused_trade = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, order, exchange, GapSourceExecution{12U, 21U, 2});
  const auto reused = fixture.admit_or_throw(reused_trade);
  fixture.execute_admitted_turns_or_throw();
  static_cast<void>(
      require_gap_fallback(fixture, GapAdmissionLane::Reconciliation, reused_trade, reused));

  const auto reused_exchange = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, competing_order, exchange, GapSourceExecution{13U, 22U, 2});
  const auto competing = fixture.admit_or_throw(reused_exchange);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_competing =
      require_gap_fallback(fixture, GapAdmissionLane::Reconciliation, reused_exchange, competing);
  CHECK(retained_competing.safety_reason == risk::AccountSafetyReason::CorrelationConflict);
  const auto exchange_key = oms::ExchangeOrderKey{order.provenance().venue_id,
                                                  order.provenance().logical_account_id, exchange};
  CHECK(fixture.owner().find_committed_exchange_mapping(exchange_key) == nullptr);
  CHECK(order.private_projection() == before);
  CHECK(competing_order.private_projection() == competing_before);
  CHECK(fixture.owner().pending_private_executions(order.order_id()).empty());
  CHECK(fixture.owner().pending_private_executions(competing_order.order_id()).empty());
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
  CHECK(store.accepted_private_event_record_count() == 0U);
  CHECK(store.accepted_private_audit_record_count() == 0U);
  CHECK(store.next_audit_ordinal().value() == 1U);
  CHECK_FALSE(store.has_outstanding_preparation());
  CHECK(fixture.owner().retained_identity_turn_count() == 5U);
  check_gap_economics_unchanged(fixture, order, held);
  check_gap_economics_unchanged(fixture, competing_order, competing_held);
  auto& medium = *fixture.transaction.medium;
  fixture.end_incarnation();
  CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 1U);
  CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 1U);
}

// --------------------------------------------------------
// A valid contiguous execution awaits its economics reducer. Until then, a distinct overlapping
// execution cannot acquire a canonical gap on the same local order.
TEST_CASE("valid retained contiguous execution blocks an overlapping private gap",
          "[private-gap-commit][runtime][m4][containment]") {
  PrivateGapCommitFixture fixture;
  const auto& order = fixture.submit_additional_order_or_throw(5);
  const auto before = order.private_projection();
  const auto held = capture_gap_economics(fixture, order);
  const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
  const auto prefix_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, order, exchange, GapSourceExecution{11U, 21U, 3, 3});
  const auto prefix = fixture.admit_or_throw(prefix_attempt);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_prefix =
      require_gap_fallback(fixture, GapAdmissionLane::Venue, prefix_attempt, prefix);
  REQUIRE(retained_prefix.first_admission_resolution);
  REQUIRE(retained_prefix.first_admission_resolution->known_resolution() != nullptr);
  CHECK(retained_prefix.first_admission_resolution->known_resolution()->order_id ==
        order.order_id());

  const auto overlap_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, order, exchange, GapSourceExecution{12U, 22U, 2});
  const auto overlap = fixture.admit_or_throw(overlap_attempt);
  fixture.execute_admitted_turns_or_throw();
  const auto& retained_overlap =
      require_gap_fallback(fixture, GapAdmissionLane::Reconciliation, overlap_attempt, overlap);
  CHECK(retained_overlap.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  CHECK_FALSE(retained_overlap.preparation);
  CHECK(retained_overlap.first_admission_resolution == retained_prefix.first_admission_resolution);
  CHECK(fixture.owner().account_safety_state(order.provenance().logical_account_id) ==
        risk::AccountSafetyState::Quarantined);
  CHECK(fixture.owner().retained_identity_turn_count() == 2U);
  CHECK(order.private_projection() == before);
  CHECK(fixture.owner().pending_private_executions(order.order_id()).empty());
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
  const auto& store = fixture.owner().business_evidence_store();
  CHECK(store.accepted_private_event_record_count() == 0U);
  CHECK(store.accepted_private_audit_record_count() == 0U);
  check_gap_economics_unchanged(fixture, order, held);
  auto& medium = *fixture.transaction.medium;
  fixture.end_incarnation();
  CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) == 1U);
}

// --------------------------------------------------------
// Select a single limiting resource so each failure proves that all other prepared components
// release their claims without consuming a journal position, audit row, or domain entry.
TEST_CASE("private gap preflight leaves the accepted prefix unchanged at each capacity boundary",
          "[private-gap-commit][runtime][m4][capacity]") {
  for (const auto resource :
       {GapCapacityResource::Journal, GapCapacityResource::EventEvidence,
        GapCapacityResource::Audit, GapCapacityResource::EventIdentity,
        GapCapacityResource::TradeIdentity, GapCapacityResource::PendingInterval}) {
    CAPTURE(static_cast<int>(resource));
    auto capacities = test_support::create_ordinary_m4_policy_capacities();
    const bool journal_full = resource == GapCapacityResource::Journal;
    const bool events_full = resource == GapCapacityResource::EventEvidence;
    const bool audit_full = resource == GapCapacityResource::Audit;
    if (journal_full) {
      capacities.max_journal_records = 2U;
    } else if (events_full) {
      capacities.max_private_event_records = 1U;
    } else if (audit_full) {
      capacities.max_pending_fill_intervals_per_order = 1U;
      capacities.max_private_audit_records = 4U;
    } else if (resource == GapCapacityResource::EventIdentity) {
      capacities.max_event_identity_records = 1U;
    } else if (resource == GapCapacityResource::TradeIdentity) {
      capacities.max_trade_identity_records = 1U;
    } else {
      capacities.max_pending_fill_intervals_per_order = 1U;
    }
    PrivateGapCommitFixture fixture{execution::OrderSide::Buy, capacities};
    const auto& order = fixture.submit_additional_order_or_throw(5);
    const auto held = capture_gap_economics(fixture, order);
    const auto exchange = fixture.transaction.create_exchange_order_id_or_throw();
    const auto first_attempt = fixture.create_execution_attempt_or_throw(
        GapAdmissionLane::Venue, order, exchange, GapSourceExecution{11U, 21U, 2});
    const auto first = fixture.admit_or_throw(first_attempt);
    fixture.execute_admitted_turns_or_throw();
    check_gap_completion(fixture, GapAdmissionLane::Venue, first,
                         oms::PrivateEventDisposition::BufferedGap);

    // ++++++++++++++++++++++++++++++++++++++++
    // For audit capacity, exact repeats consume audit slots but do not add identity or OMS rows.
    const std::uint32_t accepted = audit_full ? 4U : 1U;
    for (std::uint32_t repeat_index = 1U; repeat_index < accepted; ++repeat_index) {
      const auto duplicate = fixture.admit_or_throw(first_attempt);
      fixture.execute_admitted_turns_or_throw();
      check_gap_completion(fixture, GapAdmissionLane::Venue, duplicate,
                           oms::PrivateEventDisposition::ExactEventDuplicate);
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // A journal/evidence-full repeat needs no new domain capacity; a new disjoint interval
    // independently tests exhausted event identity, trade identity, or pending interval capacity.
    const bool evidence_full = journal_full || events_full || audit_full;
    const auto failed_attempt =
        evidence_full
            ? first_attempt
            : fixture.create_execution_attempt_or_throw(GapAdmissionLane::Venue, order, exchange,
                                                        GapSourceExecution{12U, 22U, 4});
    const auto failed = fixture.admit_or_throw(failed_attempt);
    const auto projection = order.private_projection();
    fixture.execute_admitted_turns_or_throw();
    const auto& retained =
        require_gap_fallback(fixture, GapAdmissionLane::Venue, failed_attempt, failed);
    CHECK(retained.safety_reason ==
          (evidence_full ? risk::AccountSafetyReason::EvidenceCapacityExhausted
                         : risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure));
    CHECK(fixture.owner().account_safety_state(order.provenance().logical_account_id) ==
          risk::AccountSafetyState::Quarantined);
    CHECK(order.private_projection() == projection);
    CHECK(fixture.owner().pending_private_executions(order.order_id()).size() == 1U);
    CHECK(fixture.owner().event_identity_record_count() == 1U);
    CHECK(fixture.owner().trade_identity_record_count() == 1U);
    CHECK(fixture.owner().exchange_order_mapping_count() == 1U);
    check_gap_economics_unchanged(fixture, order, held);
    const auto& store = fixture.owner().business_evidence_store();
    CHECK(store.accepted_private_event_record_count() == accepted);
    CHECK(store.accepted_private_audit_record_count() == accepted);
    CHECK(store.next_audit_ordinal().value() == static_cast<std::uint64_t>(accepted) + 1U);
    CHECK(store.accepted_private_event_record_at(accepted) == nullptr);
    CHECK(store.accepted_private_audit_record_at(accepted) == nullptr);
    CHECK_FALSE(store.has_outstanding_preparation());
    auto& medium = *fixture.transaction.medium;
    fixture.end_incarnation();
    CHECK(extract_evidence_value_or_throw(medium.published_journal_record_count()) ==
          accepted + 1U);
    CHECK(extract_evidence_value_or_throw(medium.acknowledged_journal_record_count()) == 1U);
    CHECK_FALSE(medium.published_journal_record_at(accepted + 1U));

    // ++++++++++++++++++++++++++++++++++++++++
  }
}

// --------------------------------------------------------
// Exercise immutable completion oracles while another thread commits in both lanes. Neither an
// incomplete slot nor a different nominal lane may satisfy an admission's disposition query.
TEST_CASE("private gap completion oracles publish immutable lane-specific prefixes",
          "[private-gap-commit][runtime][m4][concurrency]") {
  PrivateGapCommitFixture fixture;
  const auto& venue_order = fixture.transaction.order();
  const auto& reconciliation_order = fixture.submit_additional_order_or_throw(2);
  const auto venue_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Venue, venue_order, fixture.transaction.create_exchange_order_id_or_throw(),
      GapSourceExecution{11U, 21U, 2});
  const auto reconciliation_attempt = fixture.create_execution_attempt_or_throw(
      GapAdmissionLane::Reconciliation, reconciliation_order,
      create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x52U),
      GapSourceExecution{12U, 22U, 2});
  std::vector<model::AdmissionOrdinal> attempts;
  for (std::uint32_t index = 0U; index < 24U; ++index) {
    const auto& source = index % 2U == 0U ? venue_attempt : reconciliation_attempt;
    attempts.push_back(fixture.admit_or_throw(source).attempt_ordinal);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Keep fixture lifetime and thread cleanup explicit; a reader sees absence or the exact final
  // value during publication, and once observed, the same value must remain observable.
  const auto& owner = fixture.owner();
  std::atomic<bool> ready{false};
  std::atomic<bool> finished{false};
  bool valid = true;
  std::thread reader{[&] {
    std::array<bool, 24U> seen{};
    ready.store(true, std::memory_order_release);
    for (;;) {
      const bool final_pass = finished.load(std::memory_order_acquire);
      for (std::size_t index = 0U; index < attempts.size(); ++index) {
        const auto lane =
            index % 2U == 0U ? GapAdmissionLane::Venue : GapAdmissionLane::Reconciliation;
        const auto other = lane == GapAdmissionLane::Venue ? GapAdmissionLane::Reconciliation
                                                           : GapAdmissionLane::Venue;
        const auto expected = index < 2U ? oms::PrivateEventDisposition::BufferedGap
                                         : oms::PrivateEventDisposition::ExactEventDuplicate;
        const auto ordinal = attempts[index];
        const auto actual = find_gap_disposition(owner, lane, ordinal);
        if (find_gap_disposition(owner, other, ordinal) ||
            (actual ? *actual != expected : (seen[index] || final_pass))) {
          valid = false;
        }
        seen[index] = seen[index] || actual.has_value();
      }
      if (final_pass) {
        break;
      }
    }
  }};
  while (!ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  try {
    fixture.execute_admitted_turns_or_throw();
  } catch (...) {
    finished.store(true, std::memory_order_release);
    reader.join();
    throw;
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  CHECK(valid);

  // ++++++++++++++++++++++++++++++++++++++++
  // Cross-check the complete fixed prefix and each initial gap after synchronized inspection.
  CHECK(owner.event_identity_record_count() == 2U);
  CHECK(owner.trade_identity_record_count() == 2U);
  CHECK(owner.exchange_order_mapping_count() == 2U);
  CHECK(owner.pending_private_executions(venue_order.order_id()).size() == 1U);
  CHECK(owner.pending_private_executions(reconciliation_order.order_id()).size() == 1U);
  CHECK(owner.business_evidence_store().accepted_private_event_record_count() == 24U);
  CHECK(owner.business_evidence_store().accepted_private_audit_record_count() == 24U);
  CHECK(owner.retained_identity_turn_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
