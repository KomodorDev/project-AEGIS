// Purpose: copy genuine initial known-order business proposals into policy-sized prospective
// evidence backing without changing live OMS, economics, safety, callbacks, or recovery state.

#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Copy all seven genuine risk-scope baselines and proposed replacements with exact nominal keys.
// Checked signed differences prevent malformed or overflowed evidence from escaping preparation.
[[nodiscard]] model::Result<std::array<trace::InventoryScopeAuditEffect, 7U>>
derive_initial_inventory_audit_effects(const risk::ReservationLedger& reservations,
                                       const execution::InstalledSubmissionRoute& route,
                                       const risk::ReservationInventoryPlan& economics) {
  using Effects = std::array<trace::InventoryScopeAuditEffect, 7U>;
  using EffectsResult = model::Result<Effects>;
  auto currency = trace::QuoteCurrency::parse_quote_currency(route.metadata().quote_currency());
  if (!currency) {
    return EffectsResult::create_failure(std::move(currency).error());
  }
  const auto& attribution = route.attribution();
  const auto& installed = route.route();
  const auto instrument = route.metadata().instrument_id();
  const std::array<trace::RiskScopeAuditSubject, 7U> subjects{
      attribution.bot_id,           attribution.desk_id, attribution.firm_id,
      installed.logical_account_id, installed.id,        instrument,
      route.metadata().venue_id()};
  std::array<std::optional<trace::InventoryScopeAuditEffect>, 7U> prepared;

  // ++++++++++++++++++++++++++++++++++++++++
  // The economics planner's replacements follow the fixed Bot through Venue scope ordering.
  for (std::size_t index = 0U; index < subjects.size(); ++index) {
    const auto& subject = subjects[index];
    const auto before = reservations.calculate_scope_exposure(
        attribution.firm_id, trace::risk_scope_kind(subject),
        trace::risk_scope_subject_value(subject), instrument, currency.value().value());
    if (!before) {
      return EffectsResult::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                              "private_business_evidence.scope_baseline"));
    }
    const auto& after = economics.scope_replacements()[index].exposure;
    auto quantity_delta = after.confirmed_quantity.checked_subtract(before->confirmed_quantity);
    auto notional_delta =
        after.confirmed_quote_notional.checked_subtract(before->confirmed_quote_notional);
    if (!quantity_delta || !notional_delta) {
      return EffectsResult::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                              "private_business_evidence.scope_delta"));
    }
    prepared[index].emplace(trace::InventoryScopeAuditEffect{
        attribution.firm_id, subject, instrument, currency.value(), *before, after,
        quantity_delta.value(), notional_delta.value()});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve canonical key order independently from planner implementation details.
  Effects result{std::move(*prepared[0U]), std::move(*prepared[1U]), std::move(*prepared[2U]),
                 std::move(*prepared[3U]), std::move(*prepared[4U]), std::move(*prepared[5U]),
                 std::move(*prepared[6U])};
  std::sort(result.begin(), result.end(), trace::is_inventory_scope_audit_effect_less);
  return EffectsResult::create_success(std::move(result));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Translate a detached lifecycle classification to its proposed evidence vocabulary.
[[nodiscard]] oms::PrivateEventDisposition
private_event_disposition_from_proposed_oms_classification(
    oms::ProposedPrivateOmsClassification classification) noexcept {
  switch (classification) {
  case oms::ProposedPrivateOmsClassification::Applied:
    return oms::PrivateEventDisposition::Applied;
  case oms::ProposedPrivateOmsClassification::ProjectionOnly:
    return oms::PrivateEventDisposition::ProjectionOnly;
  case oms::ProposedPrivateOmsClassification::BufferedGap:
    return oms::PrivateEventDisposition::BufferedGap;
  case oms::ProposedPrivateOmsClassification::SafetyContained:
    return oms::PrivateEventDisposition::SafetyContained;
  case oms::ProposedPrivateOmsClassification::ForbiddenRejected:
    return oms::PrivateEventDisposition::ForbiddenRejected;
  }
  std::terminate();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Join genuine initial component proposals with fully copied prospective event, economic, and
// callback evidence. Both component and evidence leases remain unpublished and independent.
model::Result<PreparedPrivateBusinessEvidence>
PrivateOrderReconciler::prepare_initial_known_authoritative_business_evidence(
    const oms::NormalizedPrivateOrderInput& input,
    recovery::JournalReplayProvenance replay_provenance,
    std::optional<model::CallbackOrdinal> first_callback_ordinal,
    std::optional<recovery::JournalSequence> journal_sequence,
    std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) const {
  using PreparationResult = model::Result<PreparedPrivateBusinessEvidence>;

  // ++++++++++++++++++++++++++++++++++++++++
  // A live detached lease prevents scratch reuse before component planning can acquire its lease.
  if (business_evidence_store_->has_outstanding_preparation()) {
    return PreparationResult::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                            "private_business_evidence.preparation_busy"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive every consequence from unchanged owner components at the store's prospective position.
  auto proposal = derive_initial_known_authoritative_business_proposal(
      input, business_evidence_store_->next_audit_ordinal());
  if (!proposal) {
    return PreparationResult::create_failure(std::move(proposal).error());
  }
  auto identity = derive_first_seen_authoritative_identity_plan(
      oms::PrivateEventIngressSemanticValue::from_normalized_input(input));
  if (!identity) {
    return PreparationResult::create_failure(std::move(identity).error());
  }
  const auto& business = proposal.value();
  const auto& resolution = business.first_admission_resolution();
  const auto* known = resolution.known_resolution();
  const auto& lifecycle = business.oms_transition();
  const auto* economics = business.economics_plan();
  std::optional<oms::PrivateTradeSemanticValue> trade;
  if (const auto* derived =
          std::get_if<FirstSeenPrivateTradeIdentityPlan>(&identity.value().trade_plan())) {
    trade = derived->semantic_value;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy complete before/after economics with exact owner-derived scope keys when economics exist.
  std::optional<std::array<trace::InventoryScopeAuditEffect, 7U>> inventory_effects;
  if (economics != nullptr) {
    const auto& subject = *known->provenance.subject();
    const auto* route = owner_->routes().find_route(subject.route()->route_id);
    auto scopes =
        derive_initial_inventory_audit_effects(owner_->reservations(), *route, *economics);
    if (!scopes) {
      return PreparationResult::create_failure(std::move(scopes).error());
    }
    inventory_effects.emplace(std::move(scopes).value());
  }
  std::optional<trace::AccountSafetyAuditTransition> safety;
  if (lifecycle.safety_reason) {
    const auto before = account_safety_state(known->provenance.subject()->logical_account_id());
    const auto reason = *lifecycle.safety_reason;
    const auto required = reason <= risk::AccountSafetyReason::RecoveryGap
                              ? risk::AccountSafetyState::ReconciliationRequired
                              : risk::AccountSafetyState::Quarantined;
    safety.emplace(trace::AccountSafetyAuditTransition{before, std::max(before, required), reason});
  }
  const std::array effects{trace::PrivateOrderAuditEffect{
      known->order_id, known->provenance, lifecycle.before, lifecycle.after,
      business.reservation_before(),
      economics == nullptr ? business.reservation_before() : economics->reservation_after(),
      std::move(inventory_effects), safety}};

  // ++++++++++++++++++++++++++++++++++++++++
  // Assign only a prospective callback range. Execution notifications carry the actual applied
  // interval endpoint; contradictions retain the source trade with no applied endpoint.
  std::optional<trace::CallbackOrdinalRange> range;
  std::array<std::optional<trace::OrderCallbackAuditValue>, 1U> callbacks;
  if (business.evidence_requirements().order_callback_count != 0U) {
    if (!first_callback_ordinal) {
      return PreparationResult::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                              "private_business_evidence.callback_ordinal"));
    }
    auto candidate = trace::CallbackOrdinalRange::create_callback_ordinal_range(
        *first_callback_ordinal, business.evidence_requirements().order_callback_count);
    if (!candidate) {
      return PreparationResult::create_failure(std::move(candidate).error());
    }
    range.emplace(std::move(candidate).value());
    std::optional<oms::TradeId> callback_trade;
    std::optional<model::Quantity> applied_endpoint;
    if (const auto* execution = std::get_if<oms::ExecutionPayload>(&input.payload())) {
      callback_trade = execution->trade_id;
      if (economics != nullptr && economics->execution_effect_count() != 0U) {
        applied_endpoint = execution->cumulative_quantity;
      }
    }
    callbacks[0U].emplace(trace::OrderCallbackAuditValue{
        known->order_id, known->provenance,
        trace::originating_event_identity_from_normalized_input(input), callback_trade,
        applied_endpoint});
  } else if (first_callback_ordinal) {
    return PreparationResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateOmsState,
        "private_business_evidence.unexpected_callback_ordinal"));
  }
  const auto disposition =
      private_event_disposition_from_proposed_oms_classification(lifecycle.classification);
  const std::array primary_rows{
      PrimaryPrivateAuditInput{input, resolution, trade, disposition, effects,
                               disposition == oms::PrivateEventDisposition::BufferedGap
                                   ? trace::CallbackDecision::SuppressedBuffered
                                   : trace::CallbackDecision::None,
                               std::nullopt}};
  const std::span<const trace::OrderCallbackAuditValue> callback_values =
      callbacks[0U] ? std::span<const trace::OrderCallbackAuditValue>{&*callbacks[0U], 1U}
                    : std::span<const trace::OrderCallbackAuditValue>{};

  // ++++++++++++++++++++++++++++++++++++++++
  // The store validates every bound and copies values before the component scratch lease ends.
  return business_evidence_store_->prepare_known_private_business_evidence(
      input, resolution, trade, replay_provenance, primary_rows, range, callback_values,
      journal_sequence, diagnostic_ordinal);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
