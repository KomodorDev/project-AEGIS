// Purpose: allocate the sole bounded confirmed-inventory backing and expose owner-local const
// observations; joint reservation planning and mutation live beside the reservation storage.

#include "inventory_ledger.hpp"

#include "aegis/runtime/m4_policy.hpp"

#include <algorithm>
#include <limits>
#include <tuple>
#include <utility>

namespace aegis::risk {

// --------------------------------------------------------
// Capture fixed scratch and its non-reusable owner identity without allocating during planning.
ReservationInventoryPlan::ReservationInventoryPlan(
    std::shared_ptr<const InventoryPlanIncarnation> incarnation, std::uint64_t generation,
    const oms::OutboundOrderRecord& order, std::size_t reservation_index,
    ReservationEvidence before, ReservationEvidence after,
    std::array<ReservationInventoryScopeReplacement, 7U> scopes,
    std::optional<std::size_t> source_index,
    std::optional<InventorySourceRecord> source_after) noexcept
    : incarnation_{std::move(incarnation)}, generation_{generation}, order_{&order},
      order_admission_{order.admission()}, order_projection_{order.private_projection()},
      reservation_index_{reservation_index}, before_{before}, after_{after}, scopes_{scopes},
      source_index_{source_index}, source_after_{std::move(source_after)} {}

// --------------------------------------------------------
// Allocate policy-sized source and aggregate backing before any reservation is admitted.
InventoryLedger::InventoryLedger(ReservationLedger& reservations, const oms::OutboundOms& orders,
                                 const execution::OwnerLocalRouteCatalog& routes,
                                 const runtime::M4Policy& policy)
    : reservations_{&reservations}, orders_{&orders},
      orders_incarnation_{orders.storage_incarnation_}, routes_{&routes},
      root_{policy.root_provenance()}, incarnation_{std::make_shared<InventoryPlanIncarnation>()},
      sources_(static_cast<std::size_t>(policy.capacities().max_inventory_source_rows)),
      aggregates_(static_cast<std::size_t>(policy.capacities().max_inventory_aggregate_cells)) {
  const auto zero_quantity = model::Quantity::from_scaled(0, 0).value();
  const auto zero_notional = model::Notional::from_scaled(0, 0).value();
  for (const auto& row : reservations.policy().limit_sets()) {
    aggregates_[aggregate_count_].emplace(InventoryAggregateCell{
        row.firm_id(), row.scope(), std::string{row.scope_subject()}, row.instrument_id(),
        std::string{row.quote_currency()}, zero_quantity, zero_notional});
    ++aggregate_count_;
  }
}

// --------------------------------------------------------
// Return the exact fixed source-row capacity allocated at installation.
std::uint32_t InventoryLedger::source_row_capacity() const noexcept {
  return static_cast<std::uint32_t>(sources_.size());
}

// --------------------------------------------------------
// Return the exact policy aggregate capacity, including unused reserved backing.
std::uint32_t InventoryLedger::aggregate_cell_capacity() const noexcept {
  return static_cast<std::uint32_t>(aggregates_.size());
}

// --------------------------------------------------------
// Return the append-only number of order sources that have acquired confirmed economics.
std::uint32_t InventoryLedger::source_row_count() const noexcept { return source_count_; }

// --------------------------------------------------------
// Return the canonical complete-key count derived from the immutable risk policy.
std::size_t InventoryLedger::aggregate_cell_count() const noexcept { return aggregate_count_; }

// --------------------------------------------------------
// Borrow one live const source row under owner serialization or quiescence; later fills may update
// its cumulative values, while the row remains at the same address for the ledger lifetime.
const InventorySourceRecord* InventoryLedger::source_at(std::size_t index) const noexcept {
  return index < source_count_ ? &sources_[index].value() : nullptr;
}

// --------------------------------------------------------
// Resolve one complete source order identity without assigning ownership or deciding duplicates.
const InventorySourceRecord*
InventoryLedger::find_source(const model::OrderId& order_id) const noexcept {
  for (std::size_t index = 0U; index < source_count_; ++index) {
    if (sources_[index]->admission.order_id == order_id) {
      return &sources_[index].value();
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Borrow one live canonical aggregate only under owner serialization or quiescence.
const InventoryAggregateCell* InventoryLedger::aggregate_at(std::size_t index) const noexcept {
  return index < aggregate_count_ ? &aggregates_[index].value() : nullptr;
}

// --------------------------------------------------------
// Resolve a complete canonical policy key without constructing an owning temporary string.
const InventoryAggregateCell*
InventoryLedger::find_aggregate(const model::FirmId& firm_id, RiskScopeKind scope,
                                std::string_view subject, const model::InstrumentId& instrument_id,
                                std::string_view quote_currency) const noexcept {
  const auto key =
      std::tuple{firm_id.value(), scope, subject, instrument_id.value(), quote_currency};
  const auto end = aggregates_.begin() + static_cast<std::ptrdiff_t>(aggregate_count_);
  const auto found =
      std::lower_bound(aggregates_.begin(), end, key, [](const auto& cell, const auto& target) {
        return std::tuple{cell->firm_id.value(), cell->scope, std::string_view{cell->scope_subject},
                          cell->instrument_id.value(),
                          std::string_view{cell->quote_currency}} < target;
      });
  if (found == end || std::tuple{found->value().firm_id.value(), found->value().scope,
                                 std::string_view{found->value().scope_subject},
                                 found->value().instrument_id.value(),
                                 std::string_view{found->value().quote_currency}} != key) {
    return nullptr;
  }
  return &found->value();
}

// --------------------------------------------------------
// Sum one instrument's canonical complete-key contributions, optionally substituting a planned
// cell before checking each prefix; calculation order matches every later query exactly.
model::Result<model::Quantity> InventoryLedger::calculate_confirmed_quantity(
    const model::FirmId& firm_id, RiskScopeKind scope, std::string_view subject,
    const model::InstrumentId& instrument_id,
    std::optional<std::pair<std::size_t, model::Quantity>> replacement) const {
  auto total = model::Quantity::from_scaled(0, 0).value();
  for (std::size_t index = 0U; index < aggregate_count_; ++index) {
    const auto& cell = aggregates_[index].value();
    if (cell.firm_id == firm_id && cell.scope == scope && cell.scope_subject == subject &&
        cell.instrument_id == instrument_id) {
      const auto contribution = replacement && replacement->first == index
                                    ? replacement->second
                                    : cell.confirmed_quantity;
      auto added = total.checked_add(contribution);
      if (!added) {
        return model::Result<model::Quantity>::create_failure(std::move(added).error());
      }
      total = added.value();
    }
  }
  return model::Result<model::Quantity>::create_success(total);
}

// --------------------------------------------------------
// Never wrap a generation into authority already carried by a detached plan. At the terminal value,
// every planning, commit, reserve, and release operation rejects additional economic mutation.
void InventoryLedger::invalidate_plans() noexcept {
  if (generation_ != std::numeric_limits<std::uint64_t>::max()) {
    ++generation_;
  }
}

// --------------------------------------------------------

} // namespace aegis::risk
