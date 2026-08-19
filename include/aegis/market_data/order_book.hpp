// Purpose: own fixed-capacity market-by-price storage and expose only immutable, turn-scoped Ready
// views to strategy dispatch.

#pragma once

#include "aegis/market_data/market_event.hpp"
#include "aegis/market_data/market_limits.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/instrument_metadata.hpp"
#include "aegis/model/result.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace aegis::market_data {

// ########################################################################
// The market state machine is the sole mutable owner of current and scratch books.
class MarketStateMachine;

// ########################################################################

// ########################################################################
// The fixed current/scratch representation may create views only for its owner state machine.
class FixedDepthOrderBook;

// ########################################################################

// ########################################################################
// One retained market-by-price level owns an exact positive price and quantity.
struct BookLevel {
  model::Price price;
  model::Quantity quantity;

  // --------------------------------------------------------
  // Structural equality supports deterministic book and callback-vector assertions.
  friend bool operator==(const BookLevel&, const BookLevel&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A Ready view privately borrows canonical storage for one synchronous turn while public access
// returns only counts and copied levels, so strategy code cannot retain a storage alias.
class ReadyBookView final {
public:

  // --------------------------------------------------------
  // A view may cross the private factory boundary once but cannot be copied into retained state.
  ReadyBookView(const ReadyBookView&) = delete;
  ReadyBookView& operator=(const ReadyBookView&) = delete;
  ReadyBookView(ReadyBookView&&) noexcept = default;
  ReadyBookView& operator=(ReadyBookView&&) noexcept = default;

  // --------------------------------------------------------
  // Return the number of retained bids ordered best to worst.
  [[nodiscard]] std::size_t bid_count() const noexcept { return bids_.size(); }

  // --------------------------------------------------------
  // Return the number of retained asks ordered best to worst.
  [[nodiscard]] std::size_t ask_count() const noexcept { return asks_.size(); }

  // --------------------------------------------------------
  // Copy one retained bid by canonical index without exposing owner storage.
  [[nodiscard]] std::optional<BookLevel> bid_at(std::size_t index) const noexcept {
    return index < bids_.size() ? std::optional<BookLevel>{bids_[index]} : std::nullopt;
  }

  // --------------------------------------------------------
  // Copy one retained ask by canonical index without exposing owner storage.
  [[nodiscard]] std::optional<BookLevel> ask_at(std::size_t index) const noexcept {
    return index < asks_.size() ? std::optional<BookLevel>{asks_[index]} : std::nullopt;
  }

  // --------------------------------------------------------
  // Return the best bid independently so coherent one-sided books remain representable.
  [[nodiscard]] std::optional<model::Price> best_bid() const noexcept {
    return bids_.empty() ? std::nullopt : std::optional<model::Price>{bids_.front().price};
  }

  // --------------------------------------------------------
  // Return the best ask independently so coherent one-sided books remain representable.
  [[nodiscard]] std::optional<model::Price> best_ask() const noexcept {
    return asks_.empty() ? std::nullopt : std::optional<model::Price>{asks_.front().price};
  }

  // --------------------------------------------------------
  // Structural equality compares exact canonical level sequences, not storage addresses.
  friend bool operator==(const ReadyBookView& lhs, const ReadyBookView& rhs) noexcept;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Interesting syntax: private construction prevents callers from presenting arbitrary spans as
  // a coherent strategy-visible book.
  friend class MarketStateMachine;
  friend class FixedDepthOrderBook;

  // ########################################################################

  // --------------------------------------------------------
  // Bind one view to the stable current-book arrays owned by the state machine.
  ReadyBookView(std::span<const BookLevel> bids, std::span<const BookLevel> asks) noexcept
      : bids_{bids}, asks_{asks} {}

  // --------------------------------------------------------
  std::span<const BookLevel> bids_;
  std::span<const BookLevel> asks_;
};

// ########################################################################

// ########################################################################
// FixedDepthOrderBook stores current or scratch state without heap growth. Mutation is private so
// only the transactional state owner can build and swap a fully validated candidate.
class FixedDepthOrderBook final {
private:

  // ########################################################################
  // The serialized market state owner alone may mutate, validate, swap, or expose this storage.
  friend class MarketStateMachine;

  // ########################################################################

  // --------------------------------------------------------
  // Reserve the compiled ceiling once; later candidate copies and edits cannot grow storage.
  FixedDepthOrderBook();

  // --------------------------------------------------------
  // Scratch assignment reuses construction-time capacity; mutable books are otherwise not copied.
  FixedDepthOrderBook(const FixedDepthOrderBook&) = delete;
  FixedDepthOrderBook& operator=(const FixedDepthOrderBook& other);
  FixedDepthOrderBook(FixedDepthOrderBook&&) noexcept = default;
  FixedDepthOrderBook& operator=(FixedDepthOrderBook&&) noexcept = default;

  // --------------------------------------------------------
  // Compare populated canonical sequences; reserved capacity has no semantic meaning.
  friend bool operator==(const FixedDepthOrderBook& lhs, const FixedDepthOrderBook& rhs) noexcept;

  // --------------------------------------------------------
  // Reset both sides before constructing an authoritative snapshot candidate.
  void clear() noexcept;

  // --------------------------------------------------------
  // Insert, replace, or delete one absolute-quantity level while preserving canonical side order.
  [[nodiscard]] model::Result<void> apply(BookSide side, model::Price price,
                                          model::Quantity quantity, std::size_t retained_depth);

  // --------------------------------------------------------
  // Validate positivity, metadata increments, depth, side ordering, and strict uncrossedness.
  [[nodiscard]] model::Result<void> validate(const model::InstrumentMetadata& metadata,
                                             std::size_t retained_depth) const;

  // --------------------------------------------------------
  // Exchange complete fixed storage only after every fallible candidate check has succeeded.
  void swap(FixedDepthOrderBook& other) noexcept;

  // --------------------------------------------------------
  // Produce a turn-scoped immutable view over populated canonical prefixes.
  [[nodiscard]] ReadyBookView ready_view() const noexcept;

  // --------------------------------------------------------
  // Interesting syntax: vectors are pre-reserved fixed-capacity arrays; private spans let the view
  // inspect them without constructing financial-domain sentinels or exposing storage aliases.
  std::vector<BookLevel> bids_;
  std::vector<BookLevel> asks_;
};

// ########################################################################

} // namespace aegis::market_data
