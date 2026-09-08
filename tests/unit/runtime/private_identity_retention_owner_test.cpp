// Purpose: qualify the production private-admission owner's genuine token authority, immutable
// lane-specific retained evidence, transactional preparation capacity, and concurrent publication.

#include "aegis/runtime/private_order_reconciler.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

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

// --------------------------------------------------------
// Extract a checked fixture value or stop setup before exercising the production owner.
template <typename Value> [[nodiscard]] Value extract_result_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid private identity retention owner fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Derive one exact policy from a genuine coordinator, varying bounded storage before installation.
[[nodiscard]] test_support::M4OwnerTestAuthority
create_retention_authority_or_throw(runtime::M4PolicyCapacities capacities) {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  authority.m4_policy = extract_result_or_throw(runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities));
  return authority;
}

// --------------------------------------------------------

// ########################################################################
// Owns a genuine installed reconciler, one real local order, and a matching serialized executor.
// The executor dies before the borrowed production owner; all ordinary fixture inspection is
// quiescent, with only immutable retained-record queries used by concurrent readers.
class RetentionOwnerFixture final {
public:

  // --------------------------------------------------------
  // Install exact fixed capacities, submit once through BotContext, and bind the resulting private
  // owner to a matching executor only after every submission callback has completed.
  explicit RetentionOwnerFixture(
      runtime::M4PolicyCapacities capacities = test_support::create_ordinary_m4_policy_capacities())
      : authority{create_retention_authority_or_throw(capacities)},
        factory{
            extract_result_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
                authority.configuration, authority.m4_policy))} {
    test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
    const auto result = test_support::submit_m4_order_or_throw(
        authority, test_support::create_m4_reference_order_request_or_throw());
    if (!result.order_id() ||
        result.disposition() != execution::SubmitDisposition::WriteInitiated) {
      throw std::logic_error{"missing genuine local order for retention"};
    }
    order_id = *result.order_id();
    executor = std::make_unique<runtime::SerializedExecutor>(
        0U, clock,
        extract_result_or_throw(
            runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                authority.configuration, authority.m4_policy)),
        *authority.submission->private_admission_owner());
  }

  // --------------------------------------------------------
  // Borrow the installed production implementation without introducing another mutable owner.
  [[nodiscard]] const runtime::PrivateOrderReconciler& owner() const noexcept {
    return *authority.submission->private_order_reconciler();
  }

  // --------------------------------------------------------
  // Borrow the real OMS provenance after construction established the order's retained ownership.
  [[nodiscard]] const oms::OutboundOrderRecord& order_or_throw() const {
    const auto* row = authority.submission->outbound_oms().find_order(*order_id);
    if (row == nullptr) {
      throw std::logic_error{"missing retained local order"};
    }
    return *row;
  }

  // --------------------------------------------------------
  // Normalize a unique ordinary execution while permitting independent trade-capacity pressure.
  [[nodiscard]] oms::PrivateOrderIngressAttempt
  create_execution_attempt_or_throw(std::uint8_t event_byte, std::uint8_t trade_byte = 0x71U,
                                    std::int64_t price = 99) const {
    const auto& provenance = order_or_throw().provenance();
    auto locator = extract_result_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
        order_id, test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U)));
    return extract_result_or_throw(factory.create_venue_execution_attempt(
        oms::VenuePrivateIngressOrigin{
            oms::VenuePrivateEventKey{
                provenance.venue_id, provenance.logical_account_id,
                test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U),
                test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event_byte)},
            model::SourceTimestamp{100U}},
        std::move(locator),
        test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(trade_byte),
        provenance.instrument_id, provenance.metadata_revision,
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Price>(price), std::nullopt));
  }

  // --------------------------------------------------------
  // Normalize a distinct reconciliation row whose required side reproduces the ordinary tuple.
  [[nodiscard]] oms::ReconciliationPrivateEventIngressAttempt
  create_reconciliation_attempt_or_throw(std::uint64_t row = 1U) const {
    const auto& provenance = order_or_throw().provenance();
    auto locator = extract_result_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(order_id, std::nullopt));
    return extract_result_or_throw(factory.create_reconciliation_execution_attempt(
        oms::ReconciliationPrivateIngressOrigin{
            test_support::create_m4_reconciliation_epoch_or_throw(),
            test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
            test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(row),
            model::SourceTimestamp{100U}},
        provenance.logical_account_id, provenance.venue_id, std::move(locator),
        test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U),
        provenance.instrument_id, provenance.metadata_revision,
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Price>(99), execution::OrderSide::Buy));
  }

  // --------------------------------------------------------
  // Drain a bounded admitted script including its interleaved account-fence turns.
  void execute_pending_turns_or_throw() {
    auto bound = executor->bind_to_current_thread();
    if (!bound) {
      throw std::logic_error{"failed to bind retention owner executor"};
    }
    const auto executed = executor->execute_pending_turns(128U);
    const auto released = executor->release_from_current_thread();
    if (!executed || !released) {
      throw std::logic_error{"failed to complete retention owner script"};
    }
  }

  // --------------------------------------------------------
  // Destruction order encloses the executor's borrowed clock and private owner.
  test_support::M4OwnerTestAuthority authority;
  runtime::PrivateOrderEventFactory factory;
  std::optional<model::OrderId> order_id;
  model::DeterministicClockProvider clock{100U};
  std::unique_ptr<runtime::SerializedExecutor> executor;
};

// ########################################################################
// The executor owns this adapter, so neither its live event tokens nor an executor-authored
// account fence may authorize mutation in the separately borrowed production owner. The optional
// fence mode retains one authentic token locally to make that real control call reachable.
class ForeignAuthorityForwarder final : public runtime::PrivateAdmissionOwner {
public:

  // --------------------------------------------------------
  // Borrow a foreign implementation that remains alive until this adapter and executor are gone.
  explicit ForeignAuthorityForwarder(runtime::PrivateAdmissionOwner& target,
                                     bool forward_account_fence = false) noexcept
      : target_{target}, forward_account_fence_{forward_account_fence} {}

  // --------------------------------------------------------
  // Forward a live token, or retain it locally to request an authentic account fence afterward.
  runtime::PrivateTurnCompletion
  commit_private_order_turn(runtime::AdmittedPrivateOrderSlot admitted) noexcept override {
    if (forward_account_fence_) {
      const auto view = admitted.inspect_admitted_private_order_slot();
      if (view) {
        retained_ordinal_ = view.value().admission_receipt().attempt_ordinal;
        retained_attempt_ = view.value().ingress_attempt();
        return runtime::RetainedPrivateTurn::create_retained_private_turn_for_account(
            retained_error_, risk::AccountSafetyReason::CriticalAdmissionLoss);
      }
    }
    return target_.commit_private_order_turn(std::move(admitted));
  }

  // --------------------------------------------------------
  // Attempt the same wrong-owner transfer through the distinct reconciliation capability.
  runtime::PrivateTurnCompletion commit_reconciliation_event_turn(
      runtime::AdmittedReconciliationEventSlot admitted) noexcept override {
    return target_.commit_reconciliation_event_turn(std::move(admitted));
  }

  // --------------------------------------------------------
  // Forward both canonical-oracle names so a foreign retained record cannot hide a rejection.
  std::optional<oms::PrivateEventDisposition> find_committed_private_event_disposition(
      model::AdmissionOrdinal ordinal) const noexcept override {
    return target_.find_committed_private_event_disposition(ordinal);
  }

  // --------------------------------------------------------
  // Preserve the independent reconciliation canonical-oracle domain.
  std::optional<oms::PrivateEventDisposition> find_committed_reconciliation_event_disposition(
      model::AdmissionOrdinal ordinal) const noexcept override {
    return target_.find_committed_reconciliation_event_disposition(ordinal);
  }

  // --------------------------------------------------------
  // Forward ordinary retained lookup to prove the rejected call published no owner evidence.
  const model::DomainError* find_committed_retained_private_event_error(
      model::AdmissionOrdinal ordinal) const noexcept override {
    if (retained_ordinal_ == ordinal) {
      return &retained_error_;
    }
    return target_.find_committed_retained_private_event_error(ordinal);
  }

  // --------------------------------------------------------
  // Forward reconciliation retained lookup without borrowing the ordinary lane's identity.
  const model::DomainError* find_committed_retained_reconciliation_event_error(
      model::AdmissionOrdinal ordinal) const noexcept override {
    return target_.find_committed_retained_reconciliation_event_error(ordinal);
  }

  // --------------------------------------------------------
  // Forward only the scripted authentic account fence into the foreign production implementation.
  model::Result<void>
  apply_account_safety_fence(const runtime::AccountSafetyFenceTurn& fence,
                             const runtime::ControlTurnContext& context) noexcept override {
    if (forward_account_fence_) {
      account_fence_forwarded_ = true;
      return target_.apply_account_safety_fence(fence, context);
    }
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }

  // --------------------------------------------------------
  // Reject reasonless follow-on global fences for the same owner-isolation boundary.
  model::Result<void>
  apply_global_private_fence(const runtime::GlobalPrivateFenceTurn&,
                             const runtime::ControlTurnContext&) noexcept override {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }

  // --------------------------------------------------------
  // Confirm that the adversarial script reached the authentic account-fence callback.
  [[nodiscard]] bool has_forwarded_account_fence() const noexcept {
    return account_fence_forwarded_;
  }

  // --------------------------------------------------------
private:
  runtime::PrivateAdmissionOwner& target_;
  bool forward_account_fence_;
  bool account_fence_forwarded_{false};
  std::optional<model::AdmissionOrdinal> retained_ordinal_;
  std::optional<oms::PrivateOrderIngressAttempt> retained_attempt_;
  model::DomainError retained_error_{model::DomainErrorCode::ExecutionNotPermitted, {}};
};

// ########################################################################

// --------------------------------------------------------
// Produce a valid source fact for an unconfigured account, forcing the executor's reasonless
// global-fence path without assigning the unknown source to any configured account.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_unattributable_attempt_or_throw(const RetentionOwnerFixture& fixture) {
  return extract_result_or_throw(fixture.factory.create_venue_acknowledgement_attempt(
      oms::VenuePrivateIngressOrigin{
          oms::VenuePrivateEventKey{
              fixture.order_or_throw().provenance().venue_id,
              test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
                  "account.foreign"),
              test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U),
              test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(0x52U)},
          model::SourceTimestamp{100U}},
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x62U), std::nullopt));
}

// --------------------------------------------------------
// Genuine tokens in distinct lanes retain exact ingress, receipts and resolutions, share trade
// comparison, and publish only each lane's retained oracle while canonical registries stay empty.
TEST_CASE("production identity owner retains ordinary and reconciliation turns separately",
          "[runtime][m4][identity-retention-owner]") {
  RetentionOwnerFixture fixture;
  const auto ordinary = fixture.create_execution_attempt_or_throw(1U);
  const auto reconciliation = fixture.create_reconciliation_attempt_or_throw();
  const auto admitted = extract_result_or_throw(fixture.executor->try_admit_private(ordinary));
  const auto reconciled =
      extract_result_or_throw(fixture.executor->try_admit_reconciliation_event(reconciliation));
  REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(reconciled.outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(admitted.receipt);
  REQUIRE(reconciled.receipt);
  CHECK(fixture.owner().find_retained_identity_turn(admitted.attempt_ordinal, false) == nullptr);
  CHECK(fixture.executor->private_admission_observation(admitted.attempt_ordinal)->state ==
        runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);
  fixture.execute_pending_turns_or_throw();

  const auto* ordinary_record =
      fixture.owner().find_retained_identity_turn(admitted.attempt_ordinal, false);
  const auto* reconciliation_record =
      fixture.owner().find_retained_identity_turn(reconciled.attempt_ordinal, true);
  REQUIRE(ordinary_record != nullptr);
  REQUIRE(reconciliation_record != nullptr);
  REQUIRE(ordinary_record->attempt);
  REQUIRE(reconciliation_record->attempt);
  CHECK(std::get<oms::PrivateOrderIngressAttempt>(*ordinary_record->attempt) == ordinary);
  CHECK(std::get<oms::ReconciliationPrivateEventIngressAttempt>(*reconciliation_record->attempt) ==
        reconciliation);
  CHECK(ordinary_record->receipt == admitted.receipt);
  CHECK(reconciliation_record->receipt == reconciled.receipt);
  REQUIRE(ordinary_record->preparation);
  REQUIRE(reconciliation_record->preparation);
  CHECK(ordinary_record->preparation->classification ==
        runtime::PrivateIdentityPreparationClassification::FirstObservation);
  CHECK(reconciliation_record->preparation->classification ==
        runtime::PrivateIdentityPreparationClassification::RepeatedTrade);
  CHECK(ordinary_record->preparation->resolution == reconciliation_record->preparation->resolution);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 2U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
  CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 1U);
  CHECK(fixture.owner().retained_identity_turn_count() == 2U);
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);

  const auto ordinary_terminal =
      fixture.executor->private_admission_observation(admitted.attempt_ordinal);
  const auto reconciliation_terminal =
      fixture.executor->reconciliation_admission_observation(reconciled.attempt_ordinal);
  REQUIRE(ordinary_terminal);
  REQUIRE(reconciliation_terminal);
  CHECK(ordinary_terminal->state ==
        runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK(reconciliation_terminal->state ==
        runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(ordinary_terminal->disposition);
  CHECK_FALSE(reconciliation_terminal->disposition);
  CHECK(fixture.owner().find_retained_identity_turn(admitted.attempt_ordinal, true) == nullptr);
  CHECK(fixture.owner().find_retained_identity_turn(reconciled.attempt_ordinal, false) == nullptr);
  CHECK(fixture.owner().find_committed_retained_reconciliation_event_error(
            admitted.attempt_ordinal) == nullptr);
  CHECK(fixture.owner().find_committed_retained_private_event_error(reconciled.attempt_ordinal) ==
        nullptr);
  CHECK_FALSE(fixture.owner().find_committed_private_event_disposition(admitted.attempt_ordinal));
  CHECK_FALSE(
      fixture.owner().find_committed_reconciliation_event_disposition(reconciled.attempt_ordinal));
}

// --------------------------------------------------------
// The admitted-turn evidence reservation precedes table preparation. Trade-capacity failure
// retains the entire later source and preserves every first event/trade/candidate value.
TEST_CASE("production identity owner retains preparation capacity failure without partial tables",
          "[runtime][m4][identity-retention-owner][capacity]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_trade_identity_records = 1U;
  RetentionOwnerFixture fixture{capacities};
  const auto first = fixture.create_execution_attempt_or_throw(1U);
  const auto second = fixture.create_execution_attempt_or_throw(2U, 0x72U);
  const auto first_admitted = extract_result_or_throw(fixture.executor->try_admit_private(first));
  const auto second_admitted = extract_result_or_throw(fixture.executor->try_admit_private(second));
  fixture.execute_pending_turns_or_throw();

  const auto* first_record =
      fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false);
  const auto* second_record =
      fixture.owner().find_retained_identity_turn(second_admitted.attempt_ordinal, false);
  REQUIRE(first_record != nullptr);
  REQUIRE(second_record != nullptr);
  REQUIRE(first_record->preparation);
  REQUIRE(second_record->preparation);
  CHECK_FALSE(first_record->preparation->capacity_exhaustion);
  CHECK(second_record->preparation->capacity_exhaustion ==
        runtime::PrivateIdentityPreparationCapacity::TradeRecords);
  CHECK(second_record->error_kind == runtime::PrivateIdentityRetentionError::IdentityCapacity);
  REQUIRE(second_record->attempt);
  CHECK(std::get<oms::PrivateOrderIngressAttempt>(*second_record->attempt) == second);
  CHECK(second_record->receipt == second_admitted.receipt);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 1U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
  CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 1U);
  CHECK(fixture.owner().identity_preparations().event_record_at(0U)->ingress_semantic_value ==
        first.semantic_value());
  CHECK(fixture.owner().identity_preparations().find_prepared_event(
            oms::PrivateEventRegistryKey::from_ingress_semantic_value(second.semantic_value())) ==
        nullptr);
  const auto observed =
      fixture.executor->private_admission_observation(second_admitted.attempt_ordinal);
  REQUIRE(observed);
  CHECK(observed->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  REQUIRE(observed->retention_error);
  CHECK(observed->retention_error->code == model::DomainErrorCode::PrivateEventCapacityExceeded);
  CHECK(fixture.owner().retained_identity_turn_count() == 2U);
}

// --------------------------------------------------------
// The first retained-turn overflow owns a dedicated immutable emergency record, faults the
// executor, and cannot partially prepare another event/trade even when those tables have headroom.
TEST_CASE("production identity owner preserves the first evidence overflow and fails closed",
          "[runtime][m4][identity-retention-owner][capacity]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_event_records = 1U;
  RetentionOwnerFixture fixture{capacities};
  const auto first = fixture.create_execution_attempt_or_throw(1U);
  const auto second = fixture.create_reconciliation_attempt_or_throw();
  const auto first_admitted = extract_result_or_throw(fixture.executor->try_admit_private(first));
  const auto second_admitted =
      extract_result_or_throw(fixture.executor->try_admit_reconciliation_event(second));
  REQUIRE(fixture.executor->bind_to_current_thread());
  for (std::size_t index = 0U; index < 4U && fixture.owner().find_retained_identity_turn(
                                                 second_admitted.attempt_ordinal, true) == nullptr;
       ++index) {
    const auto turn = fixture.executor->execute_next_turn();
    REQUIRE(turn);
    REQUIRE(turn.value());
  }
  const auto* first_record =
      fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false);
  const auto* overflow =
      fixture.owner().find_retained_identity_turn(second_admitted.attempt_ordinal, true);
  REQUIRE(first_record != nullptr);
  REQUIRE(overflow != nullptr);
  CHECK(overflow->error_kind == runtime::PrivateIdentityRetentionError::EvidenceCapacity);
  CHECK_FALSE(overflow->preparation);
  REQUIRE(overflow->attempt);
  CHECK(std::get<oms::ReconciliationPrivateEventIngressAttempt>(*overflow->attempt) == second);
  CHECK(overflow->receipt == second_admitted.receipt);
  CHECK(fixture.owner().retained_identity_turn_count() == 1U);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 1U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
  CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 1U);
  CHECK(fixture.executor->queue_snapshot().faulted);
  CHECK_FALSE(fixture.executor->execute_next_turn());
  CHECK(fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false) ==
        first_record);
  CHECK(fixture.owner().find_retained_identity_turn(second_admitted.attempt_ordinal, true) ==
        overflow);
  const auto* error = fixture.owner().find_committed_retained_reconciliation_event_error(
      second_admitted.attempt_ordinal);
  REQUIRE(error != nullptr);
  CHECK(error->code == model::DomainErrorCode::PrivateEvidenceExhausted);
  CHECK(fixture.owner().find_committed_retained_private_event_error(
            second_admitted.attempt_ordinal) == nullptr);
  REQUIRE(fixture.executor->release_from_current_thread());
}

// --------------------------------------------------------
// A live capability belongs to its exact installed executor consumer. An adapter cannot transfer
// either token family into another production owner, and rejection publishes no foreign evidence.
TEST_CASE("production identity owner rejects forwarded tokens from another consumer",
          "[runtime][m4][identity-retention-owner][authority]") {
  for (const bool reconciliation : {false, true}) {
    RetentionOwnerFixture fixture;
    ForeignAuthorityForwarder forwarder{*fixture.authority.submission->private_admission_owner()};
    model::DeterministicClockProvider clock{100U};
    runtime::SerializedExecutor foreign{
        0U, clock,
        extract_result_or_throw(
            runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                fixture.authority.configuration, fixture.authority.m4_policy)),
        forwarder};
    const auto ordinary = fixture.create_execution_attempt_or_throw(1U);
    const auto authoritative = fixture.create_reconciliation_attempt_or_throw();
    std::optional<model::AdmissionOrdinal> admitted_ordinal;
    if (reconciliation) {
      const auto admitted =
          extract_result_or_throw(foreign.try_admit_reconciliation_event(authoritative));
      REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
      admitted_ordinal = admitted.attempt_ordinal;
    } else {
      const auto admitted = extract_result_or_throw(foreign.try_admit_private(ordinary));
      REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
      admitted_ordinal = admitted.attempt_ordinal;
    }
    REQUIRE(foreign.bind_to_current_thread());
    CHECK_FALSE(foreign.execute_next_turn());
    CHECK(foreign.queue_snapshot().faulted);
    CHECK(fixture.owner().retained_identity_turn_count() == 0U);
    CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().trade_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 0U);
    CHECK(fixture.owner().find_retained_identity_turn(*admitted_ordinal, reconciliation) ==
          nullptr);
    CHECK(fixture.owner().account_safety_state(
              fixture.order_or_throw().provenance().logical_account_id) ==
          risk::AccountSafetyState::Synchronized);
    REQUIRE(foreign.release_from_current_thread());
  }
}

// --------------------------------------------------------
// A genuine event binds its concrete owner to one executor. A second executor's real account or
// global fence must fail before changing that owner's containment, retained facts, or preparations.
TEST_CASE("production identity owner rejects account and global fences from a second executor",
          "[runtime][m4][identity-retention-owner][authority]") {
  for (const bool global : {false, true}) {
    RetentionOwnerFixture fixture;
    const auto first = fixture.create_execution_attempt_or_throw(1U);
    const auto first_admitted = extract_result_or_throw(fixture.executor->try_admit_private(first));
    fixture.execute_pending_turns_or_throw();
    const auto& account = fixture.order_or_throw().provenance().logical_account_id;
    REQUIRE(fixture.owner().account_safety_state(account) ==
            risk::AccountSafetyState::ReconciliationRequired);
    const auto* const first_record =
        fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false);
    REQUIRE(first_record != nullptr);
    const auto prepared_before = first_record->preparation;

    model::DeterministicClockProvider clock{200U};
    ForeignAuthorityForwarder account_forwarder{
        *fixture.authority.submission->private_admission_owner(), true};
    auto& foreign_owner = global ? *fixture.authority.submission->private_admission_owner()
                                 : static_cast<runtime::PrivateAdmissionOwner&>(account_forwarder);
    runtime::SerializedExecutor foreign{
        0U, clock,
        extract_result_or_throw(
            runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                fixture.authority.configuration, fixture.authority.m4_policy)),
        foreign_owner};
    const auto attempted = global ? create_unattributable_attempt_or_throw(fixture)
                                  : fixture.create_execution_attempt_or_throw(2U);
    const auto admitted = extract_result_or_throw(foreign.try_admit_private(attempted));
    REQUIRE(foreign.bind_to_current_thread());
    if (global) {
      REQUIRE(admitted.outcome != runtime::AdmissionOutcome::Accepted);
    } else {
      REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
      const auto retained_turn = foreign.execute_next_turn();
      REQUIRE(retained_turn);
      REQUIRE(retained_turn.value());
      REQUIRE(retained_turn.value()->kind == runtime::TurnKind::PrivateCommand);
    }
    const auto turn = foreign.execute_next_turn();
    CHECK_FALSE(turn);
    if (!global) {
      CHECK(account_forwarder.has_forwarded_account_fence());
    }
    CHECK(foreign.queue_snapshot().faulted);
    CHECK(fixture.owner().account_safety_state(account) ==
          risk::AccountSafetyState::ReconciliationRequired);
    CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());
    CHECK(fixture.owner().retained_identity_turn_count() == 1U);
    CHECK(fixture.owner().identity_preparations().event_record_count() == 1U);
    CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
    CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 1U);
    CHECK(fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false) ==
          first_record);
    CHECK(first_record->preparation == prepared_before);
    REQUIRE(foreign.release_from_current_thread());
  }
}

// --------------------------------------------------------
// Public control-context values are observations, not mutation capabilities. Direct fabricated
// fence calls on this thread and another thread must neither mutate nor bind a pristine owner.
TEST_CASE("production identity owner rejects fabricated off-owner fence calls",
          "[runtime][m4][identity-retention-owner][authority]") {
  RetentionOwnerFixture fixture;
  const auto ordinary = fixture.create_execution_attempt_or_throw(1U);
  const auto unknown = create_unattributable_attempt_or_throw(fixture);
  const auto& provenance = fixture.order_or_throw().provenance();
  const auto ordinal = model::AdmissionOrdinal::create_initial();
  runtime::AccountSafetyFenceTurn account_fence{
      provenance.logical_account_id, provenance.venue_id, 1U, {}, 1U};
  account_fence.ordered_unique_reason_occurrences[0U].emplace(
      runtime::AccountSafetyReasonOccurrence{risk::AccountSafetyReason::CriticalAdmissionLoss,
                                             ordinary, ordinal});
  const runtime::GlobalPrivateFenceTurn global_fence{unknown, ordinal, 1U};
  const runtime::ControlTurnContext fabricated{model::TurnOrdinal::create_initial(),
                                               model::ProcessingTimestamp{100U}};
  auto& owner = *fixture.authority.submission->private_admission_owner();
  const auto direct_account = owner.apply_account_safety_fence(account_fence, fabricated);
  const auto direct_global = owner.apply_global_private_fence(global_fence, fabricated);
  REQUIRE_FALSE(direct_account);
  REQUIRE_FALSE(direct_global);
  CHECK(direct_account.error().code == model::DomainErrorCode::ExecutionNotPermitted);
  CHECK(direct_global.error().code == model::DomainErrorCode::ExecutionNotPermitted);

  std::optional<model::Result<void>> threaded_account;
  std::optional<model::Result<void>> threaded_global;
  std::thread stranger{[&] {
    threaded_account.emplace(owner.apply_account_safety_fence(account_fence, fabricated));
    threaded_global.emplace(owner.apply_global_private_fence(global_fence, fabricated));
  }};
  stranger.join();
  REQUIRE(threaded_account);
  REQUIRE(threaded_global);
  CHECK_FALSE(*threaded_account);
  CHECK_FALSE(*threaded_global);
  CHECK(fixture.owner().account_safety_state(provenance.logical_account_id) ==
        risk::AccountSafetyState::Synchronized);
  CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());
  CHECK(fixture.owner().retained_identity_turn_count() == 0U);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);

  const auto admitted = extract_result_or_throw(fixture.executor->try_admit_private(ordinary));
  fixture.execute_pending_turns_or_throw();
  CHECK(fixture.owner().find_retained_identity_turn(admitted.attempt_ordinal, false) != nullptr);
  CHECK(fixture.owner().account_safety_state(provenance.logical_account_id) ==
        risk::AccountSafetyState::ReconciliationRequired);
}

// --------------------------------------------------------
// The first legitimate fence establishes executor ownership even before any accepted event.
// A genuine unattributable source makes the first global fence runnable without an older accepted
// event. Another executor cannot then admit either token family into the already bound owner.
TEST_CASE("production identity owner binds the first fence before any private event",
          "[runtime][m4][identity-retention-owner][authority]") {
  for (const bool reconciliation : {false, true}) {
    RetentionOwnerFixture fixture;
    const auto ordinary = fixture.create_execution_attempt_or_throw(1U);
    const auto& account = fixture.order_or_throw().provenance().logical_account_id;
    const auto unknown = create_unattributable_attempt_or_throw(fixture);
    const auto loss = extract_result_or_throw(fixture.executor->try_admit_private(unknown));
    REQUIRE(loss.outcome != runtime::AdmissionOutcome::Accepted);
    REQUIRE(fixture.executor->bind_to_current_thread());
    const auto turn = fixture.executor->execute_next_turn();
    REQUIRE(turn);
    REQUIRE(turn.value());
    CHECK(turn.value()->kind == runtime::TurnKind::GlobalPrivateFence);
    CHECK_FALSE(fixture.executor->queue_snapshot().faulted);
    REQUIRE(fixture.executor->release_from_current_thread());
    REQUIRE(fixture.owner().account_safety_state(account) ==
            risk::AccountSafetyState::Synchronized);
    REQUIRE(fixture.owner().is_private_consumption_globally_blocked());
    CHECK(fixture.owner().retained_identity_turn_count() == 0U);

    model::DeterministicClockProvider clock{200U};
    runtime::SerializedExecutor foreign{
        0U, clock,
        extract_result_or_throw(
            runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                fixture.authority.configuration, fixture.authority.m4_policy)),
        *fixture.authority.submission->private_admission_owner()};
    if (reconciliation) {
      const auto authoritative = fixture.create_reconciliation_attempt_or_throw();
      const auto decision =
          extract_result_or_throw(foreign.try_admit_reconciliation_event(authoritative));
      REQUIRE(decision.outcome == runtime::AdmissionOutcome::Accepted);
    } else {
      const auto decision = extract_result_or_throw(foreign.try_admit_private(ordinary));
      REQUIRE(decision.outcome == runtime::AdmissionOutcome::Accepted);
    }
    REQUIRE(foreign.bind_to_current_thread());
    CHECK_FALSE(foreign.execute_next_turn());
    CHECK(foreign.queue_snapshot().faulted);
    CHECK(fixture.owner().account_safety_state(account) == risk::AccountSafetyState::Synchronized);
    CHECK(fixture.owner().is_private_consumption_globally_blocked());
    CHECK(fixture.owner().retained_identity_turn_count() == 0U);
    CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().trade_record_count() == 0U);
    CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 0U);
    REQUIRE(foreign.release_from_current_thread());
  }
}

// --------------------------------------------------------
// An executor's address is reusable storage, not its incarnation identity. After the first owner
// has retained a fact, replacement events and global fences must reject without aliasing restarted
// admission ordinals or changing the retained completion history. An account-loss fence requires
// an older accepted event, so the separate forwarding regression proves its authority boundary.
TEST_CASE("production identity owner rejects events and global fences from reused executor storage",
          "[runtime][m4][identity-retention-owner][authority][lifetime]") {
  for (const auto entry :
       {runtime::TurnKind::PrivateCommand, runtime::TurnKind::ReconciliationCommand,
        runtime::TurnKind::GlobalPrivateFence}) {
    RetentionOwnerFixture fixture;
    model::DeterministicClockProvider clock{200U};

    // ++++++++++++++++++++++++++++++++++++++++
    // Interesting syntax: aligned storage and construct_at start distinct executor lifetimes at
    // exactly the same address. The custom unique_ptr deleter ends each lifetime without freeing
    // stack storage, including when an assertion unwinds the fixture.
    alignas(runtime::SerializedExecutor) std::byte storage[sizeof(runtime::SerializedExecutor)];
    const auto create_executor_at_fixed_address_or_throw = [&] {
      return std::construct_at(
          reinterpret_cast<runtime::SerializedExecutor*>(storage), 0U, clock,
          extract_result_or_throw(
              runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
                  fixture.authority.configuration, fixture.authority.m4_policy)),
          *fixture.authority.submission->private_admission_owner());
    };
    const auto destroy_executor = [](runtime::SerializedExecutor* executor) {
      std::destroy_at(executor);
    };
    std::unique_ptr<runtime::SerializedExecutor, decltype(destroy_executor)> executor{
        create_executor_at_fixed_address_or_throw(), destroy_executor};

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish one complete fact and its error before destroying the first executor incarnation.
    const auto first = fixture.create_execution_attempt_or_throw(1U);
    const auto first_admitted = extract_result_or_throw(executor->try_admit_private(first));
    REQUIRE(executor->bind_to_current_thread());
    REQUIRE(executor->execute_pending_turns(4U));
    REQUIRE(executor->release_from_current_thread());
    REQUIRE(fixture.owner().retained_identity_turn_count() == 1U);
    const auto* const first_record =
        fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false);
    REQUIRE(first_record != nullptr);
    const auto preparation_before = first_record->preparation;
    const auto* const first_error =
        fixture.owner().find_committed_retained_private_event_error(first_admitted.attempt_ordinal);
    REQUIRE(first_error != nullptr);
    const auto* const first_address = executor.get();
    executor.reset();
    executor.reset(create_executor_at_fixed_address_or_throw());
    REQUIRE(executor.get() == first_address);

    // ++++++++++++++++++++++++++++++++++++++++
    // Each replacement obtains genuine executor authority; an unattributable source reaches the
    // real global-fence handler without first executing an accepted private event.
    if (entry == runtime::TurnKind::ReconciliationCommand) {
      const auto attempt = fixture.create_reconciliation_attempt_or_throw();
      const auto admitted =
          extract_result_or_throw(executor->try_admit_reconciliation_event(attempt));
      REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
    } else if (entry == runtime::TurnKind::GlobalPrivateFence) {
      const auto attempt = create_unattributable_attempt_or_throw(fixture);
      const auto admitted = extract_result_or_throw(executor->try_admit_private(attempt));
      REQUIRE(admitted.outcome != runtime::AdmissionOutcome::Accepted);
    } else {
      const auto attempt = fixture.create_execution_attempt_or_throw(2U, 0x72U);
      const auto admitted = extract_result_or_throw(executor->try_admit_private(attempt));
      REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
    }
    REQUIRE(executor->bind_to_current_thread());
    const auto rejected_turn = executor->execute_next_turn();
    REQUIRE(executor->release_from_current_thread());
    CHECK_FALSE(rejected_turn);
    CHECK(executor->queue_snapshot().faulted);

    // ++++++++++++++++++++++++++++++++++++++++
    // Restarted ordinals cannot rewrite or extend the first incarnation's immutable history.
    CHECK(fixture.owner().retained_identity_turn_count() == 1U);
    CHECK(fixture.owner().identity_preparations().event_record_count() == 1U);
    CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
    CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 1U);
    CHECK(fixture.owner().find_retained_identity_turn(first_admitted.attempt_ordinal, false) ==
          first_record);
    CHECK(first_record->preparation == preparation_before);
    CHECK(fixture.owner().find_committed_retained_private_event_error(
              first_admitted.attempt_ordinal) == first_error);
    CHECK(fixture.owner().account_safety_state(
              fixture.order_or_throw().provenance().logical_account_id) ==
          risk::AccountSafetyState::ReconciliationRequired);
    CHECK_FALSE(fixture.owner().is_private_consumption_globally_blocked());

    // ++++++++++++++++++++++++++++++++++++++++
  }
}

// --------------------------------------------------------
// Valid local observations have no authoritative identity plan in this slice. They retain their
// complete origin and pending-reducer obligation without being called malformed or consumed.
TEST_CASE("production identity owner retains valid local observations for the pending reducer",
          "[runtime][m4][identity-retention-owner]") {
  RetentionOwnerFixture fixture;
  const auto& provenance = fixture.order_or_throw().provenance();
  const auto timeout = extract_result_or_throw(fixture.factory.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                     model::SourceTimestamp{100U}},
      provenance.logical_account_id, provenance.venue_id));
  const auto admitted = extract_result_or_throw(fixture.executor->try_admit_private(timeout));
  REQUIRE(admitted.outcome == runtime::AdmissionOutcome::Accepted);
  fixture.execute_pending_turns_or_throw();
  const auto* retained =
      fixture.owner().find_retained_identity_turn(admitted.attempt_ordinal, false);
  REQUIRE(retained != nullptr);
  REQUIRE(retained->attempt);
  REQUIRE(retained->normalized);
  CHECK(std::get<oms::PrivateOrderIngressAttempt>(*retained->attempt) == timeout);
  CHECK(retained->error_kind == runtime::PrivateIdentityRetentionError::PendingReducer);
  CHECK_FALSE(retained->preparation);
  CHECK(retained->safety_reason == risk::AccountSafetyReason::IncompleteReconciliation);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 0U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 0U);
  CHECK(fixture.owner().identity_preparations().mapping_candidate_count() == 0U);
  CHECK(fixture.owner().account_safety_state(provenance.logical_account_id) ==
        risk::AccountSafetyState::ReconciliationRequired);
  const auto observed = fixture.executor->private_admission_observation(admitted.attempt_ordinal);
  REQUIRE(observed);
  CHECK(observed->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(observed->disposition);
  REQUIRE(observed->retention_error);
  CHECK(observed->retention_error->code == model::DomainErrorCode::ReconciliationIncomplete);
}

// --------------------------------------------------------
// Concurrent readers acquire immutable lane-specific records while the serialized owner appends
// later facts. The first published pointer and its allocated error text remain unchanged forever.
TEST_CASE("production identity owner publishes stable retained pointers to concurrent readers",
          "[runtime][m4][identity-retention-owner][concurrency]") {
  RetentionOwnerFixture fixture;
  std::vector<model::AdmissionOrdinal> admitted;
  for (std::uint8_t index = 1U; index <= 24U; ++index) {
    if (index % 2U == 0U) {
      const auto attempt = fixture.create_reconciliation_attempt_or_throw(index);
      const auto decision =
          extract_result_or_throw(fixture.executor->try_admit_reconciliation_event(attempt));
      REQUIRE(decision.outcome == runtime::AdmissionOutcome::Accepted);
      admitted.push_back(decision.attempt_ordinal);
    } else {
      const auto attempt = fixture.create_execution_attempt_or_throw(index);
      const auto decision = extract_result_or_throw(fixture.executor->try_admit_private(attempt));
      REQUIRE(decision.outcome == runtime::AdmissionOutcome::Accepted);
      admitted.push_back(decision.attempt_ordinal);
    }
  }
  std::atomic<bool> ready{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> observations_valid{true};
  std::atomic<std::uint64_t> observation_count{0U};
  const runtime::RetainedPrivateIdentityTurn* first_pointer = nullptr;
  const model::DomainError* first_error = nullptr;
  std::thread reader{[&] {
    ready.store(true, std::memory_order_release);
    for (;;) {
      const bool final_pass = finished.load(std::memory_order_acquire);
      for (std::size_t index = 0U; index < admitted.size(); ++index) {
        const bool reconciliation = (index + 1U) % 2U == 0U;
        const auto ordinal = admitted[index];
        const auto* record = fixture.owner().find_retained_identity_turn(ordinal, reconciliation);
        const auto* wrong_lane =
            fixture.owner().find_retained_identity_turn(ordinal, !reconciliation);
        if (wrong_lane != nullptr) {
          observations_valid.store(false, std::memory_order_relaxed);
        }
        if (record == nullptr) {
          continue;
        }
        const auto* error =
            reconciliation
                ? fixture.owner().find_committed_retained_reconciliation_event_error(ordinal)
                : fixture.owner().find_committed_retained_private_event_error(ordinal);
        if (!record->receipt || record->receipt->attempt_ordinal != ordinal || !record->attempt ||
            !record->preparation || error == nullptr ||
            error->code != model::DomainErrorCode::ReconciliationIncomplete ||
            error->context.field != "private_identity.pending_business_reducer") {
          observations_valid.store(false, std::memory_order_relaxed);
        }
        if (index == 0U) {
          if (first_pointer != nullptr && (record != first_pointer || error != first_error)) {
            observations_valid.store(false, std::memory_order_relaxed);
          }
          first_pointer = record;
          first_error = error;
        }
        observation_count.fetch_add(1U, std::memory_order_relaxed);
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
    fixture.execute_pending_turns_or_throw();
  } catch (...) {
    finished.store(true, std::memory_order_release);
    reader.join();
    throw;
  }
  finished.store(true, std::memory_order_release);
  reader.join();
  CHECK(observations_valid.load(std::memory_order_relaxed));
  CHECK(observation_count.load(std::memory_order_relaxed) > 0U);
  REQUIRE(first_pointer != nullptr);
  REQUIRE(first_error != nullptr);
  CHECK(first_pointer == fixture.owner().find_retained_identity_turn(admitted[0U], false));
  CHECK(first_error == fixture.owner().find_committed_retained_private_event_error(admitted[0U]));
  CHECK(fixture.owner().retained_identity_turn_count() == 24U);
  CHECK(fixture.owner().identity_preparations().event_record_count() == 24U);
  CHECK(fixture.owner().identity_preparations().trade_record_count() == 1U);
  CHECK(fixture.owner().event_identity_record_count() == 0U);
  CHECK(fixture.owner().trade_identity_record_count() == 0U);
  CHECK(fixture.owner().exchange_order_mapping_count() == 0U);
}

// --------------------------------------------------------

} // namespace
