// Purpose: define stable private-event dispositions, nominal registry keys, immutable
// first-admission resolutions, and trade-comparison values without storage or OMS mutation.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/risk/account_safety.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace aegis::oms {

// ########################################################################
// Disposition assignments are stable semantic outcomes retained before ADR-0014 adds exact bytes.
enum class PrivateEventDisposition : std::uint8_t {
  Applied = 1,
  BufferedGap = 2,
  AppliedFromBuffer = 3,
  ProjectionOnly = 4,
  ExactEventDuplicate = 5,
  ExactTradeDuplicate = 6,
  SafetyContained = 7,
  ForbiddenRejected = 8,
};

// ########################################################################

// ########################################################################
// A reconciliation registry key retains its complete row identity while excluding every timestamp.
struct ReconciliationPrivateEventKey {
  recovery::ReconciliationEpochId reconciliation_epoch_id;
  AuthoritativeCutId authoritative_cut_id;
  recovery::ReconciliationRowOrdinal row_ordinal;

  // --------------------------------------------------------
  // Equality and total order compare the three semantic identity components in declaration order.
  friend bool operator==(const ReconciliationPrivateEventKey&,
                         const ReconciliationPrivateEventKey&) = default;
  friend auto operator<=>(const ReconciliationPrivateEventKey&,
                          const ReconciliationPrivateEventKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The tagged registry key preserves the origin identity domain while excluding every timestamp.
using PrivateEventRegistryKeyValue =
    std::variant<LocalOrderEventId, VenuePrivateEventKey, ReconciliationPrivateEventKey>;

// ########################################################################

// ########################################################################
// One nominal registry key is derived from the same retained receive-time-free semantic value used
// for event comparison. It grants neither correlation nor registry-mutation authority.
class PrivateEventRegistryKey final {
public:

  // --------------------------------------------------------
  // Derive the exact active identity alternative without consulting mutable OMS correlation.
  // Callers normally retain the semantic value once, then use it for both key and event comparison.
  [[nodiscard]] static PrivateEventRegistryKey
  from_ingress_semantic_value(const PrivateEventIngressSemanticValue& semantic_value) noexcept;

  // --------------------------------------------------------
  // Return the origin domain selected by the active registry-key alternative.
  [[nodiscard]] PrivateEventOrigin origin() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete timestamp-free key in its origin-tagged alternative.
  [[nodiscard]] const PrivateEventRegistryKeyValue& registry_key_value() const noexcept {
    return registry_key_value_;
  }

  // --------------------------------------------------------
  // Equality and ordering exclude timestamps, disposition, resolution, and container position.
  // Ordering compares Local, Venue, and Reconciliation alternatives before their active key value.
  friend bool operator==(const PrivateEventRegistryKey&, const PrivateEventRegistryKey&) = default;
  friend auto operator<=>(const PrivateEventRegistryKey&, const PrivateEventRegistryKey&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain exactly one origin-tagged value selected from the trusted ingress semantic value.
  explicit PrivateEventRegistryKey(PrivateEventRegistryKeyValue registry_key_value) noexcept
      : registry_key_value_{std::move(registry_key_value)} {}

  // --------------------------------------------------------
  // Store the complete nominal identity without source time or local receive time.
  PrivateEventRegistryKeyValue registry_key_value_;
};

// ########################################################################

// ########################################################################
// A known resolution binds exact retained local identity to its complete owner-derived provenance.
struct KnownPrivateEventResolution {
  model::OrderId order_id;
  model::M4Provenance provenance;

  // --------------------------------------------------------
  // Structural equality compares the resolved local order and complete owner provenance.
  friend bool operator==(const KnownPrivateEventResolution&,
                         const KnownPrivateEventResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Unknown adds no mapping-derived ownership to the already retained source-normalized input.
struct UnknownPrivateEventResolution {

  // --------------------------------------------------------
  // All unknown resolutions are equal because they intentionally retain no invented ownership.
  friend bool operator==(const UnknownPrivateEventResolution&,
                         const UnknownPrivateEventResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Pre-trade conflict retains exactly the provenance or correlation reason fixed by ADR-0010.
struct ConflictPrivateEventResolution {
  risk::AccountSafetyReason reason;

  // --------------------------------------------------------
  // Structural equality compares the exact containment reason selected before trade processing.
  friend bool operator==(const ConflictPrivateEventResolution&,
                         const ConflictPrivateEventResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Account timeout and source disconnect are total rows but never pretend to resolve one order.
struct NotOrderScopedPrivateEventResolution {

  // --------------------------------------------------------
  // All non-order resolutions are equal because the retained ingress owns their source subject.
  friend bool operator==(const NotOrderScopedPrivateEventResolution&,
                         const NotOrderScopedPrivateEventResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The variant makes all four first-admission payload shapes mutually exclusive. Only the sealed
// wrapper retains its selected payload immutably.
using PrivateEventResolutionValue =
    std::variant<KnownPrivateEventResolution, UnknownPrivateEventResolution,
                 ConflictPrivateEventResolution, NotOrderScopedPrivateEventResolution>;

// ########################################################################

// ########################################################################
// A retained resolution is immutable evidence. This slice grants no construction authority; the
// later concrete bound owner must add exact access together with its implementation and tests.
class PrivateEventResolution final {
public:

  // --------------------------------------------------------
  // Return which known, unknown, conflict, or non-order resolution alternative is active.
  [[nodiscard]] PrivateEventResolutionKind resolution_kind() const noexcept;

  // --------------------------------------------------------
  // Borrow the complete immutable first-admission resolution value.
  [[nodiscard]] const PrivateEventResolutionValue& resolution_value() const noexcept {
    return resolution_value_;
  }

  // --------------------------------------------------------
  // Return the known alternative when exact bound-owner correlation selected it; otherwise null.
  [[nodiscard]] const KnownPrivateEventResolution* known_resolution() const noexcept;

  // --------------------------------------------------------
  // Structural equality is the complete replay comparison; no current mapping is consulted.
  friend bool operator==(const PrivateEventResolution&, const PrivateEventResolution&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish a full known subject copied from the exact bound OMS row.
  [[nodiscard]] static PrivateEventResolution
  create_known_order_resolution(model::OrderId order_id, model::M4Provenance provenance);

  // --------------------------------------------------------
  // Publish an unknown result without enriching the source provenance.
  [[nodiscard]] static PrivateEventResolution create_unknown_resolution();

  // --------------------------------------------------------
  // Publish the sole pre-correlation provenance mismatch result after first event-key admission.
  [[nodiscard]] static PrivateEventResolution create_provenance_conflict_resolution();

  // --------------------------------------------------------
  // Publish the sole correlation-conflict result before trade lookup or economics.
  [[nodiscard]] static PrivateEventResolution create_correlation_conflict_resolution();

  // --------------------------------------------------------
  // Publish the closed non-order result for account timeout or source disconnect.
  [[nodiscard]] static PrivateEventResolution create_not_order_scoped_resolution();

  // --------------------------------------------------------
  // Retain exactly one future-owner-selected resolution alternative without further validation.
  explicit PrivateEventResolution(PrivateEventResolutionValue resolution_value) noexcept
      : resolution_value_{std::move(resolution_value)} {}

  // --------------------------------------------------------
  // Store exactly one immutable first-admission alternative.
  PrivateEventResolutionValue resolution_value_;
};

// ########################################################################

// ########################################################################
// Known trade comparison owns its exact local order and OMS-derived canonical economic side.
struct KnownPrivateTradeResolution {
  model::OrderId order_id;
  execution::OrderSide canonical_side;

  // --------------------------------------------------------
  // Structural equality compares the resolved order and its OMS-derived canonical side.
  friend bool operator==(const KnownPrivateTradeResolution&,
                         const KnownPrivateTradeResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Unknown trade comparison retains every raw locator presence bit and authoritative-side presence.
struct UnknownPrivateTradeResolution {
  PrivateOrderLocator locator;
  std::optional<execution::OrderSide> canonical_side;

  // --------------------------------------------------------
  // Structural equality compares the raw locator and authoritative-side presence exactly.
  friend bool operator==(const UnknownPrivateTradeResolution&,
                         const UnknownPrivateTradeResolution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Exactly one known or unknown trade-resolution shape is active, preventing contradictory fields.
using PrivateTradeResolutionValue =
    std::variant<KnownPrivateTradeResolution, UnknownPrivateTradeResolution>;

// ########################################################################

// ########################################################################
// The closed trade value contains exactly the ADR-0010 comparison tuple. This slice grants no
// construction authority; the later concrete bound owner must add exact access with its tests.
class PrivateTradeSemanticValue final {
public:

  // --------------------------------------------------------
  // Borrow the normalized instrument claimed by the execution.
  [[nodiscard]] const model::InstrumentId& instrument_id() const noexcept { return instrument_id_; }

  // --------------------------------------------------------
  // Return the claimed instrument-metadata revision used for execution comparison.
  [[nodiscard]] model::InstrumentMetadataRevision metadata_revision() const noexcept {
    return metadata_revision_;
  }

  // --------------------------------------------------------
  // Return the exact incremental execution quantity.
  [[nodiscard]] model::Quantity incremental_quantity() const noexcept {
    return incremental_quantity_;
  }

  // --------------------------------------------------------
  // Return the exact cumulative execution endpoint.
  [[nodiscard]] model::Quantity cumulative_quantity() const noexcept {
    return cumulative_quantity_;
  }

  // --------------------------------------------------------
  // Return the exact execution price used by trade-identity comparison.
  [[nodiscard]] model::Price execution_price() const noexcept { return execution_price_; }

  // --------------------------------------------------------
  // Borrow the exact known or unknown post-correlation resolution alternative.
  [[nodiscard]] const PrivateTradeResolutionValue& resolution_value() const noexcept {
    return resolution_value_;
  }

  // --------------------------------------------------------
  // Equality is the exact independent trade-key duplicate/conflict oracle.
  friend bool operator==(const PrivateTradeSemanticValue&,
                         const PrivateTradeSemanticValue&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Build the closed tuple after source-side validation against the exact retained OMS row.
  [[nodiscard]] static PrivateTradeSemanticValue
  create_known_trade_semantic_value(const ExecutionPayload& execution,
                                    const model::OrderId& order_id,
                                    execution::OrderSide canonical_side);

  // --------------------------------------------------------
  // Build the unknown tuple without inventing a missing source side or local identity.
  [[nodiscard]] static PrivateTradeSemanticValue
  create_unknown_trade_semantic_value(const ExecutionPayload& execution);

  // --------------------------------------------------------
  // Retain one fully derived trade-comparison tuple without exposing construction authority.
  PrivateTradeSemanticValue(model::InstrumentId instrument_id,
                            model::InstrumentMetadataRevision metadata_revision,
                            model::Quantity incremental_quantity,
                            model::Quantity cumulative_quantity, model::Price execution_price,
                            PrivateTradeResolutionValue resolution_value) noexcept
      : instrument_id_{std::move(instrument_id)}, metadata_revision_{metadata_revision},
        incremental_quantity_{incremental_quantity}, cumulative_quantity_{cumulative_quantity},
        execution_price_{execution_price}, resolution_value_{std::move(resolution_value)} {}

  // --------------------------------------------------------
  // Store exactly the trade-key-independent comparison fields fixed by ADR-0010.
  model::InstrumentId instrument_id_;
  model::InstrumentMetadataRevision metadata_revision_;
  model::Quantity incremental_quantity_;
  model::Quantity cumulative_quantity_;
  model::Price execution_price_;
  PrivateTradeResolutionValue resolution_value_;
};

// ########################################################################

} // namespace aegis::oms
