// Purpose: prove M4 private reconciliation state atomically consumes acknowledged recovery
// authority before callback exposure, fences submission identity, and remains read-only across
// genuine owner submissions.

#include "aegis/model/domain_error.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "aegis/runtime/runtime_diagnostics.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "aegis/trace/runtime_trace.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: these requires expressions prove at compile time that the owner-bound state
// exposes only the exact semantic-value first-seen query and no normalized, consuming, or applying
// operation.
template <typename Value>
concept HasNormalizedPrivateEventPlan =
    requires(const Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.plan_authoritative_identity(input);
    };

template <typename Value>
concept AcceptsNormalizedFirstSeenPrivateIdentityInput =
    requires(const Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.derive_first_seen_authoritative_identity_plan(input);
    };

template <typename Value>
concept HasFirstSeenPrivateIdentityPlan =
    requires(const Value& value, const oms::PrivateEventIngressSemanticValue& input) {
      {
        value.derive_first_seen_authoritative_identity_plan(input)
      } -> std::same_as<model::Result<runtime::FirstSeenAuthoritativePrivateIdentityPlan>>;
    };

template <typename Value>
concept HasPrivateEventConsume =
    requires(Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.consume_private_order_event(input);
    };

template <typename Value>
concept HasPrivateEventApply =
    requires(Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.apply_private_order_event(input);
    };

template <typename Value>
concept HasLegacyPrivateOrderReconcilerInstall =
    requires(Value& value, const configuration::StartupConfiguration& configuration,
             const runtime::M4Policy& policy) {
      value.install_private_order_reconciler(configuration, policy);
    };

template <typename Value>
concept HasPublicOrderIdProviderExtraction =
    requires(Value& value) { value.take_order_id_provider(); };

static_assert(!std::is_copy_constructible_v<runtime::PrivateOrderReconciler>);
static_assert(!std::is_copy_assignable_v<runtime::PrivateOrderReconciler>);
static_assert(!std::is_move_constructible_v<runtime::PrivateOrderReconciler>);
static_assert(!std::is_move_assignable_v<runtime::PrivateOrderReconciler>);
static_assert(!std::is_default_constructible_v<runtime::PrivateOrderReconciler>);
static_assert(!std::is_constructible_v<runtime::PrivateOrderReconciler,
                                       runtime::SubmissionCoordinator&, runtime::M4Policy>);
static_assert(!HasNormalizedPrivateEventPlan<runtime::PrivateOrderReconciler>);
static_assert(!AcceptsNormalizedFirstSeenPrivateIdentityInput<runtime::PrivateOrderReconciler>);
static_assert(HasFirstSeenPrivateIdentityPlan<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateEventConsume<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateEventApply<runtime::PrivateOrderReconciler>);
static_assert(!HasLegacyPrivateOrderReconcilerInstall<runtime::SubmissionCoordinator>);
static_assert(!HasPublicOrderIdProviderExtraction<runtime::SubmissionCoordinator>);
static_assert(!HasPublicOrderIdProviderExtraction<runtime::PrivateOrderReconciler>);
static_assert(!HasPublicOrderIdProviderExtraction<recovery::RecoveryBootstrap>);
static_assert(std::same_as<decltype(std::declval<const runtime::SubmissionCoordinator&>()
                                        .private_order_reconciler()),
                           const runtime::PrivateOrderReconciler*>);

// ########################################################################

// ########################################################################
// IdleRecoveryFenceStrategy makes BotRuntime callback authority concrete without submitting or
// mutating any owner state; the fence must close as soon as this callback-capable owner exists.
class IdleRecoveryFenceStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Accept a Ready callback without causing a submission or any other strategy-side effect.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
  // Accept a readiness callback without causing a submission or any other strategy-side effect.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One detached observation covers every publicly inspectable mutable M3 owner component. Equality
// proves installation or a rejected attempt changed none of those components.
struct SubmissionOwnerStateObservation {
  std::uint32_t outbound_oms_size;
  std::uint32_t held_reservation_count;
  std::uint32_t trace_size;
  std::uint32_t diagnostic_size;
  std::uint64_t accepted_diagnostic_count;
  std::uint64_t dropped_diagnostic_count;
  std::uint64_t encoder_invocation_count;
  std::uint64_t initiator_invocation_count;
  bool runtime_faulted;
  std::optional<model::DomainError> terminal_error;

  // --------------------------------------------------------
  // Structural equality compares every publicly inspectable mutable M3 owner component.
  friend bool operator==(const SubmissionOwnerStateObservation&,
                         const SubmissionOwnerStateObservation&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One copied oracle proves a reported install failure did not consume or alter public bootstrap
// identity facts before the same sealed authority is retried.
struct RecoveryBootstrapIdentityObservation {
  recovery::RecoveryLineageId recovery_lineage_id;
  recovery::RuntimeEpochId runtime_epoch_id;
  model::OrderNamespace registered_order_namespace;
  model::M4RootProvenance root_provenance;

  // --------------------------------------------------------
  // Structural equality compares every immutable public bootstrap identity.
  friend bool operator==(const RecoveryBootstrapIdentityObservation&,
                         const RecoveryBootstrapIdentityObservation&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Copy every publicly inspectable mutable M3 component into one coherent test oracle.
[[nodiscard]] SubmissionOwnerStateObservation
create_submission_owner_state_observation(const runtime::SubmissionCoordinator& owner) {
  return SubmissionOwnerStateObservation{
      owner.outbound_oms().size(),
      owner.reservations().held_reservation_count(),
      owner.trace_sink().size(),
      owner.diagnostics().size(),
      owner.diagnostics().accepted_count(),
      owner.diagnostics().dropped_count(),
      owner.encoder().invocations_consumed(),
      owner.initiator().invocations_consumed(),
      owner.runtime_faulted(),
      owner.terminal_error(),
  };
}

// --------------------------------------------------------

// --------------------------------------------------------
// Copy every public bootstrap identity without exposing its sealed lease or order-ID provider.
[[nodiscard]] RecoveryBootstrapIdentityObservation create_recovery_bootstrap_identity_observation(
    const recovery::RecoveryBootstrap& bootstrap) noexcept {
  return RecoveryBootstrapIdentityObservation{bootstrap.lineage_id(), bootstrap.runtime_epoch_id(),
                                              bootstrap.registered_order_namespace(),
                                              bootstrap.root_provenance()};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Require cold fake-medium inspection to remain fenced while bootstrap or installed child lives.
void check_recovery_medium_has_live_lease(const recovery::DeterministicFakeRecoveryMedium& medium) {
  const auto inspected = medium.published_journal_record_count();
  REQUIRE_FALSE(inspected);
  CHECK(inspected.error().code == model::DomainErrorCode::InvalidJournalState);
  CHECK(inspected.error().context.field == "journal.read_lease");
}

// --------------------------------------------------------

// --------------------------------------------------------
// Mint one expected canonical identity from an independent test-only provider or throw on fixture
// failure before comparing the production submission result.
[[nodiscard]] model::OrderId
create_expected_order_id_or_throw(model::OrderNamespace order_namespace, std::uint64_t counter) {
  auto provider = model::DeterministicOrderIdProvider::create(order_namespace, counter);
  if (!provider) {
    throw std::logic_error{"invalid expected order-ID provider"};
  }
  auto order_id = provider.value().next();
  if (!order_id) {
    throw std::logic_error{"invalid expected order ID"};
  }
  return std::move(order_id).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Construct one deterministic restart namespace for a second-incarnation lifecycle assertion.
[[nodiscard]] model::OrderNamespace create_order_namespace(std::uint8_t seed) noexcept {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return model::OrderNamespace{bytes};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Create exact idle strategy coverage so successful BotRuntime construction exposes callback
// authority without relying on a callback or submission side effect.
[[nodiscard]] std::vector<runtime::BotStrategyRegistration>
create_idle_recovery_fence_registrations(const configuration::StartupConfiguration& configuration) {
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.reserve(configuration.organization().bot_attributions().size());
  for (const auto& attribution : configuration.organization().bot_attributions()) {
    registrations.push_back(runtime::BotStrategyRegistration{
        attribution.bot_id, std::make_unique<IdleRecoveryFenceStrategy>()});
  }
  return registrations;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Create a valid changed configuration whose fingerprint cannot match the authored M4 root; an
// invalid fixture revision or configuration throws std::logic_error.
[[nodiscard]] configuration::StartupConfiguration create_changed_configuration_or_throw() {
  auto params = test_support::m3_enabled_two_firm_configuration_params();
  auto next_revision = params.revision.next();
  if (!next_revision) {
    throw std::logic_error{"invalid changed M4 configuration revision"};
  }
  params.revision = std::move(next_revision).value();
  auto created = configuration::StartupConfiguration::create(std::move(params));
  if (!created) {
    throw std::logic_error{"invalid changed M4 configuration fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build a coherent foreign owner whose configuration and all derived M2-M4 policies agree with one
// another but cannot match the primary coordinator; invalid fixture authority throws.
[[nodiscard]] test_support::M4OwnerTestAuthority create_changed_owner_test_authority_or_throw() {
  auto params = test_support::m3_enabled_two_firm_configuration_params();
  auto next_revision = params.revision.next();
  if (!next_revision) {
    throw std::logic_error{"invalid changed M4 owner revision"};
  }
  params.revision = std::move(next_revision).value();
  return test_support::create_m4_owner_test_authority_or_throw(std::move(params));
}

// --------------------------------------------------------

// --------------------------------------------------------
// Exact owner installation binds all recovery identities and fixed capacities without business
// mutation.
TEST_CASE("M4 private reconciliation state installs acknowledged recovery authority once",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Create one pristine exact owner and capture every publicly inspectable M3 mutable value.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(authority.submission->private_order_reconciler() == nullptr);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Build one real namespace-acknowledged bootstrap and retain its immutable identity oracle.
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto bootstrap_identity =
      create_recovery_bootstrap_identity_observation(recovery.bootstrap);

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the private child through the sole recovery-consuming composition command.
  const auto installed = authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap));
  REQUIRE(installed);

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove exact policy-sized empty storage and absence of every M3 business-side effect.
  const auto* const reconciler = authority.submission->private_order_reconciler();
  REQUIRE(reconciler != nullptr);
  const auto& capacities = authority.m4_policy.capacities();
  CHECK(reconciler->event_identity_record_capacity() == capacities.max_event_identity_records);
  CHECK(reconciler->trade_identity_record_capacity() == capacities.max_trade_identity_records);
  CHECK(reconciler->exchange_order_mapping_capacity() == capacities.max_exchange_order_mappings);
  CHECK(reconciler->event_identity_record_count() == 0U);
  CHECK(reconciler->trade_identity_record_count() == 0U);
  CHECK(reconciler->exchange_order_mapping_count() == 0U);
  CHECK(reconciler->m4_policy() == authority.m4_policy);
  CHECK(reconciler->recovery_lineage_id() == bootstrap_identity.recovery_lineage_id);
  CHECK(reconciler->runtime_epoch_id() == bootstrap_identity.runtime_epoch_id);
  CHECK(reconciler->registered_order_namespace() == bootstrap_identity.registered_order_namespace);
  CHECK(reconciler->m4_policy().root_provenance() == bootstrap_identity.root_provenance);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Successful BotRuntime composition closes recovery retrofit before its callback authority can
// escape, even when no callback or submission has changed coordinator-local state.
TEST_CASE("M4 recovery installation closes before callback authority becomes reachable",
          "[runtime][m4][reconciler][bot]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct a callback-capable runtime around one otherwise pristine submission coordinator.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);
  trace::RuntimeTraceSink trace_sink{authority.runtime_policy};
  runtime::RuntimeDiagnosticSink diagnostics{authority.runtime_policy};
  model::DeterministicClockProvider callback_clock{50U};
  auto callback_authority = runtime::BotRuntime::create(
      authority.configuration, authority.runtime_policy, callback_clock, trace_sink, diagnostics,
      create_idle_recovery_fence_registrations(authority.configuration), {},
      authority.submission.get());
  REQUIRE(callback_authority);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt a late install with a real acknowledged bootstrap and require install-state precedence.
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto bootstrap_before = create_recovery_bootstrap_identity_observation(recovery.bootstrap);
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.install_state");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK(create_recovery_bootstrap_identity_observation(recovery.bootstrap) == bootstrap_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // The rejected bootstrap remains usable by a fresh exact owner with no callback authority.
  auto retry_owner = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(retry_owner.submission->install_recovery_bound_private_order_reconciler(
      retry_owner.configuration, retry_owner.m4_policy, std::move(recovery.bootstrap)));
  REQUIRE(retry_owner.submission->private_order_reconciler() != nullptr);
  CHECK(retry_owner.submission->private_order_reconciler()->runtime_epoch_id() ==
        bootstrap_before.runtime_epoch_id);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Failed BotRuntime composition publishes no callback authority and therefore leaves recovery
// installation open and its counter-one recovered identity stream usable.
TEST_CASE("M4 failed callback composition leaves recovery installation open",
          "[runtime][m4][reconciler][bot]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject missing strategy coverage before a BotRuntime result or callback authority exists.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);
  trace::RuntimeTraceSink trace_sink{authority.runtime_policy};
  runtime::RuntimeDiagnosticSink diagnostics{authority.runtime_policy};
  model::DeterministicClockProvider callback_clock{50U};
  auto rejected_callback_authority =
      runtime::BotRuntime::create(authority.configuration, authority.runtime_policy, callback_clock,
                                  trace_sink, diagnostics, {}, {}, authority.submission.get());
  REQUIRE_FALSE(rejected_callback_authority);
  CHECK(rejected_callback_authority.error().code == model::DomainErrorCode::StrategyNotConfigured);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the acknowledged bootstrap after that failure and prove its first real submission uses
  // the recovered namespace at counter one.
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto registered_namespace = recovery.bootstrap.registered_order_namespace();
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap)));
  const auto submitted = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  CHECK(*submitted.order_id() == create_expected_order_id_or_throw(registered_namespace, 1U));
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// A BotRuntime for one sealed authority cannot close another coordinator's recovery-install seam;
// provenance rejection leaves that foreign owner and its acknowledged bootstrap reusable.
TEST_CASE("M4 callback composition rejects a foreign submission coordinator atomically",
          "[runtime][m4][reconciler][bot]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Pair one valid callback composition with a distinct coordinator and retain the foreign state.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto foreign = create_changed_owner_test_authority_or_throw();
  const auto foreign_state_before = create_submission_owner_state_observation(*foreign.submission);
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(foreign.m4_policy);
  const auto registered_namespace = recovery.bootstrap.registered_order_namespace();
  trace::RuntimeTraceSink trace_sink{authority.runtime_policy};
  runtime::RuntimeDiagnosticSink diagnostics{authority.runtime_policy};
  model::DeterministicClockProvider callback_clock{50U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject the cross-authority pointer before creating contexts or closing the foreign seam.
  auto rejected_callback_authority = runtime::BotRuntime::create(
      authority.configuration, authority.runtime_policy, callback_clock, trace_sink, diagnostics,
      create_idle_recovery_fence_registrations(authority.configuration), {},
      foreign.submission.get());
  REQUIRE_FALSE(rejected_callback_authority);
  CHECK(rejected_callback_authority.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                     "bot_runtime.submission_coordinator_provenance"));
  CHECK(create_submission_owner_state_observation(*foreign.submission) == foreign_state_before);

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the foreign owner's matching bootstrap and prove its recovered counter-one stream.
  REQUIRE(foreign.submission->install_recovery_bound_private_order_reconciler(
      foreign.configuration, foreign.m4_policy, std::move(recovery.bootstrap)));
  const auto submitted = test_support::submit_m4_order_or_throw(
      foreign, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(submitted.order_id());
  CHECK(*submitted.order_id() == create_expected_order_id_or_throw(registered_namespace, 1U));
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Reinstallation rejects before replacing or changing the already published owner state.
TEST_CASE("M4 private reconciliation state cannot replace an installed owner",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Install one exact child and retain its stable address plus the complete M3 observation.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto first_recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(first_recovery.bootstrap)));
  const auto* const first = authority.submission->private_order_reconciler();
  REQUIRE(first != nullptr);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt a second installation with distinct but otherwise matching acknowledged authority.
  auto second_recovery = test_support::create_m4_recovery_bootstrap_test_authority_or_throw(
      authority.m4_policy, 0x50U, 0x70U);
  const auto second_bootstrap_before =
      create_recovery_bootstrap_identity_observation(second_recovery.bootstrap);
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(second_recovery.bootstrap));

  // ++++++++++++++++++++++++++++++++++++++++
  // Require install-state precedence without replacement or any owner/table mutation.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.install_state");
  CHECK(authority.submission->private_order_reconciler() == first);
  CHECK(first->event_identity_record_count() == 0U);
  CHECK(first->trade_identity_record_count() == 0U);
  CHECK(first->exchange_order_mapping_count() == 0U);
  CHECK(first->m4_policy() == authority.m4_policy);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK(create_recovery_bootstrap_identity_observation(second_recovery.bootstrap) ==
        second_bootstrap_before);
  check_recovery_medium_has_live_lease(*second_recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // The unconsumed second bootstrap remains installable into a fresh exact owner.
  auto retry_owner = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(retry_owner.submission->install_recovery_bound_private_order_reconciler(
      retry_owner.configuration, retry_owner.m4_policy, std::move(second_recovery.bootstrap)));
  REQUIRE(retry_owner.submission->private_order_reconciler() != nullptr);
  CHECK(retry_owner.submission->private_order_reconciler()->runtime_epoch_id() ==
        second_bootstrap_before.runtime_epoch_id);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Foreign configuration authority fails before any reconciler pointer or capacity becomes visible.
TEST_CASE("M4 private reconciliation install rejects foreign configuration atomically",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Pair one pristine owner/policy with a valid but fingerprint-incompatible configuration.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  const auto foreign = create_changed_configuration_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto bootstrap_before = create_recovery_bootstrap_identity_observation(recovery.bootstrap);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt installation with exact owner policy but foreign configuration authority.
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      foreign, authority.m4_policy, std::move(recovery.bootstrap));

  // ++++++++++++++++++++++++++++++++++++++++
  // Require configuration precedence and complete nonpublication/nonmutation.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.configuration_provenance");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK(create_recovery_bootstrap_identity_observation(recovery.bootstrap) == bootstrap_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // Retrying the same bootstrap against the exact configuration succeeds at counter-one authority.
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap)));
  REQUIRE(authority.submission->private_order_reconciler() != nullptr);
  const auto retried_submission = test_support::submit_m4_order_or_throw(
      authority, test_support::create_m4_reference_order_request_or_throw());
  REQUIRE(retried_submission.order_id());
  CHECK(*retried_submission.order_id() ==
        create_expected_order_id_or_throw(bootstrap_before.registered_order_namespace, 1U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// A self-consistent foreign policy still fails because only the coordinator's exact sealed M3
// authorities may own its M4 identity state.
TEST_CASE("M4 private reconciliation install rejects a coherent foreign owner policy",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct two internally coherent but mutually incompatible sealed owner chains.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto foreign = create_changed_owner_test_authority_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto bootstrap_before = create_recovery_bootstrap_identity_observation(recovery.bootstrap);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt to attach the foreign configuration/policy pair to the primary coordinator.
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      foreign.configuration, foreign.m4_policy, std::move(recovery.bootstrap));

  // ++++++++++++++++++++++++++++++++++++++++
  // Require owner-provenance precedence and complete nonpublication/nonmutation.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.owner_provenance");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK(create_recovery_bootstrap_identity_observation(recovery.bootstrap) == bootstrap_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // Owner-precedence rejection leaves the matching bootstrap usable by its exact primary owner.
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap)));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// A bootstrap from a different complete M4 root is rejected without consuming its sealed
// authority, then remains usable by the owner from which that root was derived.
TEST_CASE("M4 private reconciliation install rejects a foreign recovery root atomically",
          "[runtime][m4][reconciler][recovery]") {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto foreign = create_changed_owner_test_authority_or_throw();
  auto recovery = test_support::create_m4_recovery_bootstrap_test_authority_or_throw(
      foreign.m4_policy, 0x60U, 0x80U);
  const auto bootstrap_before = create_recovery_bootstrap_identity_observation(recovery.bootstrap);

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact owner-policy validation succeeds first, exposing the independent bootstrap-root error.
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::RecoveryProvenanceMismatch);
  CHECK(rejected.error().context.field == "private_order_reconciler.recovery_root_provenance");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_recovery_bootstrap_identity_observation(recovery.bootstrap) == bootstrap_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // The same unconsumed bootstrap installs under its exact foreign owner and root.
  REQUIRE(foreign.submission->install_recovery_bound_private_order_reconciler(
      foreign.configuration, foreign.m4_policy, std::move(recovery.bootstrap)));
  REQUIRE(foreign.submission->private_order_reconciler() != nullptr);
  CHECK(foreign.submission->private_order_reconciler()->runtime_epoch_id() ==
        bootstrap_before.runtime_epoch_id);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// A consumed bootstrap is detectably moved and cannot seed a second pristine coordinator.
TEST_CASE("M4 private reconciliation install rejects a consumed recovery bootstrap",
          "[runtime][m4][reconciler][recovery]") {
  auto first_owner = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(first_owner.m4_policy);
  REQUIRE(first_owner.submission->install_recovery_bound_private_order_reconciler(
      first_owner.configuration, first_owner.m4_policy, std::move(recovery.bootstrap)));

  // ++++++++++++++++++++++++++++++++++++++++
  // A second matching-root owner reaches moved-state validation and remains entirely pristine.
  auto second_owner = test_support::create_m4_owner_test_authority_or_throw();
  const auto rejected = second_owner.submission->install_recovery_bound_private_order_reconciler(
      second_owner.configuration, second_owner.m4_policy, std::move(recovery.bootstrap));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidJournalState);
  CHECK(rejected.error().context.field == "private_order_reconciler.recovery_bootstrap_state");
  CHECK(second_owner.submission->private_order_reconciler() == nullptr);
  CHECK(second_owner.submission->outbound_oms().size() == 0U);
  CHECK(second_owner.submission->reservations().held_reservation_count() == 0U);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Pristine install-state rejection wins before foreign authority validation and preserves the
// already armed source-private fault probe together with every public owner observation.
TEST_CASE("M4 private reconciliation install rejects a dirty owner before authority checks",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Dirty one exact owner through a safe fixed-enum probe and also prepare foreign authority.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto foreign = create_changed_owner_test_authority_or_throw();
  REQUIRE(authority.submission->arm_trace_append_fault_for_test(
      runtime::TraceAppendFaultPointForTest::FirstReentryRejected));
  auto recovery =
      test_support::create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
  const auto bootstrap_before = create_recovery_bootstrap_identity_observation(recovery.bootstrap);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt installation so pristine-state and owner-provenance failures are both present.
  const auto rejected = authority.submission->install_recovery_bound_private_order_reconciler(
      foreign.configuration, foreign.m4_policy, std::move(recovery.bootstrap));

  // ++++++++++++++++++++++++++++++++++++++++
  // Require install-state dominance, no child/M3 mutation, and preservation of the armed probe.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.install_state");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK_FALSE(authority.submission->arm_trace_append_fault_for_test(
      runtime::TraceAppendFaultPointForTest::RiskReservedBeforeOms));
  CHECK(create_recovery_bootstrap_identity_observation(recovery.bootstrap) == bootstrap_before);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // A fresh exact owner can consume the bootstrap rejected before any authority inspection.
  auto retry_owner = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(retry_owner.submission->install_recovery_bound_private_order_reconciler(
      retry_owner.configuration, retry_owner.m4_policy, std::move(recovery.bootstrap)));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// One pristine installation remains owned at the same address while a genuine active BotContext
// admits its caller-unforgeable M3 row for later read-only correlation.
TEST_CASE("M4 owner fixture submits through a genuine active bot context",
          "[runtime][m4][reconciler][submission]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Install while the exact coordinator is wholly pristine, before any callback or submission.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery = test_support::create_m4_recovery_bootstrap_test_authority_or_throw(
      authority.m4_policy, 0x10U, 0x90U);
  const auto acknowledged_namespace = recovery.bootstrap.registered_order_namespace();
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap)));
  const auto* const reconciler = authority.submission->private_order_reconciler();
  REQUIRE(reconciler != nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
  // Submit one fixed request through the route owner's genuine active callback-local capability.
  const auto request = test_support::create_m4_reference_order_request_or_throw();
  const auto result = test_support::submit_m4_order_or_throw(authority, request);

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the real retained row, reservation, route-derived bot, and stable empty private tables.
  REQUIRE(result.disposition() == execution::SubmitDisposition::WriteInitiated);
  REQUIRE(result.order_id());
  CHECK(*result.order_id() == create_expected_order_id_or_throw(acknowledged_namespace, 1U));
  CHECK(authority.submission->private_order_reconciler() == reconciler);
  CHECK(authority.submission->outbound_oms().size() == 1U);
  const auto* const row = authority.submission->outbound_oms().find(*result.order_id());
  REQUIRE(row != nullptr);
  CHECK(row->state() == oms::OutboundOrderState::WriteInitiated);
  const auto* const route = authority.configuration.routes().find(request.route_id);
  REQUIRE(route != nullptr);
  CHECK(row->provenance().bot_id == route->bot_id);
  CHECK(authority.submission->reservations().held_reservation_count() == 1U);
  CHECK(reconciler->event_identity_record_count() == 0U);
  CHECK(reconciler->trade_identity_record_count() == 0U);
  CHECK(reconciler->exchange_order_mapping_count() == 0U);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Rebuilding the callback harness preserves global callback/dispatch predecessors and advances
// owner turns instead of reusing an identity inside one retained installed coordinator.
TEST_CASE("M4 owner fixture advances longitudinal callback and turn identities",
          "[runtime][m4][reconciler][submission]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Install once while pristine, then drive two separate genuine callback-owned submissions.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery = test_support::create_m4_recovery_bootstrap_test_authority_or_throw(
      authority.m4_policy, 0x10U, 0xA0U);
  const auto acknowledged_namespace = recovery.bootstrap.registered_order_namespace();
  REQUIRE(authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap)));
  const auto* const reconciler = authority.submission->private_order_reconciler();
  REQUIRE(reconciler != nullptr);
  const auto request = test_support::create_m4_reference_order_request_or_throw();
  const auto first = test_support::submit_m4_order_or_throw(authority, request);
  const auto second = test_support::submit_m4_order_or_throw(authority, request);

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove all local identities and longitudinal predecessors advanced exactly once per dispatch.
  REQUIRE(first.disposition() == execution::SubmitDisposition::WriteInitiated);
  REQUIRE(second.disposition() == execution::SubmitDisposition::WriteInitiated);
  REQUIRE(first.order_id());
  REQUIRE(second.order_id());
  CHECK(*first.order_id() != *second.order_id());
  CHECK(*first.order_id() == create_expected_order_id_or_throw(acknowledged_namespace, 1U));
  CHECK(*second.order_id() == create_expected_order_id_or_throw(acknowledged_namespace, 2U));
  CHECK(authority.submission->private_order_reconciler() == reconciler);
  CHECK(authority.submission->outbound_oms().size() == 2U);
  CHECK(authority.submission->reservations().held_reservation_count() == 2U);
  REQUIRE(authority.last_callback_ordinal);
  CHECK(authority.last_callback_ordinal->value() == 2U);
  CHECK(authority.completed_dispatch_count == 2U);
  CHECK(authority.next_owner_turn.value() == 3U);
  CHECK(authority.next_processing_timestamp_nanoseconds == 1'234'569U);
  CHECK(reconciler->event_identity_record_count() == 0U);
  CHECK(reconciler->trade_identity_record_count() == 0U);
  CHECK(reconciler->exchange_order_mapping_count() == 0U);
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Destroying the installed owner releases the medium lease, and the next acknowledged namespace
// starts an independent counter-one submission stream without changing prior journal rows.
TEST_CASE("M4 recovery-bound owner releases its lease before the next incarnation",
          "[runtime][m4][reconciler][recovery][submission]") {
  auto first_owner = test_support::create_m4_owner_test_authority_or_throw();
  auto recovery = test_support::create_m4_recovery_bootstrap_test_authority_or_throw(
      first_owner.m4_policy, 0x20U, 0xB0U);
  const auto first_namespace = recovery.bootstrap.registered_order_namespace();
  const auto first_runtime_epoch = recovery.bootstrap.runtime_epoch_id();
  REQUIRE(first_owner.submission->install_recovery_bound_private_order_reconciler(
      first_owner.configuration, first_owner.m4_policy, std::move(recovery.bootstrap)));

  // ++++++++++++++++++++++++++++++++++++++++
  // The first incarnation consumes counter one while its installed child keeps inspection fenced.
  const auto request = test_support::create_m4_reference_order_request_or_throw();
  const auto first = test_support::submit_m4_order_or_throw(first_owner, request);
  REQUIRE(first.order_id());
  CHECK(*first.order_id() == create_expected_order_id_or_throw(first_namespace, 1U));
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // Destroying the coordinator destroys its last-declared child and releases the live lease.
  first_owner.submission.reset();
  const auto first_cold_count = recovery.medium->published_journal_record_count();
  REQUIRE(first_cold_count);
  CHECK(first_cold_count.value() == 1U);
  const auto first_cold_acknowledged = recovery.medium->acknowledged_journal_record_count();
  REQUIRE(first_cold_acknowledged);
  CHECK(first_cold_acknowledged.value() == 1U);
  const auto first_record = recovery.medium->published_journal_record_at(0U);
  REQUIRE(first_record);
  CHECK(first_record.value().kind() == recovery::JournalRecordKind::NamespaceRegistered);
  CHECK_FALSE(first_record.value().audit_span());

  // ++++++++++++++++++++++++++++++++++++++++
  // The same retained medium acknowledges a distinct second namespace and runtime epoch.
  const auto second_namespace = create_order_namespace(0xD0U);
  auto second_bootstrap =
      recovery.medium->bootstrap_recovery_from_namespace(first_owner.m4_policy, second_namespace);
  REQUIRE(second_bootstrap);
  CHECK(second_bootstrap.value().runtime_epoch_id().order_namespace() == second_namespace);
  CHECK(second_bootstrap.value().runtime_epoch_id().counter() == 1U);
  CHECK(second_bootstrap.value().runtime_epoch_id() != first_runtime_epoch);

  // ++++++++++++++++++++++++++++++++++++++++
  // A fresh exact coordinator adopts the second provider and independently begins at counter one.
  auto second_owner = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(second_owner.submission->install_recovery_bound_private_order_reconciler(
      second_owner.configuration, second_owner.m4_policy, std::move(second_bootstrap).value()));
  const auto second = test_support::submit_m4_order_or_throw(second_owner, request);
  REQUIRE(second.order_id());
  CHECK(*second.order_id() == create_expected_order_id_or_throw(second_namespace, 1U));
  CHECK(*second.order_id() != *first.order_id());
  check_recovery_medium_has_live_lease(*recovery.medium);

  // ++++++++++++++++++++++++++++++++++++++++
  // Final destruction exposes exactly two acknowledged namespace-only rows and no business append.
  second_owner.submission.reset();
  const auto final_published = recovery.medium->published_journal_record_count();
  const auto final_acknowledged = recovery.medium->acknowledged_journal_record_count();
  REQUIRE(final_published);
  REQUIRE(final_acknowledged);
  CHECK(final_published.value() == 2U);
  CHECK(final_acknowledged.value() == 2U);
  const auto second_record = recovery.medium->published_journal_record_at(1U);
  REQUIRE(second_record);
  CHECK(second_record.value().kind() == recovery::JournalRecordKind::NamespaceRegistered);
  CHECK_FALSE(second_record.value().audit_span());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
