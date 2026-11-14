// Purpose: bind one submission owner and recovery policy, derive exact canonical correlation,
// and apply validated gap identities and OMS state inside the private joint commit.

#include "private_order_reconciler.hpp"

#include "aegis/model/domain_error.hpp"
#include "private_business_transaction.hpp"
#include "submission_coordinator.hpp"

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
[[nodiscard]] model::Result<Value>
create_private_order_reconciler_failure_from_field(std::string field) {
  return model::Result<Value>::create_failure(model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidM4Policy, std::move(field)));
}

// --------------------------------------------------------
// Return a planner failure in the exact requested stable domain without changing any owner state.
template <typename Value>
[[nodiscard]] model::Result<Value>
private_identity_planning_failure_from_field(model::DomainErrorCode code, std::string_view field) {
  return model::Result<Value>::create_failure(
      model::DomainError::create_at_field(code, std::string{field}));
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
    return model::Result<AuthoritativeOrderEventShape>::create_success(AuthoritativeOrderEventShape{
        acknowledgement->local_order_locator, acknowledgement->exchange_order_id,
        std::move(source_instrument_id), nullptr});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Rejection correlation uses its complete nonempty raw locator and no instrument claim.
  if (const auto* const rejection =
          std::get_if<oms::ExchangeRejectedPayload>(&ingress_semantic_value.payload())) {
    return model::Result<AuthoritativeOrderEventShape>::create_success(AuthoritativeOrderEventShape{
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
    return model::Result<AuthoritativeOrderEventShape>::create_success(AuthoritativeOrderEventShape{
        execution->locator.local_order_id(), execution->locator.exchange_order_id(),
        execution->instrument_id, execution});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation-result correlation uses its complete nonempty locator and no trade semantics.
  if (const auto* const cancellation =
          std::get_if<oms::CancellationResultPayload>(&ingress_semantic_value.payload())) {
    return model::Result<AuthoritativeOrderEventShape>::create_success(AuthoritativeOrderEventShape{
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
    const SubmissionCoordinator& owner, oms::OutboundOms& outbound_oms,
    const configuration::StartupConfiguration& configuration, const M4Policy& policy,
    const recovery::RecoveryBootstrap& recovery_bootstrap) try {
  using PreparedReconciler = PreparedRecoveryBoundPrivateOrderReconciler;
  const auto& root = policy.root_provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind to exact risk, submission, runtime, and configuration identities using immutable public
  // owner views, and accept only the exact mutable OMS supplied by the sealed coordinator factory.
  if (&outbound_oms != &owner.outbound_oms() ||
      root.risk_policy_fingerprint() != owner.reservations().policy().fingerprint().bytes() ||
      root.risk_policy_revision() != owner.reservations().policy().revision() ||
      root.submission_policy_fingerprint() != owner.policy().fingerprint().bytes() ||
      root.runtime_policy_fingerprint() != owner.policy().runtime_policy_fingerprint() ||
      root.configuration_fingerprint() != owner.policy().configuration_fingerprint()) {
    return create_private_order_reconciler_failure_from_field<PreparedReconciler>(
        "private_order_reconciler.owner_provenance");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Full root equality binds the fake recovery medium to this exact owner and every M4 capacity.
  if (recovery_bootstrap.root_provenance_ != root) {
    return model::Result<PreparedReconciler>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::RecoveryProvenanceMismatch,
                                            "private_order_reconciler.recovery_root_provenance"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject a moved or internally incoherent bootstrap before deriving configuration state.
  if (!recovery_bootstrap.lease_ || recovery_bootstrap.runtime_epoch_id_.order_namespace() !=
                                        recovery_bootstrap.registered_order_namespace_) {
    return model::Result<PreparedReconciler>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidJournalState,
                                            "private_order_reconciler.recovery_bootstrap_state"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the trusted resolver to prove configuration/organization agreement before copying its
  // source-normalization and planning authority into the child. Preserve allocation as the earlier
  // capacity failure class instead of misreporting it as provenance disagreement.
  auto resolver = M4ProvenanceResolver::create_m4_provenance_resolver(configuration, policy);
  if (!resolver) {
    const auto* const failure_field =
        resolver.error().context.field == "m4_provenance.capacity_allocation"
            ? "private_order_reconciler.capacity_allocation"
            : "private_order_reconciler.configuration_provenance";
    return create_private_order_reconciler_failure_from_field<PreparedReconciler>(failure_field);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Recheck the implementation widths and the exact owner's permanent mapping lower bound before
  // any fixed storage is allocated or installed.
  const auto& capacities = policy.capacities();
  if (!are_private_identity_storage_capacities_implementable(capacities) ||
      capacities.max_exchange_order_mappings < owner.outbound_oms().capacity()) {
    return create_private_order_reconciler_failure_from_field<PreparedReconciler>(
        "private_order_reconciler.capacities");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize empty canonical slots; the independently allocated retention state owns pending
  // preparations without populating these event, trade, or active mapping records.
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
  // Allocate the complete future business-evidence backing before any bootstrap authority moves.
  auto business_evidence_store =
      PrivateBusinessEvidenceStore::create_private_business_evidence_store(
          policy, recovery_lineage_id, runtime_epoch_id);
  if (!business_evidence_store) {
    return model::Result<PreparedReconciler>::create_failure(
        std::move(business_evidence_store).error());
  }
  auto business_incarnation =
      std::make_shared<const detail::PrivateBusinessTransactionIncarnation>();

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
      owner, outbound_oms, std::move(owned_m4_policy), recovery_lineage_id, runtime_epoch_id,
      registered_order_namespace, std::move(event_factory), std::move(event_identity_records),
      std::move(trade_identity_records), std::move(exchange_order_mappings),
      std::move(business_evidence_store).value(), std::move(business_incarnation),
      std::move(recovery_identity_lease), configuration}};

  // ++++++++++++++++++++++++++++++++++++++++
  // Wrap the fully allocated child before consuming either bootstrap authority. The returned
  // transaction moves without failure into the coordinator's final publication phase.
  static_assert(
      std::is_nothrow_constructible_v<PreparedReconciler, std::unique_ptr<PrivateOrderReconciler>>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedReconciler>);
  static_assert(std::is_nothrow_move_constructible_v<model::Result<PreparedReconciler>>);
  return model::Result<PreparedReconciler>::create_success(
      PreparedReconciler{std::move(reconciler)});

  // ++++++++++++++++++++++++++++++++++++++++
} catch (const std::bad_alloc&) {
  return create_private_order_reconciler_failure_from_field<
      PrivateOrderReconciler::PreparedRecoveryBoundPrivateOrderReconciler>(
      "private_order_reconciler.capacity_allocation");
} catch (const std::length_error&) {
  return create_private_order_reconciler_failure_from_field<
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
// Derive one detached first-seen identity plan from exact canonical ownership. No branch changes
// the bound owner, OMS, risk, evidence, counts, or fixed slots.
model::Result<FirstSeenAuthoritativePrivateIdentityPlan>
PrivateOrderReconciler::derive_first_seen_authoritative_identity_plan(
    const oms::PrivateEventIngressSemanticValue& ingress_semantic_value) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept only the authoritative order-scoped vocabulary and never re-correlate a canonical key.
  auto shape_result = derive_authoritative_order_event_shape(ingress_semantic_value);
  if (!shape_result) {
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_failure(
        std::move(shape_result).error());
  }
  auto shape = std::move(shape_result).value();
  auto event_key =
      oms::PrivateEventRegistryKey::from_ingress_semantic_value(ingress_semantic_value);
  if (find_committed_event_identity(event_key) != nullptr) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateEventConflict, "private_order_reconciler.canonical_event");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove exact configured account/venue ownership independently before comparing a recomputed
  // source provenance. Unknown accounts and wrong venues must not pass by recomputing themselves.
  if (!event_factory_.has_configured_account_venue_binding(
          ingress_semantic_value.logical_account_id(), ingress_semantic_value.venue_id())) {
    auto resolution = oms::PrivateEventResolution::create_provenance_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
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
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::ProvenanceMismatch});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve each locator independently; neither an unbound exchange identity nor a volatile
  // preparation supplies ownership. A supplied unknown local locator conflicts with a binding.
  const auto* local_order =
      shape.local_order_id ? outbound_oms_->find_order(*shape.local_order_id) : nullptr;
  const auto exchange_key =
      shape.exchange_order_id
          ? std::optional{oms::ExchangeOrderKey{ingress_semantic_value.venue_id(),
                                                ingress_semantic_value.logical_account_id(),
                                                *shape.exchange_order_id}}
          : std::nullopt;
  const auto* mapping = exchange_key ? find_committed_exchange_mapping(*exchange_key) : nullptr;
  const auto* mapped_order = mapping ? outbound_oms_->find_order(mapping->order_id) : nullptr;
  if (mapping && !mapped_order) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed, "private_order_reconciler.mapping_owner");
  }
  if (mapped_order && shape.local_order_id && local_order != mapped_order) {
    auto resolution = oms::PrivateEventResolution::create_correlation_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::CorrelationConflict});
  }
  const auto* retained_order = local_order ? local_order : mapped_order;
  if (retained_order == nullptr) {
    auto resolution = oms::PrivateEventResolution::create_unknown_resolution();
    if (shape.execution != nullptr) {
      auto trade_semantic_value =
          oms::PrivateTradeSemanticValue::create_unknown_trade_semantic_value(*shape.execution);
      auto trade_key =
          oms::TradeKey{ingress_semantic_value.venue_id(),
                        ingress_semantic_value.logical_account_id(), shape.execution->trade_id};
      return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
          FirstSeenAuthoritativePrivateIdentityPlan{
              std::move(event_key), ingress_semantic_value,
              UnknownFirstSeenPrivateCorrelationPlan{std::move(resolution)},
              FirstSeenPrivateTradeIdentityPlan{std::move(trade_key),
                                                std::move(trade_semantic_value)},
              risk::AccountSafetyReason::UnknownTrade});
    }
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
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
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::CorrelationConflict});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the OMS projection to agree with the exact reverse binding. A different incoming
  // exchange key conflicts even if its own forward mapping does not exist yet.
  const auto private_projection = retained_order->private_projection();
  const auto* reverse_mapping =
      find_committed_exchange_mapping_for_order(retained_order->order_id());
  if (private_projection.exchange_order_id.has_value() != (reverse_mapping != nullptr) ||
      (reverse_mapping &&
       (reverse_mapping->exchange_order_key.exchange_order_id !=
            *private_projection.exchange_order_id ||
        reverse_mapping->exchange_order_key.logical_account_id !=
            retained_provenance.logical_account_id ||
        reverse_mapping->exchange_order_key.venue_id != retained_provenance.venue_id)) ||
      (!reverse_mapping && (private_projection.exchange_acknowledged ||
                            private_projection.exchange_mapping_established_by_execution))) {
    return private_identity_planning_failure_from_field<FirstSeenAuthoritativePrivateIdentityPlan>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.first_seen_owner_exchange_projection");
  }
  if (exchange_key && reverse_mapping && reverse_mapping->exchange_order_key != *exchange_key) {
    auto resolution = oms::PrivateEventResolution::create_correlation_conflict_resolution();
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
        FirstSeenAuthoritativePrivateIdentityPlan{
            std::move(event_key), ingress_semantic_value,
            ConflictFirstSeenPrivateCorrelationPlan{std::move(resolution)},
            FirstSeenPrivateTradeNotReachedPlan{}, risk::AccountSafetyReason::CorrelationConflict});
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
  if (exchange_key && !reverse_mapping) {
    candidate_mapping = PrivateExchangeOrderMapping{*exchange_key, retained_order->order_id()};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Known execution economics derive canonical side only from the retained admission. A supplied
  // contradiction suppresses the uncommitted mapping candidate and produces no trade tuple.
  if (shape.execution != nullptr) {
    const auto canonical_side = retained_order->economics().side;
    if (shape.execution->source_side.has_value() &&
        *shape.execution->source_side != canonical_side) {
      candidate_mapping.reset();
      return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
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
    return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
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
  return model::Result<FirstSeenAuthoritativePrivateIdentityPlan>::create_success(
      FirstSeenAuthoritativePrivateIdentityPlan{
          std::move(event_key), ingress_semantic_value,
          KnownFirstSeenPrivateCorrelationPlan{std::move(resolution), std::move(candidate_mapping)},
          FirstSeenPrivateTradeNotReachedPlan{}, std::nullopt});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Search only the immutable published event prefix, leaving prepared candidate identities inert.
const PrivateEventIdentityRecord* PrivateOrderReconciler::find_committed_event_identity(
    const oms::PrivateEventRegistryKey& key) const noexcept {
  for (std::uint32_t index = 0U; index < event_identity_record_count_; ++index) {
    const auto& slot = event_identity_records_[index];
    if (slot && slot->key == key) {
      return &*slot;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Search the canonical trade prefix by exact account, venue, and trade identity.
const PrivateTradeIdentityRecord*
PrivateOrderReconciler::find_committed_trade_identity(const oms::TradeKey& key) const noexcept {
  for (std::uint32_t index = 0U; index < trade_identity_record_count_; ++index) {
    const auto& slot = trade_identity_records_[index];
    if (slot && slot->key == key) {
      return &*slot;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Resolve the exact scoped forward binding from canonical storage only.
const PrivateExchangeOrderMapping* PrivateOrderReconciler::find_committed_exchange_mapping(
    const oms::ExchangeOrderKey& key) const noexcept {
  for (std::uint32_t index = 0U; index < exchange_order_mapping_count_; ++index) {
    const auto& slot = exchange_order_mappings_[index];
    if (slot && slot->exchange_order_key == key) {
      return &*slot;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Resolve the unique reverse binding from the same canonical table as forward correlation.
const PrivateExchangeOrderMapping*
PrivateOrderReconciler::find_committed_exchange_mapping_for_order(
    const model::OrderId& order_id) const noexcept {
  for (std::uint32_t index = 0U; index < exchange_order_mapping_count_; ++index) {
    const auto& slot = exchange_order_mappings_[index];
    if (slot && slot->order_id == order_id) {
      return &*slot;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Borrow an immutable pending interval sequence from this exact owner's OMS storage.
std::span<const oms::PendingPrivateExecution>
PrivateOrderReconciler::pending_private_executions(const model::OrderId& order_id) const noexcept {
  return outbound_oms_->pending_private_executions(order_id);
}

// --------------------------------------------------------
// Validate a coherent known gap and every distinct identity insertion before journal publication.
model::Result<void> PrivateOrderReconciler::validate_buffered_gap_state(
    const FirstSeenAuthoritativePrivateIdentityPlan& identity,
    const oms::PrivateOmsTransitionPlan& transition) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Only the planner's exact known execution can add canonical state. Recheck immutable linkage
  // rather than taking a candidate mapping or trade tuple as standalone mutation authority.
  const auto* correlation =
      std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&identity.correlation_plan());
  const auto* known = correlation ? correlation->resolution.known_resolution() : nullptr;
  const auto* trade = std::get_if<FirstSeenPrivateTradeIdentityPlan>(&identity.trade_plan());
  if (!known || !trade || identity.preliminary_safety_reason() || !transition.gap_insertion ||
      oms::PrivateEventIngressSemanticValue::from_normalized_input(
          transition.gap_insertion->execution) != identity.ingress_semantic_value()) {
    return private_identity_planning_failure_from_field<void>(
        model::DomainErrorCode::InvalidPrivateOmsState,
        "private_order_reconciler.buffered_gap_plan");
  }
  const auto& input = transition.gap_insertion->execution;
  if (find_committed_event_identity(identity.event_key()) ||
      find_committed_trade_identity(trade->key)) {
    return private_identity_planning_failure_from_field<void>(
        model::DomainErrorCode::PrivateEventConflict,
        "private_order_reconciler.buffered_gap_identity");
  }
  if (correlation->candidate_mapping) {
    const auto& candidate = *correlation->candidate_mapping;
    if (candidate.order_id != known->order_id ||
        candidate.exchange_order_key.venue_id != input.venue_id() ||
        candidate.exchange_order_key.logical_account_id != input.logical_account_id() ||
        transition.before.exchange_order_id ||
        transition.after.exchange_order_id != candidate.exchange_order_key.exchange_order_id ||
        !transition.after.exchange_mapping_established_by_execution ||
        find_committed_exchange_mapping(candidate.exchange_order_key) ||
        find_committed_exchange_mapping_for_order(known->order_id)) {
      return private_identity_planning_failure_from_field<void>(
          model::DomainErrorCode::PrivateCorrelationFailed,
          "private_order_reconciler.buffered_gap_mapping");
    }
  } else if (transition.before.exchange_order_id != transition.after.exchange_order_id) {
    return private_identity_planning_failure_from_field<void>(
        model::DomainErrorCode::PrivateCorrelationFailed,
        "private_order_reconciler.buffered_gap_mapping");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Bound every insertion, including absence of any unexpected already populated target slot.
  if (event_identity_record_count_ >= event_identity_records_.size() ||
      trade_identity_record_count_ >= trade_identity_records_.size() ||
      (correlation->candidate_mapping &&
       exchange_order_mapping_count_ >= exchange_order_mappings_.size())) {
    return private_identity_planning_failure_from_field<void>(
        model::DomainErrorCode::PrivateEventCapacityExceeded,
        "private_order_reconciler.buffered_gap_identity_capacity");
  }
  if (event_identity_records_[event_identity_record_count_] ||
      trade_identity_records_[trade_identity_record_count_] ||
      (correlation->candidate_mapping && exchange_order_mappings_[exchange_order_mapping_count_])) {
    return private_identity_planning_failure_from_field<void>(
        model::DomainErrorCode::InvalidPrivateOmsState,
        "private_order_reconciler.buffered_gap_identity_slot");
  }
  return outbound_oms_->validate_buffered_private_execution(known->order_id, transition);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Apply only previously validated owner-local state. All new records are bounded inline values;
// the OMS projection and permanent registry prefixes become visible in the same serialized turn.
void PrivateOrderReconciler::apply_validated_buffered_gap_state(
    const FirstSeenAuthoritativePrivateIdentityPlan& identity,
    const oms::PrivateOmsTransitionPlan& transition) noexcept {
  static_assert(std::is_nothrow_move_constructible_v<PrivateEventIdentityRecord>);
  static_assert(std::is_nothrow_move_constructible_v<PrivateTradeIdentityRecord>);
  static_assert(std::is_nothrow_copy_constructible_v<PrivateExchangeOrderMapping>);
  static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateEventIngressSemanticValue>);
  static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateEventResolution>);
  static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateTradeSemanticValue>);
  const auto& correlation =
      *std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&identity.correlation_plan());
  const auto& trade = *std::get_if<FirstSeenPrivateTradeIdentityPlan>(&identity.trade_plan());
  const auto disposition = oms::PrivateEventDisposition::BufferedGap;
  outbound_oms_->apply_validated_buffered_private_execution(
      correlation.resolution.known_resolution()->order_id, transition);
  event_identity_records_[event_identity_record_count_++].emplace(
      PrivateEventIdentityRecord{identity.event_key(), identity.ingress_semantic_value(),
                                 correlation.resolution, disposition});
  trade_identity_records_[trade_identity_record_count_++].emplace(
      PrivateTradeIdentityRecord{trade.key, trade.semantic_value, disposition, disposition});
  if (correlation.candidate_mapping) {
    exchange_order_mappings_[exchange_order_mapping_count_++].emplace(
        *correlation.candidate_mapping);
  }
}

// --------------------------------------------------------
// Retain the stable owner, recovery identities, bounded canonical tables, and live lease, then
// allocate preparation and containment storage before bootstrap authority can be consumed.
PrivateOrderReconciler::PrivateOrderReconciler(
    const SubmissionCoordinator& owner, oms::OutboundOms& outbound_oms, M4Policy m4_policy,
    recovery::RecoveryLineageId recovery_lineage_id, recovery::RuntimeEpochId runtime_epoch_id,
    model::OrderNamespace registered_order_namespace, PrivateOrderEventFactory event_factory,
    std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
    std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
    std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings,
    std::unique_ptr<PrivateBusinessEvidenceStore> business_evidence_store,
    std::shared_ptr<const detail::PrivateBusinessTransactionIncarnation> business_incarnation,
    std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_lease,
    const configuration::StartupConfiguration& configuration)
    : owner_{&owner}, outbound_oms_{&outbound_oms}, m4_policy_{std::move(m4_policy)},
      recovery_lineage_id_{recovery_lineage_id}, runtime_epoch_id_{runtime_epoch_id},
      registered_order_namespace_{registered_order_namespace},
      event_factory_{std::move(event_factory)},
      event_identity_records_{std::move(event_identity_records)},
      trade_identity_records_{std::move(trade_identity_records)},
      exchange_order_mappings_{std::move(exchange_order_mappings)},
      business_evidence_store_{std::move(business_evidence_store)},
      business_incarnation_{std::move(business_incarnation)},
      recovery_lease_{std::move(recovery_lease)}, retention_{configuration, m4_policy_} {
  if (!recovery_lease_ || !business_evidence_store_ || !business_incarnation_) {
    std::terminate();
  }
}

// --------------------------------------------------------

} // namespace aegis::runtime
