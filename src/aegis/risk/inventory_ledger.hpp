// Purpose: expose the source-private joint reservation/inventory planning seam and bounded signed
// inventory observations without granting public private-event application or deduplication rights.

#pragma once

#include "aegis/model/m4_provenance.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/risk/reservation_ledger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The immutable runtime capacity policy is consumed only during cold inventory installation.
class M4Policy;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::risk {

// ########################################################################
// A retained source row owns one order's cumulative signed contribution and latest execution
// provenance. Rows survive reservation closure/reuse; deduplication and fill ordering belong to
// OMS.
struct InventorySourceRecord {
  oms::OutboundOrderAdmission admission;
  model::Quantity confirmed_quantity;
  model::Notional confirmed_quote_notional;
  oms::NormalizedPrivateOrderInput latest_execution;
  recovery::AuditOrdinal latest_audit_ordinal;

  // --------------------------------------------------------
  // Compare the complete immutable admission, accumulated economics, and latest evidence.
  friend bool operator==(const InventorySourceRecord&, const InventorySourceRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Each confirmed cell uses one canonical complete risk key; no independently mutable confirmed
// copy exists in reservation storage or risk admission.
struct InventoryAggregateCell {
  model::FirmId firm_id;
  RiskScopeKind scope;
  std::string scope_subject;
  model::InstrumentId instrument_id;
  std::string quote_currency;
  model::Quantity confirmed_quantity;
  model::Notional confirmed_quote_notional;

  // --------------------------------------------------------
  // Compare the complete canonical key and exact signed aggregate.
  friend bool operator==(const InventoryAggregateCell&, const InventoryAggregateCell&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One fixed scratch row contains every residual and confirmed replacement required for a scope.
// Its indices remain private to the opaque plan that validates their exact owning incarnation.
struct ReservationInventoryScopeReplacement {
  std::size_t count_index;
  std::size_t quantity_index;
  std::size_t notional_index;
  std::size_t directional_index;
  std::size_t inventory_index;
  RiskScopeExposure exposure;
};

// ########################################################################
// Keeping this empty identity alive with a detached plan prevents allocator-address reuse from
// ever turning a stale plan into authority over a replacement inventory owner.
struct InventoryPlanIncarnation final {};

// ########################################################################
// An opaque move-only plan retains complete fixed scratch without reserving business capacity or
// changing either ledger. Only its original unchanged owner may consume it once; every economic
// mutation and ledger relocation invalidates previously derived plans.
class ReservationInventoryPlan final {
public:

  // --------------------------------------------------------
  // Transfer one plan without duplicating its commit authority; copying is forbidden.
  ReservationInventoryPlan(const ReservationInventoryPlan&) = delete;
  ReservationInventoryPlan& operator=(const ReservationInventoryPlan&) = delete;
  ReservationInventoryPlan(ReservationInventoryPlan&&) noexcept = default;
  ReservationInventoryPlan& operator=(ReservationInventoryPlan&&) noexcept = default;

  // --------------------------------------------------------
  // Borrow the complete original reservation snapshot used for derivation.
  [[nodiscard]] const ReservationEvidence& reservation_before() const noexcept { return before_; }

  // --------------------------------------------------------
  // Borrow the candidate reservation without granting access to mutable owned storage.
  [[nodiscard]] const ReservationEvidence& reservation_after() const noexcept { return after_; }

  // --------------------------------------------------------
  // Borrow the candidate per-order inventory contribution, absent for terminal release.
  [[nodiscard]] const std::optional<InventorySourceRecord>& source_after() const noexcept {
    return source_after_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain an already validated complete plan whose lifetime identity cannot be caller-authored.
  ReservationInventoryPlan(std::shared_ptr<const InventoryPlanIncarnation> incarnation,
                           std::uint64_t generation, const oms::OutboundOrderRecord& order,
                           std::size_t reservation_index, ReservationEvidence before,
                           ReservationEvidence after,
                           std::array<ReservationInventoryScopeReplacement, 7U> scopes,
                           std::optional<std::size_t> source_index,
                           std::optional<InventorySourceRecord> source_after) noexcept;

  // --------------------------------------------------------
  std::shared_ptr<const InventoryPlanIncarnation> incarnation_;
  std::uint64_t generation_;
  const oms::OutboundOrderRecord* order_;
  oms::OutboundOrderAdmission order_admission_;
  oms::PrivateOrderProjection order_projection_;
  std::size_t reservation_index_;
  ReservationEvidence before_;
  ReservationEvidence after_;
  std::array<ReservationInventoryScopeReplacement, 7U> scopes_;
  std::optional<std::size_t> source_index_;
  std::optional<InventorySourceRecord> source_after_;

  // ########################################################################
  // Only this source-private owner may mint or inspect mutation authority within a plan.
  friend class InventoryLedger;

  // ########################################################################
};

// ########################################################################
// The owner-local inventory component owns the sole signed confirmed source and aggregate cells.
// It borrows its enclosing reservations plus stable OMS/catalog authority. Planning is side-effect
// free and commit replaces both economic components atomically to the serialized owner. This seam
// does not decide OMS transitions, dedupe, journal publication, audit capacity, or callbacks.
class InventoryLedger final {
public:

  // --------------------------------------------------------
  // Allocate and validate complete M4 storage before attaching it to pristine reservations. The
  // exact OMS and catalog must outlive the ledger; any reported failure changes no owner state.
  [[nodiscard]] static model::Result<void>
  install_on_pristine_reservations(ReservationLedger& reservations, const oms::OutboundOms& orders,
                                   const execution::OwnerLocalRouteCatalog& routes,
                                   const runtime::M4Policy& policy);

  // --------------------------------------------------------
  // Borrow the installed closed component, or null before the explicit cold installation.
  [[nodiscard]] static InventoryLedger*
  installed_inventory(ReservationLedger& reservations) noexcept;
  [[nodiscard]] static const InventoryLedger*
  installed_inventory(const ReservationLedger& reservations) noexcept;

  // --------------------------------------------------------
  // Preserve the stable component identity and its borrowed authoritative object graph.
  InventoryLedger(const InventoryLedger&) = delete;
  InventoryLedger& operator=(const InventoryLedger&) = delete;
  InventoryLedger(InventoryLedger&&) = delete;
  InventoryLedger& operator=(InventoryLedger&&) = delete;

  // --------------------------------------------------------
  // Validate genuine OMS ownership and complete provenance, then calculate one strictly advancing
  // contiguous execution's residual/confirmed replacement. The caller owns dedupe and the OMS
  // transition decision; failure preserves all component state and exact arithmetic error codes.
  [[nodiscard]] model::Result<ReservationInventoryPlan>
  plan_cumulative_fill(const oms::OutboundOrderRecord& order,
                       const oms::NormalizedPrivateOrderInput& execution,
                       recovery::AuditOrdinal audit_ordinal) const;

  // --------------------------------------------------------
  // Plan exact residual removal for one genuine Held order and an assigned non-fill terminal
  // cause. The caller supplies the definitive business authority; no confirmed position changes.
  [[nodiscard]] model::Result<ReservationInventoryPlan>
  plan_terminal_release(const oms::OutboundOrderRecord& order, ReservationClosureCause cause) const;

  // --------------------------------------------------------
  // Reject foreign, stale, moved-from, or already consumed plans before writes; otherwise apply
  // every checked replacement with no fallible step and consume the plan's sole authority.
  [[nodiscard]] model::Result<void>
  commit_reservation_inventory_plan(ReservationInventoryPlan&& plan);

  // --------------------------------------------------------
  // Report fixed cold capacities and the append-only retained-source count.
  [[nodiscard]] std::uint32_t source_row_capacity() const noexcept;
  [[nodiscard]] std::uint32_t aggregate_cell_capacity() const noexcept;
  [[nodiscard]] std::uint32_t source_row_count() const noexcept;
  [[nodiscard]] std::size_t aggregate_cell_count() const noexcept;

  // --------------------------------------------------------
  // Borrow live const observations in source insertion or canonical aggregate order. Reads require
  // owner serialization or quiescence; later fills update these stable-address rows.
  [[nodiscard]] const InventorySourceRecord* source_at(std::size_t index) const noexcept;
  [[nodiscard]] const InventorySourceRecord*
  find_source(const model::OrderId& order_id) const noexcept;
  [[nodiscard]] const InventoryAggregateCell* aggregate_at(std::size_t index) const noexcept;

  // --------------------------------------------------------
  // Borrow the complete policy authority under which this component was installed.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept { return root_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Preallocate complete source and aggregate backing while the enclosing owner is still pristine.
  InventoryLedger(ReservationLedger& reservations, const oms::OutboundOms& orders,
                  const execution::OwnerLocalRouteCatalog& routes, const runtime::M4Policy& policy);

  // --------------------------------------------------------
  // Resolve one canonical complete risk key without allocating or mutating confirmed truth.
  [[nodiscard]] const InventoryAggregateCell*
  find_aggregate(const model::FirmId& firm_id, RiskScopeKind scope, std::string_view subject,
                 const model::InstrumentId& instrument_id,
                 std::string_view quote_currency) const noexcept;

  // --------------------------------------------------------
  // Sum signed quantity across canonical currency projections for one shared quantity bucket.
  // An optional exact cell replacement lets preflight execute the identical post-commit fold.
  [[nodiscard]] model::Result<model::Quantity> calculate_confirmed_quantity(
      const model::FirmId& firm_id, RiskScopeKind scope, std::string_view subject,
      const model::InstrumentId& instrument_id,
      std::optional<std::pair<std::size_t, model::Quantity>> replacement = std::nullopt) const;

  // --------------------------------------------------------
  // Validate exact bound OMS ownership, immutable route/economics/provenance, and held reservation.
  [[nodiscard]] model::Result<std::size_t>
  find_owned_held_reservation(const oms::OutboundOrderRecord& order) const;

  // --------------------------------------------------------
  // Derive every scope replacement for one fully validated reservation and signed fill delta.
  [[nodiscard]] model::Result<std::array<ReservationInventoryScopeReplacement, 7U>>
  calculate_scope_replacements(std::size_t reservation_index, const ReservationEvidence& after,
                               model::Quantity signed_quantity_delta,
                               model::Notional signed_notional_delta) const;

  // --------------------------------------------------------
  // Invalidate all detached plans after relocation or mutation; exhaustion permanently prevents
  // additional economic writes rather than wrapping into a previously issued generation.
  void invalidate_plans() noexcept;

  // --------------------------------------------------------
  ReservationLedger* reservations_;
  const oms::OutboundOms* orders_;
  std::shared_ptr<const oms::OutboundOms::StorageIncarnation> orders_incarnation_;
  const execution::OwnerLocalRouteCatalog* routes_;
  model::M4RootProvenance root_;
  std::shared_ptr<const InventoryPlanIncarnation> incarnation_;
  std::uint64_t generation_{0U};
  std::uint32_t source_count_{0U};
  std::vector<std::optional<InventorySourceRecord>> sources_;
  std::vector<std::optional<InventoryAggregateCell>> aggregates_;
  std::size_t aggregate_count_{0U};

  // ########################################################################
  // Reservation operations read the sole confirmed cells and invalidate pending joint plans.
  friend class ReservationLedger;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::risk
