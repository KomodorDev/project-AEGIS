// Purpose: reserve one complete typed private-event journal input against the actual fake-medium
// lease without publishing a row, advancing a sequence, or granting business mutation authority.

#pragma once

#include "aegis/model/result.hpp"
#include "aegis/recovery/journal_record.hpp"

#include <memory>
#include <optional>

namespace aegis::runtime {

// ########################################################################
// The installed private owner alone may reserve a journal input after deriving its complete plan.
class PrivateOrderReconciler;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::recovery {
namespace detail {

// ########################################################################
// The opaque shared control retains the exact acknowledged runtime incarnation and backing.
struct FakeJournalLeaseControl;

// ########################################################################

} // namespace detail

// ########################################################################
// One move-only reservation retains a complete prospective PrivateEventInput record and the exact
// live-incarnation lease. Its source-private factory assigns sequence/predecessor from the medium;
// inspection grants no append, business commit, acknowledgement, replay, or refresh operation.
// Moves revoke the source, destruction releases the sole reservation, and abandonment consumes no
// sequence or slot. A detached value keeps cold reopening fenced after owner/medium destruction.
// Every operation and borrowed-record access requires caller serialization. Normalized venue and
// reconciliation events share this payload; original reconciliation batch rows remain a separate,
// unsupported ReconciliationInput kind.
class PreparedPrivateEventJournalInput final {
public:

  // --------------------------------------------------------
  // Prevent duplication of the sole unpublished slot reservation and its incarnation lease.
  PreparedPrivateEventJournalInput(const PreparedPrivateEventJournalInput&) = delete;
  PreparedPrivateEventJournalInput& operator=(const PreparedPrivateEventJournalInput&) = delete;

  // --------------------------------------------------------
  // Transfer the complete reservation without allocation, leaving the source without authority.
  PreparedPrivateEventJournalInput(PreparedPrivateEventJournalInput&& other) noexcept;
  PreparedPrivateEventJournalInput& operator=(PreparedPrivateEventJournalInput&& other) noexcept;

  // --------------------------------------------------------
  // Release only this unpublished reservation; published and acknowledged prefixes stay unchanged.
  ~PreparedPrivateEventJournalInput();

  // --------------------------------------------------------
  // Return whether this value still retains its reserved candidate, including after owner death.
  [[nodiscard]] bool has_reservation() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete prospective typed record, or return null from a moved-from value.
  [[nodiscard]] const JournalRecord* record() const noexcept;

  // --------------------------------------------------------
  // Check the original lease generation, unchanged causal prefix, and empty destination slot.
  // Missing or stale authority returns InvalidJournalState without renewing or changing anything.
  [[nodiscard]] model::Result<void> validate_reservation() const;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Validate the acknowledged incarnation, authoritative known input, and exact remaining journal
  // capacity before reserving the next real sequence. Failure consumes no slot or counter, and a
  // second outstanding reservation returns InvalidJournalState. The complete audit span is copied
  // as a required candidate; no row or callback terminal outcome is claimed to exist.
  [[nodiscard]] static model::Result<PreparedPrivateEventJournalInput>
  reserve_private_event_journal_input(std::shared_ptr<detail::FakeJournalLeaseControl> lease,
                                      RuntimeEpochId runtime_epoch_id,
                                      const oms::NormalizedPrivateOrderInput& input,
                                      const oms::PrivateEventResolution& first_admission_resolution,
                                      JournalReplayProvenance replay_provenance,
                                      AuditSpan audit_span);

  // --------------------------------------------------------
  // Retain one fully validated fixed record and lease before activating its reservation flag.
  PreparedPrivateEventJournalInput(std::shared_ptr<detail::FakeJournalLeaseControl> lease,
                                   JournalRecord record) noexcept;

  // --------------------------------------------------------
  // Clear the exact incarnation's unpublished flag once and revoke local inspection authority.
  void release_reservation() noexcept;

  // --------------------------------------------------------
  std::shared_ptr<detail::FakeJournalLeaseControl> lease_;
  std::optional<JournalRecord> record_;

  // ########################################################################
  // Only the genuine installed owner can acquire this journal capacity during joint preparation.
  friend class runtime::PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::recovery
