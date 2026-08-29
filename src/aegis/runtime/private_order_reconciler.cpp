// Purpose: validate one submission-owner/M4-policy/recovery relationship, preallocate empty
// identity storage, and derive detached first-seen plans without mutation.

#include "private_order_reconciler.hpp"

#include "aegis/model/domain_error.hpp"
#include "submission_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Return an install-time failure through the stable M4 policy domain without publishing a child.
template <typename Value>
[[nodiscard]] model::Result<Value> private_order_reconciler_failure_from_field(std::string field) {
  return model::Result<Value>::failure(
      model::DomainError::at_field(model::DomainErrorCode::InvalidM4Policy, std::move(field)));
}

// --------------------------------------------------------
// Return a planner failure in the exact requested stable domain without changing any owner state.
template <typename Value>
[[nodiscard]] model::Result<Value>
private_identity_planning_failure_from_field(model::DomainErrorCode code, std::string_view field) {
  return model::Result<Value>::failure(model::DomainError::at_field(code, std::string{field}));
}

// ########################################################################
// Carry only locator and source-instrument facts derived from an accepted authoritative shape. The
// optional execution pointer borrows the planner input for this one read-only call.
struct AuthoritativeOrderEventShape {
  std::optional<model::OrderId> local_order_id;
  std::optional<oms::ExchangeOrderId> exchange_order_id;
  std::optional<model::InstrumentId> source_instrument_id;
  const oms::ExecutionPayload* execution;
};

// ########################################################################

// --------------------------------------------------------
// Validate venue/reconciliation order scope and the four currently admitted authoritative
// payloads before deriving any key, provenance, correlation, or trade value.
[[nodiscard]] model::Result<AuthoritativeOrderEventShape> derive_authoritative_order_event_shape(
    const oms::PrivateEventIngressSemanticValue& ingress_semantic_value) {
  const auto* const venue_origin =
      std::get_if<oms::VenuePrivateIngressOrigin>(&ingress_semantic_value.origin());
  const bool is_reconciliation_origin =
      std::holds_alternative<oms::ReconciliationPrivateIngressOrigin>(
          ingress_semantic_value.origin());
  if (venue_origin == nullptr && !is_reconciliation_origin) {
    return private_identity_planning_failure_from_field<AuthoritativeOrderEventShape>(
        model::DomainErrorCode::InvalidPrivateEvent, "private_event.authoritative_origin");
  }
  if (ingress_semantic_value.subject_scope() != oms::PrivateEventSubjectScope::Order) {
    return private_identity_planning_failure_from_field<AuthoritativeOrderEventShape>(
        model::DomainErrorCode::InvalidPrivateEvent, "private_event.authoritative_order_scope");
  }
  if (venue_origin != nullptr &&
      (venue_origin->event_key.logical_account_id != ingress_semantic_value.logical_account_id() ||
       venue_origin->event_key.venue_id != ingress_semantic_value.venue_id())) {
    return private_identity_planning_failure_from_field<AuthoritativeOrderEventShape>(
        model::DomainErrorCode::InvalidPrivateEvent, "private_event.authoritative_source_scope");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Acknowledgement has a mandatory exchange identity and may carry reconciliation-only
  // source-instrument provenance that its payload deliberately does not repeat.
  if (const auto* const acknowledgement =
          std::get_if<oms::ExchangeAcknowledgedPayload>(&ingress_semantic_value.payload())) {
    std::optional<model::InstrumentId> source_instrument_id;
    if (is_reconciliation_origin && ingress_semantic_value.provenance().subject().has_value() &&
        ingress_semantic_value.provenance().subject()->instrument().has_value()) {
      source_instrument_id =
          ingress_semantic_value.provenance().subject()->instrument()->instrument_id;
    }
    return model::Result<AuthoritativeOrderEventShape>::success(AuthoritativeOrderEventShape{
        acknowledgement->local_order_locator, acknowledgement->exchange_order_id,
        std::move(source_instrument_id), nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Rejection correlation uses its complete nonempty raw locator and no instrument claim.
  if (const auto* const rejection =
          std::get_if<oms::ExchangeRejectedPayload>(&ingress_semantic_value.payload())) {
    return model::Result<AuthoritativeOrderEventShape>::success(AuthoritativeOrderEventShape{
        rejection->locator.local_order_id(), rejection->locator.exchange_order_id(), std::nullopt,
        nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Execution retains raw locator and instrument facts; reconciliation requires an authoritative
  // source side before owner correlation.
  if (const auto* const execution =
          std::get_if<oms::ExecutionPayload>(&ingress_semantic_value.payload())) {
    if (is_reconciliation_origin && !execution->source_side.has_value()) {
      return private_identity_planning_failure_from_field<AuthoritativeOrderEventShape>(
          model::DomainErrorCode::InvalidPrivateEvent,
          "private_event.authoritative_execution_side");
    }
    return model::Result<AuthoritativeOrderEventShape>::success(AuthoritativeOrderEventShape{
        execution->locator.local_order_id(), execution->locator.exchange_order_id(),
        execution->instrument_id, execution});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation-result correlation uses its complete nonempty locator and no trade semantics.
  if (const auto* const cancellation =
          std::get_if<oms::CancellationResultPayload>(&ingress_semantic_value.payload())) {
    return model::Result<AuthoritativeOrderEventShape>::success(AuthoritativeOrderEventShape{
        cancellation->locator.local_order_id(), cancellation->locator.exchange_order_id(),
        std::nullopt, nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Local commands/failures and observation payloads are outside this first authoritative slice.
  return private_identity_planning_failure_from_field<AuthoritativeOrderEventShape>(
      model::DomainErrorCode::InvalidPrivateEvent, "private_event.authoritative_payload");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Return whether all three fixed table capacities remain exactly representable by their u32
// inspection surface, even if a future policy construction path regresses its accepted bound.
[[nodiscard]] bool are_private_identity_storage_capacities_implementable(
    const M4PolicyCapacities& capacities) noexcept {
  return std::in_range<std::uint32_t>(capacities.max_event_identity_records) &&
         std::in_range<std::uint32_t>(capacities.max_trade_identity_records) &&
         std::in_range<std::uint32_t>(capacities.max_exchange_order_mappings);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate inherited and recovery authority, allocate all empty identity slots, and wrap the child
// while the bootstrap remains intact for the coordinator's later no-fail consumption.
model::Result<PrivateOrderReconciler::PreparedRecoveryBoundPrivateOrderReconciler>
PrivateOrderReconciler::prepare_recovery_bound_private_order_reconciler(
    const SubmissionCoordinator& owner, const configuration::StartupConfiguration& configuration,
    const M4Policy& policy, const recovery::RecoveryBootstrap& recovery_bootstrap) try {
  using PreparedReconciler = PreparedRecoveryBoundPrivateOrderReconciler;
  const auto& root = policy.root_provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind to exact risk, submission, runtime, and configuration identities using immutable public
  // owner views; this read-only child receives no mutation friendship from the coordinator.
  if (root.risk_policy_fingerprint() != owner.reservations().policy().fingerprint().bytes() ||
      root.risk_policy_revision() != owner.reservations().policy().revision() ||
      root.submission_policy_fingerprint() != owner.policy().fingerprint().bytes() ||
      root.runtime_policy_fingerprint() != owner.policy().runtime_policy_fingerprint() ||
      root.configuration_fingerprint() != owner.policy().configuration_fingerprint()) {
    return private_order_reconciler_failure_from_field<PreparedReconciler>(
        "private_order_reconciler.owner_provenance");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Full root equality binds the fake recovery medium to this exact owner and every M4 capacity.
  if (recovery_bootstrap.root_provenance_ != root) {
    return model::Result<PreparedReconciler>::failure(
        model::DomainError::at_field(model::DomainErrorCode::RecoveryProvenanceMismatch,
                                     "private_order_reconciler.recovery_root_provenance"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject a moved or internally incoherent bootstrap before deriving configuration state.
  if (!recovery_bootstrap.lease_ || recovery_bootstrap.runtime_epoch_id_.order_namespace() !=
                                        recovery_bootstrap.registered_order_namespace_) {
    return model::Result<PreparedReconciler>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidJournalState,
                                     "private_order_reconciler.recovery_bootstrap_state"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the trusted resolver to prove configuration/organization agreement before copying its
  // source-normalization and planning authority into the child. Preserve allocation as the earlier
  // capacity failure class instead of misreporting it as provenance disagreement.
  auto resolver = M4ProvenanceResolver::create(configuration, policy);
  if (!resolver) {
    const auto* const failure_field =
        resolver.error().context.field == "m4_provenance.capacity_allocation"
            ? "private_order_reconciler.capacity_allocation"
            : "private_order_reconciler.configuration_provenance";
    return private_order_reconciler_failure_from_field<PreparedReconciler>(failure_field);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Recheck the implementation widths and the exact owner's permanent mapping lower bound before
  // any fixed storage is allocated or installed.
  const auto& capacities = policy.capacities();
  if (!are_private_identity_storage_capacities_implementable(capacities) ||
      capacities.max_exchange_order_mappings < owner.outbound_oms().capacity()) {
    return private_order_reconciler_failure_from_field<PreparedReconciler>(
        "private_order_reconciler.capacities");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize every slot as empty local state; this boundary provides no operation that can
  // populate an event, trade, or mapping record.
  auto event_identity_records = std::vector<std::optional<PrivateEventIdentityRecord>>(
      static_cast<std::size_t>(capacities.max_event_identity_records));
  auto trade_identity_records = std::vector<std::optional<PrivateTradeIdentityRecord>>(
      static_cast<std::size_t>(capacities.max_trade_identity_records));
  auto exchange_order_mappings = std::vector<std::optional<PrivateExchangeOrderMapping>>(
      static_cast<std::size_t>(capacities.max_exchange_order_mappings));
  auto event_factory = PrivateOrderEventFactory{std::move(resolver).value()};
  auto owned_m4_policy = policy;
  const auto recovery_lineage_id = recovery_bootstrap.lineage_id_;
  const auto runtime_epoch_id = recovery_bootstrap.runtime_epoch_id_;
  const auto registered_order_namespace = recovery_bootstrap.registered_order_namespace_;
  auto recovery_identity_lease = recovery_bootstrap.lease_;

  // ++++++++++++++++++++++++++++++++++++++++
  // Interesting syntax: a new-expression obtains storage before evaluating constructor arguments.
  // Allocation failure therefore cannot affect bootstrap authority; the child receives only the
  // copied lease share while the caller-owned bootstrap remains completely intact.
  static_assert(std::is_nothrow_move_constructible_v<model::DeterministicOrderIdProvider>);
  static_assert(std::is_nothrow_move_constructible_v<M4Policy>);
  static_assert(std::is_nothrow_move_constructible_v<PrivateOrderEventFactory>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(event_identity_records)>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(trade_identity_records)>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(exchange_order_mappings)>);
  static_assert(std::is_nothrow_move_constructible_v<
                std::shared_ptr<recovery::detail::FakeJournalLeaseControl>>);
  static_assert(std::is_nothrow_destructible_v<PrivateOrderReconciler>);
  auto reconciler = std::unique_ptr<PrivateOrderReconciler>{new PrivateOrderReconciler{
      owner, std::move(owned_m4_policy), recovery_lineage_id, runtime_epoch_id,
      registered_order_namespace, std::move(event_factory), std::move(event_identity_records),
      std::move(trade_identity_records), std::move(exchange_order_mappings),
      std::move(recovery_identity_lease)}};

  // ++++++++++++++++++++++++++++++++++++++++
  // Wrap the fully allocated child before consuming either bootstrap authority. The returned
  // transaction moves without failure into the coordinator's final publication phase.
  static_assert(
      std::is_nothrow_constructible_v<PreparedReconciler, std::unique_ptr<PrivateOrderReconciler>>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedReconciler>);
  static_assert(std::is_nothrow_move_constructible_v<model::Result<PreparedReconciler>>);
  return model::Result<PreparedReconciler>::success(PreparedReconciler{std::move(reconciler)});

  // ++++++++++++++++++++++++++++++++++++++++
} catch (const std::bad_alloc&) {
  return private_order_reconciler_failure_from_field<
      PrivateOrderReconciler::PreparedRecoveryBoundPrivateOrderReconciler>(
      "private_order_reconciler.capacity_allocation");
} catch (const std::length_error&) {
  return private_order_reconciler_failure_from_field<
      PrivateOrderReconciler::PreparedRecoveryBoundPrivateOrderReconciler>(
      "private_order_reconciler.capacity_allocation");
}

// --------------------------------------------------------
// Consume the previously validated bootstrap through one no-throw transfer after the prepared
// child and its Result wrapper already exist.
PrivateOrderReconciler::ConsumedRecoveryIdentityAuthority
PrivateOrderReconciler::consume_recovery_identity_authority(
    recovery::RecoveryBootstrap&& recovery_bootstrap) noexcept {
  using ConsumedAuthority = ConsumedRecoveryIdentityAuthority;
  static_assert(
      std::is_nothrow_constructible_v<ConsumedAuthority,
                                      std::shared_ptr<recovery::detail::FakeJournalLeaseControl>,
                                      model::DeterministicOrderIdProvider>);
  static_assert(std::is_nothrow_move_constructible_v<ConsumedAuthority>);
  return ConsumedAuthority{std::move(recovery_bootstrap.lease_),
                           std::move(recovery_bootstrap.order_ids_)};
}

// --------------------------------------------------------
// Derive one detached first-seen identity plan after proving this slice's identity storage remains
// wholly pristine. No branch changes the bound owner, OMS, risk, evidence, counts, or fixed slots.
model::Result<FirstSeenAuthoritativePrivateIdentityPlan>
PrivateOrderReconciler::derive_first_seen_authoritative_identity_plan(
    const oms::PrivateEventIngressSemanticValue& ingress_semantic_value) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Inspect every count and every fixed slot before deriving input shape or event identity. The
  // ordered fields give a deterministic internal-state failure precedence.
  const bool event_identity_slots_are_empty =
      std::all_of(event_identity_records_.begin(), event_identity_records_.end(),
                  [](const auto& slot) { return !slot.has_value(); });
  const bool trade_identity_slots_are_empty =
      std::all_of(trade_identity_records_.begin(), trade_identity_records_.end(),
                  [](const auto& slot) { return !slot.has_value(); });
  const bool exchange_mapping_slots_are_empty =
      std::all_of(exchange_order_mappings_.begin(), exchange_order_mappings_.end(),
                  [](const auto& slot) { return !slot.has_value(); });
  if (event_identity_record_count_ != 0U) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_event_identity_count");
  }
  if (trade_identity_record_count_ != 0U) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_trade_identity_count");
  }
  if (exchange_order_mapping_count_ != 0U) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_exchange_mapping_count");
  }
  if (!event_identity_slots_are_empty) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_event_identity_slots");
  }
  if (!trade_identity_slots_are_empty) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_trade_identity_slots");
  }
  if (!exchange_mapping_slots_are_empty) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_exchange_mapping_slots");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept only the authoritative order-scoped source and payload vocabulary in this slice.
  auto shape_result = derive_authoritative_order_event_shape(ingress_semantic_value);
  if (!shape_result) {
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::failure(
        std::move(shape_result).error());
  }
  auto shape = std::move(shape_result).value();
  auto event_key =
      oms::PrivateEventRegistryKey::from_ingress_semantic_value(ingress_semantic_value);

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove exact configured account/venue ownership independently before comparing a recomputed
  // source provenance. Unknown accounts and wrong venues must not pass by recomputing themselves.
  if (!event_factory_.has_configured_account_venue_binding(
          ingress_semantic_value.logical_account_id(), ingress_semantic_value.venue_id())) {
    auto resolution = oms::PrivateEventResolution::create_provenance_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::ProvenanceMismatch});
  }
  const auto expected_source_provenance = event_factory_.derive_authoritative_source_provenance(
      ingress_semantic_value.logical_account_id(), ingress_semantic_value.venue_id(),
      shape.source_instrument_id);
  if (expected_source_provenance != ingress_semantic_value.provenance()) {
    auto resolution = oms::PrivateEventResolution::create_provenance_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::ProvenanceMismatch});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Empty mapping storage makes only the unbound truth-table rows reachable. A known local locator
  // derives from the genuine owner OMS; an absent or syntactically valid unknown locator stays
  // unknown even when an unbound exchange identity is present.
  const oms::OutboundOrderRecord* retained_order = nullptr;
  if (shape.local_order_id.has_value()) {
    retained_order = owner_->outbound_oms().find(*shape.local_order_id);
  }
  if (retained_order == nullptr) {
    auto resolution = oms::PrivateEventResolution::create_unknown_resolution();
    if (shape.execution != nullptr) {
      auto trade_semantic_value =
          oms::PrivateTradeSemanticValue::create_unknown_trade_semantic_value(*shape.execution);
      auto trade_key =
          oms::TradeKey{ingress_semantic_value.venue_id(),
                        ingress_semantic_value.logical_account_id(), shape.execution->trade_id};
      return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
          FirstSeenAuthoritativePrivateIdentityPlan{
              std::move(event_key), ingress_semantic_value,
              UnknownFirstSeenPrivateCorrelationPlan{std::move(resolution)},
              FirstSeenPrivateTradeIdentityPlan{std::move(trade_key),
                                                std::move(trade_semantic_value)},
              risk::AccountSafetyReason::UnknownTrade});
    }
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            UnknownFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::UnknownOrder});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A globally valid local identity cannot cross the event's configured account or venue. This is
  // a successful safety conflict and stops before retained-provenance and trade derivation.
  const auto& retained_provenance = retained_order->provenance();
  if (retained_provenance.logical_account_id != ingress_semantic_value.logical_account_id() ||
      retained_provenance.venue_id != ingress_semantic_value.venue_id()) {
    auto resolution = oms::PrivateEventResolution::create_correlation_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::CorrelationConflict});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject an impossible pre-existing OMS exchange projection because this planner implements only
  // the currently reachable unbound mapping rows and owns no recovery or activation authority.
  const auto private_projection = retained_order->private_projection();
  if (private_projection.exchange_acknowledged ||
      private_projection.exchange_order_id.has_value() ||
      private_projection.exchange_mapping_established_by_execution) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_owner_exchange_projection");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Revalidate every retained admission authority from the real owner row before sealing the full
  // known-order subject. Any disagreement is an internal correlation failure, not source conflict.
  auto known_provenance = event_factory_.derive_retained_order_provenance(*retained_order);
  if (!known_provenance) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_retained_order_provenance");
  }
  auto resolution = oms::PrivateEventResolution::create_known_order_resolution(
      retained_order->order_id(), std::move(known_provenance).value());
  std::optional<PrivateExchangeOrderMapping> candidate_mapping;
  if (shape.exchange_order_id.has_value()) {
    candidate_mapping = PrivateExchangeOrderMapping{
        oms::ExchangeOrderKey{ingress_semantic_value.venue_id(),
                              ingress_semantic_value.logical_account_id(),
                              *shape.exchange_order_id},
        retained_order->order_id()};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Known execution economics derive canonical side only from the retained admission. A supplied
  // contradiction suppresses the uncommitted mapping candidate and produces no trade tuple.
  if (shape.execution != nullptr) {
    const auto canonical_side = retained_order->economics().side;
    if (shape.execution->source_side.has_value() &&
        *shape.execution->source_side != canonical_side) {
      candidate_mapping.reset();
      return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
          FirstSeenAuthoritativePrivateIdentityPlan{
              std::move(event_key), ingress_semantic_value,
              KnownFirstSeenPrivateCorrelationPlan{std::move(resolution),
                                                   std::move(candidate_mapping)},
              FirstSeenPrivateTradeSourceSideConflictPlan{},
              risk::AccountSafetyReason::AuthoritativeContradiction});
    }
    auto trade_semantic_value = oms::PrivateTradeSemanticValue::create_known_trade_semantic_value(
        *shape.execution, retained_order->order_id(), canonical_side);
    auto trade_key =
        oms::TradeKey{ingress_semantic_value.venue_id(),
                      ingress_semantic_value.logical_account_id(), shape.execution->trade_id};
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            KnownFirstSeenPrivateCorrelationPlan{std::move(resolution),
                                                 std::move(candidate_mapping)},
            FirstSeenPrivateTradeIdentityPlan{std::move(trade_key),
                                              std::move(trade_semantic_value)},
            std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A known non-execution reaches correlation only; later admission owns all transition and mapping
  // publication decisions.
  return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::success(
      FirstSeenAuthoritativePrivateIdentityPlan{
          std::move(event_key), ingress_semantic_value,
          KnownFirstSeenPrivateCorrelationPlan{std::move(resolution), std::move(candidate_mapping)},
          FirstSeenPrivateTradeNotReachedPlan{}, std::nullopt});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the stable owner, recovery identities, empty tables, and live lease; unique ownership
// destroys the lease-bearing child before the coordinator or any inspected M3 component.
PrivateOrderReconciler::PrivateOrderReconciler(
    const SubmissionCoordinator& owner, M4Policy m4_policy,
    recovery::RecoveryLineageId recovery_lineage_id, recovery::RuntimeEpochId runtime_epoch_id,
    model::OrderNamespace registered_order_namespace, PrivateOrderEventFactory event_factory,
    std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
    std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
    std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings,
    std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_lease) noexcept
    : owner_{&owner}, m4_policy_{std::move(m4_policy)}, recovery_lineage_id_{recovery_lineage_id},
      runtime_epoch_id_{runtime_epoch_id}, registered_order_namespace_{registered_order_namespace},
      event_factory_{std::move(event_factory)},
      event_identity_records_{std::move(event_identity_records)},
      trade_identity_records_{std::move(trade_identity_records)},
      exchange_order_mappings_{std::move(exchange_order_mappings)},
      recovery_lease_{std::move(recovery_lease)} {
  if (!recovery_lease_) {
    std::terminate();
  }
}

// --------------------------------------------------------

} // namespace aegis::runtime
