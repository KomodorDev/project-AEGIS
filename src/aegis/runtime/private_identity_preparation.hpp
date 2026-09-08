// Purpose: retain bounded private identity preparations transactionally without publishing
// canonical dispositions, ownership mappings, economics, callbacks, or recovery evidence.

#pragma once

#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The owner-bound planner alone seals the detached identities accepted by preparation storage.
class FirstSeenAuthoritativePrivateIdentityPlan;

// ########################################################################
// These source-private observations describe preparations only; they are neither stable M4
// dispositions nor proof that an event has been economically consumed.
enum class PrivateIdentityPreparationClassification : std::uint8_t {
  FirstObservation,
  RepeatedEvent,
  EventConflict,
  RepeatedTrade,
  TradeConflict,
  MappingConflict,
  SourceSideConflict,
  UnknownOrder,
  PreTradeConflict,
};

// ########################################################################
// The first exhausted preparation table is reported without allocating a domain-error string.
enum class PrivateIdentityPreparationCapacity : std::uint8_t {
  EventRecords,
  TradeRecords,
  CandidateMappings,
};

// ########################################################################
// One result copies the immutable resolution and diagnostic classification. A capacity value
// means that no new record was retained; every success remains a preparation, not consumption.
struct PrivateIdentityPreparationResult {
  PrivateIdentityPreparationClassification classification;
  std::optional<PrivateIdentityPreparationCapacity> capacity_exhaustion;
  oms::PrivateEventResolution resolution;
  std::optional<risk::AccountSafetyReason> safety_reason;

  // --------------------------------------------------------
  // Compare every observation while excluding storage addresses and incidental lookup ordering.
  friend bool operator==(const PrivateIdentityPreparationResult&,
                         const PrivateIdentityPreparationResult&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One immutable preparation preserves complete ingress and its original sealed resolution across
// later submissions, repeats, and conflicts; no canonical disposition is claimed.
struct PreparedPrivateEventRecord {
  oms::PrivateEventRegistryKey key;
  oms::PrivateEventIngressSemanticValue ingress_semantic_value;
  oms::PrivateEventResolution resolution;
  PrivateIdentityPreparationClassification classification;
  std::optional<risk::AccountSafetyReason> safety_reason;

  // --------------------------------------------------------
  // Compare the complete original preparation to detect any accidental rewriting on replay.
  friend bool operator==(const PreparedPrivateEventRecord&,
                         const PreparedPrivateEventRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One immutable preparation preserves the complete ADR-0010 trade comparison tuple, independently
// of ordinary source epochs and event identities, without an applied or buffered disposition.
struct PreparedPrivateTradeRecord {
  oms::TradeKey key;
  oms::PrivateTradeSemanticValue semantic_value;

  // --------------------------------------------------------
  // Compare the exact retained key and tuple without deriving a digest or economic projection.
  friend bool operator==(const PreparedPrivateTradeRecord&,
                         const PreparedPrivateTradeRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One diagnostic candidate records a possible future mapping. It is never an ownership lookup
// source and cannot authorize correlation, canonical mapping publication, or economic effects.
struct PreparedPrivateMappingCandidate {
  oms::ExchangeOrderKey exchange_order_key;
  model::OrderId order_id;

  // --------------------------------------------------------
  // Compare the complete diagnostic claim without treating equality as publication authority.
  friend bool operator==(const PreparedPrivateMappingCandidate&,
                         const PreparedPrivateMappingCandidate&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Owns three preallocated preparation tables whose occupied prefixes never shrink or change.
// All allocation occurs during construction from a sealed policy. Later writes are no-throw and
// atomic to the serialized owner: every required slot is checked before any insertion. Calls and
// borrowed records require the same owner context or externally guaranteed quiescence; this store
// provides no synchronization. Candidate mappings are diagnostic and never correlation authority.
class PrivateIdentityPreparationStore final {
public:

  // --------------------------------------------------------
  // Allocate all preparation slots from the three installed identity capacities; allocation
  // failure propagates before construction publishes a usable store.
  explicit PrivateIdentityPreparationStore(const M4Policy& policy);

  // --------------------------------------------------------
  // Preserve each borrowed record's address and the single immutable preparation history.
  PrivateIdentityPreparationStore(const PrivateIdentityPreparationStore&) = delete;
  PrivateIdentityPreparationStore& operator=(const PrivateIdentityPreparationStore&) = delete;
  PrivateIdentityPreparationStore(PrivateIdentityPreparationStore&&) = delete;
  PrivateIdentityPreparationStore& operator=(PrivateIdentityPreparationStore&&) = delete;

  // --------------------------------------------------------
  // Retain a sealed plan only after every required table has room. Existing event keys compare
  // ingress before trade/candidate checks and reuse their original resolution. Trade repeats or
  // conflicts suppress candidate additions and leave original trades unchanged. Candidate claim
  // conflicts are preparation diagnostics only; they never rewrite the plan's sealed resolution.
  [[nodiscard]] PrivateIdentityPreparationResult
  prepare_and_retain_identity(const FirstSeenAuthoritativePrivateIdentityPlan& plan) noexcept;

  // --------------------------------------------------------
  // Borrow the immutable first preparation for this event key, or return null. Check this before
  // replanning so a repeated event cannot acquire a different resolution from later owner state.
  [[nodiscard]] const PreparedPrivateEventRecord*
  find_prepared_event(const oms::PrivateEventRegistryKey& key) const noexcept;

  // --------------------------------------------------------
  // Compare a stored event with the same incoming key while preserving its original resolution.
  // The caller must supply the record returned by find_prepared_event for the input's exact key.
  [[nodiscard]] static PrivateIdentityPreparationResult
  classify_repeated_event(const PreparedPrivateEventRecord& prepared,
                          const oms::PrivateEventIngressSemanticValue& input) noexcept;

  // --------------------------------------------------------
  // Return the fixed number of allocated slots in each distinct preparation table.
  [[nodiscard]] std::uint32_t event_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(event_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of independent trade comparison slots.
  [[nodiscard]] std::uint32_t trade_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(trade_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of diagnostic candidate claims; none is an ownership mapping.
  [[nodiscard]] std::uint32_t mapping_candidate_capacity() const noexcept {
    return static_cast<std::uint32_t>(mapping_candidates_.size());
  }

  // --------------------------------------------------------
  // Return the number of immutable event preparations in insertion order.
  [[nodiscard]] std::uint32_t event_record_count() const noexcept { return event_record_count_; }

  // --------------------------------------------------------
  // Return the number of immutable independent trade preparations.
  [[nodiscard]] std::uint32_t trade_record_count() const noexcept { return trade_record_count_; }

  // --------------------------------------------------------
  // Return the number of diagnostic candidate claims retained without ownership publication.
  [[nodiscard]] std::uint32_t mapping_candidate_count() const noexcept {
    return mapping_candidate_count_;
  }

  // --------------------------------------------------------
  // Borrow one insertion-ordered event preparation, or return null beyond the occupied prefix.
  [[nodiscard]] const PreparedPrivateEventRecord*
  event_record_at(std::uint32_t index) const noexcept;

  // --------------------------------------------------------
  // Borrow one insertion-ordered trade preparation, or return null beyond the occupied prefix.
  [[nodiscard]] const PreparedPrivateTradeRecord*
  trade_record_at(std::uint32_t index) const noexcept;

  // --------------------------------------------------------
  // Borrow one diagnostic claim for inspection only, or return null beyond the occupied prefix.
  [[nodiscard]] const PreparedPrivateMappingCandidate*
  mapping_candidate_at(std::uint32_t index) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Borrow the first exact account/venue trade key, or return null without altering any table.
  [[nodiscard]] const PreparedPrivateTradeRecord*
  find_prepared_trade(const oms::TradeKey& key) const noexcept;

  // --------------------------------------------------------
  // Storage is fully sized once; only the occupied-prefix counters advance during owner turns.
  std::vector<std::optional<PreparedPrivateEventRecord>> event_records_;
  std::vector<std::optional<PreparedPrivateTradeRecord>> trade_records_;
  std::vector<std::optional<PreparedPrivateMappingCandidate>> mapping_candidates_;
  std::uint32_t event_record_count_{0U};
  std::uint32_t trade_record_count_{0U};
  std::uint32_t mapping_candidate_count_{0U};
};

// ########################################################################

} // namespace aegis::runtime
