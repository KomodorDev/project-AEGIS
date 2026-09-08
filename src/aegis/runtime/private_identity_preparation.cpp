// Purpose: preflight and retain immutable private identity preparations in fixed storage without
// promoting candidate ownership or committing canonical M4 business state and evidence.

#include "private_identity_preparation.hpp"

#include "private_order_reconciler.hpp"

#include <type_traits>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Borrow the plan's sealed first-seen resolution regardless of its correlation alternative.
[[nodiscard]] const oms::PrivateEventResolution&
first_seen_resolution(const FirstSeenAuthoritativePrivateIdentityPlan& plan) noexcept {
  return std::visit(
      [](const auto& correlation) -> const oms::PrivateEventResolution& {
        return correlation.resolution;
      },
      plan.correlation_plan());
}

// --------------------------------------------------------
// Derive the initial diagnostic class before any independent trade or candidate lookup.
[[nodiscard]] PrivateIdentityPreparationClassification
classify_first_preparation(const FirstSeenAuthoritativePrivateIdentityPlan& plan) noexcept {
  if (std::holds_alternative<FirstSeenPrivateTradeSourceSideConflictPlan>(plan.trade_plan())) {
    return PrivateIdentityPreparationClassification::SourceSideConflict;
  }
  if (std::holds_alternative<UnknownFirstSeenPrivateCorrelationPlan>(plan.correlation_plan())) {
    return PrivateIdentityPreparationClassification::UnknownOrder;
  }
  if (std::holds_alternative<ConflictFirstSeenPrivateCorrelationPlan>(plan.correlation_plan())) {
    return PrivateIdentityPreparationClassification::PreTradeConflict;
  }
  return PrivateIdentityPreparationClassification::FirstObservation;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Allocate every optional slot from already validated policy bounds before an owner can use it.
PrivateIdentityPreparationStore::PrivateIdentityPreparationStore(const M4Policy& policy)
    : event_records_(static_cast<std::uint32_t>(policy.capacities().max_event_identity_records)),
      trade_records_(static_cast<std::uint32_t>(policy.capacities().max_trade_identity_records)),
      mapping_candidates_(
          static_cast<std::uint32_t>(policy.capacities().max_exchange_order_mappings)) {}

// --------------------------------------------------------
// Resolve exact event reuse before every independent identity lookup and retain only complete
// preparations after deterministic event/trade/candidate capacity preflight succeeds.
PrivateIdentityPreparationResult PrivateIdentityPreparationStore::prepare_and_retain_identity(
    const FirstSeenAuthoritativePrivateIdentityPlan& plan) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // A previously observed event always reuses its original first resolution and cannot consume
  // any new trade or candidate slot even when every preparation table is already full.
  if (const auto* const existing_event = find_prepared_event(plan.event_key())) {
    return classify_repeated_event(*existing_event, plan.ingress_semantic_value());
  }
  PrivateIdentityPreparationResult result{classify_first_preparation(plan), std::nullopt,
                                          first_seen_resolution(plan),
                                          plan.preliminary_safety_reason()};

  // ++++++++++++++++++++++++++++++++++++++++
  // Trade identity excludes source event identity. Repeats and contradictions preserve the first
  // complete comparison tuple and suppress every candidate carried by the later event.
  const auto* new_trade = std::get_if<FirstSeenPrivateTradeIdentityPlan>(&plan.trade_plan());
  const auto* const known =
      std::get_if<KnownFirstSeenPrivateCorrelationPlan>(&plan.correlation_plan());
  const PrivateExchangeOrderMapping* new_candidate =
      known != nullptr && known->candidate_mapping && !plan.preliminary_safety_reason()
          ? &*known->candidate_mapping
          : nullptr;
  if (new_trade != nullptr) {
    if (const auto* const existing_trade = find_prepared_trade(new_trade->key)) {
      if (existing_trade->semantic_value == new_trade->semantic_value) {
        result.classification = PrivateIdentityPreparationClassification::RepeatedTrade;
        result.safety_reason = std::nullopt;
      } else {
        result.classification = PrivateIdentityPreparationClassification::TradeConflict;
        result.safety_reason = risk::AccountSafetyReason::TradeIdentityConflict;
      }
      new_trade = nullptr;
      new_candidate = nullptr;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Candidate equality avoids extra slots. A conflicting diagnostic claim is retained on the
  // event without publishing either claim as ownership or rewriting the sealed resolution.
  if (new_candidate != nullptr) {
    for (std::uint32_t index = 0U; index < mapping_candidate_count_; ++index) {
      const auto& existing = *mapping_candidates_[index];
      const bool same_exchange = existing.exchange_order_key == new_candidate->exchange_order_key;
      const bool same_order = existing.order_id == new_candidate->order_id;
      if (same_exchange || same_order) {
        if (!(same_exchange && same_order)) {
          result.classification = PrivateIdentityPreparationClassification::MappingConflict;
        }
        new_candidate = nullptr;
        break;
      }
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Check every required capacity before touching an optional or counter. Error precedence is
  // event, then independent trade, then candidate storage, and failure leaves all prefixes intact.
  if (event_record_count_ == event_records_.size()) {
    result.capacity_exhaustion = PrivateIdentityPreparationCapacity::EventRecords;
  } else if (new_trade != nullptr && trade_record_count_ == trade_records_.size()) {
    result.capacity_exhaustion = PrivateIdentityPreparationCapacity::TradeRecords;
  } else if (new_candidate != nullptr && mapping_candidate_count_ == mapping_candidates_.size()) {
    result.capacity_exhaustion = PrivateIdentityPreparationCapacity::CandidateMappings;
  }
  if (result.capacity_exhaustion) {
    return result;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Complete bounded copies cannot throw or allocate, so no owner-visible partial insertion can
  // occur between the preflight and the final counter updates in this serialized operation.
  static_assert(std::is_nothrow_copy_constructible_v<PreparedPrivateEventRecord>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedPrivateEventRecord>);
  static_assert(std::is_nothrow_copy_constructible_v<PreparedPrivateTradeRecord>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedPrivateTradeRecord>);
  static_assert(std::is_nothrow_copy_constructible_v<PreparedPrivateMappingCandidate>);
  static_assert(std::is_nothrow_move_constructible_v<PreparedPrivateMappingCandidate>);
  event_records_[event_record_count_].emplace(
      PreparedPrivateEventRecord{plan.event_key(), plan.ingress_semantic_value(), result.resolution,
                                 result.classification, result.safety_reason});
  if (new_trade != nullptr) {
    trade_records_[trade_record_count_].emplace(
        PreparedPrivateTradeRecord{new_trade->key, new_trade->semantic_value});
    ++trade_record_count_;
  }
  if (new_candidate != nullptr) {
    mapping_candidates_[mapping_candidate_count_].emplace(PreparedPrivateMappingCandidate{
        new_candidate->exchange_order_key, new_candidate->order_id});
    ++mapping_candidate_count_;
  }
  ++event_record_count_;
  return result;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Search only occupied immutable slots, preserving insertion order and pointer lifetime.
const PreparedPrivateEventRecord* PrivateIdentityPreparationStore::find_prepared_event(
    const oms::PrivateEventRegistryKey& key) const noexcept {
  for (std::uint32_t index = 0U; index < event_record_count_; ++index) {
    if (event_records_[index]->key == key) {
      return &*event_records_[index];
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Preserve original event ownership on exact replay or contradictory semantic key reuse.
PrivateIdentityPreparationResult PrivateIdentityPreparationStore::classify_repeated_event(
    const PreparedPrivateEventRecord& prepared,
    const oms::PrivateEventIngressSemanticValue& input) noexcept {
  const bool identical = prepared.ingress_semantic_value == input;
  return PrivateIdentityPreparationResult{
      identical ? PrivateIdentityPreparationClassification::RepeatedEvent
                : PrivateIdentityPreparationClassification::EventConflict,
      std::nullopt, prepared.resolution,
      identical ? std::nullopt : std::optional{risk::AccountSafetyReason::EventIdentityConflict}};
}

// --------------------------------------------------------
// Return immutable insertion-ordered event storage without exposing spare slots.
const PreparedPrivateEventRecord*
PrivateIdentityPreparationStore::event_record_at(std::uint32_t index) const noexcept {
  return index < event_record_count_ ? &*event_records_[index] : nullptr;
}

// --------------------------------------------------------
// Return immutable insertion-ordered trade storage without exposing spare slots.
const PreparedPrivateTradeRecord*
PrivateIdentityPreparationStore::trade_record_at(std::uint32_t index) const noexcept {
  return index < trade_record_count_ ? &*trade_records_[index] : nullptr;
}

// --------------------------------------------------------
// Return one diagnostic claim for inspection, without offering an ownership lookup operation.
const PreparedPrivateMappingCandidate*
PrivateIdentityPreparationStore::mapping_candidate_at(std::uint32_t index) const noexcept {
  return index < mapping_candidate_count_ ? &*mapping_candidates_[index] : nullptr;
}

// --------------------------------------------------------
// Find one immutable account/venue-scoped trade independently of source epoch and event identity.
const PreparedPrivateTradeRecord*
PrivateIdentityPreparationStore::find_prepared_trade(const oms::TradeKey& key) const noexcept {
  for (std::uint32_t index = 0U; index < trade_record_count_; ++index) {
    if (trade_records_[index]->key == key) {
      return &*trade_records_[index];
    }
  }
  return nullptr;
}

// --------------------------------------------------------

} // namespace aegis::runtime
