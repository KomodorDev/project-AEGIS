// Purpose: derive per-execution audit effects and callback candidates for a live known-order
// execution drain, leaving all business state and publication authority with the owner.

#pragma once

#include "../oms/private_oms_transition.hpp"
#include "../risk/inventory_ledger.hpp"
#include "private_business_evidence_store.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Borrow canonical first-admission comparison values for one execution in drain order. The owner
// keeps these values stable until the evidence store copies the complete proposal.
struct PrivateExecutionDrainIdentity {
  const oms::PrivateEventResolution& resolution;
  const oms::PrivateTradeSemanticValue& trade_semantic_value;
};

// ########################################################################
// Own policy-sized scratch for assembling a detached drain proposal. Calls require the same
// serialized owner context as the evidence store. Returned evidence owns its own separate lease.
class PrivateExecutionDrainEvidenceScratch final {
public:

  // --------------------------------------------------------
  // Reserve every candidate slot during cold installation. Allocation failure propagates before
  // the caller can publish the enclosing owner.
  explicit PrivateExecutionDrainEvidenceScratch(std::uint32_t capacity);

  // --------------------------------------------------------
  // Keep the reserved capacity and borrowed descriptor storage in the original owner.
  PrivateExecutionDrainEvidenceScratch(const PrivateExecutionDrainEvidenceScratch&) = delete;
  PrivateExecutionDrainEvidenceScratch&
  operator=(const PrivateExecutionDrainEvidenceScratch&) = delete;
  PrivateExecutionDrainEvidenceScratch(PrivateExecutionDrainEvidenceScratch&&) = delete;
  PrivateExecutionDrainEvidenceScratch& operator=(PrivateExecutionDrainEvidenceScratch&&) = delete;

  // --------------------------------------------------------
  // Copy the genuine economic prefix and matching sealed identities into a detached event,
  // primary audit rows, and prospective callback range. All inputs must describe one unchanged
  // owner baseline. Failure leaves the store's accepted prefixes, ordinals, and business state
  // untouched. The store copies every proposed value before any scratch can be reused.
  [[nodiscard]] model::Result<PreparedPrivateBusinessEvidence>
  prepare_private_execution_drain_evidence(
      PrivateBusinessEvidenceStore& store, const risk::ReservationLedger& reservations,
      const execution::InstalledSubmissionRoute& route,
      const oms::PrivateOmsTransitionPlan& lifecycle,
      const risk::ReservationInventoryPlan& economics,
      std::span<const PrivateExecutionDrainIdentity> identities,
      recovery::JournalReplayProvenance replay, model::CallbackOrdinal first_callback,
      recovery::JournalSequence sequence);

  // --------------------------------------------------------
private:
  const std::uint32_t capacity_;
  std::vector<trace::PrivateOrderAuditEffect> effects_;
  std::vector<PrimaryPrivateAuditInput> primaries_;
  std::vector<trace::OrderCallbackAuditValue> callbacks_;
};

// ########################################################################

} // namespace aegis::runtime
