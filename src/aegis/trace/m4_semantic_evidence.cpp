// Purpose: construct the closed storage-free M4 originating-event identities without assigning
// canonical bytes, retaining evidence, or mutating runtime state.

#include "aegis/trace/m4_semantic_evidence.hpp"

#include <utility>

namespace aegis::trace {

// --------------------------------------------------------
// Construct the explicit identity-free alternative without a sentinel value.
OriginatingEventIdentity OriginatingEventIdentity::create_without_originating_event() noexcept {
  return OriginatingEventIdentity{OriginatingEventIdentityValue{NoOriginatingEventIdentity{}}};
}

// --------------------------------------------------------
// Preserve the complete AEGIS-local event identity in its distinct alternative.
OriginatingEventIdentity
OriginatingEventIdentity::create_from_local_event_id(oms::LocalOrderEventId event_id) noexcept {
  return OriginatingEventIdentity{
      OriginatingEventIdentityValue{LocalOriginatingEventIdentity{event_id}}};
}

// --------------------------------------------------------
// Preserve every account and source component of one venue-scoped event key.
OriginatingEventIdentity OriginatingEventIdentity::create_from_venue_event_key(
    oms::VenuePrivateEventKey event_key) noexcept {
  return OriginatingEventIdentity{
      OriginatingEventIdentityValue{VenueOriginatingEventIdentity{std::move(event_key)}}};
}

// --------------------------------------------------------
// Keep authoritative cut and row identity scoped by the exact reconciliation epoch.
OriginatingEventIdentity OriginatingEventIdentity::create_from_reconciliation_row(
    recovery::ReconciliationEpochId reconciliation_epoch_id,
    oms::AuthoritativeCutId authoritative_cut_id,
    recovery::ReconciliationRowOrdinal row_ordinal) noexcept {
  return OriginatingEventIdentity{
      OriginatingEventIdentityValue{ReconciliationOriginatingEventIdentity{
          reconciliation_epoch_id, std::move(authoritative_cut_id), row_ordinal}}};
}

// --------------------------------------------------------
// Keep owner-authored recovery action order distinct from reconciliation row order.
OriginatingEventIdentity OriginatingEventIdentity::create_from_recovery_action(
    recovery::ReconciliationEpochId reconciliation_epoch_id,
    RecoveryActionOrdinal action_ordinal) noexcept {
  return OriginatingEventIdentity{OriginatingEventIdentityValue{
      RecoveryActionOriginatingEventIdentity{reconciliation_epoch_id, action_ordinal}}};
}

// --------------------------------------------------------
// Resolve the stable tag directly from the closed variant alternative.
OriginatingEventIdentityKind
OriginatingEventIdentity::originating_event_identity_kind() const noexcept {
  if (std::holds_alternative<NoOriginatingEventIdentity>(value_)) {
    return OriginatingEventIdentityKind::None;
  }
  if (std::holds_alternative<LocalOriginatingEventIdentity>(value_)) {
    return OriginatingEventIdentityKind::Local;
  }
  if (std::holds_alternative<VenueOriginatingEventIdentity>(value_)) {
    return OriginatingEventIdentityKind::Venue;
  }
  if (std::holds_alternative<ReconciliationOriginatingEventIdentity>(value_)) {
    return OriginatingEventIdentityKind::Reconciliation;
  }
  return OriginatingEventIdentityKind::RecoveryAction;
}

// --------------------------------------------------------

} // namespace aegis::trace
