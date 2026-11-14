// Purpose: route authenticated private turns to bounded gap publication or full-source retention
// and own the monotonic account and global containment records.

#include "aegis/runtime/serialized_executor.hpp"
#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Allocate the exact error vocabulary once per cold record; owner turns only move selected copies.
[[nodiscard]] auto create_retention_errors() {
  using Code = model::DomainErrorCode;
  return std::array{
      model::DomainError::create_at_field(Code::ReconciliationIncomplete,
                                          "private_identity.pending_business_reducer"),
      model::DomainError::create_at_field(Code::PrivateEventConflict,
                                          "private_identity.preparation_conflict"),
      model::DomainError::create_at_field(Code::PrivateEventCapacityExceeded,
                                          "private_identity.preparation_capacity"),
      model::DomainError::create_at_field(Code::InvalidPrivateEvent,
                                          "private_identity.authoritative_input"),
      model::DomainError::create_at_field(Code::PrivateEvidenceExhausted,
                                          "private_identity.retained_turn_capacity"),
      model::DomainError::create_at_field(Code::PrivateCorrelationFailed,
                                          "private_identity.preparation_failure"),
      model::DomainError::create_at_field(Code::ExecutionNotPermitted,
                                          "private_identity.global_containment"),
      model::DomainError::create_at_field(Code::JournalCapacityExceeded,
                                          "private_business.journal_capacity"),
      model::DomainError::create_at_field(Code::PrivateEvidenceExhausted,
                                          "private_business.evidence_capacity")};
}

// --------------------------------------------------------
// Borrow a configured account only after exact root, account, and venue attribution is proved.
[[nodiscard]] PrivateAccountContainment*
find_attributable_account(PrivateIdentityRetentionState& state, const M4Policy& policy,
                          const oms::PrivateEventIngressSemanticValue& semantic) noexcept {
  if (semantic.provenance().root() != policy.root_provenance()) {
    return nullptr;
  }
  for (auto& account : state.accounts) {
    if (account.binding.logical_account_id == semantic.logical_account_id() &&
        account.binding.venue_id == semantic.venue_id()) {
      return &account;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Borrow the common immutable semantic source without confusing the two nominal admission lanes.
[[nodiscard]] const oms::PrivateEventIngressSemanticValue&
private_attempt_semantic_value(const CriticalPrivateEventAttempt& attempt) noexcept {
  return std::visit(
      [](const auto& source) -> const oms::PrivateEventIngressSemanticValue& {
        return source.semantic_value();
      },
      attempt);
}

// --------------------------------------------------------
// Append only a reason's first complete source and monotonically escalate the account gate.
// Assigned reasons fit the fixed nineteen-element set, so this commit cannot exhaust capacity.
void retain_account_safety_cause(PrivateAccountContainment& account,
                                 PrivateAccountSafetyCause cause) noexcept {
  for (std::size_t index = 0U; index < account.cause_count; ++index) {
    if (account.causes[index]->reason == cause.reason) {
      return;
    }
  }
  const bool quarantines = static_cast<std::uint8_t>(cause.reason) >= 6U;
  if (quarantines && !account.first_quarantine_cause.has_value()) {
    account.first_quarantine_cause = account.cause_count;
  }
  account.causes[account.cause_count++].emplace(std::move(cause));
  if (quarantines) {
    account.state = risk::AccountSafetyState::Quarantined;
  } else if (account.state == risk::AccountSafetyState::Synchronized) {
    account.state = risk::AccountSafetyState::ReconciliationRequired;
  }
}

// ########################################################################
// A successful atomic entry owns the sole release; a rejected recursive/concurrent call cannot
// clear another turn's guard or access owner-local preparation and account state.
class PrivateRetentionTurnGuard final {
public:

  // --------------------------------------------------------
  // Attempt one nonblocking entry into the owner-local mutation boundary.
  explicit PrivateRetentionTurnGuard(std::atomic_flag& active) noexcept
      : active_{active}, entered_{!active.test_and_set(std::memory_order_acquire)} {}

  // --------------------------------------------------------
  // A guard's release right cannot be duplicated or transferred to another stack lifetime.
  PrivateRetentionTurnGuard(const PrivateRetentionTurnGuard&) = delete;
  PrivateRetentionTurnGuard& operator=(const PrivateRetentionTurnGuard&) = delete;
  PrivateRetentionTurnGuard(PrivateRetentionTurnGuard&&) = delete;
  PrivateRetentionTurnGuard& operator=(PrivateRetentionTurnGuard&&) = delete;

  // --------------------------------------------------------
  // Release only this guard's successful entry after every owner-local write has completed.
  ~PrivateRetentionTurnGuard() {
    if (entered_) {
      active_.clear(std::memory_order_release);
    }
  }

  // --------------------------------------------------------
  // Query whether this call owns access to the non-atomic tables and containment state.
  [[nodiscard]] bool has_entered() const noexcept { return entered_; }

  // --------------------------------------------------------
private:
  std::atomic_flag& active_;
  bool entered_;
};

// ########################################################################

// --------------------------------------------------------
// Reject unauthorised entry without reserving evidence, allocating text, or changing owner state.
[[nodiscard]] PrivateTurnCompletion reject_private_retention_authority() noexcept {
  return RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
      model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Retain independent error ownership for immutable inspection and one-time completion transfer.
RetainedPrivateIdentityTurn::RetainedPrivateIdentityTurn()
    : oracle_errors{create_retention_errors()}, completion_errors{create_retention_errors()} {}

// --------------------------------------------------------
// Materialize every normal fact slot and configured-account fence before bootstrap is consumed.
PrivateIdentityRetentionState::PrivateIdentityRetentionState(
    const configuration::StartupConfiguration& configuration, const M4Policy& policy)
    : preparations{policy},
      turns(static_cast<std::size_t>(policy.capacities().max_private_event_records)),
      saturation_fault{
          model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                              "private_identity.retained_turn_capacity")} {
  accounts.reserve(configuration.logical_accounts().size());
  for (const auto& binding : configuration.logical_accounts()) {
    accounts.push_back(PrivateAccountContainment{
        PrivateAdmissionAccountBinding{binding.logical_account_id, binding.venue_id}});
  }
}

// --------------------------------------------------------
// Validate the token's live owner stack before inspecting the immutable consumer pointer.
PrivateTurnCompletion
PrivateOrderReconciler::commit_private_order_turn(AdmittedPrivateOrderSlot admitted) noexcept try {
  auto view = admitted.inspect_admitted_private_order_slot();
  if (!view || admitted.owner_->private_owner_ != this) {
    return reject_private_retention_authority();
  }
  return commit_or_retain_admitted_private_turn(*admitted.owner_, view.value().ingress_attempt(),
                                                view.value().admission_receipt(),
                                                view.value().turn_ordinal());
} catch (...) {
  return reject_private_retention_authority();
}

// --------------------------------------------------------
// Preserve the reconciliation token's nominal lane through the same bounded preparation reducer.
PrivateTurnCompletion PrivateOrderReconciler::commit_reconciliation_event_turn(
    AdmittedReconciliationEventSlot admitted) noexcept try {
  auto view = admitted.inspect_admitted_reconciliation_event_slot();
  if (!view || admitted.owner_->private_owner_ != this) {
    return reject_private_retention_authority();
  }
  return commit_or_retain_admitted_private_turn(*admitted.owner_, view.value().ingress_attempt(),
                                                view.value().admission_receipt(),
                                                view.value().turn_ordinal());
} catch (...) {
  return reject_private_retention_authority();
}

// --------------------------------------------------------
// Bind guarded private entry surfaces to one matching executor incarnation. Retaining its existing
// opaque lease cannot allocate and prevents a later executor at the same address from rebinding.
bool PrivateOrderReconciler::bind_private_executor(SerializedExecutor& executor) noexcept {
  if (executor.private_owner_ != this || !executor.private_configuration_.has_value() ||
      executor.private_configuration_->root_provenance() != m4_policy_.root_provenance() ||
      executor.private_admission_lease_ == nullptr) {
    return false;
  }
  if (retention_.admission_lease == nullptr) {
    retention_.admission_lease = executor.private_admission_lease_;
  }
  return retention_.admission_lease == executor.private_admission_lease_;
}

// --------------------------------------------------------
// Apply supported admitted facts under the owner guard. Otherwise preserve the complete source,
// exact failure, and containment before returning its retained completion.
PrivateTurnCompletion PrivateOrderReconciler::commit_or_retain_admitted_private_turn(
    SerializedExecutor& executor, CriticalPrivateEventAttempt attempt,
    const AdmissionReceipt& receipt, model::TurnOrdinal turn_ordinal) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind to one executor incarnation and reject concurrent/reentrant access without table writes.
  PrivateRetentionTurnGuard guard{retention_.owner_turn_active};
  if (!guard.has_entered() || !bind_private_executor(executor) ||
      retention_.emergency_turn_published.load(std::memory_order_acquire)) {
    return reject_private_retention_authority();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Route a supported fact through the closed joint commit only when failure can still retain the
  // complete input. A saturated retention store instead takes its existing fail-stop path.
  const auto count = retention_.published_turn_count.load(std::memory_order_relaxed);
  const bool saturated = count == retention_.turns.size();
  const auto& semantic = private_attempt_semantic_value(attempt);
  auto* account = find_attributable_account(retention_, m4_policy_, semantic);
  const auto normalized =
      event_factory_.normalize_private_event_ingress_semantic_value(semantic, receipt.received_at);
  CallbackFreePrivateCommitResult commit;
  if (!saturated && account != nullptr && !retention_.globally_blocked &&
      !owner_->is_runtime_faulted()) {
    try {
      commit = commit_callback_free_private_input_if_supported(normalized, receipt);
    } catch (...) {
      commit.safety_reason = risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure;
    }
    if (commit.disposition) {
      if (*commit.disposition == oms::PrivateEventDisposition::BufferedGap) {
        return BufferedPrivateTurn{};
      }
      return ConsumedPrivateTurn{*commit.disposition};
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve failed or unsupported inputs in fixed retention storage. No retained comparison
  // becomes an active identity, even when its input had a valid canonical order resolution.
  auto& record = saturated ? retention_.emergency_turn : retention_.turns[count];
  record.receipt = receipt;
  record.turn_ordinal = turn_ordinal;
  record.attempt.emplace(std::move(attempt));
  const auto& retained_semantic = private_attempt_semantic_value(*record.attempt);
  record.normalized = normalized;
  record.safety_reason = account != nullptr
                             ? std::optional{risk::AccountSafetyReason::IncompleteReconciliation}
                             : std::nullopt;

  // ++++++++++++++++++++++++++++++++++++++++
  // Repeated event keys reuse their original resolution before any new provenance or correlation
  // derivation. Preparation failures retain the entire input without changing its identity tables.
  if (saturated) {
    record.error_kind = PrivateIdentityRetentionError::EvidenceCapacity;
    if (account != nullptr) {
      record.safety_reason = risk::AccountSafetyReason::EvidenceCapacityExhausted;
    }
  } else if (retention_.globally_blocked) {
    record.error_kind = PrivateIdentityRetentionError::GlobalContainment;
  } else if (commit.safety_reason) {
    record.safety_reason = commit.safety_reason;
    if (commit.error) {
      const auto code = commit.error->code;
      if (code == model::DomainErrorCode::JournalCapacityExceeded) {
        record.error_kind = PrivateIdentityRetentionError::JournalCapacity;
      } else if (*commit.safety_reason == risk::AccountSafetyReason::EvidenceCapacityExhausted) {
        record.error_kind = PrivateIdentityRetentionError::BusinessPreflightFailure;
      } else if (code == model::DomainErrorCode::PrivateEventCapacityExceeded) {
        record.error_kind = PrivateIdentityRetentionError::IdentityCapacity;
      } else if (code == model::DomainErrorCode::PrivateEventConflict ||
                 *commit.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction) {
        record.error_kind = PrivateIdentityRetentionError::IdentityConflict;
      } else if (code != model::DomainErrorCode::ReconciliationIncomplete) {
        record.error_kind = PrivateIdentityRetentionError::InternalFailure;
      }
      record.planning_error = std::move(commit.error);
    } else {
      record.error_kind = PrivateIdentityRetentionError::InternalFailure;
    }
  } else if (!std::holds_alternative<oms::LocalPrivateIngressOrigin>(retained_semantic.origin())) {
    try {
      const auto key = oms::PrivateEventRegistryKey::from_ingress_semantic_value(retained_semantic);
      const auto* existing = retention_.preparations.find_prepared_event(key);
      if (existing != nullptr) {
        record.preparation =
            PrivateIdentityPreparationStore::classify_repeated_event(*existing, retained_semantic);
        if (account != nullptr && existing->safety_reason.has_value()) {
          record.safety_reason = existing->safety_reason;
        }
      } else {
        auto plan = derive_first_seen_authoritative_identity_plan(retained_semantic);
        if (plan) {
          record.preparation = retention_.preparations.prepare_and_retain_identity(plan.value());
        } else {
          record.planning_error = std::move(plan).error();
          record.error_kind =
              record.planning_error->code == model::DomainErrorCode::InvalidPrivateEvent
                  ? PrivateIdentityRetentionError::InvalidInput
                  : PrivateIdentityRetentionError::InternalFailure;
          if (account != nullptr) {
            record.safety_reason = risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure;
          }
        }
      }
      if (record.preparation.has_value()) {
        if (record.preparation->capacity_exhaustion.has_value()) {
          record.error_kind = PrivateIdentityRetentionError::IdentityCapacity;
          if (account != nullptr) {
            record.safety_reason = risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure;
          }
        } else if (record.preparation->safety_reason.has_value()) {
          record.error_kind =
              *record.preparation->safety_reason == risk::AccountSafetyReason::UnknownOrder ||
                      *record.preparation->safety_reason == risk::AccountSafetyReason::UnknownTrade
                  ? PrivateIdentityRetentionError::PendingReducer
                  : PrivateIdentityRetentionError::IdentityConflict;
          if (account != nullptr) {
            record.safety_reason = record.preparation->safety_reason;
          }
        }
      }
    } catch (...) {
      record.error_kind = PrivateIdentityRetentionError::InternalFailure;
      if (account != nullptr) {
        record.safety_reason = risk::AccountSafetyReason::ArithmeticOrStateCapacityFailure;
      }
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Containment records the failed or unsupported source without disturbing committed economics,
  // identities, callbacks, or the recovery acknowledgement watermark.
  if (account != nullptr) {
    retain_account_safety_cause(*account,
                                PrivateAccountSafetyCause{*record.safety_reason, record.attempt,
                                                          receipt.attempt_ordinal, std::nullopt});
    if (record.safety_reason == risk::AccountSafetyReason::UnknownTrade) {
      retain_account_safety_cause(*account,
                                  PrivateAccountSafetyCause{risk::AccountSafetyReason::UnknownOrder,
                                                            record.attempt, receipt.attempt_ordinal,
                                                            std::nullopt});
    }
  } else {
    retention_.globally_blocked = true;
    if (!retention_.first_global_attempt.has_value()) {
      retention_.first_global_attempt = record.attempt;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prepare the completion before release-publishing its immutable oracle and full source. Moving
  // the preallocated error avoids allocating after any preparation or gate mutation.
  static_assert(std::is_nothrow_move_constructible_v<model::DomainError>);
  static_assert(std::is_nothrow_copy_constructible_v<CriticalPrivateEventAttempt>);
  const auto error_index = static_cast<std::size_t>(record.error_kind);
  record.first_admission_resolution =
      record.preparation ? std::optional{record.preparation->resolution} : commit.resolution;
  record.execution_progress = std::move(commit.execution_progress);
  auto completion =
      account != nullptr
          ? RetainedPrivateTurn::create_retained_private_turn_for_account(
                std::move(record.completion_errors[error_index]), *record.safety_reason,
                record.normalized, record.first_admission_resolution)
          : RetainedPrivateTurn::create_retained_private_turn_for_global_containment(
                std::move(record.completion_errors[error_index]), record.normalized,
                record.first_admission_resolution);
  if (saturated) {
    retention_.emergency_turn_published.store(true, std::memory_order_release);
    const auto fault = executor.request_owner_fault(std::move(retention_.saturation_fault));
    if (!fault) {
      std::terminate();
    }
  } else {
    retention_.published_turn_count.store(count + 1U, std::memory_order_release);
  }
  return completion;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Acquire immutable records through the published prefix; lane identity is part of every lookup.
const RetainedPrivateIdentityTurn*
PrivateOrderReconciler::find_retained_identity_turn(model::AdmissionOrdinal attempt_ordinal,
                                                    bool reconciliation) const noexcept {
  const auto matches = [attempt_ordinal, reconciliation](const auto& record) {
    return record.receipt.has_value() && record.receipt->attempt_ordinal == attempt_ordinal &&
           std::holds_alternative<oms::ReconciliationPrivateEventIngressAttempt>(*record.attempt) ==
               reconciliation;
  };
  const auto count = retention_.published_turn_count.load(std::memory_order_acquire);
  for (std::size_t index = 0U; index < count; ++index) {
    if (matches(retention_.turns[index])) {
      return &retention_.turns[index];
    }
  }
  if (retention_.emergency_turn_published.load(std::memory_order_acquire) &&
      matches(retention_.emergency_turn)) {
    return &retention_.emergency_turn;
  }
  return nullptr;
}

// --------------------------------------------------------
// Ordinary completion evidence can be satisfied only by the same ordinary admission ordinal.
const model::DomainError* PrivateOrderReconciler::find_committed_retained_private_event_error(
    model::AdmissionOrdinal attempt_ordinal) const noexcept {
  const auto* record = find_retained_identity_turn(attempt_ordinal, false);
  return record == nullptr ? nullptr
                           : &record->oracle_errors[static_cast<std::size_t>(record->error_kind)];
}

// --------------------------------------------------------
// Reconciliation completion evidence cannot borrow any ordinary admission's retained error.
const model::DomainError*
PrivateOrderReconciler::find_committed_retained_reconciliation_event_error(
    model::AdmissionOrdinal attempt_ordinal) const noexcept {
  const auto* record = find_retained_identity_turn(attempt_ordinal, true);
  return record == nullptr ? nullptr
                           : &record->oracle_errors[static_cast<std::size_t>(record->error_kind)];
}

// --------------------------------------------------------
// Return the monotonic owner-local gate; absence cannot authorize a new account's exposure.
risk::AccountSafetyState
PrivateOrderReconciler::account_safety_state(model::LogicalAccountId account_id) const noexcept {
  for (const auto& account : retention_.accounts) {
    if (account.binding.logical_account_id == account_id) {
      return account.state;
    }
  }
  return risk::AccountSafetyState::Quarantined;
}

// --------------------------------------------------------
// Preserve uncertainty only from the exact retained row owned by the bound M3 coordinator.
void PrivateOrderReconciler::record_submission_uncertainty(
    const oms::OutboundOrderRecord& order) noexcept {
  if (owner_->outbound_oms().find_order(order.order_id()) != &order ||
      order.state() != oms::OutboundOrderState::SubmissionUnknown) {
    retention_.globally_blocked = true;
    return;
  }
  for (auto& account : retention_.accounts) {
    if (account.binding.logical_account_id == order.provenance().logical_account_id &&
        account.binding.venue_id == order.provenance().venue_id) {
      retain_account_safety_cause(
          account, PrivateAccountSafetyCause{risk::AccountSafetyReason::SubmissionUnknown,
                                             std::nullopt, std::nullopt, order.admission()});
      return;
    }
  }
  retention_.globally_blocked = true;
}

// --------------------------------------------------------
// Validate the entire dedicated account fence before appending any new cause or changing the gate.
model::Result<void>
PrivateOrderReconciler::apply_account_safety_fence(const AccountSafetyFenceTurn& fence,
                                                   const ControlTurnContext& context) noexcept {
  auto* executor =
      PrivateFenceTurnAuthority::find_current_account_fence_executor(*this, fence, context);
  if (executor == nullptr) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }
  PrivateRetentionTurnGuard guard{retention_.owner_turn_active};
  if (!guard.has_entered() || !bind_private_executor(*executor)) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }
  auto* account = static_cast<PrivateAccountContainment*>(nullptr);
  for (auto& candidate : retention_.accounts) {
    if (candidate.binding.logical_account_id == fence.logical_account_id &&
        candidate.binding.venue_id == fence.venue_id) {
      account = &candidate;
      break;
    }
  }
  if (account == nullptr || fence.reason_occurrence_count == 0U ||
      fence.reason_occurrence_count > account_safety_reason_occurrence_capacity ||
      fence.lost_attempt_count < fence.reason_occurrence_count ||
      fence.lost_attempt_count >
          std::numeric_limits<std::uint64_t>::max() - account->fenced_attempt_count) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require exact attribution, unique assigned reasons, and increasing original attempt order.
  std::array<bool, account_safety_reason_occurrence_capacity> seen{};
  std::uint64_t last_ordinal = 0U;
  for (std::size_t index = 0U; index < account_safety_reason_occurrence_capacity; ++index) {
    const auto& occurrence = fence.ordered_unique_reason_occurrences[index];
    if (index >= fence.reason_occurrence_count) {
      if (occurrence.has_value()) {
        return model::Result<void>::create_failure(
            model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
      }
      continue;
    }
    if (!occurrence.has_value()) {
      return model::Result<void>::create_failure(
          model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
    }
    const auto reason = static_cast<std::uint8_t>(occurrence->reason);
    if (reason == 0U || reason > account_safety_reason_occurrence_capacity || seen[reason - 1U] ||
        occurrence->first_attempt_ordinal.value() <= last_ordinal ||
        find_attributable_account(retention_, m4_policy_,
                                  private_attempt_semantic_value(occurrence->first_attempt)) !=
            account) {
      return model::Result<void>::create_failure(
          model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
    }
    seen[reason - 1U] = true;
    last_ordinal = occurrence->first_attempt_ordinal.value();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish retained fence provenance and monotonic gating only after the full interval validates.
  for (std::size_t index = 0U; index < fence.reason_occurrence_count; ++index) {
    const auto& occurrence = *fence.ordered_unique_reason_occurrences[index];
    retain_account_safety_cause(
        *account, PrivateAccountSafetyCause{occurrence.reason, occurrence.first_attempt,
                                            occurrence.first_attempt_ordinal, std::nullopt});
  }
  account->fenced_attempt_count += fence.lost_attempt_count;
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preserve a reasonless unattributable fence without choosing an account or clearing prior gates.
model::Result<void>
PrivateOrderReconciler::apply_global_private_fence(const GlobalPrivateFenceTurn& fence,
                                                   const ControlTurnContext& context) noexcept {
  auto* executor =
      PrivateFenceTurnAuthority::find_current_global_fence_executor(*this, fence, context);
  if (executor == nullptr) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }
  PrivateRetentionTurnGuard guard{retention_.owner_turn_active};
  if (!guard.has_entered() || !bind_private_executor(*executor)) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::ExecutionNotPermitted, {}});
  }
  if (fence.lost_attempt_count == 0U ||
      find_attributable_account(retention_, m4_policy_,
                                private_attempt_semantic_value(fence.first_attempt)) != nullptr) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
  }
  if (!retention_.global_fence.has_value()) {
    retention_.global_fence = fence;
  } else if (*retention_.global_fence != fence) {
    return model::Result<void>::create_failure(
        model::DomainError{model::DomainErrorCode::InvalidAccountSafetyState, {}});
  }
  retention_.globally_blocked = true;
  return model::Result<void>::create_success();
}

// --------------------------------------------------------

} // namespace aegis::runtime
