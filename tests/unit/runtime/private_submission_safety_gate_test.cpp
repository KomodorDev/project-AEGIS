// Purpose: prove installed M4 account/global containment rejects genuine bot submissions after
// identity generation but before reservation, while preserving peer-account and M3 behavior.

#include "aegis/runtime/private_order_reconciler.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace aegis;
using test_support::create_m4_decimal_or_throw;
using test_support::create_m4_opaque_identity_or_throw;
using test_support::parse_m4_identifier_or_throw;

// --------------------------------------------------------
// Build fixed fake submission resources, optionally losing only the first accepted write outcome.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
create_gate_coordinator_or_throw(const configuration::StartupConfiguration& configuration,
                                 const runtime::RuntimePolicy& runtime_policy,
                                 bool first_write_is_ambiguous);

// --------------------------------------------------------
// Give both configured account owners a real callback, plus two independent routes sharing the
// primary account, so account-wide blocking can be distinguished from route-specific blocking.
[[nodiscard]] test_support::M4OwnerTestAuthority
create_safety_gate_authority_or_throw(bool first_write_is_ambiguous = false) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Observe both independently configured account owners through their real strategy callbacks.
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  auto peer_subscription = params.subscriptions.front();
  peer_subscription.id = parse_m4_identifier_or_throw<model::SubscriptionId>("subscription.peer");
  peer_subscription.bot_id = parse_m4_identifier_or_throw<model::BotId>("bot.subsidiary-reference");
  params.subscriptions.push_back(std::move(peer_subscription));

  // ++++++++++++++++++++++++++++++++++++++++
  // Route semantic keys forbid aliases for one bot, so independent bots in the same desk prove
  // that the logical account, rather than caller identity, owns the shared safety gate.
  auto shared_bot = params.bots.front();
  shared_bot.id = parse_m4_identifier_or_throw<model::BotId>("bot.shared-account");
  params.bots.push_back(shared_bot);
  auto shared_settings = params.strategy_settings.front();
  shared_settings.bot_id = shared_bot.id;
  params.strategy_settings.push_back(shared_settings);
  auto shared_subscription = params.subscriptions.front();
  shared_subscription.id =
      parse_m4_identifier_or_throw<model::SubscriptionId>("subscription.shared-account");
  shared_subscription.bot_id = shared_bot.id;
  params.subscriptions.push_back(shared_subscription);
  auto shared_route = params.routes.front();
  shared_route.id = parse_m4_identifier_or_throw<model::RouteId>("route.shared-account");
  shared_route.bot_id = shared_bot.id;
  params.routes.push_back(shared_route);
  shared_bot.id = parse_m4_identifier_or_throw<model::BotId>("bot.disabled");
  params.bots.push_back(shared_bot);
  shared_settings.bot_id = shared_bot.id;
  params.strategy_settings.push_back(shared_settings);
  shared_subscription.id =
      parse_m4_identifier_or_throw<model::SubscriptionId>("subscription.disabled");
  shared_subscription.bot_id = shared_bot.id;
  params.subscriptions.push_back(shared_subscription);
  shared_route.id = parse_m4_identifier_or_throw<model::RouteId>("route.disabled");
  shared_route.bot_id = shared_bot.id;
  shared_route.state = execution::ExecutionRouteState::Disabled;
  params.routes.push_back(std::move(shared_route));

  // ++++++++++++++++++++++++++++++++++++++++
  // Four observation grants require eight callback slots under the two-event M2 turn bound.
  auto configuration =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configuration) {
    throw std::logic_error{"invalid safety-gate startup configuration"};
  }
  auto runtime_policy = runtime::RuntimePolicy::create_runtime_policy(
      configuration.value(),
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 8U, 64U, 256U, 32U,
                                       100'000U},
          {{parse_m4_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_m4_identifier_or_throw<model::VenueId>("deribit"),
            parse_m4_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_m4_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::create_initial()}}});
  if (!runtime_policy) {
    throw std::logic_error{"invalid safety-gate runtime policy"};
  }
  auto coordinator = create_gate_coordinator_or_throw(configuration.value(), runtime_policy.value(),
                                                      first_write_is_ambiguous);
  auto m4_policy = runtime::M4Policy::create_m4_policy(
      configuration.value(), runtime_policy.value(), coordinator->reservations().policy(),
      coordinator->policy(), test_support::create_ordinary_m4_policy_capacities());
  if (!m4_policy) {
    throw std::logic_error{"invalid safety-gate M4 policy"};
  }
  return test_support::M4OwnerTestAuthority{std::move(configuration).value(),
                                            std::move(runtime_policy).value(),
                                            std::move(coordinator),
                                            std::move(m4_policy).value(),
                                            std::nullopt,
                                            0U,
                                            model::TurnOrdinal::create_initial(),
                                            1'234'567U};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Build fixed fake submission resources, optionally losing only the first accepted write outcome.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
create_gate_coordinator_or_throw(const configuration::StartupConfiguration& configuration,
                                 const runtime::RuntimePolicy& runtime_policy,
                                 bool first_write_is_ambiguous) {
  const execution::SubmissionPolicyCapacities capacities{10U, 4U, 4U, 1'024U, 4U, 110U, 8U};
  const auto maximum_attempts = capacities.maximum_submission_attempts;
  auto encoder = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, maximum_attempts, {});
  std::vector<execution::FakeInitiationOverride> overrides;
  if (first_write_is_ambiguous) {
    overrides.push_back({1U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost});
  }
  auto initiator = execution::FakeInitiatorScript::create_fake_initiator_script(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts,
      std::move(overrides));
  auto ids = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      test_support::create_m4_restart_namespace());
  if (!encoder || !initiator || !ids) {
    throw std::logic_error{"invalid ambiguous-write safety fixture"};
  }
  std::vector<std::optional<std::uint64_t>> readings;
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    readings.emplace_back(10'000U + index);
  }
  auto coordinator = runtime::SubmissionCoordinator::create_submission_coordinator(
      configuration, runtime_policy,
      runtime::FakeSubmissionRuntimeParams{
          test_support::create_m3_reference_risk_policy_params_or_throw(configuration), capacities,
          std::move(encoder).value(), std::move(initiator).value(),
          std::make_unique<execution::DeterministicSubmissionMeasurementClock>(std::move(readings)),
          model::DeterministicOrderIdSource{std::move(ids).value()}});
  if (!coordinator) {
    throw std::logic_error{"invalid ambiguous-write coordinator"};
  }
  return std::move(coordinator).value();
}

// --------------------------------------------------------
// Produce a complete unknown acknowledgement for the requested account without inventing a local
// locator; foreign accounts retain source evidence and exercise the global admission fence.
[[nodiscard]] oms::PrivateOrderIngressAttempt
create_unknown_acknowledgement_or_throw(const test_support::M4OwnerTestAuthority& authority,
                                        model::LogicalAccountId account, std::uint8_t event) {
  auto resolver = runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
      authority.configuration, authority.m4_policy);
  if (!resolver) {
    throw std::logic_error{"invalid safety-gate source provenance"};
  }
  runtime::PrivateOrderEventFactory factory{std::move(resolver).value()};
  auto attempt = factory.create_venue_acknowledgement_attempt(
      oms::VenuePrivateIngressOrigin{
          oms::VenuePrivateEventKey{
              parse_m4_identifier_or_throw<model::VenueId>("deribit"), std::move(account),
              create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(1U),
              create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
          model::SourceTimestamp{10U}},
      create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(event), std::nullopt);
  if (!attempt) {
    throw std::logic_error{"invalid safety-gate acknowledgement"};
  }
  return std::move(attempt).value();
}

// --------------------------------------------------------
// Match the installed owner's exact policy and configured account bindings before building its
// executor; fixture defects throw before any private input or callback can be processed.
[[nodiscard]] runtime::PrivateAdmissionConfiguration
create_admission_configuration_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  auto created = runtime::PrivateAdmissionConfiguration::create_private_admission_configuration(
      authority.configuration, authority.m4_policy);
  if (!created) {
    throw std::logic_error{"invalid safety-gate admission configuration"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Assert the complete terminal risk rejection and its exact six-record evidence prefix. These
// checks prove that no numeric risk limit or reservation was invented to explain account safety.
void check_account_safety_rejection(const runtime::SubmissionCoordinator& owner,
                                    const execution::SubmitResult& result,
                                    execution::SubmissionReason reason) {
  CHECK(result.disposition() == execution::SubmitDisposition::LocallyRejected);
  CHECK(result.stage() == execution::SubmissionStage::Risk);
  CHECK(result.reason() == reason);
  REQUIRE(result.order_id().has_value());
  REQUIRE(result.attempt_id().has_value());
  CHECK_FALSE(result.risk_evidence().has_value());
  const auto records = owner.trace_sink().records();
  REQUIRE(records.size() >= 6U);
  const auto offset = records.size() - 6U;
  CHECK(records[offset].kind() == trace::SubmissionTraceEventKind::Attempt);
  CHECK(records[offset + 1U].kind() == trace::SubmissionTraceEventKind::RouteAuthorized);
  CHECK(records[offset + 2U].kind() == trace::SubmissionTraceEventKind::CanonicalValidated);
  CHECK(records[offset + 3U].kind() == trace::SubmissionTraceEventKind::IdentityGenerated);
  CHECK(records[offset + 4U].kind() == trace::SubmissionTraceEventKind::RiskRejected);
  CHECK(records[offset + 5U].kind() == trace::SubmissionTraceEventKind::SubmissionCompleted);
  CHECK_FALSE(records.back().fields().reservation_id.has_value());
  CHECK_FALSE(records.back().fields().risk_rejection.has_value());
  CHECK_FALSE(owner.is_runtime_faulted());
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// A producer loss reaches the real owner before later callbacks, blocks every route on its account,
// and cannot consume reservation/OMS/encoder capacity or disable an unrelated configured account.
TEST_CASE("M4 account fence gates every shared-account route before reservation",
          "[runtime][m4][submission-safety]") {
  auto authority = create_safety_gate_authority_or_throw();
  auto capacities = authority.m4_policy.capacities();
  capacities.max_private_admissions = 1U;
  auto policy = runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities);
  REQUIRE(policy);
  authority.m4_policy = std::move(policy).value();
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  auto request = test_support::create_m4_reference_order_request_or_throw();
  const auto account =
      authority.configuration.routes().find_route(request.route_id)->logical_account_id;

  // ++++++++++++++++++++++++++++++++++++++++
  // Saturate only the private reserve; the second loss is a real coordinator-issued fence.
  model::DeterministicClockProvider clock{20U};
  runtime::SerializedExecutor executor{1U, clock,
                                       create_admission_configuration_or_throw(authority),
                                       *authority.submission->private_admission_owner()};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto first_attempt = create_unknown_acknowledgement_or_throw(authority, account, 1U);
  const auto lost_attempt = create_unknown_acknowledgement_or_throw(authority, account, 2U);
  const auto first = executor.try_admit_private(first_attempt);
  const auto lost = executor.try_admit_private(lost_attempt);
  REQUIRE(first);
  REQUIRE(lost);
  CHECK(first.value().outcome == runtime::AdmissionOutcome::Accepted);
  CHECK(lost.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(driver.execute_pending_turns(4U));
  REQUIRE(driver.release_from_current_thread());
  CHECK(authority.submission->private_order_reconciler()->account_safety_state(account) ==
        risk::AccountSafetyState::Quarantined);

  // ++++++++++++++++++++++++++++++++++++++++
  // Both routes obtain distinct fresh identities and reject before the first economic operation.
  const auto first_rejection = test_support::submit_m4_order_or_throw(authority, request);
  check_account_safety_rejection(*authority.submission, first_rejection,
                                 execution::SubmissionReason::AccountQuarantined);
  request.route_id = parse_m4_identifier_or_throw<model::RouteId>("route.shared-account");
  const auto second_rejection = test_support::submit_m4_order_or_throw(authority, request);
  check_account_safety_rejection(*authority.submission, second_rejection,
                                 execution::SubmissionReason::AccountQuarantined);
  CHECK(first_rejection.order_id() != second_rejection.order_id());
  CHECK(authority.submission->reservations().held_reservation_count() == 0U);
  CHECK(authority.submission->outbound_oms().order_count() == 0U);
  CHECK(authority.submission->encoder().invocations_consumed() == 0U);
  CHECK(authority.submission->initiator().invocations_consumed() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Route and canonical validation retain their earlier precedence over the account gate.
  request.quantity = create_m4_decimal_or_throw<model::Quantity>(0);
  const auto malformed = test_support::submit_m4_order_or_throw(authority, request);
  CHECK(malformed.stage() == execution::SubmissionStage::CanonicalValidation);
  CHECK_FALSE(malformed.order_id().has_value());
  request.route_id = parse_m4_identifier_or_throw<model::RouteId>("route.disabled");
  const auto disabled = test_support::submit_m4_order_or_throw(authority, request);
  CHECK(disabled.stage() == execution::SubmissionStage::Route);
  CHECK_FALSE(disabled.order_id().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A peer firm/account remains enabled and takes the untouched first fake write slot.
  request = test_support::create_m4_reference_order_request_or_throw();
  request.route_id = parse_m4_identifier_or_throw<model::RouteId>(
      "route.deribit-testnet-subsidiary-btc-perpetual");
  const auto peer = test_support::submit_m4_order_or_throw(authority, request);
  CHECK(peer.disposition() == execution::SubmitDisposition::WriteInitiated);
  CHECK(authority.submission->reservations().held_reservation_count() == 1U);
  CHECK(authority.submission->outbound_oms().order_count() == 1U);
  CHECK(authority.submission->initiator().invocations_consumed() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Unattributable private input cannot quarantine an invented account; the global owner gate instead
// prevents every configured account from initiating a later order.
TEST_CASE("M4 global private fence gates real submissions without account attribution",
          "[runtime][m4][submission-safety]") {
  auto authority = create_safety_gate_authority_or_throw();
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  model::DeterministicClockProvider clock{20U};
  runtime::SerializedExecutor executor{1U, clock,
                                       create_admission_configuration_or_throw(authority),
                                       *authority.submission->private_admission_owner()};
  runtime::DeterministicExecutorDriver driver{executor};
  const auto unconfigured_attempt = create_unknown_acknowledgement_or_throw(
      authority, parse_m4_identifier_or_throw<model::LogicalAccountId>("account.unconfigured"), 1U);
  const auto failed = executor.try_admit_private(unconfigured_attempt);
  REQUIRE(failed);
  CHECK(failed.value().outcome != runtime::AdmissionOutcome::Accepted);
  REQUIRE(driver.bind_to_current_thread());
  REQUIRE(driver.execute_pending_turns(2U));
  REQUIRE(driver.release_from_current_thread());
  CHECK(
      authority.submission->private_order_reconciler()->is_private_consumption_globally_blocked());
  for (const auto& account : authority.configuration.logical_accounts()) {
    CHECK(authority.submission->private_order_reconciler()->account_safety_state(
              account.logical_account_id) == risk::AccountSafetyState::Synchronized);
  }
  auto request = test_support::create_m4_reference_order_request_or_throw();
  const auto primary = test_support::submit_m4_order_or_throw(authority, request);
  check_account_safety_rejection(*authority.submission, primary,
                                 execution::SubmissionReason::AccountQuarantined);
  request.route_id = parse_m4_identifier_or_throw<model::RouteId>(
      "route.deribit-testnet-subsidiary-btc-perpetual");
  const auto peer = test_support::submit_m4_order_or_throw(authority, request);
  check_account_safety_rejection(*authority.submission, peer,
                                 execution::SubmissionReason::AccountQuarantined);
  CHECK(authority.submission->reservations().held_reservation_count() == 0U);
  CHECK(authority.submission->initiator().invocations_consumed() == 0U);
}

// --------------------------------------------------------
// M4 records local accepted-write uncertainty before another callback can reserve, while the same
// deliberately uninstalled M3 fixture retains its historical submission behavior.
TEST_CASE("M4 ambiguous submission gates its account before the next callback",
          "[runtime][m4][submission-safety]") {
  for (const bool install_m4 : {false, true}) {
    auto authority = create_safety_gate_authority_or_throw(true);
    if (install_m4) {
      test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
    }
    auto request = test_support::create_m4_reference_order_request_or_throw();
    const auto first = test_support::submit_m4_order_or_throw(authority, request);
    REQUIRE(first.disposition() == execution::SubmitDisposition::SubmissionUnknown);
    REQUIRE(first.order_id().has_value());
    const auto second = test_support::submit_m4_order_or_throw(authority, request);
    if (install_m4) {
      check_account_safety_rejection(*authority.submission, second,
                                     execution::SubmissionReason::AccountReconciliationRequired);
      CHECK(authority.submission->reservations().held_reservation_count() == 1U);
      CHECK(authority.submission->outbound_oms().order_count() == 1U);
      CHECK(authority.submission->initiator().invocations_consumed() == 1U);
    } else {
      CHECK(second.disposition() == execution::SubmitDisposition::WriteInitiated);
      CHECK(authority.submission->reservations().held_reservation_count() == 2U);
    }
    request.route_id = parse_m4_identifier_or_throw<model::RouteId>(
        "route.deribit-testnet-subsidiary-btc-perpetual");
    const auto peer = test_support::submit_m4_order_or_throw(authority, request);
    CHECK(peer.disposition() == execution::SubmitDisposition::WriteInitiated);
    CHECK(authority.submission->outbound_oms().find_order(*first.order_id())->state() ==
          oms::OutboundOrderState::SubmissionUnknown);
  }
}

// --------------------------------------------------------
// Internal evidence failure after accepted initiation still records account uncertainty from the
// exact retained row before returning; its retained reservation cannot become a local rejection.
TEST_CASE("M4 accepted submission fault records account uncertainty before returning",
          "[runtime][m4][submission-safety]") {
  for (const auto fault :
       {runtime::TraceAppendFaultPointForTest::WriteInitiatedAfterAcceptance,
        runtime::TraceAppendFaultPointForTest::SubmissionCompletedAfterInitiation}) {
    auto authority = create_safety_gate_authority_or_throw();
    test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
    REQUIRE(authority.submission->arm_trace_append_fault_for_test(fault));
    const auto request = test_support::create_m4_reference_order_request_or_throw();
    const auto result = test_support::submit_m4_order_or_throw(authority, request);
    CHECK(result.disposition() == execution::SubmitDisposition::SubmissionUnknown);
    REQUIRE(result.order_id().has_value());
    const auto* order = authority.submission->outbound_oms().find_order(*result.order_id());
    REQUIRE(order != nullptr);
    CHECK(order->state() == oms::OutboundOrderState::SubmissionUnknown);
    CHECK(authority.submission->private_order_reconciler()->account_safety_state(
              order->provenance().logical_account_id) ==
          risk::AccountSafetyState::ReconciliationRequired);
    CHECK(authority.submission->reservations().held_reservation_count() == 1U);
    CHECK(authority.submission->is_runtime_faulted());
  }
}

// --------------------------------------------------------
