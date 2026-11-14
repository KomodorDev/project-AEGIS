// Purpose: retain a verified live-order fill/drain proposal and its exclusive economic, evidence,
// and journal reservations for immutable inspection before callback delivery is wired.

#pragma once

#include "../recovery/private_event_journal_reservation.hpp"
#include "private_order_reconciler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace aegis::runtime {

// ########################################################################
// Owns complete detached values and one lease on each reserved component. Inspection survives
// owner destruction. Moves revoke inspection through the source, and abandonment releases every
// reservation without publishing. While the owner is live, access, moves, and destruction require
// the same owner serialization or quiescence. Also serialize release with operations on the same
// fake medium. Only the original owner can validate this value.
class PreparedKnownPrivateExecutionDrain final {
public:

  // --------------------------------------------------------
  // Keep every component lease exclusive and transfer the complete group when moved.
  PreparedKnownPrivateExecutionDrain(const PreparedKnownPrivateExecutionDrain&) = delete;
  PreparedKnownPrivateExecutionDrain& operator=(const PreparedKnownPrivateExecutionDrain&) = delete;
  PreparedKnownPrivateExecutionDrain(PreparedKnownPrivateExecutionDrain&& other) noexcept;
  PreparedKnownPrivateExecutionDrain&
  operator=(PreparedKnownPrivateExecutionDrain&& other) noexcept;

  // --------------------------------------------------------
  // Release the unpublished economic, audit, and journal reservations.
  ~PreparedKnownPrivateExecutionDrain() = default;

  // --------------------------------------------------------
  // Return whether inspection through this value still owns the complete reservation group.
  [[nodiscard]] bool has_preparation() const noexcept { return state_.has_value(); }

  // --------------------------------------------------------
  // Borrow the incoming identity, or null after movement.
  [[nodiscard]] const FirstSeenAuthoritativePrivateIdentityPlan* identity_plan() const noexcept;

  // --------------------------------------------------------
  // Borrow the exact before/after projection and drained prefix length.
  [[nodiscard]] const oms::PrivateOmsTransitionPlan* oms_transition() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete economic plan, including the intermediate execution prefixes.
  [[nodiscard]] const risk::ReservationInventoryPlan* economics_plan() const noexcept;

  // --------------------------------------------------------
  // Borrow the prospective application rows and callback decisions.
  [[nodiscard]] const PreparedPrivateBusinessEvidence* business_evidence() const noexcept;

  // --------------------------------------------------------
  // Borrow the record reserved at the real journal tail; it is still unpublished.
  [[nodiscard]] const recovery::JournalRecord* journal_record() const noexcept;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Compare the append-only identity and safety baselines before any future use of the plan.
  struct OwnerBaseline {
    std::shared_ptr<const detail::PrivateBusinessTransactionIncarnation> incarnation;
    std::uint32_t event_count;
    std::uint32_t trade_count;
    std::uint32_t mapping_count;
    std::size_t retained_turn_count;
    std::size_t account_cause_count;
    std::uint64_t account_fenced_attempt_count;
    risk::AccountSafetyState account_state;
  };

  // ########################################################################
  // Keep all leases and the exact owner binding together; no borrowed projection or scratch
  // descriptor is stored here.
  struct State {
    FirstSeenAuthoritativePrivateIdentityPlan identity;
    oms::PrivateOmsTransitionPlan transition;
    risk::ReservationInventoryPlan economics;
    PreparedPrivateBusinessEvidence evidence;
    recovery::PreparedPrivateEventJournalInput journal;
    OwnerBaseline baseline;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Seal one fully prepared transaction once all fallible component work has succeeded.
  explicit PreparedKnownPrivateExecutionDrain(State state) noexcept;

  // --------------------------------------------------------
  std::optional<State> state_;

  // ########################################################################
  // Only the settled live owner may create or revalidate its complete prepared state.
  friend class PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
