// Purpose: define the bounded typed M4 journal records used by deterministic fake recovery before
// ADR-0014 assigns canonical bytes, semantic digests, or a persistent stream schema.

#pragma once

#include "aegis/model/integer_input.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace aegis::recovery {

// ########################################################################
// Stable kinds select the owning replay reducer without casting non-event records into the OMS.
enum class JournalRecordKind : std::uint8_t {
  NamespaceRegistered = 1,
  SubmissionProjection = 2,
  PrivateEventInput = 3,
  ReconciliationInput = 4,
  AccountSafetyFence = 5,
  ReferenceIntentState = 6,
  IdentityHighWater = 7,
  RecoveryDecision = 8,
  RecoveryNotificationDecision = 9,
};

// ########################################################################

// ########################################################################
// One immutable audit span owns a nonempty contiguous range; construction proves the count fits
// the retained width and the inclusive final ordinal cannot wrap.
class AuditSpan final {
public:

  // --------------------------------------------------------
  // Create a span from its first ordinal and authored record count, reporting a stable domain
  // error when the count is invalid or the inclusive final ordinal would exceed uint64_t.
  // Interesting syntax: CheckedIntegerInput retains signedness for checked narrowing while
  // excluding Boolean, plain/wide/Unicode character, enum, and floating-point inputs.
  template <model::detail::CheckedIntegerInput Count>
  [[nodiscard]] static model::Result<AuditSpan> create_audit_span(AuditOrdinal first_audit_ordinal,
                                                                  Count audit_record_count) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Reject zero, negative, or wider-than-uint32 counts before any narrowing conversion.
    if (!std::in_range<std::uint32_t>(audit_record_count) || audit_record_count == 0) {
      return model::Result<AuditSpan>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidRecoveryPolicy, "audit_span.count"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Check the inclusive-range addition before deriving the final typed ordinal.
    const auto validated_audit_record_count = static_cast<std::uint32_t>(audit_record_count);
    const auto additional_audit_records =
        static_cast<std::uint64_t>(validated_audit_record_count - 1U);
    if (first_audit_ordinal.value() >
        std::numeric_limits<std::uint64_t>::max() - additional_audit_records) {
      return model::Result<AuditSpan>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::RecoveryCounterExhausted, "audit_span.last"));
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Construct the already range-checked final ordinal and retain the complete span atomically.
    auto last_audit_ordinal =
        AuditOrdinal::from_value(first_audit_ordinal.value() + additional_audit_records);
    if (!last_audit_ordinal) {
      return model::Result<AuditSpan>::create_failure(std::move(last_audit_ordinal).error());
    }
    return model::Result<AuditSpan>::create_success(AuditSpan{
        first_audit_ordinal, validated_audit_record_count, std::move(last_audit_ordinal).value()});

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Return the inclusive first ordinal of the contiguous range.
  [[nodiscard]] AuditOrdinal first_audit_ordinal() const noexcept { return first_audit_ordinal_; }

  // --------------------------------------------------------
  // Return the exact nonzero number of audit records in the range.
  [[nodiscard]] std::uint32_t audit_record_count() const noexcept { return audit_record_count_; }

  // --------------------------------------------------------
  // Return the inclusive final ordinal already proven not to wrap.
  [[nodiscard]] AuditOrdinal last_audit_ordinal() const noexcept { return last_audit_ordinal_; }

  // --------------------------------------------------------
  // Structural equality pins the complete preassigned contiguous range.
  friend bool operator==(const AuditSpan&, const AuditSpan&) = default;

  // --------------------------------------------------------
  // Restrict construction to the checked count and final-ordinal derivation above.
private:

  // --------------------------------------------------------
  // Retain only values that jointly satisfy the class's nonempty, non-wrapping invariant.
  AuditSpan(AuditOrdinal first_audit_ordinal, std::uint32_t audit_record_count,
            AuditOrdinal last_audit_ordinal) noexcept
      : first_audit_ordinal_{first_audit_ordinal}, audit_record_count_{audit_record_count},
        last_audit_ordinal_{last_audit_ordinal} {}

  // --------------------------------------------------------
  // The explicit first, count, and final values make consumers independent of unchecked arithmetic.
  AuditOrdinal first_audit_ordinal_;
  std::uint32_t audit_record_count_;
  AuditOrdinal last_audit_ordinal_;
};

// ########################################################################

// ########################################################################
// Replay provenance preserves global executor ordering without changing event-key equality.
struct JournalReplayProvenance {
  model::AdmissionOrdinal admission_ordinal;
  model::ReceiveSequence receive_sequence;

  // --------------------------------------------------------
  // Structural equality includes both independently non-wrapping executor identities.
  friend bool operator==(const JournalReplayProvenance&, const JournalReplayProvenance&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Namespace registration is root-scoped and records the acknowledged append-only registry count.
struct NamespaceRegisteredJournalPayload {
  model::OrderNamespace order_namespace;
  std::uint32_t registry_count_after_append;

  // --------------------------------------------------------
  // Equality proves the exact namespace and its expected acknowledged registry position.
  friend bool operator==(const NamespaceRegisteredJournalPayload&,
                         const NamespaceRegisteredJournalPayload&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The namespace-only fake medium accepts no business input; later owner slices extend this closed
// variant only together with their complete typed replay and audit contracts.
using JournalPayloadValue = std::variant<NamespaceRegisteredJournalPayload>;

// ########################################################################

// ########################################################################
// One immutable semantic journal record owns causal lineage order, applicable provenance, and a
// typed replay payload without claiming an AEGISJRN byte encoding or digest.
class JournalRecord final {
public:

  // --------------------------------------------------------
  // Identify the external fake-media lineage across all runtime incarnations.
  [[nodiscard]] const RecoveryLineageId& lineage_id() const noexcept { return lineage_id_; }

  // --------------------------------------------------------
  // Expose the one-based lineage-global publication position.
  [[nodiscard]] JournalSequence sequence() const noexcept { return sequence_; }

  // --------------------------------------------------------
  // Sequence one has no predecessor; every later record names the immediately prior sequence.
  [[nodiscard]] const std::optional<JournalSequence>& predecessor() const noexcept {
    return predecessor_;
  }

  // --------------------------------------------------------
  // Runtime-scoped records carry the exact active epoch; lineage bootstrap records do not.
  [[nodiscard]] const std::optional<RuntimeEpochId>& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Select the exact typed reducer that owns replay of this record.
  [[nodiscard]] JournalRecordKind kind() const noexcept { return kind_; }

  // --------------------------------------------------------
  // Retain the complete seven-field authority common to every record in the medium.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Subject absence is exact for namespace rows; later owner records retain applicable attribution.
  [[nodiscard]] const std::optional<model::M4SubjectProvenance>&
  subject_provenance() const noexcept {
    return subject_provenance_;
  }

  // --------------------------------------------------------
  // Namespace rows omit executor order; later admitted-value slices may retain replay identities.
  [[nodiscard]] const std::optional<JournalReplayProvenance>& replay_provenance() const noexcept {
    return replay_provenance_;
  }

  // --------------------------------------------------------
  // Borrow the closed typed payload without exposing mutable media storage.
  [[nodiscard]] const JournalPayloadValue& payload() const noexcept { return payload_; }

  // --------------------------------------------------------
  // Namespace rows omit audit spans; later record slices may retain complete preassigned ranges.
  [[nodiscard]] const std::optional<AuditSpan>& audit_span() const noexcept { return audit_span_; }

  // --------------------------------------------------------
  // Equality is field-by-field over typed semantic values, not object memory or future bytes.
  friend bool operator==(const JournalRecord&, const JournalRecord&) = default;

  // --------------------------------------------------------
  // Only the deterministic journal owner may construct a record after complete presence checks.
private:

  // --------------------------------------------------------
  // Assemble one fully validated record whose kind agrees with its active payload alternative.
  JournalRecord(RecoveryLineageId lineage_id, JournalSequence sequence,
                std::optional<JournalSequence> predecessor,
                std::optional<RuntimeEpochId> runtime_epoch_id, JournalRecordKind kind,
                model::M4RootProvenance root_provenance,
                std::optional<model::M4SubjectProvenance> subject_provenance,
                std::optional<JournalReplayProvenance> replay_provenance,
                JournalPayloadValue payload, std::optional<AuditSpan> audit_span) noexcept
      : lineage_id_{lineage_id}, sequence_{sequence}, predecessor_{predecessor},
        runtime_epoch_id_{runtime_epoch_id}, kind_{kind},
        root_provenance_{std::move(root_provenance)},
        subject_provenance_{std::move(subject_provenance)},
        replay_provenance_{std::move(replay_provenance)}, payload_{std::move(payload)},
        audit_span_{std::move(audit_span)} {}

  // --------------------------------------------------------
  // Store only complete domain values; physical slot position and capacity are media mechanics.
  RecoveryLineageId lineage_id_;
  JournalSequence sequence_;
  std::optional<JournalSequence> predecessor_;
  std::optional<RuntimeEpochId> runtime_epoch_id_;
  JournalRecordKind kind_;
  model::M4RootProvenance root_provenance_;
  std::optional<model::M4SubjectProvenance> subject_provenance_;
  std::optional<JournalReplayProvenance> replay_provenance_;
  JournalPayloadValue payload_;
  std::optional<AuditSpan> audit_span_;

  // ########################################################################
  // The concrete fake medium creates namespace rows during fake-acknowledged bootstrap.
  friend class DeterministicFakeRecoveryMedium;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::recovery
