// Purpose: prove inert M4 private reconciliation state binds once to a pristine exact M3 owner,
// allocates every identity slot, preserves M3 state, and exposes no event-processing capability.

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
// has no premature planning, consumption, or application operation.
template <typename Value>
concept HasPrivateEventPlan =
    requires(const Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.plan_authoritative_identity(input);
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
static_assert(!HasPrivateEventPlan<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateEventConsume<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateEventApply<runtime::PrivateOrderReconciler>);
static_assert(std::same_as<decltype(std::declval<const runtime::SubmissionCoordinator&>()
                                        .private_order_reconciler()),
                           const runtime::PrivateOrderReconciler*>);

// ########################################################################

// ########################################################################
// One detached observation covers every publicly inspectable mutable M3 owner component. Equality
// proves dormant installation or a rejected attempt changed none of those components.
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
  // Install the dormant child through the sole public composition command.
  const auto installed = authority.submission->install_dormant_private_order_reconciler(
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
  REQUIRE(authority.submission->install_dormant_private_order_reconciler(authority.configuration,
                                                                         authority.m4_policy));
  const auto* const first = authority.submission->private_order_reconciler();
  REQUIRE(first != nullptr);
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt a second installation against otherwise identical authority.
  const auto rejected = authority.submission->install_dormant_private_order_reconciler(
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
      authority.submission->install_dormant_private_order_reconciler(foreign, authority.m4_policy);

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
// authorities may own its dormant M4 identity state.
TEST_CASE("M4 private reconciliation install rejects a coherent foreign owner policy",
          "[runtime][m4][reconciler]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct two internally coherent but mutually incompatible sealed owner chains.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  auto foreign = create_changed_owner_test_authority_or_throw();
  const auto owner_state_before = create_submission_owner_state_observation(*authority.submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Attempt to attach the foreign configuration/policy pair to the primary coordinator.
  const auto rejected = authority.submission->install_dormant_private_order_reconciler(
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
  const auto rejected = authority.submission->install_dormant_private_order_reconciler(
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

} // namespace
