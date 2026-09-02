// Purpose: implement fixed preallocated M3 risk cells, canonical limit precedence, atomic
// check-and-reserve, and exact-once reusable-slot release on one serialized owner.

#include "aegis/risk/reservation_ledger.hpp"

#include "aegis/model/domain_error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::risk {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// RiskScopeCountKey qualifies every mutable bucket by authoritative firm, scope kind, and scope
// subject.
struct RiskScopeCountKey {
  model::FirmId firm_id;
  RiskScopeKind scope;
  std::string subject;

  // --------------------------------------------------------
  // Structural equality compares the complete firm, scope-kind, and subject bucket identity.
  friend bool operator==(const RiskScopeCountKey&, const RiskScopeCountKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// RiskScopeInstrumentQuantityKey prevents directional contract quantities from aggregating across
// instruments.
struct RiskScopeInstrumentQuantityKey {
  RiskScopeCountKey count;
  model::InstrumentId instrument_id;

  // --------------------------------------------------------
  // Structural equality compares the complete scope and normalized-instrument bucket identity.
  friend bool operator==(const RiskScopeInstrumentQuantityKey&,
                         const RiskScopeInstrumentQuantityKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// RiskScopeQuoteNotionalKey qualifies gross and aggregate worst exposure by quote currency.
struct RiskScopeQuoteNotionalKey {
  RiskScopeCountKey count;
  std::string quote_currency;

  // --------------------------------------------------------
  // Structural equality compares the complete scope and quote-currency bucket identity.
  friend bool operator==(const RiskScopeQuoteNotionalKey&,
                         const RiskScopeQuoteNotionalKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// RiskScopeInstrumentDirectionalNotionalKey retains each instrument contribution before
// quote-currency aggregation.
struct RiskScopeInstrumentDirectionalNotionalKey {
  RiskScopeCountKey count;
  model::InstrumentId instrument_id;
  std::string quote_currency;

  // --------------------------------------------------------
  // Structural equality compares the complete scope, instrument, and quote-currency bucket
  // identity.
  friend bool operator==(const RiskScopeInstrumentDirectionalNotionalKey&,
                         const RiskScopeInstrumentDirectionalNotionalKey&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Compare the common RiskScopeCountKey prefix without constructing a temporary owning identifier.
[[nodiscard]] auto sort_tuple_from_reservation_key(const RiskScopeCountKey& key) noexcept {
  return std::tuple{key.firm_id.value(), key.scope, std::string_view{key.subject}};
}

// --------------------------------------------------------
// Compare quantity buckets by their complete ADR-0008 key.
[[nodiscard]] auto
sort_tuple_from_reservation_key(const RiskScopeInstrumentQuantityKey& key) noexcept {
  return std::tuple{key.count.firm_id.value(), key.count.scope, std::string_view{key.count.subject},
                    key.instrument_id.value()};
}

// --------------------------------------------------------
// Compare quote-currency aggregates by their complete ADR-0008 key.
[[nodiscard]] auto sort_tuple_from_reservation_key(const RiskScopeQuoteNotionalKey& key) noexcept {
  return std::tuple{key.count.firm_id.value(), key.count.scope, std::string_view{key.count.subject},
                    std::string_view{key.quote_currency}};
}

// --------------------------------------------------------
// Compare per-instrument quote contributions by their complete ADR-0008 key.
[[nodiscard]] auto
sort_tuple_from_reservation_key(const RiskScopeInstrumentDirectionalNotionalKey& key) noexcept {
  return std::tuple{key.count.firm_id.value(), key.count.scope, std::string_view{key.count.subject},
                    key.instrument_id.value(), std::string_view{key.quote_currency}};
}

// --------------------------------------------------------
// Canonicalize and deduplicate one startup key collection before cells are allocated.
template <typename Key> void canonicalize_keys(std::vector<Key>& keys) {
  std::sort(keys.begin(), keys.end(), [](const Key& left, const Key& right) {
    return sort_tuple_from_reservation_key(left) < sort_tuple_from_reservation_key(right);
  });
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

// --------------------------------------------------------
// Construct the canonical zero in a nominal decimal domain at startup or in fixed scratch.
template <typename Decimal> [[nodiscard]] Decimal zero_decimal() {
  return Decimal::from_scaled(0, 0).value();
}

// --------------------------------------------------------
// Resolve one scope subject directly from the installed route without allocating.
[[nodiscard]] std::string_view scope_subject(const execution::InstalledSubmissionRoute& installed,
                                             RiskScopeKind scope) noexcept {
  switch (scope) {
  case RiskScopeKind::Bot:
    return installed.attribution().bot_id.value();
  case RiskScopeKind::Desk:
    return installed.attribution().desk_id.value();
  case RiskScopeKind::Firm:
    return installed.attribution().firm_id.value();
  case RiskScopeKind::Account:
    return installed.route().logical_account_id.value();
  case RiskScopeKind::Route:
    return installed.route().id.value();
  case RiskScopeKind::Instrument:
    return installed.metadata().instrument_id().value();
  case RiskScopeKind::Venue:
    return installed.metadata().venue_id().value();
  default:
    return {};
  }
}

// --------------------------------------------------------
// Return the greater nonnegative directional value; confirmed M3 exposure is exactly zero.
template <typename Decimal> [[nodiscard]] Decimal greater(Decimal left, Decimal right) noexcept {
  return left > right ? left : right;
}

// --------------------------------------------------------
// Build one ordinary rejection with no mutation or reservation identity.
[[nodiscard]] RiskCheckResult
create_rejected_risk_check_result(execution::SubmissionReason reason) noexcept {
  return RiskCheckResult::create_rejected_risk_check_result(reason);
}

// --------------------------------------------------------
// Build one ordinary first-limit rejection with its exact typed evidence.
[[nodiscard]] RiskCheckResult
create_rejected_risk_check_result(execution::SubmissionReason reason,
                                  execution::RiskLimitEvidence evidence) noexcept {
  return RiskCheckResult::create_rejected_risk_check_result(reason, std::move(evidence));
}

// --------------------------------------------------------
// Map invalid or repeated release to the sole persisted reservation-state invariant error.
[[nodiscard]] model::Result<void> create_invalid_reservation_result() {
  return model::Result<void>::create_failure(DomainError::create_at_field(
      DomainErrorCode::InvalidRiskReservationState, "risk_reservation.state"));
}

// --------------------------------------------------------

} // namespace

// ########################################################################
// ReservationLedgerStorage owns every mutable cell behind a stable-address move-only ledger
// façade.
struct ReservationLedger::ReservationLedgerStorage {

  // ########################################################################
  // CountCell stores only currently held open-order count for one RiskScopeCountKey.
  struct CountCell {
    RiskScopeCountKey key;
    std::uint64_t open_order_count{0U};
  };

  // ########################################################################
  // QuantityCell keeps buy and sell reservations separate for exact directional maximum.
  struct QuantityCell {
    RiskScopeInstrumentQuantityKey key;
    model::Quantity reserved_buy;
    model::Quantity reserved_sell;
  };

  // ########################################################################
  // NotionalCell owns gross sum and the aggregate of per-instrument directional maxima.
  struct NotionalCell {
    RiskScopeQuoteNotionalKey key;
    model::Notional gross_reserved;
    model::Notional aggregate_worst;
  };

  // ########################################################################
  // DirectionalNotionalCell preserves one instrument's buy, sell, and current maximum contribution.
  struct DirectionalNotionalCell {
    RiskScopeInstrumentDirectionalNotionalKey key;
    model::Notional reserved_buy;
    model::Notional reserved_sell;
    model::Notional instrument_worst;
  };

  // ########################################################################
  // ScopeIndices binds one complete limit row to the five cells it reads and updates.
  struct ScopeIndices {
    std::size_t count;
    std::size_t quantity;
    std::size_t notional;
    std::size_t directional_notional;
    const RiskLimitSet* limits;
  };

  // ########################################################################
  // ScopeCandidate holds every post-order value before any mutable cell is committed.
  struct ScopeCandidate {
    ScopeIndices indices;
    std::uint64_t open_order_count;
    model::Notional gross_reserved;
    model::Quantity reserved_buy_quantity;
    model::Quantity reserved_sell_quantity;
    model::Quantity worst_quantity;
    model::Notional reserved_buy_notional;
    model::Notional reserved_sell_notional;
    model::Notional instrument_worst_notional;
    model::Notional aggregate_worst_notional;
  };

  // ########################################################################
  // ReservationSlot retains exact inverse deltas plus the cell bindings used by all seven scopes.
  struct ReservationSlot {
    ReservationEvidence evidence;
    std::array<ScopeIndices, 7U> scopes;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Prebuild every shared cell key and exactly capacity optional reusable reservation slots.
  ReservationLedgerStorage(RiskPolicySnapshot accepted_policy, std::uint32_t accepted_capacity)
      : policy{std::move(accepted_policy)}, capacity{accepted_capacity} {
    std::vector<RiskScopeCountKey> count_keys;
    std::vector<RiskScopeInstrumentQuantityKey> quantity_keys;
    std::vector<RiskScopeQuoteNotionalKey> notional_keys;
    std::vector<RiskScopeInstrumentDirectionalNotionalKey> directional_keys;
    count_keys.reserve(policy.limit_sets().size());
    quantity_keys.reserve(policy.limit_sets().size());
    notional_keys.reserve(policy.limit_sets().size());
    directional_keys.reserve(policy.limit_sets().size());

    for (const auto& row : policy.limit_sets()) {
      RiskScopeCountKey count{row.firm_id(), row.scope(), std::string{row.scope_subject()}};
      count_keys.push_back(count);
      quantity_keys.push_back(RiskScopeInstrumentQuantityKey{count, row.instrument_id()});
      notional_keys.push_back(RiskScopeQuoteNotionalKey{count, std::string{row.quote_currency()}});
      directional_keys.push_back(RiskScopeInstrumentDirectionalNotionalKey{
          std::move(count), row.instrument_id(), std::string{row.quote_currency()}});
    }
    canonicalize_keys(count_keys);
    canonicalize_keys(quantity_keys);
    canonicalize_keys(notional_keys);
    canonicalize_keys(directional_keys);

    const auto zero_quantity = zero_decimal<model::Quantity>();
    const auto zero_notional = zero_decimal<model::Notional>();
    count_cells.reserve(count_keys.size());
    for (auto& key : count_keys) {
      count_cells.push_back(CountCell{std::move(key), 0U});
    }
    quantity_cells.reserve(quantity_keys.size());
    for (auto& key : quantity_keys) {
      quantity_cells.push_back(QuantityCell{std::move(key), zero_quantity, zero_quantity});
    }
    notional_cells.reserve(notional_keys.size());
    for (auto& key : notional_keys) {
      notional_cells.push_back(NotionalCell{std::move(key), zero_notional, zero_notional});
    }
    directional_notional_cells.reserve(directional_keys.size());
    for (auto& key : directional_keys) {
      directional_notional_cells.push_back(
          DirectionalNotionalCell{std::move(key), zero_notional, zero_notional, zero_notional});
    }
    reservation_slots.resize(capacity);
  }

  // --------------------------------------------------------
  // Locate one preallocated cell by a non-owning canonical tuple.
  template <typename Cell, typename KeyTuple>
  [[nodiscard]] static std::optional<std::size_t> find_cell(const std::vector<Cell>& cells,
                                                            const KeyTuple& key) noexcept {
    const auto found =
        std::lower_bound(cells.begin(), cells.end(), key, [](const Cell& cell, const auto& target) {
          return sort_tuple_from_reservation_key(cell.key) < target;
        });
    if (found == cells.end() || sort_tuple_from_reservation_key(found->key) != key) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(found - cells.begin());
  }

  // --------------------------------------------------------
  // Resolve one policy row and all five corresponding cell indices without constructing a key.
  [[nodiscard]] std::optional<ScopeIndices>
  resolve_scope(const execution::InstalledSubmissionRoute& route,
                RiskScopeKind scope) const noexcept {
    const auto firm = route.attribution().firm_id.value();
    const auto subject = scope_subject(route, scope);
    const auto instrument = route.metadata().instrument_id().value();
    const auto currency = route.metadata().quote_currency();
    const auto* const limits = policy.find_limit_set(route.attribution().firm_id, scope, subject,
                                                     route.metadata().instrument_id(), currency);
    if (limits == nullptr) {
      return std::nullopt;
    }
    const auto count = find_cell(count_cells, std::tuple{firm, scope, subject});
    const auto quantity = find_cell(quantity_cells, std::tuple{firm, scope, subject, instrument});
    const auto notional = find_cell(notional_cells, std::tuple{firm, scope, subject, currency});
    const auto directional = find_cell(directional_notional_cells,
                                       std::tuple{firm, scope, subject, instrument, currency});
    if (!count || !quantity || !notional || !directional) {
      return std::nullopt;
    }
    return ScopeIndices{*count, *quantity, *notional, *directional, limits};
  }

  // --------------------------------------------------------
  // Calculate one scope's post-order cells with no mutation and checked exact arithmetic.
  [[nodiscard]] model::Result<ScopeCandidate>
  calculate_scope_candidate(const ScopeIndices& indices, execution::OrderSide side,
                            const OrderExposure& exposure) const {
    const auto& count = count_cells[indices.count];
    const auto& quantity = quantity_cells[indices.quantity];
    const auto& notional = notional_cells[indices.notional];
    const auto& directional = directional_notional_cells[indices.directional_notional];
    if (count.open_order_count == std::numeric_limits<std::uint64_t>::max()) {
      return model::Result<ScopeCandidate>::create_failure(DomainError::create_at_field(
          DomainErrorCode::ArithmeticOverflow, "risk.open_order_count"));
    }
    auto gross = notional.gross_reserved.checked_add(exposure.quote_notional);
    auto buy_quantity = side == execution::OrderSide::Buy
                            ? quantity.reserved_buy.checked_add(exposure.quantity)
                            : model::Result<model::Quantity>::create_success(quantity.reserved_buy);
    auto sell_quantity =
        side == execution::OrderSide::Sell
            ? quantity.reserved_sell.checked_add(exposure.quantity)
            : model::Result<model::Quantity>::create_success(quantity.reserved_sell);
    auto buy_notional =
        side == execution::OrderSide::Buy
            ? directional.reserved_buy.checked_add(exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_buy);
    auto sell_notional =
        side == execution::OrderSide::Sell
            ? directional.reserved_sell.checked_add(exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_sell);
    if (!gross || !buy_quantity || !sell_quantity || !buy_notional || !sell_notional) {
      return model::Result<ScopeCandidate>::create_failure(DomainError::create_at_field(
          DomainErrorCode::ArithmeticOverflow, "risk.exposure_accumulator"));
    }
    const auto instrument_worst = greater(buy_notional.value(), sell_notional.value());
    auto aggregate_without_old =
        notional.aggregate_worst.checked_subtract(directional.instrument_worst);
    if (!aggregate_without_old) {
      return model::Result<ScopeCandidate>::create_failure(
          std::move(aggregate_without_old).error());
    }
    auto aggregate = aggregate_without_old.value().checked_add(instrument_worst);
    if (!aggregate) {
      return model::Result<ScopeCandidate>::create_failure(std::move(aggregate).error());
    }
    return model::Result<ScopeCandidate>::create_success(ScopeCandidate{
        indices, count.open_order_count + 1U, gross.value(), buy_quantity.value(),
        sell_quantity.value(), greater(buy_quantity.value(), sell_quantity.value()),
        buy_notional.value(), sell_notional.value(), instrument_worst, aggregate.value()});
  }

  RiskPolicySnapshot policy;
  std::uint32_t capacity;
  std::uint32_t held_count{0U};
  std::vector<CountCell> count_cells;
  std::vector<QuantityCell> quantity_cells;
  std::vector<NotionalCell> notional_cells;
  std::vector<DirectionalNotionalCell> directional_notional_cells;
  std::vector<std::optional<ReservationSlot>> reservation_slots;
};

// ########################################################################

// --------------------------------------------------------
// Capture the stable-address private implementation only after its type is complete.
ReservationLedger::ReservationLedger(
    std::unique_ptr<ReservationLedgerStorage> implementation) noexcept
    : implementation_{std::move(implementation)} {}

// --------------------------------------------------------
// Reject zero capacity before allocating the owner-local ledger implementation.
model::Result<ReservationLedger>
ReservationLedger::create_reservation_ledger(RiskPolicySnapshot policy, std::uint32_t capacity) {
  if (capacity == 0U) {
    return model::Result<ReservationLedger>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.reservation_capacity"));
  }
  return model::Result<ReservationLedger>::create_success(
      ReservationLedger{std::make_unique<ReservationLedgerStorage>(std::move(policy), capacity)});
}

// --------------------------------------------------------
// Move transfers the sole mutable owner-local ledger capability.
ReservationLedger::ReservationLedger(ReservationLedger&&) noexcept = default;

// --------------------------------------------------------
// Move assignment transfers the sole mutable owner-local ledger capability.
ReservationLedger& ReservationLedger::operator=(ReservationLedger&&) noexcept = default;

// --------------------------------------------------------
// Out-of-line destruction keeps the private implementation incomplete in the public header.
ReservationLedger::~ReservationLedger() = default;

// --------------------------------------------------------
// Compute every candidate and limit before committing all 35 cells plus one reusable slot.
RiskCheckResult
ReservationLedger::check_and_reserve(model::SubmissionAttemptId attempt_id,
                                     const execution::InstalledSubmissionRoute& route,
                                     const execution::CanonicalOrderEconomics& economics) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 1: calculate conservative quote face notional exactly once for every scope.
  auto calculated = calculate_order_exposure(economics, route.metadata(),
                                             implementation_->policy.notional_scale());
  if (!calculated) {
    return create_rejected_risk_check_result(execution::SubmissionReason::RiskArithmeticFailure);
  }
  const auto exposure = calculated.value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 2: resolve and calculate all seven-by-five cells in fixed stack scratch without mutation.
  std::array<std::optional<ReservationLedgerStorage::ScopeCandidate>, 7U> candidates;
  for (std::size_t index = 0U; index < candidates.size(); ++index) {
    const auto scope = static_cast<RiskScopeKind>(index + 1U);
    const auto indices = implementation_->resolve_scope(route, scope);
    if (!indices) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::SubmissionRuntimeFaulted);
    }
    auto candidate = implementation_->calculate_scope_candidate(*indices, economics.side, exposure);
    if (!candidate) {
      return create_rejected_risk_check_result(execution::SubmissionReason::RiskArithmeticFailure);
    }
    candidates[index].emplace(std::move(candidate).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 3: apply exact scope-major and limit-kind-minor precedence; equality always admits.
  for (const auto& optional_candidate : candidates) {
    const auto& candidate = optional_candidate.value();
    const auto& limits = *candidate.indices.limits;
    const auto scope = limits.scope();
    if (exposure.quantity > limits.maximum_single_order_quantity()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::SingleOrderQuantityExceeded,
          execution::RiskLimitEvidence::create_quantity_evidence(
              scope, exposure.quantity, limits.maximum_single_order_quantity()));
    }
    if (exposure.quote_notional > limits.maximum_single_order_quote_notional()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::SingleOrderNotionalExceeded,
          execution::RiskLimitEvidence::create_quote_notional_evidence(
              scope, exposure.quote_notional, limits.maximum_single_order_quote_notional()));
    }
    if (candidate.open_order_count > limits.maximum_open_order_count()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::OpenOrderCountExceeded,
          execution::RiskLimitEvidence::create_order_count_evidence(
              scope, candidate.open_order_count, limits.maximum_open_order_count()));
    }
    if (candidate.gross_reserved > limits.maximum_gross_reserved_quote_notional()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::GrossReservedNotionalExceeded,
          execution::RiskLimitEvidence::create_quote_notional_evidence(
              scope, candidate.gross_reserved, limits.maximum_gross_reserved_quote_notional()));
    }
    if (candidate.worst_quantity > limits.maximum_worst_case_position_quantity()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::WorstCasePositionQuantityExceeded,
          execution::RiskLimitEvidence::create_quantity_evidence(
              scope, candidate.worst_quantity, limits.maximum_worst_case_position_quantity()));
    }
    if (candidate.aggregate_worst_notional > limits.maximum_worst_case_position_quote_notional()) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::WorstCasePositionNotionalExceeded,
          execution::RiskLimitEvidence::create_quote_notional_evidence(
              scope, candidate.aggregate_worst_notional,
              limits.maximum_worst_case_position_quote_notional()));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 4: capacity is checked only after all fixed limits and still mutates no candidate cell.
  if (implementation_->held_count >= implementation_->capacity) {
    return create_rejected_risk_check_result(
        execution::SubmissionReason::ReservationCapacityExceeded);
  }
  const auto reservation_id_result = model::ReservationId::from_value(attempt_id.value());
  if (!reservation_id_result) {
    return create_rejected_risk_check_result(execution::SubmissionReason::SubmissionRuntimeFaulted);
  }
  const auto reservation_id = reservation_id_result.value();
  auto available = implementation_->reservation_slots.end();
  for (auto slot = implementation_->reservation_slots.begin();
       slot != implementation_->reservation_slots.end(); ++slot) {
    if (slot->has_value() && slot->value().evidence.reservation_id == reservation_id) {
      return create_rejected_risk_check_result(
          execution::SubmissionReason::SubmissionRuntimeFaulted);
    }
    if (available == implementation_->reservation_slots.end() &&
        (!slot->has_value() || (slot->value().evidence.state == ReservationState::Released &&
                                slot->value().evidence.reservation_id < reservation_id))) {
      available = slot;
    }
  }
  if (available == implementation_->reservation_slots.end()) {
    return create_rejected_risk_check_result(execution::SubmissionReason::SubmissionRuntimeFaulted);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 5: commit every scratch value, then publish the matching reservation as one owner action.
  std::array<ReservationLedgerStorage::ScopeIndices, 7U> scope_indices{
      candidates[0U]->indices, candidates[1U]->indices, candidates[2U]->indices,
      candidates[3U]->indices, candidates[4U]->indices, candidates[5U]->indices,
      candidates[6U]->indices};
  for (const auto& optional_candidate : candidates) {
    const auto& candidate = optional_candidate.value();
    implementation_->count_cells[candidate.indices.count].open_order_count =
        candidate.open_order_count;
    auto& quantity = implementation_->quantity_cells[candidate.indices.quantity];
    quantity.reserved_buy = candidate.reserved_buy_quantity;
    quantity.reserved_sell = candidate.reserved_sell_quantity;
    auto& notional = implementation_->notional_cells[candidate.indices.notional];
    notional.gross_reserved = candidate.gross_reserved;
    notional.aggregate_worst = candidate.aggregate_worst_notional;
    auto& directional =
        implementation_->directional_notional_cells[candidate.indices.directional_notional];
    directional.reserved_buy = candidate.reserved_buy_notional;
    directional.reserved_sell = candidate.reserved_sell_notional;
    directional.instrument_worst = candidate.instrument_worst_notional;
  }
  available->emplace(ReservationLedgerStorage::ReservationSlot{
      ReservationEvidence{reservation_id, ReservationState::Held, economics.side, exposure},
      scope_indices});
  ++implementation_->held_count;
  return RiskCheckResult{execution::SubmissionReason::None, reservation_id, exposure, std::nullopt};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate identity/state, compute all inverse deltas in scratch, then transition exactly once.
model::Result<void> ReservationLedger::release_reservation(model::ReservationId reservation_id) {
  const auto found =
      std::find_if(implementation_->reservation_slots.begin(),
                   implementation_->reservation_slots.end(), [reservation_id](const auto& slot) {
                     return slot.has_value() && slot->evidence.reservation_id == reservation_id;
                   });
  if (found == implementation_->reservation_slots.end() ||
      found->value().evidence.state != ReservationState::Held) {
    return create_invalid_reservation_result();
  }
  const auto& record = found->value();
  std::array<std::optional<ReservationLedgerStorage::ScopeCandidate>, 7U> released;

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 1: calculate all exact inverse deltas without changing a cell.
  for (std::size_t index = 0U; index < record.scopes.size(); ++index) {
    const auto& indices = record.scopes[index];
    const auto& count = implementation_->count_cells[indices.count];
    const auto& quantity = implementation_->quantity_cells[indices.quantity];
    const auto& notional = implementation_->notional_cells[indices.notional];
    const auto& directional =
        implementation_->directional_notional_cells[indices.directional_notional];
    if (count.open_order_count == 0U) {
      return create_invalid_reservation_result();
    }
    auto gross = notional.gross_reserved.checked_subtract(record.evidence.exposure.quote_notional);
    auto buy_quantity =
        record.evidence.side == execution::OrderSide::Buy
            ? quantity.reserved_buy.checked_subtract(record.evidence.exposure.quantity)
            : model::Result<model::Quantity>::create_success(quantity.reserved_buy);
    auto sell_quantity =
        record.evidence.side == execution::OrderSide::Sell
            ? quantity.reserved_sell.checked_subtract(record.evidence.exposure.quantity)
            : model::Result<model::Quantity>::create_success(quantity.reserved_sell);
    auto buy_notional =
        record.evidence.side == execution::OrderSide::Buy
            ? directional.reserved_buy.checked_subtract(record.evidence.exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_buy);
    auto sell_notional =
        record.evidence.side == execution::OrderSide::Sell
            ? directional.reserved_sell.checked_subtract(record.evidence.exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_sell);
    if (!gross || !buy_quantity || !sell_quantity || !buy_notional || !sell_notional ||
        gross.value().coefficient() < 0 || buy_quantity.value().coefficient() < 0 ||
        sell_quantity.value().coefficient() < 0 || buy_notional.value().coefficient() < 0 ||
        sell_notional.value().coefficient() < 0) {
      return create_invalid_reservation_result();
    }
    const auto instrument_worst = greater(buy_notional.value(), sell_notional.value());
    auto aggregate_without_old =
        notional.aggregate_worst.checked_subtract(directional.instrument_worst);
    if (!aggregate_without_old) {
      return create_invalid_reservation_result();
    }
    auto aggregate = aggregate_without_old.value().checked_add(instrument_worst);
    if (!aggregate || aggregate.value().coefficient() < 0) {
      return create_invalid_reservation_result();
    }
    released[index].emplace(ReservationLedgerStorage::ScopeCandidate{
        indices, count.open_order_count - 1U, gross.value(), buy_quantity.value(),
        sell_quantity.value(), greater(buy_quantity.value(), sell_quantity.value()),
        buy_notional.value(), sell_notional.value(), instrument_worst, aggregate.value()});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Phase 2: commit every inverse candidate before publishing Released and decrementing held count.
  for (const auto& optional_candidate : released) {
    const auto& candidate = optional_candidate.value();
    implementation_->count_cells[candidate.indices.count].open_order_count =
        candidate.open_order_count;
    auto& quantity = implementation_->quantity_cells[candidate.indices.quantity];
    quantity.reserved_buy = candidate.reserved_buy_quantity;
    quantity.reserved_sell = candidate.reserved_sell_quantity;
    auto& notional = implementation_->notional_cells[candidate.indices.notional];
    notional.gross_reserved = candidate.gross_reserved;
    notional.aggregate_worst = candidate.aggregate_worst_notional;
    auto& directional =
        implementation_->directional_notional_cells[candidate.indices.directional_notional];
    directional.reserved_buy = candidate.reserved_buy_notional;
    directional.reserved_sell = candidate.reserved_sell_notional;
    directional.instrument_worst = candidate.instrument_worst_notional;
  }
  found->value().evidence.state = ReservationState::Released;
  --implementation_->held_count;
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Borrow the exact immutable policy owned by this ledger.
const RiskPolicySnapshot& ReservationLedger::policy() const noexcept {
  return implementation_->policy;
}

// --------------------------------------------------------
// Return the fixed startup reservation-slot capacity.
std::uint32_t ReservationLedger::capacity() const noexcept { return implementation_->capacity; }

// --------------------------------------------------------
// Return only currently Held records; Released history does not consume capacity.
std::uint32_t ReservationLedger::held_reservation_count() const noexcept {
  return implementation_->held_count;
}

// --------------------------------------------------------
// Borrow exact slot evidence while that identity has not been replaced by deterministic reuse.
const ReservationEvidence*
ReservationLedger::find_reservation(model::ReservationId reservation_id) const noexcept {
  const auto found =
      std::find_if(implementation_->reservation_slots.begin(),
                   implementation_->reservation_slots.end(), [reservation_id](const auto& slot) {
                     return slot.has_value() && slot->evidence.reservation_id == reservation_id;
                   });
  return found == implementation_->reservation_slots.end() ? nullptr : &found->value().evidence;
}

// --------------------------------------------------------
// Borrow current evidence by the preallocated slot's stable zero-based position.
const ReservationEvidence*
ReservationLedger::reservation_at(std::size_t stable_slot_index) const noexcept {
  if (stable_slot_index >= implementation_->reservation_slots.size()) {
    return nullptr;
  }
  const auto& slot = implementation_->reservation_slots[stable_slot_index];
  return slot ? &slot->evidence : nullptr;
}

// --------------------------------------------------------
// Match canonical scope-evidence cardinality to the accepted sorted risk-policy rows.
std::size_t ReservationLedger::scope_evidence_count() const noexcept {
  return implementation_->policy.limit_sets().size();
}

// --------------------------------------------------------
// Own one policy key and recomposed exposure without publishing any mutable-cell alias.
std::optional<RiskScopeExposureEvidence>
ReservationLedger::scope_evidence_at(std::size_t canonical_index) const {
  if (canonical_index >= implementation_->policy.limit_sets().size()) {
    return std::nullopt;
  }
  const auto& row = implementation_->policy.limit_sets()[canonical_index];
  auto exposure = calculate_scope_exposure(row.firm_id(), row.scope(), row.scope_subject(),
                                           row.instrument_id(), row.quote_currency());
  if (!exposure) {
    return std::nullopt;
  }
  return RiskScopeExposureEvidence{row.firm_id(),
                                   row.scope(),
                                   std::string{row.scope_subject()},
                                   row.instrument_id(),
                                   std::string{row.quote_currency()},
                                   std::move(*exposure)};
}

// --------------------------------------------------------
// Recompose one coherent evidence view from the same shared cells used by admission decisions.
std::optional<RiskScopeExposure> ReservationLedger::calculate_scope_exposure(
    const model::FirmId& firm_id, RiskScopeKind scope, std::string_view subject,
    const model::InstrumentId& instrument_id, std::string_view quote_currency) const noexcept {
  const auto firm = firm_id.value();
  const auto instrument = instrument_id.value();
  const auto count = ReservationLedgerStorage::find_cell(implementation_->count_cells,
                                                         std::tuple{firm, scope, subject});
  const auto quantity = ReservationLedgerStorage::find_cell(
      implementation_->quantity_cells, std::tuple{firm, scope, subject, instrument});
  const auto notional = ReservationLedgerStorage::find_cell(
      implementation_->notional_cells, std::tuple{firm, scope, subject, quote_currency});
  const auto directional = ReservationLedgerStorage::find_cell(
      implementation_->directional_notional_cells,
      std::tuple{firm, scope, subject, instrument, quote_currency});
  if (!count || !quantity || !notional || !directional) {
    return std::nullopt;
  }
  const auto& count_cell = implementation_->count_cells[*count];
  const auto& quantity_cell = implementation_->quantity_cells[*quantity];
  const auto& notional_cell = implementation_->notional_cells[*notional];
  const auto& directional_cell = implementation_->directional_notional_cells[*directional];
  return RiskScopeExposure{count_cell.open_order_count,
                           notional_cell.gross_reserved,
                           quantity_cell.reserved_buy,
                           quantity_cell.reserved_sell,
                           greater(quantity_cell.reserved_buy, quantity_cell.reserved_sell),
                           directional_cell.reserved_buy,
                           directional_cell.reserved_sell,
                           directional_cell.instrument_worst,
                           notional_cell.aggregate_worst};
}

// --------------------------------------------------------

} // namespace aegis::risk
