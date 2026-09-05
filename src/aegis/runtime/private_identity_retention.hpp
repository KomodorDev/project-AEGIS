// Purpose: preallocate production private-fact retention and account containment independently
// of canonical event consumption, economic ledgers, and recoverable business journal records.

#pragma once

#include "aegis/oms/outbound_oms.hpp"
#include "aegis/runtime/private_order_admission.hpp"
#include "private_identity_preparation.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Retention failures select constructor-owned error copies so owner publication never allocates.
enum class PrivateIdentityRetentionError : std::size_t {
  PendingReducer,
  IdentityConflict,
  IdentityCapacity,
  InvalidInput,
  EvidenceCapacity,
  InternalFailure,
  GlobalContainment,
  Count,
};

// ########################################################################
// One append-only record owns the complete source and receipt plus any completed preparation.
// It intentionally contains no PrivateEventDisposition or business-journal acknowledgement.
struct RetainedPrivateIdentityTurn {

  // --------------------------------------------------------
  // Prepare independent completion and immutable oracle errors before any owner turn exists.
  RetainedPrivateIdentityTurn();

  // --------------------------------------------------------
  std::optional<CriticalPrivateEventAttempt> attempt;
  std::optional<AdmissionReceipt> receipt;
  std::optional<model::TurnOrdinal> turn_ordinal;
  std::optional<oms::NormalizedPrivateOrderInput> normalized;
  std::optional<PrivateIdentityPreparationResult> preparation;
  std::optional<model::DomainError> planning_error;
  std::optional<risk::AccountSafetyReason> safety_reason;
  PrivateIdentityRetentionError error_kind{PrivateIdentityRetentionError::PendingReducer};
  std::array<model::DomainError, static_cast<std::size_t>(PrivateIdentityRetentionError::Count)>
      oracle_errors;
  std::array<model::DomainError, static_cast<std::size_t>(PrivateIdentityRetentionError::Count)>
      completion_errors;
};

// ########################################################################
// A first occurrence retains either a complete private fact or the authentic uncertain local row;
// the two source shapes cannot manufacture each other's attribution.
struct PrivateAccountSafetyCause {
  risk::AccountSafetyReason reason;
  std::optional<CriticalPrivateEventAttempt> private_attempt;
  std::optional<model::AdmissionOrdinal> attempt_ordinal;
  std::optional<oms::OutboundOrderAdmission> uncertain_submission;
};

// ########################################################################
// One configured account owns a monotonic gate and every unique reason's immutable first source.
// Reasons never clear in this preparation slice; explicit recovery remains a later contract.
struct PrivateAccountContainment {
  PrivateAdmissionAccountBinding binding;
  risk::AccountSafetyState state{risk::AccountSafetyState::Synchronized};
  std::array<std::optional<PrivateAccountSafetyCause>, account_safety_reason_occurrence_capacity>
      causes{};
  std::size_t cause_count{0U};
  std::optional<std::size_t> first_quarantine_cause{};
  std::uint64_t fenced_attempt_count{0U};
};

// ########################################################################
// Owns fixed normal retention and one dedicated fail-stop record outside its accepted prefix.
// The emergency record preserves the first overflow and immediately faults the executor; it is
// never reusable capacity. Release/acquire publication exposes immutable records to concurrent
// lane-specific error and retained-turn lookups; all preparation and account inspection otherwise
// requires owner serialization or quiescence. No owner turn grows a vector or takes an
// evidence-store lock. Retaining the first executor's opaque lease prevents address reuse from
// rebinding authority.
class PrivateIdentityRetentionState final {
public:

  // --------------------------------------------------------
  // Allocate every record, candidate table, account gate, and reserved error from sealed policy.
  PrivateIdentityRetentionState(const configuration::StartupConfiguration& configuration,
                                const M4Policy& policy);

  // --------------------------------------------------------
  // Atomic publication state and borrowed immutable pointers require a stable nonmoving owner.
  PrivateIdentityRetentionState(const PrivateIdentityRetentionState&) = delete;
  PrivateIdentityRetentionState& operator=(const PrivateIdentityRetentionState&) = delete;
  PrivateIdentityRetentionState(PrivateIdentityRetentionState&&) = delete;
  PrivateIdentityRetentionState& operator=(PrivateIdentityRetentionState&&) = delete;

  // --------------------------------------------------------
  PrivateIdentityPreparationStore preparations;
  std::vector<RetainedPrivateIdentityTurn> turns;
  std::atomic<std::size_t> published_turn_count{0U};
  RetainedPrivateIdentityTurn emergency_turn;
  std::atomic<bool> emergency_turn_published{false};
  std::vector<PrivateAccountContainment> accounts;
  std::optional<GlobalPrivateFenceTurn> global_fence;
  std::optional<CriticalPrivateEventAttempt> first_global_attempt;
  bool globally_blocked{false};
  std::shared_ptr<PrivateAdmissionLease> admission_lease;
  std::atomic_flag owner_turn_active = ATOMIC_FLAG_INIT;
  model::DomainError saturation_fault;
};

// ########################################################################

} // namespace aegis::runtime
