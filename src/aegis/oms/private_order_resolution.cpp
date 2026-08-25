// Purpose: derive nominal private-event registry keys and implement closed first-admission
// resolution and trade-comparison values without mutable OMS state or registry storage.

#include "aegis/oms/private_order_resolution.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace aegis::oms {

// --------------------------------------------------------
// Derive exactly one identity-domain key from the retained receive-time-free ingress origin.
PrivateEventRegistryKey PrivateEventRegistryKey::from_ingress_semantic_value(
    const PrivateEventIngressSemanticValue& semantic_value) noexcept {
  // Interesting syntax: std::visit selects the active origin, while if constexpr compiles only its
  // typed projection so no other nominal origin alternative can contribute fields.
  return PrivateEventRegistryKey{std::visit(
      [](const auto& origin) -> PrivateEventRegistryKeyValue {
        using Origin = std::decay_t<decltype(origin)>;
        if constexpr (std::is_same_v<Origin, LocalPrivateIngressOrigin>) {
          return origin.event_id;
        } else if constexpr (std::is_same_v<Origin, VenuePrivateIngressOrigin>) {
          return origin.event_key;
        } else {
          static_assert(std::is_same_v<Origin, ReconciliationPrivateIngressOrigin>);
          return ReconciliationPrivateEventKey{origin.reconciliation_epoch_id,
                                               origin.authoritative_cut_id, origin.row_ordinal};
        }
      },
      semantic_value.origin())};
}

// --------------------------------------------------------
// Project the active registry-key alternative back to its stable origin assignment.
PrivateEventOrigin PrivateEventRegistryKey::origin() const noexcept {
  if (std::holds_alternative<LocalOrderEventId>(registry_key_value_)) {
    return PrivateEventOrigin::Local;
  }
  if (std::holds_alternative<VenuePrivateEventKey>(registry_key_value_)) {
    return PrivateEventOrigin::Venue;
  }
  return PrivateEventOrigin::Reconciliation;
}

// --------------------------------------------------------
// Map the closed resolution alternative to its accepted stable numeric assignment.
PrivateEventResolutionKind PrivateEventResolution::resolution_kind() const noexcept {
  if (std::holds_alternative<KnownPrivateEventResolution>(resolution_value_)) {
    return PrivateEventResolutionKind::Known;
  }
  if (std::holds_alternative<UnknownPrivateEventResolution>(resolution_value_)) {
    return PrivateEventResolutionKind::Unknown;
  }
  if (std::holds_alternative<ConflictPrivateEventResolution>(resolution_value_)) {
    return PrivateEventResolutionKind::Conflict;
  }
  return PrivateEventResolutionKind::NotOrderScoped;
}

// --------------------------------------------------------
// Borrow the known payload without copying its complete resolved provenance.
const KnownPrivateEventResolution* PrivateEventResolution::known_resolution() const noexcept {
  return std::get_if<KnownPrivateEventResolution>(&resolution_value_);
}

// --------------------------------------------------------
// Retain exact local identity and complete resolved provenance as one immutable alternative.
PrivateEventResolution
PrivateEventResolution::create_known_order_resolution(model::OrderId order_id,
                                                      model::M4Provenance provenance) {
  return PrivateEventResolution{
      KnownPrivateEventResolution{std::move(order_id), std::move(provenance)}};
}

// --------------------------------------------------------
// Unknown correlation adds no fields beyond the source-normalized input itself.
PrivateEventResolution PrivateEventResolution::create_unknown_resolution() {
  return PrivateEventResolution{UnknownPrivateEventResolution{}};
}

// --------------------------------------------------------
// Provenance mismatch has one fixed alternative and cannot accept caller-selected reason data.
PrivateEventResolution PrivateEventResolution::create_provenance_conflict_resolution() {
  return PrivateEventResolution{
      ConflictPrivateEventResolution{risk::AccountSafetyReason::ProvenanceMismatch}};
}

// --------------------------------------------------------
// Correlation conflict has one fixed alternative and cannot accept caller-selected reason data.
PrivateEventResolution PrivateEventResolution::create_correlation_conflict_resolution() {
  return PrivateEventResolution{
      ConflictPrivateEventResolution{risk::AccountSafetyReason::CorrelationConflict}};
}

// --------------------------------------------------------
// Account and private-source observations use one truthful non-order resolution alternative.
PrivateEventResolution PrivateEventResolution::create_not_order_scoped_resolution() {
  return PrivateEventResolution{NotOrderScopedPrivateEventResolution{}};
}

// --------------------------------------------------------
// Construct the exact known-order trade tuple after the owner has derived canonical side.
PrivateTradeSemanticValue
PrivateTradeSemanticValue::create_known_trade_semantic_value(const ExecutionPayload& execution,
                                                             const model::OrderId& order_id,
                                                             execution::OrderSide canonical_side) {
  return PrivateTradeSemanticValue{
      execution.instrument_id,        execution.metadata_revision,
      execution.incremental_quantity, execution.cumulative_quantity,
      execution.execution_price,      KnownPrivateTradeResolution{order_id, canonical_side},
  };
}

// --------------------------------------------------------
// Construct the exact unknown-order trade tuple from raw locators and source-side presence.
PrivateTradeSemanticValue
PrivateTradeSemanticValue::create_unknown_trade_semantic_value(const ExecutionPayload& execution) {
  return PrivateTradeSemanticValue{
      execution.instrument_id,
      execution.metadata_revision,
      execution.incremental_quantity,
      execution.cumulative_quantity,
      execution.execution_price,
      UnknownPrivateTradeResolution{execution.locator, execution.source_side},
  };
}

// --------------------------------------------------------

} // namespace aegis::oms
