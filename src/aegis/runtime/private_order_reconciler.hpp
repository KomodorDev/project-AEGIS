// Purpose: bind private identity planning, bounded gap commits, retained preparations, and
// production admission containment to one submission owner.

#pragma once

#include "../oms/private_oms_transition.hpp"
#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/recovery/deterministic_fake_recovery_medium.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "private_business_evidence_store.hpp"
#include "private_business_proposal.hpp"
#include "private_execution_drain_evidence.hpp"
#include "private_identity_retention.hpp"
#include "private_order_event_factory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {
namespace detail {

// ########################################################################
// A cold lifetime identity prevents detached preparations from authorizing a replacement owner.
struct PrivateBusinessTransactionIncarnation;

// ########################################################################

} // namespace detail

// ########################################################################
// A sealed detached transaction retains genuine component and journal reservations together.
class PreparedInitialKnownPrivateBusinessTransaction;

// ########################################################################
// A detached fill/drain preparation owns the real journal and copied economic evidence leases.
class PreparedKnownPrivateExecutionDrain;

// ########################################################################
// The coordinator installs the child only while pristine, then owns and destroys it at one stable
// address while later M3 submissions remain independently mutable.
class SubmissionCoordinator;

// ########################################################################
// One event-identity slot schema pairs receive-time-free ingress semantics with an immutable
// resolution and original disposition. A committed gap adds the immutable row once.
struct PrivateEventIdentityRecord {
  oms::PrivateEventRegistryKey key;
  oms::PrivateEventIngressSemanticValue ingress_semantic_value;
  oms::PrivateEventResolution resolution;
  oms::PrivateEventDisposition original_disposition;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares every slot field without publishing
  // a separate comparison capability.
  friend bool operator==(const PrivateEventIdentityRecord&,
                         const PrivateEventIdentityRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One trade-identity slot schema pairs a complete semantic tuple with its original and current
// dispositions. This slice creates BufferedGap rows; it does not apply the pending execution.
struct PrivateTradeIdentityRecord {
  oms::TradeKey key;
  oms::PrivateTradeSemanticValue semantic_value;
  oms::PrivateEventDisposition original_disposition;
  oms::PrivateEventDisposition current_disposition;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares the complete inert slot value while
  // keeping equality discoverable only through the type itself.
  friend bool operator==(const PrivateTradeIdentityRecord&,
                         const PrivateTradeIdentityRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One exchange-mapping slot schema binds a venue/account-scoped exchange key to one local order.
// A committed gap can establish the binding once; volatile candidates remain separate.
struct PrivateExchangeOrderMapping {
  oms::ExchangeOrderKey exchange_order_key;
  model::OrderId order_id;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares the complete nominal mapping without
  // introducing a generic public comparison helper.
  friend bool operator==(const PrivateExchangeOrderMapping&,
                         const PrivateExchangeOrderMapping&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A known correlation retains the immutable owner-derived resolution and at most one uncommitted
// exchange mapping candidate. The candidate has no insertion authority.
struct KnownFirstSeenPrivateCorrelationPlan {
  oms::PrivateEventResolution resolution;
  std::optional<PrivateExchangeOrderMapping> candidate_mapping;

  // --------------------------------------------------------
  // Structural equality proves repeated read-only derivation selects the same detached values.
  friend bool operator==(const KnownFirstSeenPrivateCorrelationPlan&,
                         const KnownFirstSeenPrivateCorrelationPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// An unknown correlation retains the sealed unknown resolution without inventing local ownership.
struct UnknownFirstSeenPrivateCorrelationPlan {
  oms::PrivateEventResolution resolution;

  // --------------------------------------------------------
  // Structural equality proves repeated read-only derivation preserves unknown classification.
  friend bool operator==(const UnknownFirstSeenPrivateCorrelationPlan&,
                         const UnknownFirstSeenPrivateCorrelationPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A conflict correlation retains the sealed provenance or locator-conflict resolution and no
// mapping candidate.
struct ConflictFirstSeenPrivateCorrelationPlan {
  oms::PrivateEventResolution resolution;

  // --------------------------------------------------------
  // Structural equality compares the exact successful safety-contained conflict classification.
  friend bool operator==(const ConflictFirstSeenPrivateCorrelationPlan&,
                         const ConflictFirstSeenPrivateCorrelationPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Exactly one known, unknown, or conflict first-seen correlation outcome is active.
using FirstSeenPrivateCorrelationPlan =
    std::variant<KnownFirstSeenPrivateCorrelationPlan, UnknownFirstSeenPrivateCorrelationPlan,
                 ConflictFirstSeenPrivateCorrelationPlan>;

// ########################################################################
// A derived trade identity retains the exact account-scoped key and sealed comparison tuple.
struct FirstSeenPrivateTradeIdentityPlan {
  oms::TradeKey key;
  oms::PrivateTradeSemanticValue semantic_value;

  // --------------------------------------------------------
  // Structural equality is the complete detached first-seen trade-plan oracle.
  friend bool operator==(const FirstSeenPrivateTradeIdentityPlan&,
                         const FirstSeenPrivateTradeIdentityPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// This marker states that correlation or a non-execution payload stopped before trade derivation.
struct FirstSeenPrivateTradeNotReachedPlan {

  // --------------------------------------------------------
  // All not-reached markers carry the same deliberate absence of a trade identity.
  friend bool operator==(const FirstSeenPrivateTradeNotReachedPlan&,
                         const FirstSeenPrivateTradeNotReachedPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// This marker preserves the distinct known-execution source-side contradiction outcome.
struct FirstSeenPrivateTradeSourceSideConflictPlan {

  // --------------------------------------------------------
  // All source-side markers carry the same pre-trade authoritative contradiction outcome.
  friend bool operator==(const FirstSeenPrivateTradeSourceSideConflictPlan&,
                         const FirstSeenPrivateTradeSourceSideConflictPlan&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Exactly one trade tuple, ordinary not-reached marker, or source-side-conflict marker is active.
using FirstSeenPrivateTradePlan =
    std::variant<FirstSeenPrivateTradeIdentityPlan, FirstSeenPrivateTradeNotReachedPlan,
                 FirstSeenPrivateTradeSourceSideConflictPlan>;

// ########################################################################
// The sealed plan owns detached copies of every first-seen identity decision and grants no
// insertion, transition, safety-mutation, or recovery authority.
class FirstSeenAuthoritativePrivateIdentityPlan final {
public:

  // --------------------------------------------------------
  // Borrow the receive-time-free event key derived from the exact retained ingress semantics.
  [[nodiscard]] const oms::PrivateEventRegistryKey& event_key() const noexcept {
    return event_key_;
  }

  // --------------------------------------------------------
  // Borrow the exact correlation-independent ingress semantics copied into this detached plan.
  [[nodiscard]] const oms::PrivateEventIngressSemanticValue&
  ingress_semantic_value() const noexcept {
    return ingress_semantic_value_;
  }

  // --------------------------------------------------------
  // Borrow the mutually exclusive known, unknown, or conflict correlation outcome.
  [[nodiscard]] const FirstSeenPrivateCorrelationPlan& correlation_plan() const noexcept {
    return correlation_plan_;
  }

  // --------------------------------------------------------
  // Borrow the derived trade tuple or exact marker explaining why no tuple was produced.
  [[nodiscard]] const FirstSeenPrivateTradePlan& trade_plan() const noexcept { return trade_plan_; }

  // --------------------------------------------------------
  // Return the preliminary safety reason selected by correlation or execution semantics, if any.
  [[nodiscard]] const std::optional<risk::AccountSafetyReason>&
  preliminary_safety_reason() const noexcept {
    return preliminary_safety_reason_;
  }

  // --------------------------------------------------------
  // Structural equality proves repeated calls over unchanged authority return identical plans.
  friend bool operator==(const FirstSeenAuthoritativePrivateIdentityPlan&,
                         const FirstSeenAuthoritativePrivateIdentityPlan&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal one fully derived detached value after every shape and authority check has completed.
  FirstSeenAuthoritativePrivateIdentityPlan(
      oms::PrivateEventRegistryKey event_key,
      oms::PrivateEventIngressSemanticValue ingress_semantic_value,
      FirstSeenPrivateCorrelationPlan correlation_plan, FirstSeenPrivateTradePlan trade_plan,
      std::optional<risk::AccountSafetyReason> preliminary_safety_reason) noexcept
      : event_key_{std::move(event_key)},
        ingress_semantic_value_{std::move(ingress_semantic_value)},
        correlation_plan_{std::move(correlation_plan)}, trade_plan_{std::move(trade_plan)},
        preliminary_safety_reason_{preliminary_safety_reason} {}

  // --------------------------------------------------------
  // Retain complete detached values; none is a pointer, mutable alias, or commit capability.
  oms::PrivateEventRegistryKey event_key_;
  oms::PrivateEventIngressSemanticValue ingress_semantic_value_;
  FirstSeenPrivateCorrelationPlan correlation_plan_;
  FirstSeenPrivateTradePlan trade_plan_;
  std::optional<risk::AccountSafetyReason> preliminary_safety_reason_;

  // ########################################################################
  // Only the exact owner-bound reconciler may seal a fully checked first-seen plan.
  friend class PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################
// The child binds one nonmoving coordinator and acknowledged recovery incarnation. Token-gated
// owner turns commit bounded execution gaps and exact repeats or preserve unsupported facts in
// retention. Read-only planning and account/table inspection require owner serialization or
// quiescence. Completion and immutable retained-turn queries support concurrent access through
// release/acquire publication. The recovery lease keeps cold medium inspection fenced and permits
// journal publication only through the closed joint turn.
class PrivateOrderReconciler final : public PrivateAdmissionOwner {
public:

  // --------------------------------------------------------
  // Preserve the owner pointer and fixed storage addresses for the child's complete lifetime.
  PrivateOrderReconciler(const PrivateOrderReconciler&) = delete;
  PrivateOrderReconciler& operator=(const PrivateOrderReconciler&) = delete;
  PrivateOrderReconciler(PrivateOrderReconciler&&) = delete;
  PrivateOrderReconciler& operator=(PrivateOrderReconciler&&) = delete;

  // --------------------------------------------------------
  // Release preparation and containment storage before the borrowed submission owner disappears.
  ~PrivateOrderReconciler() override = default;

  // --------------------------------------------------------
  // Commit a supported execution gap or exact repeat from a genuine lane token. Retain other
  // valid inputs for reconciliation. Wrong-owner, moved, stale, or recursive tokens change nothing.
  [[nodiscard]] PrivateTurnCompletion
  commit_private_order_turn(AdmittedPrivateOrderSlot admitted) noexcept override;
  [[nodiscard]] PrivateTurnCompletion
  commit_reconciliation_event_turn(AdmittedReconciliationEventSlot admitted) noexcept override;

  // --------------------------------------------------------
  // Acquire the immutable disposition of a committed ordinary-private or reconciliation input.
  // These lane-specific completion queries may run concurrently with later appends.
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_private_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override;
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_reconciliation_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override;

  // --------------------------------------------------------
  // Acquire the matching lane's immutable published error; returned pointers survive later appends
  // and remain valid until this owner is destroyed.
  [[nodiscard]] const model::DomainError* find_committed_retained_private_event_error(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override;
  [[nodiscard]] const model::DomainError* find_committed_retained_reconciliation_event_error(
      model::AdmissionOrdinal attempt_ordinal) const noexcept override;

  // --------------------------------------------------------
  // Preserve complete fence causes and apply monotonic submission containment. These trusted
  // executor control calls publish retained preparation evidence, not a business journal/audit.
  [[nodiscard]] model::Result<void>
  apply_account_safety_fence(const AccountSafetyFenceTurn& fence,
                             const ControlTurnContext& context) noexcept override;
  [[nodiscard]] model::Result<void>
  apply_global_private_fence(const GlobalPrivateFenceTurn& fence,
                             const ControlTurnContext& context) noexcept override;

  // --------------------------------------------------------
  // Query owner-local submission containment; unknown accounts conservatively return Quarantined.
  [[nodiscard]] risk::AccountSafetyState
  account_safety_state(model::LogicalAccountId account_id) const noexcept;

  // --------------------------------------------------------
  // Query the reasonless global block without attributing it to any configured account.
  [[nodiscard]] bool is_private_consumption_globally_blocked() const noexcept {
    return retention_.globally_blocked;
  }

  // --------------------------------------------------------
  // Preserve authentic uncertain submission provenance before a later owner-local risk decision.
  void record_submission_uncertainty(const oms::OutboundOrderRecord& order) noexcept;

  // --------------------------------------------------------
  // Borrow preparation-only tables under owner serialization or quiescence; candidates confer no
  // exchange-order ownership, and retained comparisons confer no consumption acknowledgement.
  [[nodiscard]] const PrivateIdentityPreparationStore& identity_preparations() const noexcept {
    return retention_.preparations;
  }

  // --------------------------------------------------------
  // Acquire the immutable complete retained turn for the exact ordinal and lane, including the
  // dedicated first-overflow fact after evidence saturation faults the executor.
  [[nodiscard]] const RetainedPrivateIdentityTurn*
  find_retained_identity_turn(model::AdmissionOrdinal attempt_ordinal,
                              bool reconciliation) const noexcept;

  // --------------------------------------------------------
  // Observe the retained normal prefix without counting the dedicated fail-stop overflow record.
  [[nodiscard]] std::size_t retained_identity_turn_count() const noexcept {
    return retention_.published_turn_count.load(std::memory_order_acquire);
  }

  // --------------------------------------------------------
  // Return the fixed number of preallocated event-identity slots.
  [[nodiscard]] std::uint32_t event_identity_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(event_identity_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of preallocated trade-identity slots.
  [[nodiscard]] std::uint32_t trade_identity_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(trade_identity_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of preallocated exchange-order mapping slots.
  [[nodiscard]] std::uint32_t exchange_order_mapping_capacity() const noexcept {
    return static_cast<std::uint32_t>(exchange_order_mappings_.size());
  }

  // --------------------------------------------------------
  // Return the canonical first-admission event count, excluding duplicate observations.
  [[nodiscard]] std::uint32_t event_identity_record_count() const noexcept {
    return event_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return the canonical trade count, excluding retained preparations and duplicates.
  [[nodiscard]] std::uint32_t trade_identity_record_count() const noexcept {
    return trade_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return the active mapping count; pending preparation candidates are deliberately excluded.
  [[nodiscard]] std::uint32_t exchange_order_mapping_count() const noexcept {
    return exchange_order_mapping_count_;
  }

  // --------------------------------------------------------
  // Borrow one immutable canonical event row, or null. Check it before attempting correlation.
  // These identity and pending queries require owner serialization or quiescence.
  [[nodiscard]] const PrivateEventIdentityRecord*
  find_committed_event_identity(const oms::PrivateEventRegistryKey& key) const noexcept;

  // --------------------------------------------------------
  // Borrow one canonical trade tuple, or null; a preparation never establishes this record.
  [[nodiscard]] const PrivateTradeIdentityRecord*
  find_committed_trade_identity(const oms::TradeKey& key) const noexcept;

  // --------------------------------------------------------
  // Borrow the active account/venue-scoped binding, excluding all preparation candidates.
  [[nodiscard]] const PrivateExchangeOrderMapping*
  find_committed_exchange_mapping(const oms::ExchangeOrderKey& key) const noexcept;

  // --------------------------------------------------------
  // Borrow the exact retained order's immutable pending prefix in ascending cumulative endpoint
  // order, or an empty view if absent. A later gap commit can change the borrowed sequence.
  [[nodiscard]] std::span<const oms::PendingPrivateExecution>
  pending_private_executions(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Borrow the immutable policy copy that authorized and sized this child.
  [[nodiscard]] const M4Policy& m4_policy() const noexcept { return m4_policy_; }

  // --------------------------------------------------------
  // Return the external fake-medium lineage bound to this installed runtime incarnation.
  [[nodiscard]] const recovery::RecoveryLineageId& recovery_lineage_id() const noexcept {
    return recovery_lineage_id_;
  }

  // --------------------------------------------------------
  // Return the namespace-qualified runtime incarnation established by recovery bootstrap.
  [[nodiscard]] const recovery::RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Return the fake-acknowledged namespace used for every post-install client identity.
  [[nodiscard]] const model::OrderNamespace& registered_order_namespace() const noexcept {
    return registered_order_namespace_;
  }

  // --------------------------------------------------------
  // Derive the authoritative identity plan only from a venue/reconciliation order event and this
  // bound owner. The caller must compare any canonical event first; an already committed key
  // returns PrivateEventConflict without re-correlation. Invalid source shape returns
  // InvalidPrivateEvent, inconsistent owner state returns PrivateCorrelationFailed, and exact
  // locator or provenance conflicts remain detached safety results. No state changes; the caller
  // must prevent concurrent coordinator mutation throughout the call.
  [[nodiscard]] model::Result<FirstSeenAuthoritativePrivateIdentityPlan>
  derive_first_seen_authoritative_identity_plan(
      const oms::PrivateEventIngressSemanticValue& ingress_semantic_value) const;

  // --------------------------------------------------------
  // Derive a detached authoritative lifecycle proposal from this owner's genuine row with no
  // pending fills or cancellation history. Unknown or conflicting correlation fails closed.
  // No proposed state becomes live, and no dedupe, callback, journal, or commit authority escapes.
  // Reads require owner serialization or quiescence for the complete call.
  [[nodiscard]] model::Result<oms::PrivateOmsTransitionPlan>
  derive_initial_known_authoritative_oms_transition(
      const oms::NormalizedPrivateOrderInput& input) const;

  // --------------------------------------------------------
  // Join a genuine initial M3 open order's lifecycle and economic proposals with a prospective
  // audit span. Requires coherent untouched held economics; unknown/conflicting correlation or
  // inconsistent components fail before returning any proposal. Success changes no live business
  // state or counters; an execution proposal leases fixed scratch until destruction; moves transfer
  // the lease. The complete call requires owner serialization or quiescence, like the component
  // planners.
  [[nodiscard]] model::Result<InitialKnownPrivateBusinessProposal>
  derive_initial_known_authoritative_business_proposal(
      const oms::NormalizedPrivateOrderInput& input,
      recovery::AuditOrdinal prospective_first_audit_ordinal) const;

  // --------------------------------------------------------
  // Prepare complete copied known-order evidence from genuine unchanged initial components and
  // this store's prospective audit position. Replay and optional linkage values remain authored
  // candidates; they grant no admission, journal, callback, or publication authority. Only one
  // evidence lease may remain outstanding, including after moves; destruction releases scratch.
  // The entire call and lease access require owner serialization or quiescence.
  [[nodiscard]] model::Result<PreparedPrivateBusinessEvidence>
  prepare_initial_known_authoritative_business_evidence(
      const oms::NormalizedPrivateOrderInput& input,
      recovery::JournalReplayProvenance replay_provenance,
      std::optional<model::CallbackOrdinal> first_callback_ordinal = std::nullopt,
      std::optional<recovery::JournalSequence> journal_sequence = std::nullopt,
      std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal = std::nullopt) const;

  // --------------------------------------------------------
  // Retain one genuine initial business proposal, economic scratch, complete evidence, and real
  // journal-slot reservation together. Replay/callback identities remain prospective values;
  // this source-private preparation cannot publish or consume input. Failure releases every
  // acquired reservation without changing business prefixes or counters. The complete call and
  // subsequent access require owner serialization or quiescence.
  [[nodiscard]] model::Result<PreparedInitialKnownPrivateBusinessTransaction>
  prepare_initial_known_authoritative_business_transaction(
      const oms::NormalizedPrivateOrderInput& input,
      recovery::JournalReplayProvenance replay_provenance,
      std::optional<model::CallbackOrdinal> first_callback_ordinal = std::nullopt,
      std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal = std::nullopt) const;

  // --------------------------------------------------------
  // Reject moved, foreign, stale, or internally inconsistent preparations before any publication
  // or dereference of a saved row. Success proves unchanged captured owner components and live
  // reservations only; it grants no admission, callback, append, or economic-consumption right.
  [[nodiscard]] model::Result<void> validate_prepared_initial_known_business_transaction(
      const PreparedInitialKnownPrivateBusinessTransaction& transaction) const;

  // --------------------------------------------------------
  // Prepare an incoming contiguous execution and the actual pending prefix it would drain.
  // The caller supplies prospective receipt and callback identities. Preparation, inspection,
  // moves, and release require owner serialization or quiescence while the owner is live.
  // Failure releases every lease. Success grants only immutable inspection.
  [[nodiscard]] model::Result<PreparedKnownPrivateExecutionDrain>
  prepare_known_private_execution_drain(const oms::NormalizedPrivateOrderInput& input,
                                        recovery::JournalReplayProvenance replay_provenance,
                                        model::CallbackOrdinal first_callback_ordinal) const;

  // --------------------------------------------------------
  // Recheck the exact owner incarnation, original pending facts, and all live reservations.
  // A moved or stale preparation fails without refreshing state or authorizing a write.
  [[nodiscard]] model::Result<void> validate_prepared_private_execution_drain(
      const PreparedKnownPrivateExecutionDrain& preparation) const;

  // --------------------------------------------------------
  // Inspect fixed preparation capacities and prospective counters without mutation rights.
  [[nodiscard]] const PrivateBusinessEvidenceStore& business_evidence_store() const noexcept {
    return *business_evidence_store_;
  }

  // --------------------------------------------------------
private:

  // ########################################################################
  // One guarded owner turn either commits, reports a prepublication failure, or leaves the input
  // to the existing retained path. Preserve sealed resolution and established comparison facts
  // when the input stays outside the canonical registries.
  struct CallbackFreePrivateCommitResult {
    std::optional<oms::PrivateEventDisposition> disposition;
    std::optional<model::DomainError> error;
    std::optional<risk::AccountSafetyReason> safety_reason;
    std::optional<oms::PrivateEventResolution> resolution;
    RetainedPrivateExecutionProgress execution_progress{};
  };

  // ########################################################################

  // --------------------------------------------------------
  // Under the authenticated, serialized owner turn, commit a known execution gap or its exact
  // duplicate. Complete all fallible checks before journal publication; unsupported inputs stay
  // eligible for retention, and prepublication failures leave every committed prefix unchanged.
  [[nodiscard]] CallbackFreePrivateCommitResult
  commit_callback_free_private_input_if_supported(const oms::NormalizedPrivateOrderInput& input,
                                                  const AdmissionReceipt& receipt);

  // --------------------------------------------------------

  // ########################################################################
  // One private move-only transaction carries the completely allocated child while the validated
  // bootstrap still retains its lease and provider.
  struct PreparedRecoveryBoundPrivateOrderReconciler final {

    // --------------------------------------------------------
    // Bind the complete child without consuming the caller-owned recovery bootstrap.
    explicit PreparedRecoveryBoundPrivateOrderReconciler(
        std::unique_ptr<PrivateOrderReconciler> reconciler_value) noexcept
        : reconciler{std::move(reconciler_value)} {}

    // --------------------------------------------------------
    // Preserve single-use installation authority and permit only no-throw move construction.
    PreparedRecoveryBoundPrivateOrderReconciler(
        const PreparedRecoveryBoundPrivateOrderReconciler&) = delete;
    PreparedRecoveryBoundPrivateOrderReconciler&
    operator=(const PreparedRecoveryBoundPrivateOrderReconciler&) = delete;
    PreparedRecoveryBoundPrivateOrderReconciler(
        PreparedRecoveryBoundPrivateOrderReconciler&&) noexcept = default;
    PreparedRecoveryBoundPrivateOrderReconciler&
    operator=(PreparedRecoveryBoundPrivateOrderReconciler&&) = delete;

    // --------------------------------------------------------
    // Retain the fully allocated child until the coordinator completes no-fail authority transfer.
    std::unique_ptr<PrivateOrderReconciler> reconciler;

    // --------------------------------------------------------
  };

  // ########################################################################
  // One private move-only value keeps the acknowledged provider inside its recovery lease between
  // final bootstrap consumption and ordered coordinator publication.
  struct ConsumedRecoveryIdentityAuthority final {

    // --------------------------------------------------------
    // Bind the transferred lease and provider after every fallible installation step has passed.
    ConsumedRecoveryIdentityAuthority(
        std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_identity_lease_value,
        model::DeterministicOrderIdProvider order_ids_value) noexcept
        : recovery_identity_lease{std::move(recovery_identity_lease_value)},
          order_ids{std::move(order_ids_value)} {}

    // --------------------------------------------------------
    // Preserve single-use authority and permit only no-throw movement into coordinator storage.
    ConsumedRecoveryIdentityAuthority(const ConsumedRecoveryIdentityAuthority&) = delete;
    ConsumedRecoveryIdentityAuthority& operator=(const ConsumedRecoveryIdentityAuthority&) = delete;
    ConsumedRecoveryIdentityAuthority(ConsumedRecoveryIdentityAuthority&&) noexcept = default;
    ConsumedRecoveryIdentityAuthority& operator=(ConsumedRecoveryIdentityAuthority&&) = delete;

    // --------------------------------------------------------
    // Declaration order destroys the provider before releasing this temporary lease share.
    std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_identity_lease;
    model::DeterministicOrderIdProvider order_ids;

    // --------------------------------------------------------
  };

  // ########################################################################

  // --------------------------------------------------------
  // Validate owner, configuration, policy, and recovery authority; allocate every empty slot; then
  // wrap the complete child while the bootstrap remains intact. A reported failure consumes
  // nothing and publishes no child.
  [[nodiscard]] static model::Result<PreparedRecoveryBoundPrivateOrderReconciler>
  prepare_recovery_bound_private_order_reconciler(
      const SubmissionCoordinator& owner, oms::OutboundOms& outbound_oms,
      const configuration::StartupConfiguration& configuration, const M4Policy& policy,
      const recovery::RecoveryBootstrap& recovery_bootstrap);

  // --------------------------------------------------------
  // Transfer the already validated bootstrap lease and provider only after every fallible step has
  // succeeded; the returned declaration order keeps the provider inside the live incarnation.
  [[nodiscard]] static ConsumedRecoveryIdentityAuthority
  consume_recovery_identity_authority(recovery::RecoveryBootstrap&& recovery_bootstrap) noexcept;

  // --------------------------------------------------------
  // Retain a const owner view, copied recovery identities, fully allocated empty tables, and the
  // opaque live lease; owner/lease lifetimes are guaranteed by unique ownership and destruction
  // order.
  PrivateOrderReconciler(
      const SubmissionCoordinator& owner, oms::OutboundOms& outbound_oms, M4Policy m4_policy,
      recovery::RecoveryLineageId recovery_lineage_id, recovery::RuntimeEpochId runtime_epoch_id,
      model::OrderNamespace registered_order_namespace, PrivateOrderEventFactory event_factory,
      std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
      std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
      std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings,
      std::unique_ptr<PrivateBusinessEvidenceStore> business_evidence_store,
      std::shared_ptr<const detail::PrivateBusinessTransactionIncarnation> business_incarnation,
      std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_lease,
      const configuration::StartupConfiguration& configuration);

  // --------------------------------------------------------
  // Commit a supported admitted input or retain its full source, progress, and containment. The
  // guard spans validation, publication, and the final lane-specific completion.
  [[nodiscard]] PrivateTurnCompletion commit_or_retain_admitted_private_turn(
      SerializedExecutor& executor, CriticalPrivateEventAttempt attempt,
      const AdmissionReceipt& receipt, model::TurnOrdinal turn_ordinal) noexcept;

  // --------------------------------------------------------
  // Bind the first authenticated event/fence executor only when its installed policy matches;
  // later foreign incarnations return false without changing owner state. The caller must hold
  // the retention turn guard; the retained opaque lease prevents same-address executor reuse.
  [[nodiscard]] bool bind_private_executor(SerializedExecutor& executor) noexcept;

  // --------------------------------------------------------
  // Copy complete evidence from an already retained genuine proposal and exact identity plan.
  // This helper borrows their fixed values for the serialized call and preserves both leases.
  [[nodiscard]] model::Result<PreparedPrivateBusinessEvidence>
  prepare_initial_business_evidence_from_proposal(
      const InitialKnownPrivateBusinessProposal& business,
      const FirstSeenAuthoritativePrivateIdentityPlan& identity,
      recovery::JournalReplayProvenance replay_provenance,
      std::optional<model::CallbackOrdinal> first_callback_ordinal,
      std::optional<recovery::JournalSequence> journal_sequence,
      std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) const;

  // --------------------------------------------------------
  // Borrow an order's active reverse binding, if any; one order can own at most one exchange key.
  [[nodiscard]] const PrivateExchangeOrderMapping*
  find_committed_exchange_mapping_for_order(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Reject an already compared incoming execution or a conflict with retained execution facts.
  [[nodiscard]] model::Result<void> validate_first_seen_private_execution_drain_identity(
      const FirstSeenAuthoritativePrivateIdentityPlan& identity,
      const KnownFirstSeenPrivateCorrelationPlan& correlation,
      const FirstSeenPrivateTradeIdentityPlan& trade, const oms::ExecutionPayload& execution) const;

  // --------------------------------------------------------
  // Verify immutable original BufferedGap evidence together with both active registry rows.
  // Borrowed identities are valid only within the serialized preparation call.
  [[nodiscard]] model::Result<PrivateExecutionDrainIdentity>
  verify_buffered_private_execution_identity(const oms::NormalizedPrivateOrderInput& input,
                                             const oms::KnownPrivateEventResolution& owner) const;

  // --------------------------------------------------------
  // Check canonical event/trade/mapping insertion and the genuine OMS gap before publication.
  // Failure leaves every row unchanged. Callers serialize validation through application.
  [[nodiscard]] model::Result<void>
  validate_buffered_gap_state(const FirstSeenAuthoritativePrivateIdentityPlan& identity,
                              const oms::PrivateOmsTransitionPlan& transition) const;

  // --------------------------------------------------------
  // Publish the previously validated OMS gap and its immutable identities after journal
  // publication in the same uninterrupted owner turn. This phase has no fallible operation.
  void apply_validated_buffered_gap_state(const FirstSeenAuthoritativePrivateIdentityPlan& identity,
                                          const oms::PrivateOmsTransitionPlan& transition) noexcept;

  // --------------------------------------------------------
  // Retain immutable owner/recovery authority, bounded canonical tables, and the opaque live lease.
  // The last-declared retention state owns the independently mutable preparations and gates.
  const SubmissionCoordinator* owner_;
  oms::OutboundOms* outbound_oms_;
  M4Policy m4_policy_;
  recovery::RecoveryLineageId recovery_lineage_id_;
  recovery::RuntimeEpochId runtime_epoch_id_;
  model::OrderNamespace registered_order_namespace_;
  PrivateOrderEventFactory event_factory_;
  std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records_;
  std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records_;
  std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings_;
  std::uint32_t event_identity_record_count_{0U};
  std::uint32_t trade_identity_record_count_{0U};
  std::uint32_t exchange_order_mapping_count_{0U};
  std::unique_ptr<PrivateBusinessEvidenceStore> business_evidence_store_;
  mutable PrivateExecutionDrainEvidenceScratch execution_drain_evidence_scratch_;
  // Cold capacity is the same one-plus-pending width used by the economic batch.
  mutable std::vector<risk::ReservationInventoryExecutionInput> execution_drain_inputs_;
  mutable std::vector<PrivateExecutionDrainIdentity> execution_drain_identities_;
  std::shared_ptr<const detail::PrivateBusinessTransactionIncarnation> business_incarnation_;
  std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_lease_;
  PrivateIdentityRetentionState retention_;

  // ########################################################################
  // Interesting syntax: friendship lets only the owning coordinator invoke the private validating
  // factory; it publishes no construction operation to other callers.
  friend class SubmissionCoordinator;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
