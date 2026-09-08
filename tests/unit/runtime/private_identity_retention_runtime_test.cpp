// Purpose: prove opt-in MarketRuntime ownership, bounded private retention, bootstrap lease
// lifetime, and detached evidence without claiming economic application or journal durability.

#include "aegis/runtime/market_runtime.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace aegis;
using test_support::create_m4_opaque_identity_or_throw;
using test_support::parse_m4_identifier_or_throw;

// ########################################################################
// Ordinary ingress requires a persistent source value; authoritative reconciliation cannot enter
// the runtime through an ordinary source or a public reconciliation producer.
template <typename Runtime, typename Attempt>
concept AdmitsPrivateLvalue = requires(Runtime& runtime, const Attempt& attempt) {
  {
    runtime.try_admit_private(attempt)
  } -> std::same_as<model::Result<runtime::PrivateAdmissionDecision>>;
};

template <typename Runtime, typename Attempt>
concept AdmitsPrivateRvalue =
    requires(Runtime& runtime, Attempt attempt) { runtime.try_admit_private(std::move(attempt)); };

template <typename Runtime>
concept HasPublicReconciliationAdmission =
    requires(Runtime& runtime, const oms::ReconciliationPrivateEventIngressAttempt& attempt) {
      runtime.try_admit_reconciliation_event(attempt);
    };

static_assert(AdmitsPrivateLvalue<runtime::MarketRuntime, oms::PrivateOrderIngressAttempt>);
static_assert(!AdmitsPrivateRvalue<runtime::MarketRuntime, oms::PrivateOrderIngressAttempt>);
static_assert(!AdmitsPrivateRvalue<runtime::MarketRuntime, const oms::PrivateOrderIngressAttempt>);
static_assert(
    !AdmitsPrivateLvalue<runtime::MarketRuntime, oms::ReconciliationPrivateEventIngressAttempt>);
static_assert(!HasPublicReconciliationAdmission<runtime::MarketRuntime>);

// ########################################################################
// A route-owner strategy performs one genuine bootstrap submission; peers remain inert and the
// borrowed result slot outlives every callback and the runtime.
class RetentionBootstrapStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Retain a nullable result destination so only the configured route owner submits once.
  explicit RetentionBootstrapStrategy(std::optional<execution::SubmitResult>* result)
      : result_{result}, request_{test_support::create_m4_reference_order_request_or_throw()} {}

  // --------------------------------------------------------
  // Observe market updates without introducing another submission stimulus.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
  // Submit at most once under the genuine callback authority created after recovery installation.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext& context) noexcept override {
    if (result_ != nullptr && !result_->has_value()) {
      result_->emplace(context.submit_order(request_));
    }
  }

  // --------------------------------------------------------
private:
  std::optional<execution::SubmitResult>* result_;
  execution::OrderRequest request_;
};

// ########################################################################

// --------------------------------------------------------
// Recreate the exact M3 capacities used by the sealed owner fixture while keeping all fake inputs
// deterministic and credential-free; invalid fixture values throw before runtime construction.
[[nodiscard]] runtime::FakeSubmissionRuntimeParams create_retention_submission_params_or_throw(
    const configuration::StartupConfiguration& configuration) {
  auto encoder = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, 10U, {});
  auto initiator = execution::FakeInitiatorScript::create_fake_initiator_script(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, 10U, {});
  model::OrderNamespace::Bytes bytes{};
  bytes.fill(0x42U);
  auto order_ids = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{bytes});
  if (!encoder || !initiator || !order_ids) {
    throw std::logic_error{"invalid private-retention fake fixture"};
  }
  return runtime::FakeSubmissionRuntimeParams{
      test_support::create_m3_reference_risk_policy_params_or_throw(configuration),
      execution::SubmissionPolicyCapacities{10U, 4U, 4U, 1'024U, 2U, 110U, 8U},
      std::move(encoder).value(),
      std::move(initiator).value(),
      std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
          std::vector<std::optional<std::uint64_t>>{10'000U, 10'001U}),
      model::DeterministicOrderIdSource{std::move(order_ids).value()}};
}

// --------------------------------------------------------
// Cover every configured bot while selecting only the reference route owner for submission.
[[nodiscard]] std::vector<runtime::BotStrategyRegistration>
create_retention_strategies(const configuration::StartupConfiguration& configuration,
                            std::optional<execution::SubmitResult>& submitted) {
  std::vector<runtime::BotStrategyRegistration> strategies;
  for (const auto& attribution : configuration.organization().bot_attributions()) {
    auto* result =
        attribution.bot_id.value() == "bot.deribit-btc-perpetual-reference" ? &submitted : nullptr;
    strategies.push_back(runtime::BotStrategyRegistration{
        attribution.bot_id, std::make_unique<RetentionBootstrapStrategy>(result)});
  }
  return strategies;
}

// --------------------------------------------------------
// Normalize an acknowledgement from exact sealed authority while preserving its raw client
// locator; the trusted factory cannot infer order ownership from that supplied locator.
[[nodiscard]] oms::PrivateOrderIngressAttempt create_retention_acknowledgement_or_throw(
    const test_support::M4OwnerTestAuthority& authority,
    std::optional<model::OrderId> local_locator = std::nullopt,
    const runtime::M4Policy* source_policy = nullptr) {
  auto resolver = runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
      authority.configuration, source_policy != nullptr ? *source_policy : authority.m4_policy);
  if (!resolver) {
    throw std::logic_error{"invalid private-retention provenance"};
  }
  runtime::PrivateOrderEventFactory factory{std::move(resolver).value()};
  auto attempt = factory.create_venue_acknowledgement_attempt(
      oms::VenuePrivateIngressOrigin{
          oms::VenuePrivateEventKey{
              parse_m4_identifier_or_throw<model::VenueId>("deribit"),
              parse_m4_identifier_or_throw<model::LogicalAccountId>(
                  "account.deribit-testnet-aegis"),
              create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x51U),
              create_m4_opaque_identity_or_throw<oms::PrivateEventId>(0x61U)},
          model::SourceTimestamp{10U}},
      create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x71U), local_locator);
  if (!attempt) {
    throw std::logic_error{"invalid private-retention acknowledgement"};
  }
  return std::move(attempt).value();
}

// --------------------------------------------------------
// Construct the opt-in production composition while preserving all borrowed clocks and results.
[[nodiscard]] std::unique_ptr<runtime::MarketRuntime> create_retention_runtime_or_throw(
    const test_support::M4OwnerTestAuthority& authority, recovery::RecoveryBootstrap&& bootstrap,
    model::ClockProvider& executor_clock, model::ClockProvider& callback_clock,
    std::optional<execution::SubmitResult>& submitted) {
  auto created = runtime::MarketRuntime::create_with_fake_private_identity_retention(
      authority.configuration, authority.runtime_policy, executor_clock, callback_clock,
      create_retention_strategies(authority.configuration, submitted),
      create_retention_submission_params_or_throw(authority.configuration), authority.m4_policy,
      std::move(bootstrap));
  if (!created) {
    throw std::logic_error{"invalid private-retention runtime: " + created.error().context.field};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// The opt-in root sizes both private lanes and installs the recovered client namespace before its
// first callback can enter M3 submission; cold evidence owns every copied installation identity.
TEST_CASE("private identity runtime installs bounded owner before callback authority",
          "[runtime][m4][private-identity-retention-runtime]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto expected_namespace = recovery.bootstrap.registered_order_namespace();
  const auto expected_epoch = recovery.bootstrap.runtime_epoch_id();
  const auto expected_lineage = recovery.bootstrap.lineage_id();
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  std::optional<execution::SubmitResult> submitted;
  auto runtime = create_retention_runtime_or_throw(authority, std::move(recovery.bootstrap),
                                                   executor_clock, callback_clock, submitted);
  const auto initial = runtime->status();
  CHECK(initial.executor.private_lane.private_capacity == 32U);
  CHECK(initial.executor.private_lane.reconciliation_capacity == 32U);
  CHECK(initial.executor.private_lane.account_fence_capacity ==
        authority.configuration.logical_accounts().size());
  CHECK_FALSE(recovery.medium->registered_namespace_count());

  const auto attempt = create_retention_acknowledgement_or_throw(authority);
  const auto early = runtime->try_admit_private(attempt);
  REQUIRE_FALSE(early);
  CHECK(early.error().code == model::DomainErrorCode::ExecutionNotPermitted);
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  REQUIRE(submitted.has_value());
  REQUIRE(submitted->order_id().has_value());
  CHECK(std::equal(expected_namespace.bytes().begin(), expected_namespace.bytes().end(),
                   submitted->order_id()->bytes().begin()));

  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  const auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().private_identity_retention.has_value());
  const auto detached = *evidence.value().private_identity_retention;
  CHECK(detached.policy_fingerprint == authority.m4_policy.fingerprint());
  CHECK(detached.registered_order_namespace == expected_namespace);
  CHECK(detached.runtime_epoch_id == expected_epoch);
  CHECK(detached.recovery_lineage_id == expected_lineage);
  CHECK(detached.event_identity_record_capacity == 32U);
  CHECK(detached.trade_identity_record_capacity == 32U);
  CHECK(detached.exchange_order_mapping_capacity == 32U);
  CHECK(detached.event_identity_record_count == 0U);
  CHECK(detached.trade_identity_record_count == 0U);
  CHECK(detached.exchange_order_mapping_count == 0U);
  CHECK(detached.prepared_event_identity_record_count == 0U);
  CHECK(detached.prepared_trade_identity_record_count == 0U);
  CHECK(detached.prepared_exchange_order_mapping_candidate_count == 0U);
  CHECK(detached.retained_identity_turn_count == 0U);
  runtime.reset();
  REQUIRE(recovery.medium->registered_namespace_count());
  CHECK(recovery.medium->registered_namespace_count().value() == 1U);
  CHECK(detached.registered_order_namespace == expected_namespace);
}

// --------------------------------------------------------
// Pending private work prevents cold evidence even after closure and owner release. The genuine
// owner then retains the accepted fact without claiming a lifecycle or economic transition.
TEST_CASE("private identity runtime retains admitted source facts behind quiescence gates",
          "[runtime][m4][private-identity-retention-runtime]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  std::optional<execution::SubmitResult> submitted;
  auto runtime = create_retention_runtime_or_throw(authority, std::move(recovery.bootstrap),
                                                   executor_clock, callback_clock, submitted);
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  REQUIRE(submitted.has_value());
  const auto attempt = create_retention_acknowledgement_or_throw(authority, submitted->order_id());
  const auto admitted = runtime->try_admit_private(attempt);
  REQUIRE(admitted);
  CHECK(admitted.value().outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(admitted.value().receipt.has_value());
  const auto ordinal = admitted.value().attempt_ordinal;
  const auto copied = runtime->private_admission_observation(ordinal);
  REQUIRE(copied.has_value());
  CHECK(copied->state == runtime::CriticalPrivateAdmissionState::CopiedAndAdmitted);

  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  const auto pending_evidence = runtime->collect_quiescent_evidence();
  REQUIRE_FALSE(pending_evidence);
  CHECK(pending_evidence.error().context.field == "market_runtime.evidence_quiescence");
  REQUIRE(runtime->bind_to_current_thread());
  const auto consumed_turn = runtime->execute_next_turn();
  REQUIRE(consumed_turn);
  REQUIRE(consumed_turn.value().has_value());
  CHECK(consumed_turn.value()->kind == runtime::TurnKind::PrivateCommand);
  const auto retained = runtime->private_admission_observation(ordinal);
  REQUIRE(retained.has_value());
  CHECK(retained->state == runtime::CriticalPrivateAdmissionState::RetainedForReconciliation);
  CHECK_FALSE(retained->disposition.has_value());
  CHECK(retained->retention_error.has_value());
  const auto fence_turn = runtime->execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::AccountSafetyFence);
  REQUIRE(runtime->release_from_current_thread());
  const auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().submission.has_value());
  REQUIRE(evidence.value().submission->oms_orders.size() == 1U);
  CHECK(evidence.value().submission->oms_orders.front().state ==
        oms::OutboundOrderState::WriteInitiated);
  CHECK(evidence.value().submission->held_reservation_count == 1U);
  REQUIRE(evidence.value().private_identity_retention.has_value());
  CHECK(evidence.value().private_identity_retention->event_identity_record_count == 0U);
  CHECK(evidence.value().private_identity_retention->trade_identity_record_count == 0U);
  CHECK(evidence.value().private_identity_retention->exchange_order_mapping_count == 0U);
  CHECK(evidence.value().private_identity_retention->prepared_event_identity_record_count == 1U);
  CHECK(evidence.value().private_identity_retention->prepared_trade_identity_record_count == 0U);
  CHECK(evidence.value()
            .private_identity_retention->prepared_exchange_order_mapping_candidate_count == 1U);
  CHECK(evidence.value().private_identity_retention->retained_identity_turn_count == 1U);
}

// --------------------------------------------------------
// A provenance mismatch is detected before bootstrap transfer, allowing the same acknowledged
// incarnation to be installed after its exact policy is restored.
TEST_CASE("private identity runtime rejects incompatible bootstrap without consuming it",
          "[runtime][m4][private-identity-retention-runtime]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  auto capacities = authority.m4_policy.capacities();
  ++capacities.max_event_identity_records;
  const auto mismatched_policy = runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities);
  REQUIRE(mismatched_policy);
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  std::optional<execution::SubmitResult> submitted;
  const auto rejected = runtime::MarketRuntime::create_with_fake_private_identity_retention(
      authority.configuration, authority.runtime_policy, executor_clock, callback_clock,
      create_retention_strategies(authority.configuration, submitted),
      create_retention_submission_params_or_throw(authority.configuration),
      mismatched_policy.value(), std::move(recovery.bootstrap));
  REQUIRE_FALSE(rejected);
  CHECK_FALSE(submitted.has_value());
  auto runtime = create_retention_runtime_or_throw(authority, std::move(recovery.bootstrap),
                                                   executor_clock, callback_clock, submitted);
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  REQUIRE(submitted.has_value());
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  REQUIRE(runtime->collect_quiescent_evidence());
}

// --------------------------------------------------------
// The retained owner remains opt-in: unchanged M3 composition has no private reserve and rejects
// private ingress without faulting or changing its successful submission evidence.
TEST_CASE("ordinary fake submission runtime has no private identity owner",
          "[runtime][m4][private-identity-retention-runtime]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  std::optional<execution::SubmitResult> submitted;
  auto created = runtime::MarketRuntime::create_with_fake_submission(
      authority.configuration, authority.runtime_policy, executor_clock, callback_clock,
      create_retention_strategies(authority.configuration, submitted),
      create_retention_submission_params_or_throw(authority.configuration));
  REQUIRE(created);
  auto runtime = std::move(created).value();
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  const auto attempt = create_retention_acknowledgement_or_throw(authority);
  const auto rejected = runtime->try_admit_private(attempt);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::ExecutionNotPermitted);
  CHECK(runtime->status().lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(runtime->status().executor.private_lane.private_capacity == 0U);
  CHECK(runtime->status().executor.private_lane.reconciliation_capacity == 0U);
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  const auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().submission.has_value());
  CHECK_FALSE(evidence.value().private_identity_retention.has_value());
}

// --------------------------------------------------------
// A mismatched source root selects a reasonless global fence before private consumption. Two fresh
// incarnations with the same explicit namespace and clocks produce identical detached evidence.
TEST_CASE("private identity runtime globally contains incompatible sources deterministically",
          "[runtime][m4][private-identity-retention-runtime]") {
  std::optional<runtime::MarketRuntimeEvidence> previous_evidence;
  for (std::uint32_t repetition = 0U; repetition < 2U; ++repetition) {
    auto authority = test_support::create_m4_owner_test_authority_or_throw();
    auto recovery =
        test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
    auto capacities = authority.m4_policy.capacities();
    ++capacities.max_event_identity_records;
    const auto source_policy = runtime::M4Policy::create_m4_policy(
        authority.configuration, authority.runtime_policy,
        authority.submission->reservations().policy(), authority.submission->policy(), capacities);
    REQUIRE(source_policy);
    model::DeterministicClockProvider executor_clock{100U};
    model::DeterministicClockProvider callback_clock{1'000U};
    std::optional<execution::SubmitResult> submitted;
    auto runtime = create_retention_runtime_or_throw(authority, std::move(recovery.bootstrap),
                                                     executor_clock, callback_clock, submitted);
    REQUIRE(runtime->bind_to_current_thread());
    REQUIRE(runtime->execute_next_turn());
    const auto attempt =
        create_retention_acknowledgement_or_throw(authority, std::nullopt, &source_policy.value());
    const auto rejected = runtime->try_admit_private(attempt);
    REQUIRE(rejected);
    CHECK(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
    CHECK_FALSE(rejected.value().receipt.has_value());
    CHECK_FALSE(rejected.value().account_fence_recorded);
    CHECK(runtime->status().executor.private_lane.global_fence_active);
    CHECK_FALSE(runtime->private_admission_observation(rejected.value().attempt_ordinal));
    const auto fence = runtime->execute_next_turn();
    REQUIRE(fence);
    REQUIRE(fence.value().has_value());
    CHECK(fence.value()->kind == runtime::TurnKind::GlobalPrivateFence);
    CHECK(runtime->status().executor.private_lane.global_fence_owner_applied);
    runtime->close();
    REQUIRE(runtime->release_from_current_thread());
    auto evidence = runtime->collect_quiescent_evidence();
    REQUIRE(evidence);
    REQUIRE(evidence.value().private_identity_retention.has_value());
    CHECK(evidence.value().private_identity_retention->retained_identity_turn_count == 0U);
    CHECK(evidence.value().private_identity_retention->prepared_event_identity_record_count == 0U);
    if (previous_evidence) {
      CHECK(evidence.value() == *previous_evidence);
    }
    previous_evidence.emplace(std::move(evidence).value());
  }
}

// --------------------------------------------------------
