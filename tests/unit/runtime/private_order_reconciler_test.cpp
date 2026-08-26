// Purpose: prove M4 private reconciliation state binds once to a pristine exact M3 owner,
// preserves installation atomicity, and remains stable across genuine owner-bound submissions.

#include "aegis/model/domain_error.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
static_assert(std::same_as<decltype(std::declval<const runtime::SubmissionCoordinator&>()
                                        .private_order_reconciler()),
                           const runtime::PrivateOrderReconciler*>);

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
// Exact owner installation publishes all fixed capacities together and starts with empty tables.
TEST_CASE("M4 private reconciliation state installs once on the exact submission owner",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Create one pristine exact owner and capture every publicly inspectable M3 mutable value.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  REQUIRE(authority.submission->private_order_reconciler() == nullptr);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the private child through the sole public composition command.
  const auto installed = authority.submission->install_private_order_reconciler(
      authority.configuration, authority.m4_policy);
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
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);

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
  REQUIRE(authority.submission->install_private_order_reconciler(authority.configuration,
                                                                 authority.m4_policy));
  const auto* const first = authority.submission->private_order_reconciler();
  REQUIRE(first != nullptr);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt a second installation against otherwise identical authority.
  const auto rejected = authority.submission->install_private_order_reconciler(
      authority.configuration, authority.m4_policy);

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
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt installation with exact owner policy but foreign configuration authority.
  const auto rejected =
      authority.submission->install_private_order_reconciler(foreign, authority.m4_policy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Require configuration precedence and complete nonpublication/nonmutation.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.configuration_provenance");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);

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
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt to attach the foreign configuration/policy pair to the primary coordinator.
  const auto rejected = authority.submission->install_private_order_reconciler(
      foreign.configuration, foreign.m4_policy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Require owner-provenance precedence and complete nonpublication/nonmutation.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.owner_provenance");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);

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
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt installation so pristine-state and owner-provenance failures are both present.
  const auto rejected = authority.submission->install_private_order_reconciler(
      foreign.configuration, foreign.m4_policy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Require install-state dominance, no child/M3 mutation, and preservation of the armed probe.
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InvalidM4Policy);
  CHECK(rejected.error().context.field == "private_order_reconciler.install_state");
  CHECK(authority.submission->private_order_reconciler() == nullptr);
  CHECK(create_submission_owner_state_observation(*authority.submission) == owner_state_before);
  CHECK_FALSE(authority.submission->arm_trace_append_fault_for_test(
      runtime::TraceAppendFaultPointForTest::RiskReservedBeforeOms));

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
  REQUIRE(authority.submission->install_private_order_reconciler(authority.configuration,
                                                                 authority.m4_policy));
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
  REQUIRE(authority.submission->install_private_order_reconciler(authority.configuration,
                                                                 authority.m4_policy));
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

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
