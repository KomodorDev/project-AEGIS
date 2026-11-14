// Purpose: retain one sealed initial known-order business, economic, evidence, and real journal
// preparation without granting publication, callbacks, canonical identity, or consumption rights.

#pragma once

#include "../recovery/private_event_journal_reservation.hpp"
#include "private_order_reconciler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace aegis::runtime {
namespace detail {

// ########################################################################
// One cold identity belongs to the exact nonmoving private owner. Detached preparations retain it
// so allocator-address reuse cannot make a replacement owner validate an earlier preparation.
struct PrivateBusinessTransactionIncarnation final {};

// ########################################################################

} // namespace detail

// ########################################################################
// One move-only preparation retains all genuine component plans and exclusive scratch/journal
// reservations until abandonment. It exposes immutable copied facts after owner destruction;
// validating it against an owner requires serialization and unchanged captured state. Neither
// preparation nor validation publishes a record, assigns a callback, or consumes a private input.
class PreparedInitialKnownPrivateBusinessTransaction final {
public:

  // --------------------------------------------------------
  // Prevent copying the sole economic, evidence, and journal reservations.
  PreparedInitialKnownPrivateBusinessTransaction(
      const PreparedInitialKnownPrivateBusinessTransaction&) = delete;
  PreparedInitialKnownPrivateBusinessTransaction&
  operator=(const PreparedInitialKnownPrivateBusinessTransaction&) = delete;

  // --------------------------------------------------------
  // Transfer every reservation together and revoke all inspection through the source value.
  PreparedInitialKnownPrivateBusinessTransaction(
      PreparedInitialKnownPrivateBusinessTransaction&& other) noexcept;
  PreparedInitialKnownPrivateBusinessTransaction&
  operator=(PreparedInitialKnownPrivateBusinessTransaction&& other) noexcept;

  // --------------------------------------------------------
  // Abandon every unpublished reservation without consuming business slots or counters.
  ~PreparedInitialKnownPrivateBusinessTransaction();

  // --------------------------------------------------------
  // Return whether this value still retains its complete detached preparation.
  [[nodiscard]] bool has_preparation() const noexcept;

  // --------------------------------------------------------
  // Borrow complete first-seen identity candidates, or return null after movement.
  [[nodiscard]] const FirstSeenAuthoritativePrivateIdentityPlan* identity_plan() const noexcept;

  // --------------------------------------------------------
  // Borrow the genuine OMS/economic proposal with its retained lease, or null after movement.
  [[nodiscard]] const InitialKnownPrivateBusinessProposal* business_proposal() const noexcept;

  // --------------------------------------------------------
  // Borrow complete evidence and its prospective callback span, or null after movement.
  [[nodiscard]] const PreparedPrivateBusinessEvidence* business_evidence() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete immutable record reserved in the real fake medium, or null after movement.
  [[nodiscard]] const recovery::JournalRecord* journal_record() const noexcept;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Capture exact baseline identity and all mutable preparation/containment counters. Account
  // causes are append-only, so their count and first-quarantine index fence their immutable prefix.
  struct OwnerSnapshot {
    std::shared_ptr<const detail::PrivateBusinessTransactionIncarnation> incarnation;
    std::uint64_t inventory_generation;
    oms::OutboundOrderAdmission order_admission;
    risk::AccountSafetyState account_state;
    std::size_t account_cause_count;
    std::optional<std::size_t> first_quarantine_cause;
    std::uint64_t fenced_attempt_count;
    bool globally_blocked;
    std::optional<GlobalPrivateFenceTurn> global_fence;
    std::uint32_t event_count;
    std::uint32_t trade_count;
    std::uint32_t mapping_count;
    std::size_t retained_turn_count;
    bool emergency_turn_published;
    std::uint32_t prepared_event_count;
    std::uint32_t prepared_trade_count;
    std::uint32_t prepared_mapping_count;
  };

  // ########################################################################
  // Keep the closed source decisions, genuine economic plan, and both backing reservations in one
  // optional group. Its absence is the complete moved-from state; no partial group can escape.
  struct State {
    FirstSeenAuthoritativePrivateIdentityPlan identity;
    InitialKnownPrivateBusinessProposal business;
    PreparedPrivateBusinessEvidence evidence;
    recovery::PreparedPrivateEventJournalInput journal;
    OwnerSnapshot owner;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Seal only fully prepared owner-derived values after every fallible operation has succeeded.
  PreparedInitialKnownPrivateBusinessTransaction(FirstSeenAuthoritativePrivateIdentityPlan identity,
                                                 InitialKnownPrivateBusinessProposal business,
                                                 PreparedPrivateBusinessEvidence evidence,
                                                 recovery::PreparedPrivateEventJournalInput journal,
                                                 OwnerSnapshot owner) noexcept;

  // --------------------------------------------------------
  std::optional<State> state_;

  // ########################################################################
  // Only the exact genuine owner may mint or validate the complete preparation's private baseline.
  friend class PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
