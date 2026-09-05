// Purpose: bind private identity planning, bounded retained preparations, and production admission
// containment to one submission owner without claiming canonical or economic consumption.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/recovery/deterministic_fake_recovery_medium.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "private_identity_retention.hpp"
#include "private_order_event_factory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The coordinator installs the child only while pristine, then owns and destroys it at one stable
// address while later M3 submissions remain independently mutable.
class SubmissionCoordinator;

// ########################################################################
// One event-identity slot schema pairs receive-time-free ingress semantics with an immutable
// resolution and original disposition. This slice allocates slots but never populates them.
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
// dispositions. This slice allocates slots but never populates or changes them.
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
// This slice allocates slots but never populates them.
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
// owner turns retain preparations and containment; canonical identity tables remain empty until
// the later joint business reducer exists. Read-only planning and account/table inspection require
// owner serialization or quiescence. Completion-error and immutable retained-turn queries support
// concurrent access through release/acquire publication.
// The opaque lease keeps cold medium inspection fenced and grants no journal mutation operation.
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
  // Retain one genuine ordinary or reconciliation token without claiming economic consumption.
  // Wrong-owner, moved, stale, or recursive authority changes no preparation or account state.
  [[nodiscard]] PrivateTurnCompletion
  commit_private_order_turn(AdmittedPrivateOrderSlot admitted) noexcept override;
  [[nodiscard]] PrivateTurnCompletion
  commit_reconciliation_event_turn(AdmittedReconciliationEventSlot admitted) noexcept override;

  // --------------------------------------------------------
  // No canonical disposition exists in this preparation-only owner, in either admission lane.
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_private_event_disposition(model::AdmissionOrdinal) const noexcept override {
    return std::nullopt;
  }
  [[nodiscard]] std::optional<oms::PrivateEventDisposition>
  find_committed_reconciliation_event_disposition(model::AdmissionOrdinal) const noexcept override {
    return std::nullopt;
  }

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
  // Return the canonical event count, which remains zero until the joint business reducer exists.
  [[nodiscard]] std::uint32_t event_identity_record_count() const noexcept {
    return event_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return the canonical trade count, which remains zero until the joint business reducer exists.
  [[nodiscard]] std::uint32_t trade_identity_record_count() const noexcept {
    return trade_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return the active mapping count; pending preparation candidates are deliberately excluded.
  [[nodiscard]] std::uint32_t exchange_order_mapping_count() const noexcept {
    return exchange_order_mapping_count_;
  }

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
  // bound pristine identity state. State inconsistency returns PrivateCorrelationFailed before
  // input validation; invalid source/payload shape returns InvalidPrivateEvent. Successful safety
  // conflicts remain detached results. This operation changes no owner, OMS, risk, evidence,
  // count, or slot state, so equal repeated inputs return equal values. The caller must prevent a
  // concurrent submission or other coordinator mutation until the call returns.
  [[nodiscard]] model::Result<FirstSeenAuthoritativePrivateIdentityPlan>
  derive_first_seen_authoritative_identity_plan(
      const oms::PrivateEventIngressSemanticValue& ingress_semantic_value) const;

  // --------------------------------------------------------
private:

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
      const SubmissionCoordinator& owner, const configuration::StartupConfiguration& configuration,
      const M4Policy& policy, const recovery::RecoveryBootstrap& recovery_bootstrap);

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
      const SubmissionCoordinator& owner, M4Policy m4_policy,
      recovery::RecoveryLineageId recovery_lineage_id, recovery::RuntimeEpochId runtime_epoch_id,
      model::OrderNamespace registered_order_namespace, PrivateOrderEventFactory event_factory,
      std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
      std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
      std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings,
      std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_lease,
      const configuration::StartupConfiguration& configuration);

  // --------------------------------------------------------
  // Jointly retain one admitted source, its optional preparation, and containment before releasing
  // the matching completion error. All allocation belongs to cold construction.
  [[nodiscard]] PrivateTurnCompletion
  retain_admitted_identity_turn(SerializedExecutor& executor, CriticalPrivateEventAttempt attempt,
                                const AdmissionReceipt& receipt,
                                model::TurnOrdinal turn_ordinal) noexcept;

  // --------------------------------------------------------
  // Bind the first authenticated event/fence executor only when its installed policy matches;
  // later foreign incarnations return false without changing owner state. The caller must hold
  // the retention turn guard; the retained opaque lease prevents same-address executor reuse.
  [[nodiscard]] bool bind_private_executor(SerializedExecutor& executor) noexcept;

  // --------------------------------------------------------
  // Retain immutable owner/recovery authority, empty canonical tables, and the opaque live lease.
  // The last-declared retention state owns the independently mutable preparations and gates.
  const SubmissionCoordinator* owner_;
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
