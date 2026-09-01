// Purpose: apply absolute market-by-price changes to preallocated scratch storage and reject every
// incoherent candidate before it can replace current state.

#include "aegis/market_data/order_book.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace aegis::market_data {
namespace {

// --------------------------------------------------------
// Construct stable book errors without exposing container implementation details.
[[nodiscard]] model::Result<void> create_order_book_failure_result(model::DomainErrorCode code,
                                                                   std::string field) {
  return model::Result<void>::create_failure(
      model::DomainError::create_at_field(code, std::move(field)));
}

// --------------------------------------------------------
// Select the side-specific capacity field used by deterministic diagnostics and tests.
[[nodiscard]] std::string side_capacity_field(BookSide side) {
  return side == BookSide::Bid ? "market_book.bids" : "market_book.asks";
}

// --------------------------------------------------------
// Select the side-specific price field used by metadata and ordering failures.
[[nodiscard]] std::string side_price_field(BookSide side) {
  return side == BookSide::Bid ? "market_book.bids.price" : "market_book.asks.price";
}

// --------------------------------------------------------
// Select the side-specific quantity field used by positivity and alignment failures.
[[nodiscard]] std::string side_quantity_field(BookSide side) {
  return side == BookSide::Bid ? "market_book.bids.quantity" : "market_book.asks.quantity";
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Compare borrowed canonical level sequences rather than their owner addresses.
bool operator==(const ReadyBookView& lhs, const ReadyBookView& rhs) noexcept {
  return std::ranges::equal(lhs.bids_, rhs.bids_) && std::ranges::equal(lhs.asks_, rhs.asks_);
}

// --------------------------------------------------------
// Reserve both sides to the immutable compiled ceiling before the first owner turn.
FixedDepthOrderBook::FixedDepthOrderBook() {
  bids_.reserve(maximum_retained_book_depth);
  asks_.reserve(maximum_retained_book_depth);
}

// --------------------------------------------------------
// Copy current prefixes into already-reserved scratch storage without changing its capacity.
FixedDepthOrderBook& FixedDepthOrderBook::operator=(const FixedDepthOrderBook& other) {
  if (this != &other) {
    bids_.assign(other.bids_.begin(), other.bids_.end());
    asks_.assign(other.asks_.begin(), other.asks_.end());
  }
  return *this;
}

// --------------------------------------------------------
// Compare complete canonical prefixes while treating construction-time reserve as non-semantic.
bool operator==(const FixedDepthOrderBook& lhs, const FixedDepthOrderBook& rhs) noexcept {
  return lhs.bids_ == rhs.bids_ && lhs.asks_ == rhs.asks_;
}

// --------------------------------------------------------
// Reset logical sizes without releasing preallocated side storage.
void FixedDepthOrderBook::clear_levels() noexcept {
  bids_.clear();
  asks_.clear();
}

// --------------------------------------------------------
// Apply one absolute quantity while maintaining best-to-worst canonical order on the selected side.
model::Result<void> FixedDepthOrderBook::apply_level_change(BookSide side, model::Price price,
                                                            model::Quantity quantity,
                                                            std::size_t retained_depth) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Select the fixed side prefix and its strict ordering predicate.
  auto* levels = side == BookSide::Bid ? &bids_ : &asks_;
  const auto before = [side](const BookLevel& level, model::Price target) {
    return side == BookSide::Bid ? level.price > target : level.price < target;
  };
  const auto position =
      std::find_if(levels->begin(), levels->end(),
                   [price, before](const BookLevel& level) { return !before(level, price); });

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero is deletion intent. Deleting an absent price is a deterministic no-op.
  if (quantity.coefficient() == 0) {
    if (position != levels->end() && position->price == price) {
      levels->erase(position);
    }
    return model::Result<void>::create_success();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Replace an existing absolute quantity without changing price priority.
  if (position != levels->end() && position->price == price) {
    position->quantity = quantity;
    return model::Result<void>::create_success();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject before insertion when the policy depth would be exceeded; compiled capacity is already
  // guaranteed by policy validation but remains defended here.
  if (levels->size() >= retained_depth || levels->size() >= maximum_retained_book_depth) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookCapacityExceeded,
                                            side_capacity_field(side));
  }
  levels->insert(position, BookLevel{price, quantity});
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate the complete candidate only after all changes have been applied to scratch storage.
model::Result<void>
FixedDepthOrderBook::validate_book_state(const model::InstrumentMetadata& metadata,
                                         std::size_t retained_depth) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Defend both the authored policy depth and current side sizes before indexing best levels.
  if (retained_depth == 0U || retained_depth > maximum_retained_book_depth) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookCapacityExceeded,
                                            "market_book.retained_depth");
  }
  if (bids_.size() > retained_depth) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookCapacityExceeded,
                                            "market_book.bids");
  }
  if (asks_.size() > retained_depth) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookCapacityExceeded,
                                            "market_book.asks");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate exact positivity and metadata alignment for every retained level on both sides.
  const auto validate_side = [&metadata](const std::vector<BookLevel>& levels, BookSide side) {
    for (const auto& level : levels) {
      if (level.price.coefficient() <= 0) {
        return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                                side_price_field(side));
      }
      if (level.quantity.coefficient() <= 0) {
        return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                                side_quantity_field(side));
      }
      auto price_alignment = metadata.validate_price_alignment(level.price);
      if (!price_alignment) {
        return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                                side_price_field(side));
      }
      auto quantity_alignment = metadata.validate_quantity_alignment(level.quantity);
      if (!quantity_alignment) {
        return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                                side_quantity_field(side));
      }
    }
    return model::Result<void>::create_success();
  };
  auto bids_valid = validate_side(bids_, BookSide::Bid);
  if (!bids_valid) {
    return bids_valid;
  }
  auto asks_valid = validate_side(asks_, BookSide::Ask);
  if (!asks_valid) {
    return asks_valid;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical sequences must be strict, excluding duplicate prices even if an internal caller ever
  // bypasses the ordinary replace path.
  const auto bids_ordered = std::adjacent_find(
      bids_.begin(), bids_.end(),
      [](const BookLevel& lhs, const BookLevel& rhs) { return lhs.price <= rhs.price; });
  if (bids_ordered != bids_.end()) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                            "market_book.bids.order");
  }
  const auto asks_ordered = std::adjacent_find(
      asks_.begin(), asks_.end(),
      [](const BookLevel& lhs, const BookLevel& rhs) { return lhs.price >= rhs.price; });
  if (asks_ordered != asks_.end()) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                            "market_book.asks.order");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // One-sided candidates are coherent; a two-sided candidate must be strictly uncrossed and
  // unlocked at its independently available best prices.
  if (!bids_.empty() && !asks_.empty() && bids_.front().price >= asks_.front().price) {
    return create_order_book_failure_result(model::DomainErrorCode::MarketBookInvalid,
                                            "market_book.crossed_or_locked");
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Swap complete current/scratch prefixes without allocating or invalidating vector capacities.
void FixedDepthOrderBook::swap(FixedDepthOrderBook& other) noexcept {
  bids_.swap(other.bids_);
  asks_.swap(other.asks_);
}

// --------------------------------------------------------
// Expose only populated fixed-capacity prefixes as immutable contiguous ranges.
ReadyBookView FixedDepthOrderBook::ready_view() const noexcept {
  return ReadyBookView{bids_, asks_};
}

// --------------------------------------------------------

} // namespace aegis::market_data
