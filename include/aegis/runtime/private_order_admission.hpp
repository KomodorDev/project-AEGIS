// Purpose: define sealed M4 private-admission configuration, lifecycle observations, owner-only
// slot authority, completion values, and account/global loss-fence turns without executor storage.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/domain_error.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/risk/account_safety.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The serialized executor alone may mint and validate private owner-turn capabilities.
class SerializedExecutor;

// ########################################################################
// A construction-time lease lets a retained token reject executor destruction before touching its
// opaque raw owner address.
class PrivateAdmissionLease;

// ########################################################################
// Ordinary admission outcomes remain successful Result values so overload and shutdown cannot be
// confused with terminal executor faults.
enum class AdmissionOutcome : std::uint8_t {
  Accepted = 1,
  CapacityExceeded = 2,
  Closed = 3,
};

// ########################################################################
// An accepted receipt binds one attempt to its distinct receive sequence, timestamp, and atomic
// pending-queue observation.
struct AdmissionReceipt {
  model::AdmissionOrdinal attempt_ordinal;
  model::ReceiveSequence receive_sequence;
  model::ReceiveTimestamp received_at;
  std::size_t pending_depth;
  std::size_t pending_capacity;

  // --------------------------------------------------------
  // Structural equality pins the complete accepted-admission replay input.
  friend bool operator==(const AdmissionReceipt&, const AdmissionReceipt&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// AcceptedTurnContext is stack-scoped owner authority for one accepted command. Handlers must use
// stable runtime handles in their copied command and must not retain this context reference.
struct AcceptedTurnContext {
  AdmissionReceipt receipt;
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;
  model::ElapsedNanoseconds queue_age;

  // --------------------------------------------------------
  // Structural equality pins every value available while one accepted command runs.
  friend bool operator==(const AcceptedTurnContext&, const AcceptedTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// ControlTurnContext gives a fence handler only its owner-turn identity and processing time; a
// fence has no receive sequence, timestamp, or queue age.
struct ControlTurnContext {
  model::TurnOrdinal turn_ordinal;
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins the complete control-turn authority.
  friend bool operator==(const ControlTurnContext&, const ControlTurnContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One canonical configured account/venue pair is copied from sealed startup configuration and
// remains observational only; callers cannot assemble an accepted admission configuration from it.
struct PrivateAdmissionAccountBinding {
  model::LogicalAccountId logical_account_id;
  model::VenueId venue_id;

  // --------------------------------------------------------
  // Structural equality compares the complete account attribution boundary.
  friend bool operator==(const PrivateAdmissionAccountBinding&,
                         const PrivateAdmissionAccountBinding&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The sealed configuration derives all private-lane capacities and account bindings from mutually
// consistent startup and M4 authorities; no executor constructor accepts a caller-authored table.
class PrivateAdmissionConfiguration final {
public:

  // --------------------------------------------------------
  // Cross-check configuration identity and narrow only the three validated M4 admission
  // capacities. A provenance mismatch, invalid bound, or allocation failure returns
  // InvalidM4Policy without publishing a partial configuration.
  [[nodiscard]] static model::Result<PrivateAdmissionConfiguration>
  create_private_admission_configuration(const configuration::StartupConfiguration& configuration,
                                         const M4Policy& policy) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject an unrelated configuration before copying any account binding or capacity.
    const auto& root = policy.root_provenance();
    if (configuration.fingerprint().bytes() != root.configuration_fingerprint()) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.configuration_fingerprint"));
    }
    if (configuration.organization().revision() != root.organization_revision()) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.organization_revision"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Revalidate component bounds before narrowing sealed unsigned policy values to u32 storage.
    const auto& capacities = policy.capacities();
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
    if (capacities.max_private_admissions == 0U || capacities.max_private_admissions > maximum) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.max_private_admissions"));
    }
    if (capacities.max_reconciliation_admissions == 0U ||
        capacities.max_reconciliation_admissions > maximum) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.max_reconciliation_admissions"));
    }
    const auto configured_account_count =
        static_cast<std::uint64_t>(configuration.logical_accounts().size());
    if (capacities.max_account_safety_fences == 0U ||
        capacities.max_account_safety_fences > maximum ||
        capacities.max_account_safety_fences < configured_account_count) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.max_account_safety_fences"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Preserve startup configuration's canonical order while dropping caller-irrelevant firm data.
    try {
      std::vector<PrivateAdmissionAccountBinding> bindings;
      bindings.reserve(configuration.logical_accounts().size());
      for (const auto& binding : configuration.logical_accounts()) {
        bindings.push_back(
            PrivateAdmissionAccountBinding{binding.logical_account_id, binding.venue_id});
      }
      return model::Result<PrivateAdmissionConfiguration>::create_success(
          PrivateAdmissionConfiguration{
              root, static_cast<std::uint32_t>(capacities.max_private_admissions),
              static_cast<std::uint32_t>(capacities.max_reconciliation_admissions),
              static_cast<std::uint32_t>(capacities.max_account_safety_fences),
              std::move(bindings)});
    } catch (const std::bad_alloc&) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.capacity_allocation"));
    } catch (const std::length_error&) {
      return model::Result<PrivateAdmissionConfiguration>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidM4Policy,
                                              "private_admission.capacity_allocation"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Expose the fixed private reserve selected by the sealed M4 policy.
  [[nodiscard]] std::uint32_t private_admission_capacity() const noexcept {
    return private_capacity_;
  }

  // --------------------------------------------------------
  // Expose the reserved reconciliation capacity without enabling its deferred ingress lane.
  [[nodiscard]] std::uint32_t reconciliation_admission_capacity() const noexcept {
    return reconciliation_capacity_;
  }

  // --------------------------------------------------------
  // Expose the policy ceiling that was proved to cover every configured account fence.
  [[nodiscard]] std::uint32_t account_safety_fence_capacity() const noexcept {
    return account_fence_capacity_;
  }

  // --------------------------------------------------------
  // Borrow the complete sealed root that every attributable private fact must match exactly.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Borrow the exact canonical account/venue bindings copied from startup authority.
  [[nodiscard]] const std::vector<PrivateAdmissionAccountBinding>&
  account_bindings() const noexcept {
    return account_bindings_;
  }

  // --------------------------------------------------------
  // Construction remains behind the named factory so authored capacity or binding vectors cannot
  // be installed.
private:

  // --------------------------------------------------------
  // Publish only the already cross-validated narrowed capacities and canonical binding copy.
  PrivateAdmissionConfiguration(
      model::M4RootProvenance root_provenance, std::uint32_t private_capacity,
      std::uint32_t reconciliation_capacity, std::uint32_t account_fence_capacity,
      std::vector<PrivateAdmissionAccountBinding> account_bindings) noexcept
      : root_provenance_{std::move(root_provenance)}, private_capacity_{private_capacity},
        reconciliation_capacity_{reconciliation_capacity},
        account_fence_capacity_{account_fence_capacity},
        account_bindings_{std::move(account_bindings)} {}

  // --------------------------------------------------------
  model::M4RootProvenance root_provenance_;
  std::uint32_t private_capacity_;
  std::uint32_t reconciliation_capacity_;
  std::uint32_t account_fence_capacity_;
  std::vector<PrivateAdmissionAccountBinding> account_bindings_;
};

// ########################################################################
// Stable critical lifecycle assignments distinguish copied work from its two terminal owner
// results.
enum class CriticalPrivateAdmissionState : std::uint8_t {
  CopiedAndAdmitted = 1,
  EconomicallyConsumed = 2,
  RetainedForReconciliation = 3,
};

// ########################################################################
// One producer decision reports the shared attempt identity, private reserve observation, optional
// accepted receipt/state, and whether a configured-account loss fence was recorded.
struct PrivateAdmissionDecision {
  AdmissionOutcome outcome;
  model::AdmissionOrdinal attempt_ordinal;
  std::size_t pending_depth;
  std::size_t pending_capacity;
  std::optional<AdmissionReceipt> receipt;
  bool account_fence_recorded;
  std::optional<CriticalPrivateAdmissionState> state;

  // --------------------------------------------------------
  // Structural equality compares every producer-visible admission consequence.
  friend bool operator==(const PrivateAdmissionDecision&,
                         const PrivateAdmissionDecision&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One terminal or live observation exposes copied queue ownership before owner commit, a terminal
// disposition after consumption, or exactly one retention error for reconciliation work. A
// completed BufferedGap has no observation until its older ordinal advances to AppliedFromBuffer,
// because it no longer owns an admission slot and is not yet economically consumed.
struct PrivateAdmissionObservation {
  model::AdmissionOrdinal attempt_ordinal;
  CriticalPrivateAdmissionState state;
  std::optional<oms::PrivateEventDisposition> disposition;
  std::optional<model::DomainError> retention_error;

  // --------------------------------------------------------
  // Structural equality compares the complete source/driver lifecycle observation.
  friend bool operator==(const PrivateAdmissionObservation&,
                         const PrivateAdmissionObservation&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// An immutable token inspection copies owner-turn source and receipt authority, so retaining the
// returned value cannot dereference a reused slot or destroyed executor.
class AdmittedPrivateOrderSlotView final {
public:

  // --------------------------------------------------------
  // Borrow the token-owned immutable source fact without touching executor slot storage.
  [[nodiscard]] const oms::PrivateOrderIngressAttempt& ingress_attempt() const noexcept {
    return attempt_;
  }

  // --------------------------------------------------------
  // Borrow the complete accepted receipt copied into this lifetime-safe view.
  [[nodiscard]] const AdmissionReceipt& admission_receipt() const noexcept {
    return context_.receipt;
  }

  // --------------------------------------------------------
  // Return the exact shared owner-turn identity attached by the executor.
  [[nodiscard]] model::TurnOrdinal turn_ordinal() const noexcept { return context_.turn_ordinal; }

  // --------------------------------------------------------
  // Return the sole processing-time observation for this owner turn.
  [[nodiscard]] model::ProcessingTimestamp processing_timestamp() const noexcept {
    return context_.processing_timestamp;
  }

  // --------------------------------------------------------
  // Return the checked receive-to-processing delay retained by accepted authority.
  [[nodiscard]] model::ElapsedNanoseconds queue_age() const noexcept { return context_.queue_age; }

  // --------------------------------------------------------
  // Structural equality pins the complete copied view returned after owner validation.
  friend bool operator==(const AdmittedPrivateOrderSlotView&,
                         const AdmittedPrivateOrderSlotView&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Only a validated token may publish its immutable local copies as an inspection value.
  AdmittedPrivateOrderSlotView(oms::PrivateOrderIngressAttempt attempt,
                               AcceptedTurnContext context) noexcept
      : attempt_{std::move(attempt)}, context_{std::move(context)} {}

  // --------------------------------------------------------
  oms::PrivateOrderIngressAttempt attempt_;
  AcceptedTurnContext context_;

  // ########################################################################
  // The move-only token validates owner metadata before copying this view.
  friend class AdmittedPrivateOrderSlot;

  // ########################################################################
};

// ########################################################################
// The move-only capability owns immutable source/turn copies plus opaque executor metadata. Moving
// it invalidates the source token, and public inspection never dereferences pending ring storage.
class AdmittedPrivateOrderSlot final {
public:

  // --------------------------------------------------------
  // Copying is forbidden because two live values must never represent the same owner capability.
  AdmittedPrivateOrderSlot(const AdmittedPrivateOrderSlot&) = delete;
  AdmittedPrivateOrderSlot& operator=(const AdmittedPrivateOrderSlot&) = delete;

  // --------------------------------------------------------
  // Transfer the sole token authority while invalidating the moved-from owner pointer.
  AdmittedPrivateOrderSlot(AdmittedPrivateOrderSlot&& other) noexcept
      : owner_{std::exchange(other.owner_, nullptr)}, generation_{other.generation_},
        lifetime_{std::move(other.lifetime_)}, attempt_{std::move(other.attempt_)},
        context_{std::move(other.context_)} {}

  // --------------------------------------------------------
  // Move assignment and implicit lifetime extension are forbidden; destruction only drops
  // authority.
  AdmittedPrivateOrderSlot& operator=(AdmittedPrivateOrderSlot&&) = delete;
  ~AdmittedPrivateOrderSlot() = default;

  // --------------------------------------------------------
  // Validate owner lifetime, admission generation, and active turn before publishing immutable
  // copies; stale, moved-from, or out-of-turn authority returns a DomainError without a view.
  [[nodiscard]] model::Result<AdmittedPrivateOrderSlotView>
  inspect_admitted_private_order_slot() const;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Only the executor may bind one token to an exact admitted generation and owner turn.
  AdmittedPrivateOrderSlot(SerializedExecutor& owner, model::AdmissionOrdinal generation,
                           std::weak_ptr<PrivateAdmissionLease> lifetime,
                           oms::PrivateOrderIngressAttempt attempt,
                           AcceptedTurnContext context) noexcept
      : owner_{&owner}, generation_{generation}, lifetime_{std::move(lifetime)},
        attempt_{std::move(attempt)}, context_{std::move(context)} {}

  // --------------------------------------------------------
  SerializedExecutor* owner_;
  model::AdmissionOrdinal generation_;
  std::weak_ptr<PrivateAdmissionLease> lifetime_;
  oms::PrivateOrderIngressAttempt attempt_;
  AcceptedTurnContext context_;

  // ########################################################################
  // The non-relocatable executor is the sole capability minting and validation authority.
  friend class SerializedExecutor;

  // ########################################################################
};

// ########################################################################
// A consumed owner result names the exact append-only committed event disposition it observed.
struct ConsumedPrivateTurn {
  oms::PrivateEventDisposition disposition;

  // --------------------------------------------------------
  // Structural equality pins the exact append-only disposition claimed by owner consumption.
  friend bool operator==(const ConsumedPrivateTurn&, const ConsumedPrivateTurn&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A buffered owner result confirms that the exact admitted ordinal committed BufferedGap evidence
// without applying economics; the owner oracle may later advance it once to AppliedFromBuffer.
struct BufferedPrivateTurn {

  // --------------------------------------------------------
  // Structural equality keeps the successful nonterminal completion arm explicit.
  friend bool operator==(const BufferedPrivateTurn&, const BufferedPrivateTurn&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A retained owner result keeps its nominal failure and optional progress. Named constructors make
// configured-account reason presence and reasonless global containment structurally explicit.
class RetainedPrivateTurn final {
public:

  // --------------------------------------------------------
  // Retain one configured-account failure with the exact account-safety cause selected by planning.
  [[nodiscard]] static RetainedPrivateTurn create_retained_private_turn_for_account(
      model::DomainError error, risk::AccountSafetyReason account_reason,
      std::optional<oms::NormalizedPrivateOrderInput> normalized = std::nullopt,
      std::optional<oms::PrivateEventResolution> first_resolution = std::nullopt) {
    return RetainedPrivateTurn{std::move(error), std::move(normalized), std::move(first_resolution),
                               account_reason};
  }

  // --------------------------------------------------------
  // Retain one unattributable failure without inventing an account-safety reason.
  [[nodiscard]] static RetainedPrivateTurn create_retained_private_turn_for_global_containment(
      model::DomainError error,
      std::optional<oms::NormalizedPrivateOrderInput> normalized = std::nullopt,
      std::optional<oms::PrivateEventResolution> first_resolution = std::nullopt) {
    return RetainedPrivateTurn{std::move(error), std::move(normalized), std::move(first_resolution),
                               std::nullopt};
  }

  // --------------------------------------------------------
  // Borrow the nominal post-copy failure retained in the fixed executor slot.
  [[nodiscard]] const model::DomainError& retention_error() const noexcept { return error_; }

  // --------------------------------------------------------
  // Borrow normalized progress only when trusted construction reached that stage.
  [[nodiscard]] const std::optional<oms::NormalizedPrivateOrderInput>&
  normalized_input() const noexcept {
    return normalized_;
  }

  // --------------------------------------------------------
  // Borrow first-admission resolution only when normalization and classification completed.
  [[nodiscard]] const std::optional<oms::PrivateEventResolution>&
  first_admission_resolution() const noexcept {
    return first_resolution_;
  }

  // --------------------------------------------------------
  // Borrow the exact configured-account reason, or absence for global containment.
  [[nodiscard]] const std::optional<risk::AccountSafetyReason>&
  account_safety_reason() const noexcept {
    return account_reason_;
  }

  // --------------------------------------------------------
  // Structural equality includes the exact reason presence and every retained progress value.
  friend bool operator==(const RetainedPrivateTurn&, const RetainedPrivateTurn&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Named factories are the only way to select configured-account versus global reason presence.
  RetainedPrivateTurn(model::DomainError error,
                      std::optional<oms::NormalizedPrivateOrderInput> normalized,
                      std::optional<oms::PrivateEventResolution> first_resolution,
                      std::optional<risk::AccountSafetyReason> account_reason)
      : error_{std::move(error)}, normalized_{std::move(normalized)},
        first_resolution_{std::move(first_resolution)}, account_reason_{account_reason} {}

  // --------------------------------------------------------
  model::DomainError error_;
  std::optional<oms::NormalizedPrivateOrderInput> normalized_;
  std::optional<oms::PrivateEventResolution> first_resolution_;
  std::optional<risk::AccountSafetyReason> account_reason_;

  // ########################################################################
  // The executor is the sole completion consumer and may move retained values into its no-fail
  // fixed slot instead of allocating through a copy after owner return.
  friend class SerializedExecutor;

  // ########################################################################
};

// ########################################################################
// Exactly one owner completion arm is active: terminal consumption, successful buffering, or
// retained reconciliation.
using PrivateTurnCompletion =
    std::variant<ConsumedPrivateTurn, BufferedPrivateTurn, RetainedPrivateTurn>;

// ########################################################################
// The stable AccountSafetyReason vocabulary has nineteen assigned values, so one fixed occurrence
// array can retain every unique reason without allocating or dropping an escalation.
inline constexpr std::size_t account_safety_reason_occurrence_capacity = 19U;

// ########################################################################
// One reason occurrence retains the first complete fact and ordinal that introduced that reason;
// an ordered array therefore preserves first-overall and first-quarantine provenance.
struct AccountSafetyReasonOccurrence {
  risk::AccountSafetyReason reason;
  oms::PrivateOrderIngressAttempt first_attempt;
  model::AdmissionOrdinal first_attempt_ordinal;

  // --------------------------------------------------------
  // Structural equality compares the complete first occurrence for one unique safety cause.
  friend bool operator==(const AccountSafetyReasonOccurrence&,
                         const AccountSafetyReasonOccurrence&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One account fence turn retains an exact configured binding, checked total loss count, and every
// unique reason's first complete occurrence in ascending admission-ordinal order.
struct AccountSafetyFenceTurn {
  model::LogicalAccountId logical_account_id;
  model::VenueId venue_id;
  std::uint64_t lost_attempt_count;
  std::array<std::optional<AccountSafetyReasonOccurrence>,
             account_safety_reason_occurrence_capacity>
      ordered_unique_reason_occurrences;
  std::size_t reason_occurrence_count;

  // --------------------------------------------------------
  // Structural equality pins the complete configured-account loss interval delivered to its owner.
  friend bool operator==(const AccountSafetyFenceTurn&, const AccountSafetyFenceTurn&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A diagnostic reason summary exposes only the assigned cause and first ordinal, never the complete
// private source fact retained by the owner/recovery fence.
struct AccountSafetyReasonOccurrenceSnapshot {
  risk::AccountSafetyReason reason;
  model::AdmissionOrdinal first_attempt_ordinal;

  // --------------------------------------------------------
  // Structural equality compares the complete bounded diagnostic occurrence.
  friend bool operator==(const AccountSafetyReasonOccurrenceSnapshot&,
                         const AccountSafetyReasonOccurrenceSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One synchronized account-fence snapshot exposes pending counts, ordered reason/ordinal summaries,
// and the in-flight gate without publishing complete attempts or mutable recovery storage.
struct AccountSafetyFenceSnapshot {
  model::LogicalAccountId logical_account_id;
  model::VenueId venue_id;
  std::optional<model::AdmissionOrdinal> earliest_pending_attempt_ordinal;
  std::uint64_t pending_lost_attempt_count;
  std::array<std::optional<AccountSafetyReasonOccurrenceSnapshot>,
             account_safety_reason_occurrence_capacity>
      ordered_unique_reason_occurrences;
  std::size_t reason_occurrence_count;
  bool in_flight;

  // --------------------------------------------------------
  // Structural equality compares every diagnostic field under the account's synchronization cut.
  friend bool operator==(const AccountSafetyFenceSnapshot&,
                         const AccountSafetyFenceSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The global fence retains the first unattributable fact and earliest attempt without selecting or
// inventing any logical account or account-safety reason.
struct GlobalPrivateFenceTurn {
  oms::PrivateOrderIngressAttempt first_attempt;
  model::AdmissionOrdinal earliest_attempt_ordinal;
  std::uint64_t lost_attempt_count;

  // --------------------------------------------------------
  // Structural equality pins the complete reasonless global loss interval.
  friend bool operator==(const GlobalPrivateFenceTurn&, const GlobalPrivateFenceTurn&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Private-lane diagnostics expose counts and the global gate only; no slot, token, or mutable fence
// storage is published. private_capacity bounds queued_slots, while occupied_slots includes the at
// most one additional in-flight owner turn and may therefore reach private_capacity plus one.
struct PrivateLaneSnapshot {
  std::size_t occupied_slots{0U};
  std::size_t queued_slots{0U};
  std::size_t in_flight_slots{0U};
  std::size_t private_capacity{0U};
  std::size_t pending_account_fences{0U};
  std::size_t in_flight_account_fences{0U};
  std::size_t account_fence_capacity{0U};
  bool global_fence_active{false};
  bool global_fence_in_flight{false};
  bool global_fence_owner_applied{false};

  // --------------------------------------------------------
  // Structural equality compares every bounded private-lane count and gate observation.
  friend bool operator==(const PrivateLaneSnapshot&, const PrivateLaneSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The runtime-owned handler is the sole private-turn consumer and evidence oracle. An admission
// ordinal publishes exactly one disposition history or retained-error kind, never both; only a
// BufferedGap disposition may advance, exactly once, to AppliedFromBuffer. The owner's lifetime
// must enclose the executor that borrows it.
class PrivateAdmissionOwner {
public:

  // --------------------------------------------------------
  // Polymorphic cleanup preserves the runtime-owned handler lifetime contract.
  virtual ~PrivateAdmissionOwner() = default;

  // --------------------------------------------------------
  // Commit one move-only private owner turn and return exactly one non-throwing consumed, buffered,
  // or retained completion after publishing its matching evidence.
  [[nodiscard]] virtual PrivateTurnCompletion
  commit_private_order_turn(AdmittedPrivateOrderSlot admitted) noexcept = 0;

  // --------------------------------------------------------
  // Find the latest pre-callback disposition committed for the exact admission ordinal, or return
  // an empty optional when none exists. This query must be linearizable with concurrent publication
  // and lookup. BufferedGap may advance exactly once to AppliedFromBuffer; every other published
  // value is immutable.
  [[nodiscard]] virtual std::optional<oms::PrivateEventDisposition>
  find_committed_private_event_disposition(
      model::AdmissionOrdinal attempt_ordinal) const noexcept = 0;

  // --------------------------------------------------------
  // Find the stable error for an append-only retained recovery fact. The lookup must be
  // linearizable with concurrent owner publication/appends and concurrent lookups; a returned
  // pointer remains immutable and valid for this owner's lifetime.
  [[nodiscard]] virtual const model::DomainError* find_committed_retained_private_event_error(
      model::AdmissionOrdinal attempt_ordinal) const noexcept = 0;

  // --------------------------------------------------------
  // Apply one configured-account fence after all fallible validation and evidence preflight;
  // success means the complete owner-local journal/audit evidence was published, not durably
  // acknowledged by the later recovery medium.
  [[nodiscard]] virtual model::Result<void>
  apply_account_safety_fence(const AccountSafetyFenceTurn& fence,
                             const ControlTurnContext& context) noexcept = 0;

  // --------------------------------------------------------
  // Apply one reasonless global fence without selecting a logical account.
  [[nodiscard]] virtual model::Result<void>
  apply_global_private_fence(const GlobalPrivateFenceTurn& fence,
                             const ControlTurnContext& context) noexcept = 0;

  // --------------------------------------------------------
};

// ########################################################################

} // namespace aegis::runtime
