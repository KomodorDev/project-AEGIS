// Purpose: join the checked OMS and economic execution prefixes into bounded prospective audit
// and callback evidence for the genuine private drain preparation.

#include "private_execution_drain_evidence.hpp"

#include "../oms/private_execution_projection.hpp"

#include <array>
#include <optional>
#include <utility>
#include <variant>

namespace aegis::runtime {
namespace {

// ########################################################################
// One complete inventory audit prefix uses the canonical Bot-through-Venue scope order.
using InventoryAuditEffects = std::array<trace::InventoryScopeAuditEffect, 7U>;

// ########################################################################
// Drop all descriptors that borrow the input plans when copying returns or throws.
struct PrimaryAuditInputBorrowReset final {
  std::vector<PrimaryPrivateAuditInput>& descriptors;

  // --------------------------------------------------------
  // Remove the borrowed references without releasing cold capacity.
  ~PrimaryAuditInputBorrowReset() { descriptors.clear(); }

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Read all seven genuine scope baselines before the first planned execution. Proposed additions
// follow the same canonical scope order and carry the exact per-execution signed transfer.
[[nodiscard]] model::Result<InventoryAuditEffects> derive_first_execution_inventory_audit_effects(
    const risk::ReservationLedger& reservations, const execution::InstalledSubmissionRoute& route,
    const risk::ReservationInventoryExecutionEffect& execution) {
  using Result = model::Result<InventoryAuditEffects>;
  auto currency = trace::QuoteCurrency::parse_quote_currency(route.metadata().quote_currency());
  if (!currency) {
    return Result::create_failure(std::move(currency).error());
  }
  const auto& attribution = route.attribution();
  const auto& installed = route.route();
  const auto& instrument = route.metadata().instrument_id();
  const std::array<trace::RiskScopeAuditSubject, 7U> subjects{
      attribution.bot_id,           attribution.desk_id, attribution.firm_id,
      installed.logical_account_id, installed.id,        instrument,
      route.metadata().venue_id()};
  std::array<std::optional<trace::InventoryScopeAuditEffect>, 7U> effects;
  for (std::size_t index = 0U; index < effects.size(); ++index) {
    const auto& subject = subjects[index];
    const auto before = reservations.calculate_scope_exposure(
        attribution.firm_id, trace::risk_scope_kind(subject),
        trace::risk_scope_subject_value(subject), instrument, currency.value().value());
    if (!before) {
      return Result::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidReservationConversion,
                                              "private_execution_drain_evidence.scope_baseline"));
    }
    effects[index].emplace(trace::InventoryScopeAuditEffect{
        attribution.firm_id, subject, instrument, currency.value(), *before,
        execution.scopes[index].exposure, execution.signed_quantity_delta,
        execution.signed_notional_delta});
  }
  return Result::create_success(InventoryAuditEffects{
      std::move(*effects[0U]), std::move(*effects[1U]), std::move(*effects[2U]),
      std::move(*effects[3U]), std::move(*effects[4U]), std::move(*effects[5U]),
      std::move(*effects[6U])});
}

// --------------------------------------------------------
// Advance each audit scope from the exact preceding prospective prefix to this execution's
// replacement. The economic planner computed every replacement against one live baseline.
void advance_execution_inventory_audit_effects(
    InventoryAuditEffects& scopes, const risk::ReservationInventoryExecutionEffect& execution) {
  for (std::size_t index = 0U; index < scopes.size(); ++index) {
    auto& scope = scopes[index];
    scope.before = scope.after;
    scope.after = execution.scopes[index].exposure;
    scope.signed_quantity_delta = execution.signed_quantity_delta;
    scope.signed_notional_delta = execution.signed_notional_delta;
  }
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reserve all effect, descriptor, and notification slots before any owner turn can use them.
PrivateExecutionDrainEvidenceScratch::PrivateExecutionDrainEvidenceScratch(std::uint32_t capacity)
    : capacity_{capacity} {
  effects_.reserve(capacity);
  primaries_.reserve(capacity);
  callbacks_.reserve(capacity);
}

// --------------------------------------------------------
// Join the two checked prefix chains and copy them into the store's immutable detached lease.
model::Result<PreparedPrivateBusinessEvidence>
PrivateExecutionDrainEvidenceScratch::prepare_private_execution_drain_evidence(
    PrivateBusinessEvidenceStore& store, const risk::ReservationLedger& reservations,
    const execution::InstalledSubmissionRoute& route,
    const oms::PrivateOmsTransitionPlan& lifecycle, const risk::ReservationInventoryPlan& economics,
    std::span<const PrivateExecutionDrainIdentity> identities,
    recovery::JournalReplayProvenance replay, model::CallbackOrdinal first_callback,
    recovery::JournalSequence sequence) {
  using Result = model::Result<PreparedPrivateBusinessEvidence>;

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one bounded width and prove positions before writing any candidate row.
  effects_.clear();
  callbacks_.clear();
  const auto count = economics.execution_effect_count();
  if (count > capacity_) {
    return Result::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::PrivateEvidenceExhausted,
                                            "private_execution_drain_evidence.scratch_capacity"));
  }
  if (count == 0U || identities.size() != count || lifecycle.transition_effect_count != count ||
      lifecycle.order_callback_count != count ||
      lifecycle.drained_pending_prefix_count != count - 1U) {
    return Result::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                            "private_execution_drain_evidence.prefix_count"));
  }
  auto primary_span = recovery::AuditSpan::create_audit_span(store.next_audit_ordinal(), count);
  if (!primary_span) {
    return Result::create_failure(std::move(primary_span).error());
  }
  auto callback_range =
      trace::CallbackOrdinalRange::create_callback_ordinal_range(first_callback, count);
  if (!callback_range) {
    return Result::create_failure(std::move(callback_range).error());
  }
  const auto& first = *economics.execution_effect_at(0U);
  auto inventory_effects =
      derive_first_execution_inventory_audit_effects(reservations, route, first);
  if (!inventory_effects) {
    return Result::create_failure(std::move(inventory_effects).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive each newly applied execution's actual OMS and economic prefix. Store the copies before
  // adding descriptors so their borrowed one-effect spans stay stable throughout store validation.
  auto preceding_projection = lifecycle.before;
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto& economic_effect = *economics.execution_effect_at(index);
    const auto& identity = identities[index];
    const auto* known = identity.resolution.known_resolution();
    if (known == nullptr || economic_effect.audit_ordinal.value() !=
                                primary_span.value().first_audit_ordinal().value() + index) {
      return Result::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                              "private_execution_drain_evidence.prefix_linkage"));
    }
    auto projection = oms::derive_private_execution_projection(lifecycle, economic_effect.execution,
                                                               preceding_projection, index);
    if (!projection) {
      return Result::create_failure(std::move(projection).error());
    }
    if (index != 0U) {
      advance_execution_inventory_audit_effects(inventory_effects.value(), economic_effect);
    }
    const auto& execution = std::get<oms::ExecutionPayload>(economic_effect.execution.payload());
    effects_.push_back(trace::PrivateOrderAuditEffect{
        known->order_id, known->provenance, projection.value().before, projection.value().after,
        economic_effect.reservation_before, economic_effect.reservation_after,
        inventory_effects.value(), std::nullopt});
    callbacks_.push_back(trace::OrderCallbackAuditValue{
        known->order_id, known->provenance,
        trace::originating_event_identity_from_normalized_input(economic_effect.execution),
        execution.trade_id, execution.cumulative_quantity});
    preceding_projection = projection.value().after;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the already validated candidates into independent store backing. No descriptor or
  // mutable scratch alias escapes through the returned preparation.
  const PrimaryAuditInputBorrowReset reset_primary_borrows{primaries_};
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto& identity = identities[index];
    primaries_.push_back(PrimaryPrivateAuditInput{
        economics.execution_effect_at(index)->execution, identity.resolution,
        identity.trade_semantic_value,
        index == 0U ? oms::PrivateEventDisposition::Applied
                    : oms::PrivateEventDisposition::AppliedFromBuffer,
        std::span<const trace::PrivateOrderAuditEffect>{&effects_[index], 1U},
        trace::CallbackDecision::None, std::nullopt});
  }
  const auto& identity = identities.front();
  return store.prepare_known_private_business_evidence(
      first.execution, identity.resolution, identity.trade_semantic_value, replay, primaries_,
      callback_range.value(), callbacks_, sequence);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
