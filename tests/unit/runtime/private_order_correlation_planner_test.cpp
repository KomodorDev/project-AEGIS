// Purpose: prove the M4 first-seen identity planner derives detached correlation and trade values
// from genuine owner rows while leaving every owner, identity table, and evidence value unchanged.

#include "aegis/runtime/private_order_reconciler.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: these requires expressions prove the read-only first-seen seam publishes no
// construction, commit, consumption, application, insertion, restore, activation, recovery, or
// callback capability under either a generic or private-order-specific command name.
template <typename Value>
concept HasPrivateCommitCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.commit(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.commit_private_order_plan(plan);
    };

template <typename Value>
concept HasPrivateConsumeCapability =
    requires(Value& value, const oms::PrivateEventIngressSemanticValue& input) {
      value.consume(input);
    } || requires(Value& value, const oms::PrivateEventIngressSemanticValue& input) {
      value.consume_private_order_event(input);
    };

template <typename Value>
concept HasPrivateApplyCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.apply(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.apply_private_order_plan(plan);
    };

template <typename Value>
concept HasPrivateInsertCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.insert(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.insert_private_identity(plan);
    };

template <typename Value>
concept HasPrivateRestoreCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.restore(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.restore_private_identity(plan);
    };

template <typename Value>
concept HasPrivateActivateCapability = requires(Value& value) {
  value.activate();
} || requires(Value& value) { value.activate_private_order_processing(); };

template <typename Value>
concept HasPrivateRecoveryCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.recover(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.recover_private_order_event(plan);
    };

template <typename Value>
concept HasPrivateCallbackCapability =
    requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.callback(plan);
    } || requires(Value& value, const runtime::FirstSeenAuthoritativePrivateIdentityPlan& plan) {
      value.dispatch_private_order_callback(plan);
    };

using FirstSeenPlan = runtime::FirstSeenAuthoritativePrivateIdentityPlan;

template <typename Value>
concept AcceptsFirstSeenSemanticValue =
    requires(const Value& value, const oms::PrivateEventIngressSemanticValue& input) {
      {
        value.derive_first_seen_authoritative_identity_plan(input)
      } -> std::same_as<model::Result<FirstSeenPlan>>;
    };

template <typename Value>
concept AcceptsNormalizedFirstSeenInput =
    requires(const Value& value, const oms::NormalizedPrivateOrderInput& input) {
      value.derive_first_seen_authoritative_identity_plan(input);
    };

static_assert(!std::is_default_constructible_v<FirstSeenPlan>);
static_assert(!std::is_constructible_v<
              FirstSeenPlan, oms::PrivateEventRegistryKey, oms::PrivateEventIngressSemanticValue,
              runtime::FirstSeenPrivateCorrelationPlan, runtime::FirstSeenPrivateTradePlan,
              std::optional<risk::AccountSafetyReason>>);
static_assert(AcceptsFirstSeenSemanticValue<runtime::PrivateOrderReconciler>);
static_assert(!AcceptsNormalizedFirstSeenInput<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateCommitCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateConsumeCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateApplyCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateInsertCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateRestoreCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateActivateCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateRecoveryCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateCallbackCapability<runtime::PrivateOrderReconciler>);
static_assert(!HasPrivateCommitCapability<FirstSeenPlan>);
static_assert(!HasPrivateConsumeCapability<FirstSeenPlan>);
static_assert(!HasPrivateApplyCapability<FirstSeenPlan>);
static_assert(!HasPrivateInsertCapability<FirstSeenPlan>);
static_assert(!HasPrivateRestoreCapability<FirstSeenPlan>);
static_assert(!HasPrivateActivateCapability<FirstSeenPlan>);
static_assert(!HasPrivateRecoveryCapability<FirstSeenPlan>);
static_assert(!HasPrivateCallbackCapability<FirstSeenPlan>);

// ########################################################################
// One OMS row snapshot copies immutable admission and every mutable private-order projection field.
struct OmsRowSnapshot {
  oms::OutboundOrderAdmission admission;
  oms::PrivateOrderProjection projection;

  // --------------------------------------------------------
  // Structural equality compares the complete retained OMS row observation.
  friend bool operator==(const OmsRowSnapshot&, const OmsRowSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One fake-write snapshot retains the complete attempt chain and accepted byte prefix.
struct FakeWriteSnapshot {
  model::SubmissionAttemptId attempt_id;
  model::EncoderInvocationOrdinal encoder_invocation_ordinal;
  model::InitiatorInvocationOrdinal initiator_invocation_ordinal;
  model::FakeWriteOrdinal write_ordinal;
  std::vector<std::byte> bytes;

  // --------------------------------------------------------
  // Structural equality compares every accepted fake-write observation.
  friend bool operator==(const FakeWriteSnapshot&, const FakeWriteSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// This snapshot covers all publicly observable owner state plus the three identity-table counts;
// repeated planning separately proves that every private fixed slot remains empty.
struct OwnerPlanningSnapshot {
  std::vector<OmsRowSnapshot> oms_rows;
  std::vector<std::optional<risk::ReservationEvidence>> reservations;
  std::vector<risk::RiskScopeExposureEvidence> scope_evidence;
  std::vector<trace::SubmissionTraceRecord> submission_trace;
  std::vector<runtime::SubmissionDiagnosticRecord> diagnostics;
  std::vector<FakeWriteSnapshot> accepted_writes;
  runtime::M4Policy m4_policy;
  std::optional<model::CallbackOrdinal> last_callback_ordinal;
  std::uint64_t completed_dispatch_count;
  model::TurnOrdinal next_owner_turn;
  std::uint64_t next_processing_timestamp_nanoseconds;
  std::uint64_t encoder_invocations;
  std::uint64_t initiator_invocations;
  std::uint64_t diagnostic_accepted_count;
  std::uint64_t diagnostic_dropped_count;
  std::uint32_t event_identity_record_count;
  std::uint32_t trade_identity_record_count;
  std::uint32_t exchange_order_mapping_count;
  bool runtime_faulted;
  std::optional<model::DomainError> terminal_error;

  // --------------------------------------------------------
  // Structural equality detects any owner-visible mutation made by read-only planning.
  friend bool operator==(const OwnerPlanningSnapshot&, const OwnerPlanningSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Extract one successful result or fail fixture construction before production assertions begin.
template <typename Value> [[nodiscard]] Value extract_result_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid M4 first-seen planner fixture value"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build one source factory from the same sealed configuration and M4 root as its authority.
[[nodiscard]] runtime::PrivateOrderEventFactory
create_private_order_event_factory_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  return runtime::PrivateOrderEventFactory{
      extract_result_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
          authority.configuration, authority.m4_policy))};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Copy every publicly inspectable M3/M4 owner value before or after one read-only planning call.
[[nodiscard]] OwnerPlanningSnapshot
create_owner_planning_snapshot_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  const auto& coordinator = *authority.submission;
  const auto* const reconciler = coordinator.private_order_reconciler();
  if (reconciler == nullptr) {
    throw std::logic_error{"missing M4 reconciler in planner snapshot fixture"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy every retained OMS row in canonical admission order.
  std::vector<OmsRowSnapshot> oms_rows;
  oms_rows.reserve(coordinator.outbound_oms().order_count());
  for (std::size_t index = 0U; index < coordinator.outbound_oms().order_count(); ++index) {
    const auto* const row = coordinator.outbound_oms().record_at(index);
    if (row == nullptr) {
      throw std::logic_error{"missing retained OMS row in planner snapshot fixture"};
    }
    oms_rows.push_back(OmsRowSnapshot{row->admission(), row->private_projection()});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy every reservation slot and all configured scope evidence, including the submitting
  // order's seven canonical scope projections.
  std::vector<std::optional<risk::ReservationEvidence>> reservations;
  reservations.reserve(coordinator.reservations().capacity());
  for (std::size_t index = 0U; index < coordinator.reservations().capacity(); ++index) {
    const auto* const reservation = coordinator.reservations().reservation_at(index);
    if (reservation == nullptr) {
      reservations.push_back(std::nullopt);
    } else {
      reservations.push_back(*reservation);
    }
  }
  std::vector<risk::RiskScopeExposureEvidence> scope_evidence;
  scope_evidence.reserve(coordinator.reservations().scope_evidence_count());
  for (std::size_t index = 0U; index < coordinator.reservations().scope_evidence_count(); ++index) {
    auto evidence = coordinator.reservations().scope_evidence_at(index);
    if (!evidence) {
      throw std::logic_error{"missing risk scope in planner snapshot fixture"};
    }
    scope_evidence.push_back(std::move(*evidence));
  }
  if (scope_evidence.size() != coordinator.reservations().policy().limit_sets().size()) {
    throw std::logic_error{"incomplete canonical scope planner snapshot fixture"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy submission trace, diagnostics, accepted bytes, counters, and terminal state exactly.
  const auto trace_records = coordinator.trace_sink().records();
  std::vector<trace::SubmissionTraceRecord> submission_trace{trace_records.begin(),
                                                             trace_records.end()};
  std::vector<runtime::SubmissionDiagnosticRecord> diagnostics;
  diagnostics.reserve(coordinator.diagnostics().diagnostic_count());
  for (std::size_t index = 0U; index < coordinator.diagnostics().diagnostic_count(); ++index) {
    const auto* const record = coordinator.diagnostics().diagnostic_at(index);
    if (record == nullptr) {
      throw std::logic_error{"missing diagnostic in planner snapshot fixture"};
    }
    diagnostics.push_back(*record);
  }
  std::vector<FakeWriteSnapshot> accepted_writes;
  accepted_writes.reserve(coordinator.initiator().accepted_writes().size());
  for (const auto& write : coordinator.initiator().accepted_writes()) {
    accepted_writes.push_back(
        FakeWriteSnapshot{write.attempt_id(), write.encoder_invocation_ordinal(),
                          write.initiator_invocation_ordinal(), write.write_ordinal(),
                          std::vector<std::byte>{write.bytes().begin(), write.bytes().end()}});
  }
  return OwnerPlanningSnapshot{
      std::move(oms_rows),
      std::move(reservations),
      std::move(scope_evidence),
      std::move(submission_trace),
      std::move(diagnostics),
      std::move(accepted_writes),
      reconciler->m4_policy(),
      authority.last_callback_ordinal,
      authority.completed_dispatch_count,
      authority.next_owner_turn,
      authority.next_processing_timestamp_nanoseconds,
      coordinator.encoder().invocations_consumed(),
      coordinator.initiator().invocations_consumed(),
      coordinator.diagnostics().accepted_count(),
      coordinator.diagnostics().dropped_count(),
      reconciler->event_identity_record_count(),
      reconciler->trade_identity_record_count(),
      reconciler->exchange_order_mapping_count(),
      coordinator.is_runtime_faulted(),
      coordinator.terminal_error(),
  };

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// ########################################################################
// The fixture installs the exact child while its owner is pristine, then admits one genuine row
// through an active BotContext. It never seeds a private identity table or exchange mapping.
class FirstSeenPlannerFixture final {
public:

  // --------------------------------------------------------
  // Install before any submission, then retain the one real locally minted order identity.
  FirstSeenPlannerFixture()
      : authority_{test_support::create_m4_owner_test_authority_or_throw()},
        factory_{create_private_order_event_factory_or_throw(authority_)} {

    // ++++++++++++++++++++++++++++++++++++++++
    // Publish the owner-bound child before any callback, submission, or evidence can dirty it.
    test_support::install_recovery_bound_private_order_reconciler_or_throw(authority_);

    // ++++++++++++++++++++++++++++++++++++++++
    // Admit exactly one caller-unforgeable row through the genuine active BotContext seam.
    const auto submitted = test_support::submit_m4_order_or_throw(
        authority_, test_support::create_m4_reference_order_request_or_throw());
    if (submitted.disposition() != execution::SubmitDisposition::WriteInitiated ||
        !submitted.order_id()) {
      throw std::logic_error{"failed genuine M4 first-seen planner submission"};
    }
    order_id_ = *submitted.order_id();

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Borrow the installed reconciler after the constructor proved its presence.
  [[nodiscard]] const runtime::PrivateOrderReconciler& reconciler_or_throw() const {
    const auto* const value = authority_.submission->private_order_reconciler();
    if (value == nullptr) {
      throw std::logic_error{"missing installed M4 first-seen planner"};
    }
    return *value;
  }

  // --------------------------------------------------------
  // Borrow the genuine retained OMS row that supplies correlation authority.
  [[nodiscard]] const oms::OutboundOrderRecord& order_or_throw() const {
    const auto* const value = authority_.submission->outbound_oms().find_order(order_id_or_throw());
    if (value == nullptr) {
      throw std::logic_error{"missing genuine M4 first-seen planner order"};
    }
    return *value;
  }

  // --------------------------------------------------------
  // Borrow the genuine local order identity after constructor validation.
  [[nodiscard]] const model::OrderId& order_id_or_throw() const {
    if (!order_id_) {
      throw std::logic_error{"missing genuine M4 first-seen planner order identity"};
    }
    return *order_id_;
  }

  // --------------------------------------------------------
  // Build one receive-time-free venue origin under the genuine order's source scope.
  [[nodiscard]] oms::VenuePrivateIngressOrigin
  create_venue_ingress_origin_or_throw(std::uint8_t event_byte,
                                       std::uint64_t source_time = 100U) const {
    return create_venue_ingress_origin_for_scope_or_throw(
        order_or_throw().provenance().logical_account_id, order_or_throw().provenance().venue_id,
        event_byte, source_time);
  }

  // --------------------------------------------------------
  // Build a receive-time-free venue origin for an explicit isolation or foreign-authority test.
  [[nodiscard]] static oms::VenuePrivateIngressOrigin
  create_venue_ingress_origin_for_scope_or_throw(model::LogicalAccountId logical_account_id,
                                                 model::VenueId venue_id, std::uint8_t event_byte,
                                                 std::uint64_t source_time = 100U) {
    return oms::VenuePrivateIngressOrigin{
        oms::VenuePrivateEventKey{
            std::move(venue_id), std::move(logical_account_id),
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U),
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event_byte)},
        model::SourceTimestamp{source_time}};
  }

  // --------------------------------------------------------
  // Build one reconciliation origin at a deterministic authoritative row and receive time.
  [[nodiscard]] oms::ReconciliationPrivateEventOrigin
  create_reconciliation_origin_or_throw(std::uint64_t row) const {
    return oms::ReconciliationPrivateEventOrigin{
        test_support::create_m4_reconciliation_epoch_or_throw(),
        test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
        test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(row),
        model::SourceTimestamp{100U}, model::ReceiveTimestamp{200U}};
  }

  // --------------------------------------------------------
  // Retain the complete owner chain, a matching source factory, and one genuine local identity.
  test_support::M4OwnerTestAuthority authority_;
  runtime::PrivateOrderEventFactory factory_;
  std::optional<model::OrderId> order_id_;
};

// ########################################################################

// --------------------------------------------------------
// Build one nonempty raw locator from exact local/exchange presence or throw on invalid absence.
[[nodiscard]] oms::PrivateOrderLocator
create_private_order_locator_or_throw(std::optional<model::OrderId> local_order_id,
                                      std::optional<oms::ExchangeOrderId> exchange_order_id) {
  return extract_result_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
      std::move(local_order_id), std::move(exchange_order_id)));
}

// --------------------------------------------------------

// --------------------------------------------------------
// Copy one receive-time-free semantic value out of its temporary producer attempt.
[[nodiscard]] oms::PrivateEventIngressSemanticValue
copy_ingress_semantic_value(const oms::PrivateOrderIngressAttempt& attempt) {
  return attempt.semantic_value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Run the exact semantic-value planner twice and prove deterministic owned results plus complete
// owner non-mutation, including all three empty identity-table counts.
[[nodiscard]] FirstSeenPlan
require_plan_without_mutation(const FirstSeenPlannerFixture& fixture,
                              const oms::PrivateEventIngressSemanticValue& input) {
  const auto before = create_owner_planning_snapshot_or_throw(fixture.authority_);
  CHECK(before.event_identity_record_count == 0U);
  CHECK(before.trade_identity_record_count == 0U);
  CHECK(before.exchange_order_mapping_count == 0U);
  auto first = fixture.reconciler_or_throw().derive_first_seen_authoritative_identity_plan(input);
  auto second = fixture.reconciler_or_throw().derive_first_seen_authoritative_identity_plan(input);
  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value() == second.value());
  CHECK(first.value().ingress_semantic_value() == input);
  CHECK(first.value().event_key() ==
        oms::PrivateEventRegistryKey::from_ingress_semantic_value(input));
  CHECK(create_owner_planning_snapshot_or_throw(fixture.authority_) == before);
  return std::move(first).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Assert every known-order provenance field against the genuine retained owner row independently.
void check_known_resolution(const oms::PrivateEventResolution& resolution,
                            const oms::OutboundOrderRecord& row,
                            const model::M4RootProvenance& expected_root) {
  CHECK(resolution.resolution_kind() == oms::PrivateEventResolutionKind::Known);
  const auto* const known = resolution.known_resolution();
  REQUIRE(known != nullptr);
  CHECK(known->order_id == row.order_id());
  CHECK(known->provenance.root() == expected_root);
  REQUIRE(known->provenance.subject());
  const auto& subject = *known->provenance.subject();
  CHECK(subject.logical_account_id() == row.provenance().logical_account_id);
  CHECK(subject.venue_id() == row.provenance().venue_id);
  REQUIRE(subject.firm_id());
  REQUIRE(subject.desk_id());
  REQUIRE(subject.bot_id());
  REQUIRE(subject.strategy_id());
  REQUIRE(subject.route());
  REQUIRE(subject.instrument());
  CHECK(*subject.firm_id() == row.provenance().firm_id);
  CHECK(*subject.desk_id() == row.provenance().desk_id);
  CHECK(*subject.bot_id() == row.provenance().bot_id);
  CHECK(*subject.strategy_id() == row.provenance().strategy_id);
  CHECK(subject.route()->route_id == row.provenance().route_id);
  CHECK(subject.route()->route_revision == row.provenance().route_revision);
  CHECK(subject.instrument()->instrument_id == row.provenance().instrument_id);
  CHECK(subject.instrument()->metadata_revision == row.provenance().metadata_revision);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Assert one successful conflict plan retains only its exact safety reason and no trade identity.
void check_conflict_plan(const FirstSeenPlan& plan, risk::AccountSafetyReason expected_reason) {
  const auto* const conflict =
      std::get_if<runtime::ConflictFirstSeenPrivateCorrelationPlan>(&plan.correlation_plan());
  REQUIRE(conflict != nullptr);
  CHECK(conflict->resolution.resolution_kind() == oms::PrivateEventResolutionKind::Conflict);
  const auto* const resolution =
      std::get_if<oms::ConflictPrivateEventResolution>(&conflict->resolution.resolution_value());
  REQUIRE(resolution != nullptr);
  CHECK(resolution->reason == expected_reason);
  CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(plan.trade_plan()));
  REQUIRE(plan.preliminary_safety_reason());
  CHECK(*plan.preliminary_safety_reason() == expected_reason);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Assert every account-scoped trade key and closed economic tuple field independently.
void check_trade_identity_fields(
    const runtime::FirstSeenPrivateTradeIdentityPlan& trade, const model::VenueId& venue_id,
    const model::LogicalAccountId& logical_account_id, const oms::TradeId& trade_id,
    const model::InstrumentId& instrument_id, model::InstrumentMetadataRevision metadata_revision,
    model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
    model::Price execution_price) {
  CHECK(trade.key.venue_id == venue_id);
  CHECK(trade.key.logical_account_id == logical_account_id);
  CHECK(trade.key.trade_id == trade_id);
  CHECK(trade.semantic_value.instrument_id() == instrument_id);
  CHECK(trade.semantic_value.metadata_revision() == metadata_revision);
  CHECK(trade.semantic_value.incremental_quantity() == incremental_quantity);
  CHECK(trade.semantic_value.cumulative_quantity() == cumulative_quantity);
  CHECK(trade.semantic_value.execution_price() == execution_price);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Derive one acknowledgement semantic under a temporary independently sealed foreign root.
[[nodiscard]] oms::PrivateEventIngressSemanticValue
create_foreign_acknowledgement_semantic_or_throw(configuration::StartupConfigurationParams params,
                                                 const model::LogicalAccountId& logical_account_id,
                                                 const model::VenueId& venue_id,
                                                 std::uint8_t event_byte) {
  auto foreign = test_support::create_m4_owner_test_authority_or_throw(std::move(params));
  auto factory = create_private_order_event_factory_or_throw(foreign);
  auto attempt = extract_result_or_throw(factory.create_venue_acknowledgement_attempt(
      FirstSeenPlannerFixture::create_venue_ingress_origin_for_scope_or_throw(logical_account_id,
                                                                              venue_id, event_byte),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(event_byte),
      std::nullopt));
  return copy_ingress_semantic_value(attempt);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Known local-only and local-plus-unbound-exchange inputs select the exact reachable known rows.
TEST_CASE("M4 first-seen planner derives reachable known correlation rows",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto cancellation_exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x65U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A known local locator without exchange identity resolves exactly and proposes no mapping.
  auto local_only_attempt = extract_result_or_throw(fixture.factory_.create_venue_rejection_attempt(
      fixture.create_venue_ingress_origin_or_throw(1U),
      create_private_order_locator_or_throw(fixture.order_id_or_throw(), std::nullopt),
      oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  const auto local_only =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(local_only_attempt));
  const auto* const local_known =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&local_only.correlation_plan());
  REQUIRE(local_known != nullptr);
  check_known_resolution(local_known->resolution, fixture.order_or_throw(),
                         fixture.authority_.m4_policy.root_provenance());
  CHECK_FALSE(local_known->candidate_mapping);
  CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(
      local_only.trade_plan()));
  CHECK_FALSE(local_only.preliminary_safety_reason());

  // ++++++++++++++++++++++++++++++++++++++++
  // A known local locator plus an unbound exchange identity returns one detached candidate only.
  const auto with_exchange = [&]() {
    auto attempt = extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
        fixture.create_venue_ingress_origin_or_throw(2U), exchange_order_id,
        fixture.order_id_or_throw()));
    return require_plan_without_mutation(fixture, attempt.semantic_value());
  }();
  const auto* const exchange_known =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&with_exchange.correlation_plan());
  REQUIRE(exchange_known != nullptr);
  check_known_resolution(exchange_known->resolution, fixture.order_or_throw(),
                         fixture.authority_.m4_policy.root_provenance());
  REQUIRE(exchange_known->candidate_mapping);
  CHECK(exchange_known->candidate_mapping->exchange_order_key ==
        oms::ExchangeOrderKey{fixture.order_or_throw().provenance().venue_id,
                              fixture.order_or_throw().provenance().logical_account_id,
                              exchange_order_id});
  CHECK(exchange_known->candidate_mapping->order_id == fixture.order_id_or_throw());
  CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(
      with_exchange.trade_plan()));
  CHECK_FALSE(with_exchange.preliminary_safety_reason());

  // ++++++++++++++++++++++++++++++++++++++++
  // A known cancellation result uses the same locator table, proposes its unbound exchange mapping,
  // and stops before trade derivation without selecting preliminary containment.
  auto cancellation_attempt =
      extract_result_or_throw(fixture.factory_.create_venue_cancellation_result_attempt(
          fixture.create_venue_ingress_origin_or_throw(11U),
          create_private_order_locator_or_throw(fixture.order_id_or_throw(),
                                                cancellation_exchange_order_id),
          oms::CancellationResult::CancelRejected, std::nullopt));
  const auto cancellation =
      require_plan_without_mutation(fixture, cancellation_attempt.semantic_value());
  const auto* const cancellation_known =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&cancellation.correlation_plan());
  REQUIRE(cancellation_known != nullptr);
  check_known_resolution(cancellation_known->resolution, fixture.order_or_throw(),
                         fixture.authority_.m4_policy.root_provenance());
  REQUIRE(cancellation_known->candidate_mapping);
  CHECK(cancellation_known->candidate_mapping->exchange_order_key ==
        oms::ExchangeOrderKey{fixture.order_or_throw().provenance().venue_id,
                              fixture.order_or_throw().provenance().logical_account_id,
                              cancellation_exchange_order_id});
  CHECK(cancellation_known->candidate_mapping->order_id == fixture.order_id_or_throw());
  CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(
      cancellation.trade_plan()));
  CHECK_FALSE(cancellation.preliminary_safety_reason());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Every unproved locator combination reachable while mapping storage is empty stays unknown.
TEST_CASE("M4 first-seen planner preserves reachable unknown locator combinations",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto unknown_order = test_support::create_m4_order_id_or_throw(900U);
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x62U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Absent local plus unbound exchange identity proves no local ownership.
  auto exchange_only_attempt =
      extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
          fixture.create_venue_ingress_origin_or_throw(3U), exchange_order_id, std::nullopt));
  const auto exchange_only =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(exchange_only_attempt));

  // ++++++++++++++++++++++++++++++++++++++++
  // An unknown local identity remains unknown with either absent or unbound exchange identity.
  auto local_only_attempt = extract_result_or_throw(fixture.factory_.create_venue_rejection_attempt(
      fixture.create_venue_ingress_origin_or_throw(4U),
      create_private_order_locator_or_throw(unknown_order, std::nullopt),
      oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  auto both_attempt = extract_result_or_throw(fixture.factory_.create_venue_rejection_attempt(
      fixture.create_venue_ingress_origin_or_throw(5U),
      create_private_order_locator_or_throw(unknown_order, exchange_order_id),
      oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  const auto local_only =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(local_only_attempt));
  const auto both =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(both_attempt));

  // ++++++++++++++++++++++++++++++++++++++++
  // All three rows retain sealed Unknown resolution, no trade, and exact UnknownOrder containment.
  for (const auto* const plan : {&exchange_only, &local_only, &both}) {
    const auto* const unknown =
        std::get_if<runtime::UnknownFirstSeenPrivateCorrelationPlan>(&plan->correlation_plan());
    REQUIRE(unknown != nullptr);
    CHECK(unknown->resolution.resolution_kind() == oms::PrivateEventResolutionKind::Unknown);
    CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(plan->trade_plan()));
    REQUIRE(plan->preliminary_safety_reason());
    CHECK(*plan->preliminary_safety_reason() == risk::AccountSafetyReason::UnknownOrder);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Known and unknown executions retain exact trade tuples; known absent/matching source-side inputs
// converge while unknown authoritative-side presence remains part of its closed tuple.
TEST_CASE("M4 first-seen planner derives known and unknown execution identities",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto trade_id = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);
  const auto unknown_order = test_support::create_m4_order_id_or_throw(901U);
  const auto known_exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x63U);
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x64U);
  const auto quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto price = test_support::create_m4_decimal_or_throw<model::Price>(99);
  const auto& row = fixture.order_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue-side absence and reconciliation's matching side derive one identical known trade value.
  auto venue_attempt = extract_result_or_throw(fixture.factory_.create_venue_execution_attempt(
      fixture.create_venue_ingress_origin_or_throw(6U),
      create_private_order_locator_or_throw(fixture.order_id_or_throw(), known_exchange_order_id),
      trade_id, row.provenance().instrument_id, row.provenance().metadata_revision, quantity,
      quantity, price, std::nullopt));
  auto reconciliation_input =
      extract_result_or_throw(fixture.factory_.normalize_reconciliation_execution(
          fixture.create_reconciliation_origin_or_throw(1U), row.provenance().logical_account_id,
          row.provenance().venue_id,
          create_private_order_locator_or_throw(fixture.order_id_or_throw(), std::nullopt),
          trade_id, row.provenance().instrument_id, row.provenance().metadata_revision, quantity,
          quantity, price, execution::OrderSide::Buy));
  const auto venue_plan =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(venue_attempt));
  const auto reconciliation_plan = require_plan_without_mutation(
      fixture, oms::PrivateEventIngressSemanticValue::from_normalized_input(reconciliation_input));
  const auto* const venue_trade =
      std::get_if<runtime::FirstSeenPrivateTradeIdentityPlan>(&venue_plan.trade_plan());
  const auto* const reconciliation_trade =
      std::get_if<runtime::FirstSeenPrivateTradeIdentityPlan>(&reconciliation_plan.trade_plan());
  REQUIRE(venue_trade != nullptr);
  REQUIRE(reconciliation_trade != nullptr);
  const auto* const venue_correlation =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&venue_plan.correlation_plan());
  REQUIRE(venue_correlation != nullptr);
  REQUIRE(venue_correlation->candidate_mapping);
  CHECK(venue_correlation->candidate_mapping->exchange_order_key ==
        oms::ExchangeOrderKey{row.provenance().venue_id, row.provenance().logical_account_id,
                              known_exchange_order_id});
  CHECK(venue_correlation->candidate_mapping->order_id == fixture.order_id_or_throw());
  check_trade_identity_fields(*venue_trade, row.provenance().venue_id,
                              row.provenance().logical_account_id, trade_id,
                              row.provenance().instrument_id, row.provenance().metadata_revision,
                              quantity, quantity, price);
  check_trade_identity_fields(*reconciliation_trade, row.provenance().venue_id,
                              row.provenance().logical_account_id, trade_id,
                              row.provenance().instrument_id, row.provenance().metadata_revision,
                              quantity, quantity, price);
  CHECK(venue_trade->semantic_value == reconciliation_trade->semantic_value);
  const auto* const known_trade = std::get_if<oms::KnownPrivateTradeResolution>(
      &venue_trade->semantic_value.resolution_value());
  REQUIRE(known_trade != nullptr);
  CHECK(known_trade->order_id == fixture.order_id_or_throw());
  CHECK(known_trade->canonical_side == execution::OrderSide::Buy);
  CHECK_FALSE(venue_plan.preliminary_safety_reason());
  CHECK_FALSE(reconciliation_plan.preliminary_safety_reason());

  // ++++++++++++++++++++++++++++++++++++++++
  // Unknown venue/reconciliation executions retain locator identity and source-side presence.
  const auto unknown_trade_absent =
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x72U);
  const auto unknown_trade_buy =
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x73U);
  auto unknown_venue_attempt =
      extract_result_or_throw(fixture.factory_.create_venue_execution_attempt(
          fixture.create_venue_ingress_origin_or_throw(7U),
          create_private_order_locator_or_throw(unknown_order, exchange_order_id),
          unknown_trade_absent, row.provenance().instrument_id, row.provenance().metadata_revision,
          quantity, quantity, price, std::nullopt));
  auto unknown_reconciliation_input =
      extract_result_or_throw(fixture.factory_.normalize_reconciliation_execution(
          fixture.create_reconciliation_origin_or_throw(2U), row.provenance().logical_account_id,
          row.provenance().venue_id,
          create_private_order_locator_or_throw(unknown_order, exchange_order_id),
          unknown_trade_buy, row.provenance().instrument_id, row.provenance().metadata_revision,
          quantity, quantity, price, execution::OrderSide::Buy));
  const auto unknown_venue_plan =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(unknown_venue_attempt));
  const auto unknown_reconciliation_plan = require_plan_without_mutation(
      fixture,
      oms::PrivateEventIngressSemanticValue::from_normalized_input(unknown_reconciliation_input));
  const auto* const absent_trade =
      std::get_if<runtime::FirstSeenPrivateTradeIdentityPlan>(&unknown_venue_plan.trade_plan());
  const auto* const buy_trade = std::get_if<runtime::FirstSeenPrivateTradeIdentityPlan>(
      &unknown_reconciliation_plan.trade_plan());
  REQUIRE(absent_trade != nullptr);
  REQUIRE(buy_trade != nullptr);
  check_trade_identity_fields(*absent_trade, row.provenance().venue_id,
                              row.provenance().logical_account_id, unknown_trade_absent,
                              row.provenance().instrument_id, row.provenance().metadata_revision,
                              quantity, quantity, price);
  check_trade_identity_fields(*buy_trade, row.provenance().venue_id,
                              row.provenance().logical_account_id, unknown_trade_buy,
                              row.provenance().instrument_id, row.provenance().metadata_revision,
                              quantity, quantity, price);
  const auto* const absent_unknown = std::get_if<oms::UnknownPrivateTradeResolution>(
      &absent_trade->semantic_value.resolution_value());
  const auto* const buy_unknown = std::get_if<oms::UnknownPrivateTradeResolution>(
      &buy_trade->semantic_value.resolution_value());
  REQUIRE(absent_unknown != nullptr);
  REQUIRE(buy_unknown != nullptr);
  CHECK(absent_unknown->locator ==
        create_private_order_locator_or_throw(unknown_order, exchange_order_id));
  CHECK_FALSE(absent_unknown->canonical_side);
  CHECK(buy_unknown->locator ==
        create_private_order_locator_or_throw(unknown_order, exchange_order_id));
  REQUIRE(buy_unknown->canonical_side);
  CHECK(*buy_unknown->canonical_side == execution::OrderSide::Buy);
  REQUIRE(unknown_venue_plan.preliminary_safety_reason());
  REQUIRE(unknown_reconciliation_plan.preliminary_safety_reason());
  CHECK(*unknown_venue_plan.preliminary_safety_reason() == risk::AccountSafetyReason::UnknownTrade);
  CHECK(*unknown_reconciliation_plan.preliminary_safety_reason() ==
        risk::AccountSafetyReason::UnknownTrade);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Known source-side contradiction suppresses its otherwise reachable candidate mapping and trade.
TEST_CASE("M4 first-seen planner contains known source-side contradiction",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto& row = fixture.order_or_throw();
  const auto exchange_order_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x64U);
  auto attempt = extract_result_or_throw(fixture.factory_.create_venue_execution_attempt(
      fixture.create_venue_ingress_origin_or_throw(8U),
      create_private_order_locator_or_throw(fixture.order_id_or_throw(), exchange_order_id),
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x74U),
      row.provenance().instrument_id, row.provenance().metadata_revision,
      test_support::create_m4_decimal_or_throw<model::Quantity>(1),
      test_support::create_m4_decimal_or_throw<model::Quantity>(1),
      test_support::create_m4_decimal_or_throw<model::Price>(99), execution::OrderSide::Sell));
  const auto plan = require_plan_without_mutation(fixture, copy_ingress_semantic_value(attempt));
  const auto* const known =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&plan.correlation_plan());
  REQUIRE(known != nullptr);
  check_known_resolution(known->resolution, row, fixture.authority_.m4_policy.root_provenance());
  CHECK_FALSE(known->candidate_mapping);
  CHECK(std::holds_alternative<runtime::FirstSeenPrivateTradeSourceSideConflictPlan>(
      plan.trade_plan()));
  REQUIRE(plan.preliminary_safety_reason());
  CHECK(*plan.preliminary_safety_reason() == risk::AccountSafetyReason::AuthoritativeContradiction);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Foreign root, unknown account, wrong venue, and peer-account locator mismatch remain contained
// without adopting ownership or crossing the bound owner's source scope.
TEST_CASE("M4 first-seen planner contains provenance and locator source mismatches",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto baseline_account = fixture.order_or_throw().provenance().logical_account_id;
  const auto baseline_venue = fixture.order_or_throw().provenance().venue_id;

  // ++++++++++++++++++++++++++++++++++++++++
  // A coherent foreign revision preserves valid source shape but cannot match the bound M4 root.
  auto foreign_root_params =
      test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  foreign_root_params.revision =
      extract_result_or_throw(foreign_root_params.revision.derive_next_revision());
  const auto foreign_root_input = create_foreign_acknowledgement_semantic_or_throw(
      std::move(foreign_root_params), baseline_account, baseline_venue, 0x81U);
  check_conflict_plan(require_plan_without_mutation(fixture, foreign_root_input),
                      risk::AccountSafetyReason::ProvenanceMismatch);

  // ++++++++++++++++++++++++++++++++++++++++
  // Same-root source normalization cannot make an account absent from the bound configuration
  // valid.
  const auto unknown_account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.deribit-testnet-unknown");
  auto unknown_account_attempt =
      extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
          FirstSeenPlannerFixture::create_venue_ingress_origin_for_scope_or_throw(
              unknown_account, baseline_venue, 0x82U),
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x82U),
          std::nullopt));
  const auto unknown_account_input = copy_ingress_semantic_value(unknown_account_attempt);
  check_conflict_plan(require_plan_without_mutation(fixture, unknown_account_input),
                      risk::AccountSafetyReason::ProvenanceMismatch);

  // ++++++++++++++++++++++++++++++++++++++++
  // Same-root source normalization likewise cannot bless a known account at an unbound venue.
  const auto peer_account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.deribit-testnet-subsidiary");
  const auto wrong_venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("kraken");
  auto wrong_venue_attempt =
      extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
          FirstSeenPlannerFixture::create_venue_ingress_origin_for_scope_or_throw(
              peer_account, wrong_venue, 0x83U),
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x83U),
          std::nullopt));
  const auto wrong_venue_input = copy_ingress_semantic_value(wrong_venue_attempt);
  check_conflict_plan(require_plan_without_mutation(fixture, wrong_venue_input),
                      risk::AccountSafetyReason::ProvenanceMismatch);

  // ++++++++++++++++++++++++++++++++++++++++
  // A same-root peer account is valid source authority but cannot claim the baseline firm's row.
  const auto* const peer_binding =
      fixture.authority_.configuration.find_logical_account(peer_account);
  REQUIRE(peer_binding != nullptr);
  auto peer_attempt = extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
      FirstSeenPlannerFixture::create_venue_ingress_origin_for_scope_or_throw(
          peer_account, peer_binding->venue_id, 0x84U),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x84U),
      fixture.order_id_or_throw()));
  const auto peer_plan =
      require_plan_without_mutation(fixture, copy_ingress_semantic_value(peer_attempt));
  check_conflict_plan(peer_plan, risk::AccountSafetyReason::CorrelationConflict);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Local account and private-source facts cannot enter the authoritative order-event planner seam,
// and both rejections leave the complete owner unchanged.
TEST_CASE("M4 first-seen planner rejects local account and private-source seams",
          "[runtime][m4][correlation][planner]") {
  FirstSeenPlannerFixture fixture;
  const auto& row = fixture.order_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Create valid local account and source attempts whose origin is outside this
  // venue/reconciliation boundary.
  auto timeout = extract_result_or_throw(fixture.factory_.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{test_support::create_m4_local_event_id_or_throw(1U),
                                     model::SourceTimestamp{100U}},
      row.provenance().logical_account_id, row.provenance().venue_id));
  auto disconnect = extract_result_or_throw(fixture.factory_.create_disconnect_attempt(
      oms::LocalPrivateIngressOrigin{test_support::create_m4_local_event_id_or_throw(2U),
                                     model::SourceTimestamp{101U}},
      row.provenance().logical_account_id, row.provenance().venue_id,
      test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x91U)));
  const auto before = create_owner_planning_snapshot_or_throw(fixture.authority_);
  for (const auto* const input : {&timeout.semantic_value(), &disconnect.semantic_value()}) {
    const auto rejected =
        fixture.reconciler_or_throw().derive_first_seen_authoritative_identity_plan(*input);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::InvalidPrivateEvent);
    CHECK(rejected.error().context.field == "private_event.authoritative_origin");
  }

  CHECK(create_owner_planning_snapshot_or_throw(fixture.authority_) == before);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// The returned plan owns every input and owner-derived value after both producer input and bound
// owner lifetimes end; no pointer into either temporary is required for later inspection.
TEST_CASE("M4 first-seen planner returns detached values across input and owner lifetime",
          "[runtime][m4][correlation][planner]") {
  const auto detached = []() {
    FirstSeenPlannerFixture fixture;
    auto attempt = extract_result_or_throw(fixture.factory_.create_venue_acknowledgement_attempt(
        fixture.create_venue_ingress_origin_or_throw(10U),
        test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x92U),
        fixture.order_id_or_throw()));
    return require_plan_without_mutation(fixture, attempt.semantic_value());
  }();

  // ++++++++++++++++++++++++++++++++++++++++
  // Inspect complete copied correlation and ingress identity after the fixture owner was destroyed.
  const auto* const known =
      std::get_if<runtime::KnownFirstSeenPrivateCorrelationPlan>(&detached.correlation_plan());
  REQUIRE(known != nullptr);
  REQUIRE(known->resolution.known_resolution() != nullptr);
  REQUIRE(known->candidate_mapping);
  CHECK(known->candidate_mapping->order_id == known->resolution.known_resolution()->order_id);
  CHECK(detached.event_key() == oms::PrivateEventRegistryKey::from_ingress_semantic_value(
                                    detached.ingress_semantic_value()));
  CHECK(
      std::holds_alternative<runtime::FirstSeenPrivateTradeNotReachedPlan>(detached.trade_plan()));
  CHECK_FALSE(detached.preliminary_safety_reason());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
