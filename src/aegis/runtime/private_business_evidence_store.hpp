// Purpose: prepare bounded prospective known-order journal and audit evidence in policy-sized cold
// storage without publishing records, consuming ordinals, or granting business mutation authority.

#pragma once

#include "../trace/private_business_evidence.hpp"
#include "aegis/model/result.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/recovery/journal_record.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace aegis::runtime {
namespace detail {

// ########################################################################
// Shared fixed backing retains a single detached preparation after its creating store disappears.
struct PrivateBusinessEvidenceBacking;

// ########################################################################

} // namespace detail

// ########################################################################
// One synchronous borrowed input describes a proposed primary effect group. The first row owns
// the incoming fact; later rows describe actual buffered executions in cumulative order. None of
// these values proves that its proposed disposition or effects have been applied.
struct PrimaryPrivateAuditInput {
  const oms::NormalizedPrivateOrderInput& input;
  const oms::PrivateEventResolution& first_admission_resolution;
  std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value;
  oms::PrivateEventDisposition disposition;
  std::span<const trace::PrivateOrderAuditEffect> effects;
  trace::CallbackDecision callback_decision{trace::CallbackDecision::None};
  std::optional<trace::BoundedAuditDomainError> error;
};

// ########################################################################
// One opaque preparation owns the sole lease on immutable populated scratch. Moves transfer that
// lease; destruction releases it, and inspected rows survive store destruction. Moved-from values
// have no preparation and all pointer queries return null. Inspection requires caller
// serialization. Every record is prospective: this capability cannot publish, apply, acknowledge,
// or consume it.
class PreparedPrivateBusinessEvidence final {
public:

  // --------------------------------------------------------
  // Preserve exclusive scratch ownership rather than duplicating a detached preparation.
  PreparedPrivateBusinessEvidence(const PreparedPrivateBusinessEvidence&) = delete;
  PreparedPrivateBusinessEvidence& operator=(const PreparedPrivateBusinessEvidence&) = delete;

  // --------------------------------------------------------
  // Transfer the sole lease and revoke the source's inspection authority without allocation.
  PreparedPrivateBusinessEvidence(PreparedPrivateBusinessEvidence&& other) noexcept;
  PreparedPrivateBusinessEvidence& operator=(PreparedPrivateBusinessEvidence&& other) noexcept;

  // --------------------------------------------------------
  // Release scratch after all inspection through this capability has ended.
  ~PreparedPrivateBusinessEvidence();

  // --------------------------------------------------------
  // Return whether this value still owns a detached preparation, including after its store dies.
  [[nodiscard]] bool has_preparation() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete prospective incoming evidence, or return null after movement.
  [[nodiscard]] const trace::PrivateEventEvidence* private_event_evidence() const noexcept;

  // --------------------------------------------------------
  // Borrow the normalized replay payload and sealed first resolution, or null after movement.
  [[nodiscard]] const recovery::PrivateEventInputJournalPayload* journal_payload() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete proposed span; its ordinals and physical slots remain unconsumed.
  [[nodiscard]] const recovery::AuditSpan* audit_span() const noexcept;

  // --------------------------------------------------------
  // Return the exact proposed primary prefix length, or zero after movement.
  [[nodiscard]] std::uint32_t primary_audit_record_count() const noexcept;

  // --------------------------------------------------------
  // Borrow one immutable prospective primary; out-of-prefix or moved-from queries return null.
  [[nodiscard]] const trace::OrderAuditRecord*
  primary_audit_record_at(std::uint32_t chronological_index) const noexcept;

  // --------------------------------------------------------
  // Borrow the prospective aggregate Planned row, absent for callback-free preparations.
  [[nodiscard]] const trace::OrderAuditRecord* planned_callback_record() const noexcept;

  // --------------------------------------------------------
  // Return the prospective terminal ordinal when callbacks are proposed; no terminal row exists.
  [[nodiscard]] std::optional<recovery::AuditOrdinal>
  reserved_terminal_audit_ordinal() const noexcept;

  // --------------------------------------------------------
  // Return the proposed synchronous callback count, or zero after movement.
  [[nodiscard]] std::uint32_t order_callback_count() const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal one already fully validated scratch prefix without allocating or exposing its backing.
  explicit PreparedPrivateBusinessEvidence(
      std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing) noexcept;

  // --------------------------------------------------------
  // Clear the populated scratch prefix and release its exclusive lease without consuming records.
  void release_preparation() noexcept;

  // --------------------------------------------------------
  std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing_;

  // ########################################################################
  // Only the exact validated store may acquire and assemble its complete scratch lease.
  friend class PrivateBusinessEvidenceStore;

  // ########################################################################
};

// ########################################################################
// A nonmoving, source-private owner allocates all event/audit/effect/callback backing at
// construction. It accepts only known order-scoped prospective values and supports one detached
// preparation at a time. All accepted/published prefixes remain empty and the next audit ordinal
// remains one; neither preparation nor abandonment consumes any slot or counter. Caller
// serialization is required for construction, preparation, inspection, movement, and destruction;
// no internal thread or lock exists.
class PrivateBusinessEvidenceStore final {
public:

  // --------------------------------------------------------
  // Allocate exact policy-sized storage before exposing an owner; failures publish no partial
  // value.
  [[nodiscard]] static model::Result<std::unique_ptr<PrivateBusinessEvidenceStore>>
  create_private_business_evidence_store(const M4Policy& policy,
                                         recovery::RecoveryLineageId lineage_id,
                                         recovery::RuntimeEpochId runtime_epoch_id);

  // --------------------------------------------------------
  // Keep owner identity and the cold backing address stable while a detached lease may survive.
  PrivateBusinessEvidenceStore(const PrivateBusinessEvidenceStore&) = delete;
  PrivateBusinessEvidenceStore& operator=(const PrivateBusinessEvidenceStore&) = delete;
  PrivateBusinessEvidenceStore(PrivateBusinessEvidenceStore&&) = delete;
  PrivateBusinessEvidenceStore& operator=(PrivateBusinessEvidenceStore&&) = delete;

  // --------------------------------------------------------
  // Release the owner handle; any detached preparation keeps its immutable scratch alive.
  ~PrivateBusinessEvidenceStore();

  // --------------------------------------------------------
  // Validate source/resolution, complete effect frames, drain order, callback topology, and all
  // policy bounds before acquiring scratch. Failure changes no prefix, ordinal, or lease; a busy
  // store returns PrivateEvidenceExhausted. Optional journal linkage is a copied candidate
  // identity, not authority to allocate or publish that sequence. Success retains no caller span or
  // reference.
  [[nodiscard]] model::Result<PreparedPrivateBusinessEvidence>
  prepare_known_private_business_evidence(
      const oms::NormalizedPrivateOrderInput& input,
      const oms::PrivateEventResolution& first_admission_resolution,
      std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value,
      recovery::JournalReplayProvenance replay_provenance,
      std::span<const PrimaryPrivateAuditInput> primary_rows,
      std::optional<trace::CallbackOrdinalRange> callback_range,
      std::span<const trace::OrderCallbackAuditValue> callbacks,
      std::optional<recovery::JournalSequence> journal_sequence = std::nullopt,
      std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal = std::nullopt);

  // --------------------------------------------------------
  // Borrow the sealed root that owns every prospective row and sizes its immutable backing.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept;

  // --------------------------------------------------------
  // Borrow the external lineage whose future journal may publish these prepared input values.
  [[nodiscard]] const recovery::RecoveryLineageId& lineage_id() const noexcept;

  // --------------------------------------------------------
  // Borrow the exact incarnation qualifying every proposed audit ordinal.
  [[nodiscard]] const recovery::RuntimeEpochId& runtime_epoch_id() const noexcept;

  // --------------------------------------------------------
  // Return the unconsumed next audit ordinal used to derive a preparation's complete span.
  [[nodiscard]] recovery::AuditOrdinal next_audit_ordinal() const noexcept;

  // --------------------------------------------------------
  // Return exact cold capacities without counting or exposing a detached preparation as accepted.
  [[nodiscard]] std::uint32_t private_event_record_capacity() const noexcept;
  [[nodiscard]] std::uint32_t private_audit_record_capacity() const noexcept;
  [[nodiscard]] std::uint32_t primary_effect_buffer_count() const noexcept;
  [[nodiscard]] std::uint32_t primary_effect_buffer_capacity() const noexcept;
  [[nodiscard]] std::uint32_t planned_callback_buffer_count() const noexcept;
  [[nodiscard]] std::uint32_t planned_callback_buffer_capacity() const noexcept;

  // --------------------------------------------------------
  // Return empty accepted prefixes; detached scratch never becomes published evidence.
  [[nodiscard]] std::uint32_t accepted_private_event_record_count() const noexcept;
  [[nodiscard]] std::uint32_t accepted_private_audit_record_count() const noexcept;

  // --------------------------------------------------------
  // Return whether the one detached scratch reservation is presently leased.
  [[nodiscard]] bool has_outstanding_preparation() const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain one fully allocated backing after every cold validation and allocation has succeeded.
  explicit PrivateBusinessEvidenceStore(
      std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing) noexcept;

  // --------------------------------------------------------
  // Release only unpublished scratch; accepted slots and every ordinal remain untouched.
  static void release_private_business_evidence_scratch(
      detail::PrivateBusinessEvidenceBacking& backing) noexcept;

  // --------------------------------------------------------
  std::shared_ptr<detail::PrivateBusinessEvidenceBacking> backing_;

  // ########################################################################
  // The detached lease may invoke only its private cleanup phase after the store handle dies.
  friend class PreparedPrivateBusinessEvidence;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
