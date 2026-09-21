// Purpose: retain an owner-derived initial OMS and economic proposal with prospective evidence
// requirements, without granting live private-event publication or economic commit authority.

#pragma once

#include "../oms/private_oms_transition.hpp"
#include "../risk/inventory_ledger.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/recovery/journal_record.hpp"

#include <cstdint>
#include <optional>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// Exact proposed primary/callback counts determine the prospective contiguous audit span. These
// counts neither reserve storage nor prove journal, diagnostic, identity, or callback headroom.
struct PrivateBusinessEvidenceRequirements {
  std::uint32_t primary_audit_record_count;
  std::uint32_t order_callback_count;
  std::uint32_t audit_record_count;

  // --------------------------------------------------------
  // Compare the complete unreserved count proposal independently of any future storage owner.
  friend bool operator==(const PrivateBusinessEvidenceRequirements&,
                         const PrivateBusinessEvidenceRequirements&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One immutable initial proposal owns genuine owner-derived facts and prospective evidence. An
// execution proposal leases bounded economic scratch until destruction or move; copied facts and
// leased effects survive owner destruction, but no mutable plan or commit operation is exposed.
// Derivation and inspection require owner serialization or quiescence; this is not an admission
// completion, journal reservation, canonical audit row, or authority to apply a later stale value.
class InitialKnownPrivateBusinessProposal final {
public:

  // --------------------------------------------------------
  // Transfer the sole scratch lease without duplicating an economic proposal's retained backing.
  InitialKnownPrivateBusinessProposal(const InitialKnownPrivateBusinessProposal&) = delete;
  InitialKnownPrivateBusinessProposal&
  operator=(const InitialKnownPrivateBusinessProposal&) = delete;
  InitialKnownPrivateBusinessProposal(InitialKnownPrivateBusinessProposal&&) noexcept = default;
  InitialKnownPrivateBusinessProposal&
  operator=(InitialKnownPrivateBusinessProposal&&) noexcept = default;

  // --------------------------------------------------------
  // Borrow the complete normalized fact whose initial owner-derived consequences were checked.
  [[nodiscard]] const oms::NormalizedPrivateOrderInput& input() const noexcept { return input_; }

  // --------------------------------------------------------
  // Borrow the immutable known resolution selected before lifecycle or economics planning.
  [[nodiscard]] const oms::PrivateEventResolution& first_admission_resolution() const noexcept {
    return resolution_;
  }

  // --------------------------------------------------------
  // Borrow the detached lifecycle proposal; editing a copy cannot change the genuine OMS row.
  [[nodiscard]] const oms::PrivateOmsTransitionPlan& oms_transition() const noexcept {
    return transition_;
  }

  // --------------------------------------------------------
  // Borrow read-only economics, absent when the proposed transition transfers no exposure. This
  // const view cannot be moved into the inventory commit operation.
  [[nodiscard]] const risk::ReservationInventoryPlan* economics_plan() const noexcept {
    return economics_ ? &*economics_ : nullptr;
  }

  // --------------------------------------------------------
  // Borrow the genuine held reservation baseline, including for proposals without economics.
  [[nodiscard]] const risk::ReservationEvidence& reservation_before() const noexcept {
    return reservation_before_;
  }

  // --------------------------------------------------------
  // Borrow exact proposed counts without claiming any corresponding storage was reserved.
  [[nodiscard]] const PrivateBusinessEvidenceRequirements& evidence_requirements() const noexcept {
    return requirements_;
  }

  // --------------------------------------------------------
  // Borrow the non-wrapping prospective span; its ordinals have not been assigned or consumed.
  [[nodiscard]] const recovery::AuditSpan& proposed_audit_span() const noexcept { return span_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal jointly checked owner-derived values without exposing an independently authored factory.
  InitialKnownPrivateBusinessProposal(oms::NormalizedPrivateOrderInput input,
                                      oms::PrivateEventResolution resolution,
                                      oms::PrivateOmsTransitionPlan transition,
                                      risk::ReservationEvidence reservation_before,
                                      std::optional<risk::ReservationInventoryPlan> economics,
                                      PrivateBusinessEvidenceRequirements requirements,
                                      recovery::AuditSpan span) noexcept
      : input_{std::move(input)}, resolution_{std::move(resolution)},
        transition_{std::move(transition)}, reservation_before_{std::move(reservation_before)},
        economics_{std::move(economics)}, requirements_{requirements}, span_{span} {}

  // --------------------------------------------------------
  oms::NormalizedPrivateOrderInput input_;
  oms::PrivateEventResolution resolution_;
  oms::PrivateOmsTransitionPlan transition_;
  risk::ReservationEvidence reservation_before_;
  std::optional<risk::ReservationInventoryPlan> economics_;
  PrivateBusinessEvidenceRequirements requirements_;
  recovery::AuditSpan span_;

  // ########################################################################
  // Only the owner that derives genuine correlation may assemble this complete initial proposal.
  friend class PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
