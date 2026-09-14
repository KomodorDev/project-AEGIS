// Purpose: implement fixed reservation and sole confirmed-inventory ownership, canonical risk
// precedence, and checked atomic reservation/inventory replacements on one serialized owner.

#include "aegis/risk/reservation_ledger.hpp"

#include "aegis/model/domain_error.hpp"
#include "aegis/risk/reservation_conversion.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "inventory_ledger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
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
// Return the greater exact value after callers have established nonnegative magnitudes.
template <typename Decimal> [[nodiscard]] Decimal greater(Decimal left, Decimal right) noexcept {
  return left > right ? left : right;
}

// --------------------------------------------------------
// Calculate the absolute value through checked subtraction, including the signed-minimum case.
template <typename Decimal>
[[nodiscard]] model::Result<Decimal> calculate_absolute_value(Decimal value) {
  return value.coefficient() < 0 ? zero_decimal<Decimal>().checked_subtract(value)
                                 : model::Result<Decimal>::create_success(value);
}

// --------------------------------------------------------
// Include signed confirmed inventory in both possible same-instrument reservation endpoints.
template <typename Decimal>
[[nodiscard]] model::Result<Decimal>
calculate_directional_worst_case(Decimal confirmed, Decimal reserved_buy, Decimal reserved_sell) {
  auto buy_endpoint = confirmed.checked_add(reserved_buy);
  auto sell_endpoint = confirmed.checked_subtract(reserved_sell);
  if (!buy_endpoint || !sell_endpoint) {
    return model::Result<Decimal>::create_failure(!buy_endpoint ? std::move(buy_endpoint).error()
                                                                : std::move(sell_endpoint).error());
  }
  auto buy_magnitude = calculate_absolute_value(buy_endpoint.value());
  auto sell_magnitude = calculate_absolute_value(sell_endpoint.value());
  if (!buy_magnitude || !sell_magnitude) {
    return model::Result<Decimal>::create_failure(
        !buy_magnitude ? std::move(buy_magnitude).error() : std::move(sell_magnitude).error());
  }
  return model::Result<Decimal>::create_success(
      greater(buy_magnitude.value(), sell_magnitude.value()));
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
  // Signed confirmed values are read from the sole inventory owner without reinterpreting the
  // positive original-order exposure contract.
  struct ConfirmedScopeExposure {
    model::Quantity quantity;
    model::Notional quote_notional;
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
  // Read the sole signed inventory authority for this scope, preserving M3's explicit zero source.
  [[nodiscard]] model::Result<ConfirmedScopeExposure>
  calculate_confirmed_scope_exposure(const ScopeIndices& indices) const {
    if (!inventory) {
      return model::Result<ConfirmedScopeExposure>::create_success(
          ConfirmedScopeExposure{zero_decimal<model::Quantity>(), zero_decimal<model::Notional>()});
    }
    const auto& key = *indices.limits;
    auto quantity = inventory->calculate_confirmed_quantity(
        key.firm_id(), key.scope(), key.scope_subject(), key.instrument_id());
    const auto* cell = inventory->find_aggregate(key.firm_id(), key.scope(), key.scope_subject(),
                                                 key.instrument_id(), key.quote_currency());
    if (!quantity) {
      return model::Result<ConfirmedScopeExposure>::create_failure(std::move(quantity).error());
    }
    if (cell == nullptr) {
      return model::Result<ConfirmedScopeExposure>::create_failure(DomainError::create_at_field(
          DomainErrorCode::InvalidInventoryState, "inventory.aggregate_key"));
    }
    return model::Result<ConfirmedScopeExposure>::create_success(
        ConfirmedScopeExposure{quantity.value(), cell->confirmed_quote_notional});
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
    auto confirmed = calculate_confirmed_scope_exposure(indices);
    if (!confirmed) {
      return model::Result<ScopeCandidate>::create_failure(std::move(confirmed).error());
    }
    auto worst_quantity =
        inventory ? calculate_directional_worst_case(confirmed.value().quantity,
                                                     buy_quantity.value(), sell_quantity.value())
                  : model::Result<model::Quantity>::create_success(
                        greater(buy_quantity.value(), sell_quantity.value()));
    auto instrument_worst_result =
        inventory ? calculate_directional_worst_case(confirmed.value().quote_notional,
                                                     buy_notional.value(), sell_notional.value())
                  : model::Result<model::Notional>::create_success(
                        greater(buy_notional.value(), sell_notional.value()));
    if (!worst_quantity || !instrument_worst_result) {
      return model::Result<ScopeCandidate>::create_failure(
          !worst_quantity ? std::move(worst_quantity).error()
                          : std::move(instrument_worst_result).error());
    }
    const auto instrument_worst = instrument_worst_result.value();
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
    return model::Result<ScopeCandidate>::create_success(
        ScopeCandidate{indices, count.open_order_count + 1U, gross.value(), buy_quantity.value(),
                       sell_quantity.value(), worst_quantity.value(), buy_notional.value(),
                       sell_notional.value(), instrument_worst, aggregate.value()});
  }

  RiskPolicySnapshot policy;
  std::uint32_t capacity;
  std::uint32_t held_count{0U};
  std::vector<CountCell> count_cells;
  std::vector<QuantityCell> quantity_cells;
  std::vector<NotionalCell> notional_cells;
  std::vector<DirectionalNotionalCell> directional_notional_cells;
  std::vector<std::optional<ReservationSlot>> reservation_slots;
  std::unique_ptr<InventoryLedger> inventory;
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
ReservationLedger::ReservationLedger(ReservationLedger&& other) noexcept
    : implementation_{std::move(other.implementation_)} {
  if (implementation_ && implementation_->inventory) {
    implementation_->inventory->reservations_ = this;
    implementation_->inventory->invalidate_plans();
  }
}

// --------------------------------------------------------
// Move assignment transfers the sole mutable owner-local ledger capability.
ReservationLedger& ReservationLedger::operator=(ReservationLedger&& other) noexcept {
  if (this != &other) {
    implementation_ = std::move(other.implementation_);
    if (implementation_ && implementation_->inventory) {
      implementation_->inventory->reservations_ = this;
      implementation_->inventory->invalidate_plans();
    }
  }
  return *this;
}

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
  // Phase 1: reject exhausted plan generations before calculating any writable candidate.
  if (implementation_->inventory &&
      implementation_->inventory->generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return create_rejected_risk_check_result(execution::SubmissionReason::SubmissionRuntimeFaulted);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Calculate conservative quote face notional exactly once for every scope.
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
        (!slot->has_value() ||
         ((slot->value().evidence.state == ReservationState::Released ||
           slot->value().evidence.state == ReservationState::ConsumedByFill) &&
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
      ReservationEvidence{
          reservation_id, ReservationState::Held, economics.side, exposure, exposure,
          OrderExposure{zero_decimal<model::Quantity>(), zero_decimal<model::Notional>()},
          ReservationClosureCause::Unassigned},
      scope_indices});
  ++implementation_->held_count;
  if (implementation_->inventory) {
    implementation_->inventory->invalidate_plans();
  }
  return RiskCheckResult{execution::SubmissionReason::None, reservation_id, exposure, std::nullopt};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate identity/state, compute all inverse deltas in scratch, then transition exactly once.
model::Result<void> ReservationLedger::release_reservation(model::ReservationId reservation_id) {
  if (implementation_->inventory &&
      implementation_->inventory->generation_ == std::numeric_limits<std::uint64_t>::max()) {
    return create_invalid_reservation_result();
  }
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
    auto gross =
        notional.gross_reserved.checked_subtract(record.evidence.remaining_exposure.quote_notional);
    auto buy_quantity =
        record.evidence.side == execution::OrderSide::Buy
            ? quantity.reserved_buy.checked_subtract(record.evidence.remaining_exposure.quantity)
            : model::Result<model::Quantity>::create_success(quantity.reserved_buy);
    auto sell_quantity =
        record.evidence.side == execution::OrderSide::Sell
            ? quantity.reserved_sell.checked_subtract(record.evidence.remaining_exposure.quantity)
            : model::Result<model::Quantity>::create_success(quantity.reserved_sell);
    auto buy_notional =
        record.evidence.side == execution::OrderSide::Buy
            ? directional.reserved_buy.checked_subtract(
                  record.evidence.remaining_exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_buy);
    auto sell_notional =
        record.evidence.side == execution::OrderSide::Sell
            ? directional.reserved_sell.checked_subtract(
                  record.evidence.remaining_exposure.quote_notional)
            : model::Result<model::Notional>::create_success(directional.reserved_sell);
    if (!gross || !buy_quantity || !sell_quantity || !buy_notional || !sell_notional ||
        gross.value().coefficient() < 0 || buy_quantity.value().coefficient() < 0 ||
        sell_quantity.value().coefficient() < 0 || buy_notional.value().coefficient() < 0 ||
        sell_notional.value().coefficient() < 0) {
      return create_invalid_reservation_result();
    }
    auto confirmed = implementation_->calculate_confirmed_scope_exposure(indices);
    if (!confirmed) {
      return model::Result<void>::create_failure(std::move(confirmed).error());
    }
    auto worst_quantity =
        implementation_->inventory
            ? calculate_directional_worst_case(confirmed.value().quantity, buy_quantity.value(),
                                               sell_quantity.value())
            : model::Result<model::Quantity>::create_success(
                  greater(buy_quantity.value(), sell_quantity.value()));
    auto instrument_worst_result =
        implementation_->inventory
            ? calculate_directional_worst_case(confirmed.value().quote_notional,
                                               buy_notional.value(), sell_notional.value())
            : model::Result<model::Notional>::create_success(
                  greater(buy_notional.value(), sell_notional.value()));
    if (!worst_quantity || !instrument_worst_result) {
      return model::Result<void>::create_failure(!worst_quantity
                                                     ? std::move(worst_quantity).error()
                                                     : std::move(instrument_worst_result).error());
    }
    const auto instrument_worst = instrument_worst_result.value();
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
        sell_quantity.value(), worst_quantity.value(), buy_notional.value(), sell_notional.value(),
        instrument_worst, aggregate.value()});
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
  found->value().evidence.remaining_exposure =
      OrderExposure{zero_decimal<model::Quantity>(), zero_decimal<model::Notional>()};
  found->value().evidence.closure_cause = ReservationClosureCause::DefiniteLocalFailure;
  --implementation_->held_count;
  if (implementation_->inventory) {
    implementation_->inventory->invalidate_plans();
  }
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
// Return only currently Held records; both terminal closure states permit later slot reuse.
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
  auto confirmed_quantity = zero_decimal<model::Quantity>();
  auto confirmed_notional = zero_decimal<model::Notional>();
  if (implementation_->inventory) {
    const auto* confirmed = implementation_->inventory->find_aggregate(
        firm_id, scope, subject, instrument_id, quote_currency);
    auto combined = implementation_->inventory->calculate_confirmed_quantity(
        firm_id, scope, subject, instrument_id);
    if (confirmed == nullptr || !combined) {
      return std::nullopt;
    }
    confirmed_quantity = combined.value();
    confirmed_notional = confirmed->confirmed_quote_notional;
  }
  auto worst_quantity = calculate_directional_worst_case(
      confirmed_quantity, quantity_cell.reserved_buy, quantity_cell.reserved_sell);
  if (!worst_quantity) {
    return std::nullopt;
  }
  return RiskScopeExposure{count_cell.open_order_count,
                           notional_cell.gross_reserved,
                           quantity_cell.reserved_buy,
                           quantity_cell.reserved_sell,
                           worst_quantity.value(),
                           directional_cell.reserved_buy,
                           directional_cell.reserved_sell,
                           directional_cell.instrument_worst,
                           notional_cell.aggregate_worst,
                           confirmed_quantity,
                           confirmed_notional};
}

// --------------------------------------------------------
// Attach only fully validated and allocated inventory; failed installation leaves reservations
// pristine and retains no borrowed authority or partial allocation in the enclosing ledger.
model::Result<void> InventoryLedger::install_on_pristine_reservations(
    ReservationLedger& reservations, const oms::OutboundOms& orders,
    const execution::OwnerLocalRouteCatalog& routes, const runtime::M4Policy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject reused owners before retaining any pointer or allocating inventory storage.
  const auto create_installation_failure = [] {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidInventoryState, "inventory.installation"));
  };
  if (!reservations.implementation_ || reservations.implementation_->inventory ||
      !orders.storage_incarnation_ || orders.order_count() != 0U ||
      reservations.held_reservation_count() != 0U) {
    return create_installation_failure();
  }
  for (const auto& slot : reservations.implementation_->reservation_slots) {
    if (slot.has_value()) {
      return create_installation_failure();
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate policy capacities and the complete sealed authority graph before allocation.
  const auto& risk_policy = reservations.policy();
  const auto& root = policy.root_provenance();
  if (policy.capacities().max_inventory_source_rows < reservations.capacity()) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.capacities.max_inventory_source_rows"));
  }
  if (policy.capacities().max_inventory_aggregate_cells < risk_policy.limit_sets().size()) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.capacities.max_inventory_aggregate_cells"));
  }
  if (risk_policy.fingerprint().bytes() != root.risk_policy_fingerprint() ||
      risk_policy.revision() != root.risk_policy_revision() ||
      risk_policy.configuration_fingerprint().bytes() != root.configuration_fingerprint() ||
      risk_policy.organization_revision() != root.organization_revision()) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidM4Policy, "m4_policy.inventory_provenance"));
  }
  for (const auto& route : routes.routes()) {
    if (route.configuration_fingerprint().bytes() != root.configuration_fingerprint() ||
        route.configuration_revision() != risk_policy.configuration_revision() ||
        route.organization_revision() != root.organization_revision() ||
        route.route_revision() != risk_policy.route_revision()) {
      return model::Result<void>::create_failure(DomainError::create_at_field(
          DomainErrorCode::InvalidM4Policy, "m4_policy.inventory_route_provenance"));
    }
    if (!route.route().is_enabled()) {
      continue;
    }
    for (std::uint8_t value = 1U; value <= 7U; ++value) {
      if (!reservations.implementation_->resolve_scope(route, static_cast<RiskScopeKind>(value))) {
        return create_installation_failure();
      }
    }
  }
  if (routes.routes().empty()) {
    return create_installation_failure();
  }
  try {
    auto prepared =
        std::unique_ptr<InventoryLedger>{new InventoryLedger{reservations, orders, routes, policy}};
    reservations.implementation_->inventory = std::move(prepared);
    return model::Result<void>::create_success();
  } catch (const std::bad_alloc&) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InventoryCapacityExceeded, "inventory.allocation"));
  } catch (const std::length_error&) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InventoryCapacityExceeded, "inventory.allocation"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Borrow the closed component only while its enclosing ledger retains the installed storage.
InventoryLedger* InventoryLedger::installed_inventory(ReservationLedger& reservations) noexcept {
  return reservations.implementation_ ? reservations.implementation_->inventory.get() : nullptr;
}

// --------------------------------------------------------
// Preserve const authority for runtime evidence and other owner-local observations.
const InventoryLedger*
InventoryLedger::installed_inventory(const ReservationLedger& reservations) noexcept {
  return reservations.implementation_ ? reservations.implementation_->inventory.get() : nullptr;
}

// --------------------------------------------------------
// Bind economic planning to the exact live OMS row, immutable installed route, policy provenance,
// and still-held reservation. Caller-authored admission copies cannot satisfy pointer ownership.
model::Result<std::size_t>
InventoryLedger::find_owned_held_reservation(const oms::OutboundOrderRecord& order) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Require this exact live component and genuine retained OMS row before consulting provenance.
  const auto create_owner_or_state_failure = [] {
    return model::Result<std::size_t>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidReservationConversion, "reservation_conversion.owner_or_state"));
  };
  if (!reservations_->implementation_ || reservations_->implementation_->inventory.get() != this ||
      generation_ == std::numeric_limits<std::uint64_t>::max() || !orders_incarnation_ ||
      orders_->storage_incarnation_ != orders_incarnation_ ||
      orders_->find_order(order.order_id()) != &order) {
    return create_owner_or_state_failure();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject any mismatch between the immutable admission and the installed attribution/economics.
  const auto& admission = order.admission();
  const auto& provenance = admission.provenance;
  const auto* route = routes_->find_route(provenance.route_id);
  if (route == nullptr || provenance.venue_id != route->route().venue_id ||
      provenance.logical_account_id != route->route().logical_account_id ||
      provenance.instrument_id != route->metadata().instrument_id() ||
      provenance.venue_instrument_id != route->metadata().venue_instrument_id() ||
      provenance.firm_id != route->attribution().firm_id ||
      provenance.desk_id != route->attribution().desk_id ||
      provenance.bot_id != route->attribution().bot_id ||
      provenance.strategy_id != route->attribution().strategy_id ||
      provenance.configuration_fingerprint != root_.configuration_fingerprint() ||
      provenance.configuration_revision != route->configuration_revision() ||
      provenance.organization_revision != root_.organization_revision() ||
      provenance.route_revision != route->route_revision() ||
      provenance.metadata_revision != route->metadata().revision() ||
      provenance.runtime_policy_fingerprint != root_.runtime_policy_fingerprint() ||
      provenance.risk_policy_fingerprint != root_.risk_policy_fingerprint() ||
      provenance.risk_policy_revision != root_.risk_policy_revision() ||
      provenance.submission_policy_fingerprint != root_.submission_policy_fingerprint()) {
    return create_owner_or_state_failure();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the exact still-held reservation and verify every stored scope binding.
  const auto& slots = reservations_->implementation_->reservation_slots;
  for (std::size_t index = 0U; index < slots.size(); ++index) {
    if (slots[index] && slots[index]->evidence.reservation_id == admission.reservation_id) {
      const auto& evidence = slots[index]->evidence;
      if (evidence.state != ReservationState::Held ||
          evidence.closure_cause != ReservationClosureCause::Unassigned ||
          evidence.side != admission.economics.side || evidence.exposure != admission.exposure ||
          evidence.exposure.quantity != admission.economics.quantity ||
          evidence.reservation_id.value() != admission.attempt_id.value() ||
          evidence.remaining_exposure.quantity.coefficient() <= 0) {
        return create_owner_or_state_failure();
      }
      for (std::size_t scope_index = 0U; scope_index < 7U; ++scope_index) {
        const auto actual = reservations_->implementation_->resolve_scope(
            *route, static_cast<RiskScopeKind>(scope_index + 1U));
        const auto& retained = slots[index]->scopes[scope_index];
        if (!actual || actual->count != retained.count || actual->quantity != retained.quantity ||
            actual->notional != retained.notional ||
            actual->directional_notional != retained.directional_notional) {
          return create_owner_or_state_failure();
        }
      }
      return model::Result<std::size_t>::create_success(index);
    }
  }
  return create_owner_or_state_failure();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Calculate all seven replacement groups, including shared quantity and currency totals, in fixed
// scratch. Every subtraction, sign endpoint, absolute value, and aggregate replacement is checked.
model::Result<std::array<ReservationInventoryScopeReplacement, 7U>>
InventoryLedger::calculate_scope_replacements(std::size_t reservation_index,
                                              const ReservationEvidence& after,
                                              model::Quantity signed_quantity_delta,
                                              model::Notional signed_notional_delta) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Build complete replacement groups without reserving a slot or changing any shared cell.
  using Replacements = std::array<ReservationInventoryScopeReplacement, 7U>;
  const auto& storage = *reservations_->implementation_;
  const auto& slot = storage.reservation_slots[reservation_index].value();
  std::array<std::optional<ReservationInventoryScopeReplacement>, 7U> scratch;
  for (std::size_t index = 0U; index < 7U; ++index) {
    const auto& indices = slot.scopes[index];
    const auto& key = *indices.limits;
    const auto* existing = find_aggregate(key.firm_id(), key.scope(), key.scope_subject(),
                                          key.instrument_id(), key.quote_currency());
    if (existing == nullptr || storage.count_cells[indices.count].open_order_count == 0U ||
        storage.held_count == 0U) {
      return model::Result<Replacements>::create_failure(DomainError::create_at_field(
          DomainErrorCode::InvalidInventoryState, "inventory.scope_state"));
    }
    std::size_t inventory_index = 0U;
    while (inventory_index < aggregate_count_ &&
           &aggregates_[inventory_index].value() != existing) {
      ++inventory_index;
    }
    // Derive residual removals and signed confirmed additions from the same reservation snapshot.
    const auto& quantity = storage.quantity_cells[indices.quantity];
    const auto& directional = storage.directional_notional_cells[indices.directional_notional];
    const auto& notional = storage.notional_cells[indices.notional];
    const auto removed_quantity = slot.evidence.remaining_exposure.quantity.checked_subtract(
        after.remaining_exposure.quantity);
    const auto removed_notional = slot.evidence.remaining_exposure.quote_notional.checked_subtract(
        after.remaining_exposure.quote_notional);
    if (!removed_quantity || !removed_notional) {
      return model::Result<Replacements>::create_failure(
          !removed_quantity ? removed_quantity.error() : removed_notional.error());
    }
    auto gross = notional.gross_reserved.checked_subtract(removed_notional.value());
    auto buy_quantity = slot.evidence.side == execution::OrderSide::Buy
                            ? quantity.reserved_buy.checked_subtract(removed_quantity.value())
                            : model::Result<model::Quantity>::create_success(quantity.reserved_buy);
    auto sell_quantity =
        slot.evidence.side == execution::OrderSide::Sell
            ? quantity.reserved_sell.checked_subtract(removed_quantity.value())
            : model::Result<model::Quantity>::create_success(quantity.reserved_sell);
    auto buy_notional =
        slot.evidence.side == execution::OrderSide::Buy
            ? directional.reserved_buy.checked_subtract(removed_notional.value())
            : model::Result<model::Notional>::create_success(directional.reserved_buy);
    auto sell_notional =
        slot.evidence.side == execution::OrderSide::Sell
            ? directional.reserved_sell.checked_subtract(removed_notional.value())
            : model::Result<model::Notional>::create_success(directional.reserved_sell);
    auto confirmed_quantity = existing->confirmed_quantity.checked_add(signed_quantity_delta);
    auto confirmed_notional = existing->confirmed_quote_notional.checked_add(signed_notional_delta);
    if (!gross || !buy_quantity || !sell_quantity || !buy_notional || !sell_notional ||
        !confirmed_quantity || !confirmed_notional) {
      const auto& error = !gross                ? gross.error()
                          : !buy_quantity       ? buy_quantity.error()
                          : !sell_quantity      ? sell_quantity.error()
                          : !buy_notional       ? buy_notional.error()
                          : !sell_notional      ? sell_notional.error()
                          : !confirmed_quantity ? confirmed_quantity.error()
                                                : confirmed_notional.error();
      return model::Result<Replacements>::create_failure(error);
    }
    if (removed_quantity.value().coefficient() < 0 || removed_notional.value().coefficient() < 0 ||
        gross.value().coefficient() < 0 || buy_quantity.value().coefficient() < 0 ||
        sell_quantity.value().coefficient() < 0 || buy_notional.value().coefficient() < 0 ||
        sell_notional.value().coefficient() < 0) {
      return model::Result<Replacements>::create_failure(DomainError::create_at_field(
          DomainErrorCode::InvalidInventoryState, "inventory.residual_underflow"));
    }
    // Re-run the exact canonical quantity fold with the candidate substituted, so a successful
    // commit cannot produce a state whose subsequent read overflows at an intermediate prefix.
    auto next_combined = calculate_confirmed_quantity(
        key.firm_id(), key.scope(), key.scope_subject(), key.instrument_id(),
        std::pair{inventory_index, confirmed_quantity.value()});
    if (!next_combined) {
      return model::Result<Replacements>::create_failure(std::move(next_combined).error());
    }
    auto worst_quantity = calculate_directional_worst_case(
        next_combined.value(), buy_quantity.value(), sell_quantity.value());
    auto instrument_worst = calculate_directional_worst_case(
        confirmed_notional.value(), buy_notional.value(), sell_notional.value());
    if (!worst_quantity || !instrument_worst) {
      return model::Result<Replacements>::create_failure(!worst_quantity
                                                             ? std::move(worst_quantity).error()
                                                             : std::move(instrument_worst).error());
    }
    auto without_old = notional.aggregate_worst.checked_subtract(directional.instrument_worst);
    if (!without_old) {
      return model::Result<Replacements>::create_failure(std::move(without_old).error());
    }
    auto aggregate = without_old.value().checked_add(instrument_worst.value());
    if (!aggregate) {
      return model::Result<Replacements>::create_failure(std::move(aggregate).error());
    }
    const auto next_count = storage.count_cells[indices.count].open_order_count -
                            (after.state == ReservationState::Held ? 0U : 1U);
    scratch[index].emplace(ReservationInventoryScopeReplacement{
        indices.count, indices.quantity, indices.notional, indices.directional_notional,
        inventory_index,
        RiskScopeExposure{next_count, gross.value(), buy_quantity.value(), sell_quantity.value(),
                          worst_quantity.value(), buy_notional.value(), sell_notional.value(),
                          instrument_worst.value(), aggregate.value(), confirmed_quantity.value(),
                          confirmed_notional.value()}});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the complete fixed scratch only after all seven independent preflights succeed.
  return model::Result<Replacements>::create_success(
      Replacements{*scratch[0U], *scratch[1U], *scratch[2U], *scratch[3U], *scratch[4U],
                   *scratch[5U], *scratch[6U]});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate source chronology and component consistency before deriving immutable fixed scratch.
// No identity deduplication, OMS transition, audit reservation, or private-consumption claim
// occurs.
model::Result<ReservationInventoryPlan>
InventoryLedger::plan_cumulative_fill(const oms::OutboundOrderRecord& order,
                                      const oms::NormalizedPrivateOrderInput& execution_input,
                                      recovery::AuditOrdinal audit_ordinal) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the retained owner and complete incoming execution before arithmetic or capacity work.
  auto reservation_index = find_owned_held_reservation(order);
  if (!reservation_index) {
    return model::Result<ReservationInventoryPlan>::create_failure(
        std::move(reservation_index).error());
  }
  const auto create_execution_failure = [] {
    return model::Result<ReservationInventoryPlan>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidReservationConversion, "reservation_conversion.execution"));
  };
  const auto* execution = std::get_if<oms::ExecutionPayload>(&execution_input.payload());
  const auto& admission = order.admission();
  const auto& provenance = admission.provenance;
  const auto* route = routes_->find_route(provenance.route_id);
  const auto projection = order.private_projection();
  if (execution == nullptr || execution_input.origin() == oms::PrivateEventOrigin::Local ||
      execution_input.provenance().root() != root_ ||
      execution_input.logical_account_id() != provenance.logical_account_id ||
      execution_input.venue_id() != provenance.venue_id ||
      execution->instrument_id != provenance.instrument_id ||
      execution->metadata_revision != provenance.metadata_revision ||
      (execution->source_side && *execution->source_side != admission.economics.side) ||
      (execution->locator.local_order_id() &&
       *execution->locator.local_order_id() != admission.order_id) ||
      (!execution->locator.local_order_id() &&
       (!projection.exchange_order_id || !execution->locator.exchange_order_id() ||
        *projection.exchange_order_id != *execution->locator.exchange_order_id())) ||
      (projection.exchange_order_id && execution->locator.exchange_order_id() &&
       *projection.exchange_order_id != *execution->locator.exchange_order_id()) ||
      (order.state() != oms::OutboundOrderState::WriteInitiated &&
       order.state() != oms::OutboundOrderState::SubmissionUnknown &&
       order.state() != oms::OutboundOrderState::Working &&
       order.state() != oms::OutboundOrderState::PartiallyFilled) ||
      execution->incremental_quantity.coefficient() <= 0 ||
      execution->execution_price.coefficient() <= 0 ||
      execution->execution_price.scale() > route->metadata().price_scale() ||
      (admission.economics.side == execution::OrderSide::Buy &&
       execution->execution_price > admission.economics.price) ||
      (admission.economics.side == execution::OrderSide::Sell &&
       execution->execution_price < admission.economics.price)) {
    return create_execution_failure();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish exact chronology, price alignment, and partition-independent positive economics.
  auto price_alignment = route->metadata().validate_price_alignment(execution->execution_price);
  if (!price_alignment) {
    return model::Result<ReservationInventoryPlan>::create_failure(
        std::move(price_alignment).error());
  }
  const auto& before =
      reservations_->implementation_->reservation_slots[reservation_index.value()]->evidence;
  auto expected_delta = execution->cumulative_quantity.checked_subtract(
      before.cumulative_confirmed_exposure.quantity);
  if (!expected_delta) {
    return model::Result<ReservationInventoryPlan>::create_failure(
        std::move(expected_delta).error());
  }
  if (expected_delta.value() != execution->incremental_quantity) {
    return create_execution_failure();
  }
  auto conversion = calculate_cumulative_reservation_conversion(
      before.exposure, before.cumulative_confirmed_exposure, execution->cumulative_quantity,
      route->metadata(), reservations_->policy().notional_scale());
  if (!conversion) {
    return model::Result<ReservationInventoryPlan>::create_failure(std::move(conversion).error());
  }
  const auto& converted = conversion.value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Apply the order side only after the positive cumulative allocation has been checked.
  // Convert a positive quantity to the exact signed inventory contribution.
  const auto sign_quantity = [side = before.side](model::Quantity value) {
    return side == execution::OrderSide::Sell
               ? zero_decimal<model::Quantity>().checked_subtract(value)
               : model::Result<model::Quantity>::create_success(value);
  };
  // Convert a positive face allocation to the exact signed quote contribution.
  const auto sign_notional = [side = before.side](model::Notional value) {
    return side == execution::OrderSide::Sell
               ? zero_decimal<model::Notional>().checked_subtract(value)
               : model::Result<model::Notional>::create_success(value);
  };
  auto signed_quantity_delta = sign_quantity(converted.newly_confirmed.quantity);
  auto signed_notional_delta = sign_notional(converted.newly_confirmed.quote_notional);
  auto signed_quantity = sign_quantity(converted.cumulative_confirmed.quantity);
  auto signed_notional = sign_notional(converted.cumulative_confirmed.quote_notional);
  auto previous_quantity = sign_quantity(before.cumulative_confirmed_exposure.quantity);
  auto previous_notional = sign_notional(before.cumulative_confirmed_exposure.quote_notional);
  if (!signed_quantity_delta || !signed_notional_delta || !signed_quantity || !signed_notional ||
      !previous_quantity || !previous_notional) {
    const auto& error = !signed_quantity_delta   ? signed_quantity_delta.error()
                        : !signed_notional_delta ? signed_notional_delta.error()
                        : !signed_quantity       ? signed_quantity.error()
                        : !signed_notional       ? signed_notional.error()
                        : !previous_quantity     ? previous_quantity.error()
                                                 : previous_notional.error();
    return model::Result<ReservationInventoryPlan>::create_failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve cumulative source consistency and preflight append-only source capacity.
  std::size_t source_index = 0U;
  while (source_index < source_count_ &&
         sources_[source_index]->admission.order_id != order.order_id()) {
    ++source_index;
  }
  if (source_index == source_count_) {
    if (before.cumulative_confirmed_exposure.quantity.coefficient() != 0 ||
        before.cumulative_confirmed_exposure.quote_notional.coefficient() != 0) {
      return create_execution_failure();
    }
    if (source_count_ >= sources_.size()) {
      return model::Result<ReservationInventoryPlan>::create_failure(DomainError::create_at_field(
          DomainErrorCode::InventoryCapacityExceeded, "inventory.source_rows"));
    }
  } else {
    const auto& source = sources_[source_index].value();
    if (source.admission != admission || source.confirmed_quantity != previous_quantity.value() ||
        source.confirmed_quote_notional != previous_notional.value() ||
        source.latest_audit_ordinal >= audit_ordinal) {
      return create_execution_failure();
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive every shared replacement before minting the immutable owner-bound plan.
  const bool complete = converted.remaining.quantity.coefficient() == 0;
  const ReservationEvidence after{
      before.reservation_id,
      complete ? ReservationState::ConsumedByFill : ReservationState::Held,
      before.side,
      before.exposure,
      converted.remaining,
      converted.cumulative_confirmed,
      complete ? ReservationClosureCause::FullFill : ReservationClosureCause::Unassigned};
  auto scopes =
      calculate_scope_replacements(reservation_index.value(), after, signed_quantity_delta.value(),
                                   signed_notional_delta.value());
  if (!scopes) {
    return model::Result<ReservationInventoryPlan>::create_failure(std::move(scopes).error());
  }
  return model::Result<ReservationInventoryPlan>::create_success(ReservationInventoryPlan{
      incarnation_, generation_, order, reservation_index.value(), before, after,
      std::move(scopes).value(), source_index,
      InventorySourceRecord{admission, signed_quantity.value(), signed_notional.value(),
                            execution_input, audit_ordinal}});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Prepare one non-fill closure without altering any retained confirmed source contribution.
model::Result<ReservationInventoryPlan>
InventoryLedger::plan_terminal_release(const oms::OutboundOrderRecord& order,
                                       ReservationClosureCause cause) const {
  auto reservation_index = find_owned_held_reservation(order);
  if (!reservation_index) {
    return model::Result<ReservationInventoryPlan>::create_failure(
        std::move(reservation_index).error());
  }
  if (cause < ReservationClosureCause::DefiniteLocalFailure ||
      cause > ReservationClosureCause::CompleteAuthoritativeNegative) {
    return model::Result<ReservationInventoryPlan>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidReservationConversion, "reservation_conversion.closure_cause"));
  }
  const auto& before =
      reservations_->implementation_->reservation_slots[reservation_index.value()]->evidence;
  const auto zero_quantity = zero_decimal<model::Quantity>();
  const auto zero_notional = zero_decimal<model::Notional>();
  const ReservationEvidence after{before.reservation_id,
                                  ReservationState::Released,
                                  before.side,
                                  before.exposure,
                                  OrderExposure{zero_quantity, zero_notional},
                                  before.cumulative_confirmed_exposure,
                                  cause};
  auto scopes =
      calculate_scope_replacements(reservation_index.value(), after, zero_quantity, zero_notional);
  if (!scopes) {
    return model::Result<ReservationInventoryPlan>::create_failure(std::move(scopes).error());
  }
  return model::Result<ReservationInventoryPlan>::create_success(
      ReservationInventoryPlan{incarnation_, generation_, order, reservation_index.value(), before,
                               after, std::move(scopes).value(), std::nullopt, std::nullopt});
}

// --------------------------------------------------------
// Validate one unchanged incarnation and complete source projection before the no-fail commit.
model::Result<void>
InventoryLedger::commit_reservation_inventory_plan(ReservationInventoryPlan&& plan) {
  if (!plan.incarnation_ || plan.incarnation_ != incarnation_ || plan.generation_ != generation_ ||
      generation_ == std::numeric_limits<std::uint64_t>::max() || !reservations_->implementation_ ||
      reservations_->implementation_->inventory.get() != this || !orders_incarnation_ ||
      orders_->storage_incarnation_ != orders_incarnation_ ||
      orders_->find_order(plan.order_admission_.order_id) != plan.order_ ||
      plan.order_->admission() != plan.order_admission_ ||
      plan.order_->private_projection() != plan.order_projection_) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidReservationConversion, "reservation_conversion.stale_plan"));
  }
  auto& storage = *reservations_->implementation_;
  if (plan.reservation_index_ >= storage.reservation_slots.size() ||
      !storage.reservation_slots[plan.reservation_index_] ||
      storage.reservation_slots[plan.reservation_index_]->evidence != plan.before_ ||
      storage.held_count == 0U) {
    return model::Result<void>::create_failure(DomainError::create_at_field(
        DomainErrorCode::InvalidReservationConversion, "reservation_conversion.stale_reservation"));
  }
  static_assert(std::is_nothrow_copy_constructible_v<InventorySourceRecord>);
  static_assert(std::is_nothrow_copy_assignable_v<InventorySourceRecord>);
  static_assert(std::is_nothrow_copy_assignable_v<ReservationEvidence>);

  // ++++++++++++++++++++++++++++++++++++++++
  // Every write below replaces a prevalidated scalar or fixed inline record; no observer may read
  // either ledger until the serialized owner completes this whole synchronous component action.
  for (const auto& scope : plan.scopes_) {
    const auto& next = scope.exposure;
    storage.count_cells[scope.count_index].open_order_count = next.open_order_count;
    auto& quantity = storage.quantity_cells[scope.quantity_index];
    quantity.reserved_buy = next.reserved_buy_quantity;
    quantity.reserved_sell = next.reserved_sell_quantity;
    auto& notional = storage.notional_cells[scope.notional_index];
    notional.gross_reserved = next.gross_reserved_quote_notional;
    notional.aggregate_worst = next.worst_case_position_quote_notional;
    auto& directional = storage.directional_notional_cells[scope.directional_index];
    directional.reserved_buy = next.reserved_buy_quote_notional;
    directional.reserved_sell = next.reserved_sell_quote_notional;
    directional.instrument_worst = next.instrument_worst_case_quote_notional;
    auto& confirmed = aggregates_[scope.inventory_index].value();
    confirmed.confirmed_quantity = next.confirmed_quantity;
    confirmed.confirmed_quote_notional = next.confirmed_quote_notional;
  }
  storage.reservation_slots[plan.reservation_index_]->evidence = plan.after_;
  if (plan.after_.state != ReservationState::Held) {
    --storage.held_count;
  }
  if (plan.source_after_) {
    const auto source_index = plan.source_index_.value();
    sources_[source_index] = plan.source_after_;
    if (source_index == source_count_) {
      ++source_count_;
    }
  }
  invalidate_plans();
  plan.incarnation_.reset();
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::risk
