// Purpose: validate complete bounded M4 evidence proposals and implement immutable views over cold
// effect backing without publishing business records, applying economics, or claiming delivery.

#include "private_business_evidence.hpp"

#include <algorithm>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace aegis::trace {
namespace {

// --------------------------------------------------------
// Return one stable evidence-shape failure before the store can retain any proposed values.
[[nodiscard]] model::Result<void> reject_private_audit_field(const char* field) {
  return model::Result<void>::create_failure(
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent, field));
}

// --------------------------------------------------------
// Validate one reservation snapshot's state/closure and conservation without trusting the later
// transition. Released residual is zero; its historical confirmed fraction remains unchanged.
[[nodiscard]] model::Result<void>
validate_audit_reservation(const risk::ReservationEvidence& reservation) {
  if ((reservation.side != execution::OrderSide::Buy &&
       reservation.side != execution::OrderSide::Sell) ||
      reservation.exposure.quantity.coefficient() <= 0 ||
      reservation.exposure.quote_notional.coefficient() <= 0 ||
      reservation.remaining_exposure.quantity.coefficient() < 0 ||
      reservation.remaining_exposure.quote_notional.coefficient() < 0 ||
      reservation.cumulative_confirmed_exposure.quantity.coefficient() < 0 ||
      reservation.cumulative_confirmed_exposure.quote_notional.coefficient() < 0 ||
      reservation.cumulative_confirmed_exposure.quantity > reservation.exposure.quantity ||
      reservation.cumulative_confirmed_exposure.quote_notional >
          reservation.exposure.quote_notional ||
      (reservation.cumulative_confirmed_exposure.quantity.coefficient() == 0 &&
       reservation.cumulative_confirmed_exposure.quote_notional.coefficient() != 0)) {
    return reject_private_audit_field("private_audit.reservation_economics");
  }
  if (reservation.state == risk::ReservationState::Held) {
    if (reservation.closure_cause != risk::ReservationClosureCause::Unassigned ||
        reservation.remaining_exposure.quantity.coefficient() <= 0) {
      return reject_private_audit_field("private_audit.held_reservation");
    }
    const auto quantity = reservation.remaining_exposure.quantity.checked_add(
        reservation.cumulative_confirmed_exposure.quantity);
    if (!quantity) {
      return model::Result<void>::create_failure(quantity.error());
    }
    const auto notional = reservation.remaining_exposure.quote_notional.checked_add(
        reservation.cumulative_confirmed_exposure.quote_notional);
    if (!notional) {
      return model::Result<void>::create_failure(notional.error());
    }
    if (quantity.value() != reservation.exposure.quantity ||
        notional.value() != reservation.exposure.quote_notional) {
      return reject_private_audit_field("private_audit.reservation_conservation");
    }
  } else if (reservation.state == risk::ReservationState::ConsumedByFill) {
    if (reservation.closure_cause != risk::ReservationClosureCause::FullFill ||
        reservation.remaining_exposure.quantity.coefficient() != 0 ||
        reservation.remaining_exposure.quote_notional.coefficient() != 0 ||
        reservation.cumulative_confirmed_exposure != reservation.exposure) {
      return reject_private_audit_field("private_audit.consumed_reservation");
    }
  } else if (reservation.state == risk::ReservationState::Released) {
    const auto cause = reservation.closure_cause;
    if ((cause != risk::ReservationClosureCause::DefiniteLocalFailure &&
         cause != risk::ReservationClosureCause::ExchangeRejected &&
         cause != risk::ReservationClosureCause::DefinitiveCancellation &&
         cause != risk::ReservationClosureCause::CompleteAuthoritativeNegative) ||
        reservation.remaining_exposure.quantity.coefficient() != 0 ||
        reservation.remaining_exposure.quote_notional.coefficient() != 0) {
      return reject_private_audit_field("private_audit.released_reservation");
    }
  } else {
    return reject_private_audit_field("private_audit.reservation_state");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Check closed OMS assignments and their economic pair without deciding a lifecycle transition.
[[nodiscard]] bool
is_audit_oms_projection_consistent(const oms::PrivateOrderProjection& projection,
                                   const risk::ReservationEvidence& reservation) noexcept {
  const auto state = static_cast<std::uint8_t>(projection.state);
  const auto cancellation = static_cast<std::uint8_t>(projection.cancellation_state);
  if (state < static_cast<std::uint8_t>(oms::OutboundOrderState::PendingEncoding) ||
      state > static_cast<std::uint8_t>(oms::OutboundOrderState::ReconciledAbsent) ||
      cancellation < static_cast<std::uint8_t>(oms::CancellationState::None) ||
      cancellation > static_cast<std::uint8_t>(oms::CancellationState::Confirmed) ||
      projection.cumulative_filled_quantity != reservation.cumulative_confirmed_exposure.quantity ||
      (projection.exchange_acknowledged && !projection.exchange_order_id) ||
      (projection.cumulative_filled_quantity.coefficient() > 0 &&
       !projection.execution_evidence_observed) ||
      (projection.pending_fill_count > 0U && !projection.execution_evidence_observed) ||
      (projection.exchange_mapping_established_by_execution &&
       (!projection.exchange_order_id || !projection.execution_evidence_observed))) {
    return false;
  }
  if (projection.authoritative_terminal_cumulative_quantity &&
      (*projection.authoritative_terminal_cumulative_quantity <
           projection.cumulative_filled_quantity ||
       *projection.authoritative_terminal_cumulative_quantity > reservation.exposure.quantity)) {
    return false;
  }
  switch (projection.state) {
  case oms::OutboundOrderState::Filled:
    return reservation.state == risk::ReservationState::ConsumedByFill &&
           projection.pending_fill_count == 0U;
  case oms::OutboundOrderState::ExchangeRejected:
    return projection.pending_fill_count == 0U &&
           reservation.state == risk::ReservationState::Released &&
           reservation.closure_cause == risk::ReservationClosureCause::ExchangeRejected;
  case oms::OutboundOrderState::Cancelled:
    return projection.pending_fill_count == 0U &&
           reservation.state == risk::ReservationState::Released &&
           reservation.closure_cause == risk::ReservationClosureCause::DefinitiveCancellation &&
           projection.authoritative_terminal_cumulative_quantity &&
           *projection.authoritative_terminal_cumulative_quantity ==
               projection.cumulative_filled_quantity;
  case oms::OutboundOrderState::ReconciledAbsent:
    return projection.pending_fill_count == 0U &&
           reservation.state == risk::ReservationState::Released &&
           reservation.closure_cause ==
               risk::ReservationClosureCause::CompleteAuthoritativeNegative;
  case oms::OutboundOrderState::LocallyFailed:
    return projection.pending_fill_count == 0U &&
           reservation.state == risk::ReservationState::Released &&
           reservation.closure_cause == risk::ReservationClosureCause::DefiniteLocalFailure;
  case oms::OutboundOrderState::PartiallyFilled:
    return reservation.state == risk::ReservationState::Held &&
           projection.cumulative_filled_quantity.coefficient() > 0 &&
           projection.cumulative_filled_quantity < reservation.exposure.quantity;
  default:
    return reservation.state == risk::ReservationState::Held &&
           projection.cumulative_filled_quantity.coefficient() == 0;
  }
}

// --------------------------------------------------------
// Match one nominal scope identifier to its exact known-order provenance field.
[[nodiscard]] bool
does_scope_subject_match_order(const RiskScopeAuditSubject& subject,
                               const model::M4SubjectProvenance& provenance) noexcept {
  return std::visit(
      [&provenance](const auto& value) {
        using Subject = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Subject, model::BotId>) {
          return provenance.bot_id() && value == *provenance.bot_id();
        } else if constexpr (std::is_same_v<Subject, model::DeskId>) {
          return provenance.desk_id() && value == *provenance.desk_id();
        } else if constexpr (std::is_same_v<Subject, model::FirmId>) {
          return provenance.firm_id() && value == *provenance.firm_id();
        } else if constexpr (std::is_same_v<Subject, model::LogicalAccountId>) {
          return value == provenance.logical_account_id();
        } else if constexpr (std::is_same_v<Subject, model::RouteId>) {
          return provenance.route() && value == provenance.route()->route_id;
        } else if constexpr (std::is_same_v<Subject, model::InstrumentId>) {
          return provenance.instrument() && value == provenance.instrument()->instrument_id;
        } else {
          return value == provenance.venue_id();
        }
      },
      subject);
}

// --------------------------------------------------------
// Convert an already nonnegative confirmed increment to its retained economic side exactly.
template <typename Decimal>
[[nodiscard]] model::Result<Decimal> derive_signed_audit_increment(Decimal increment,
                                                                   execution::OrderSide side) {
  if (increment.coefficient() < 0) {
    return model::Result<Decimal>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateEvent, "private_audit.cumulative_decrease"));
  }
  return Decimal::from_scaled(side == execution::OrderSide::Buy ? increment.coefficient()
                                                                : -increment.coefficient(),
                              increment.scale());
}

// --------------------------------------------------------
// Prove one retained confirmed value changes by the exact proposed signed delta.
template <typename Decimal>
[[nodiscard]] model::Result<void> validate_audit_confirmed_delta(Decimal before, Decimal delta,
                                                                 Decimal after) {
  const auto expected = before.checked_add(delta);
  if (!expected) {
    return model::Result<void>::create_failure(expected.error());
  }
  if (expected.value() != after) {
    return reject_private_audit_field("private_audit.confirmed_delta");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Prove the exact residual replacement against a scope's unchanged live baseline.
template <typename Decimal>
[[nodiscard]] model::Result<void>
validate_audit_residual_replacement(Decimal before, Decimal old_residual, Decimal new_residual,
                                    Decimal after) {
  const auto removed = before.checked_subtract(old_residual);
  if (!removed) {
    return model::Result<void>::create_failure(removed.error());
  }
  if (removed.value().coefficient() < 0) {
    return reject_private_audit_field("private_audit.residual_baseline");
  }
  const auto expected = removed.value().checked_add(new_residual);
  if (!expected) {
    return model::Result<void>::create_failure(expected.error());
  }
  if (expected.value() != after) {
    return reject_private_audit_field("private_audit.residual_replacement");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Derive a signed endpoint's nonnegative magnitude without overflowing its retained coefficient.
template <typename Decimal>
[[nodiscard]] model::Result<Decimal> derive_audit_absolute_value(Decimal value) {
  if (value.coefficient() >= 0) {
    return model::Result<Decimal>::create_success(value);
  }
  const auto zero = Decimal::from_scaled(0, 0U);
  return zero.value().checked_subtract(value);
}

// --------------------------------------------------------
// Derive the directional maximum using exactly the accepted confirmed-plus-residual formula.
template <typename Decimal>
[[nodiscard]] model::Result<Decimal>
derive_audit_directional_maximum(Decimal confirmed, Decimal reserved_buy, Decimal reserved_sell) {
  const auto buy_endpoint = confirmed.checked_add(reserved_buy);
  if (!buy_endpoint) {
    return model::Result<Decimal>::create_failure(buy_endpoint.error());
  }
  const auto sell_endpoint = confirmed.checked_subtract(reserved_sell);
  if (!sell_endpoint) {
    return model::Result<Decimal>::create_failure(sell_endpoint.error());
  }
  const auto buy_magnitude = derive_audit_absolute_value(buy_endpoint.value());
  if (!buy_magnitude) {
    return model::Result<Decimal>::create_failure(buy_magnitude.error());
  }
  const auto sell_magnitude = derive_audit_absolute_value(sell_endpoint.value());
  if (!sell_magnitude) {
    return model::Result<Decimal>::create_failure(sell_magnitude.error());
  }
  return model::Result<Decimal>::create_success(
      std::max(buy_magnitude.value(), sell_magnitude.value()));
}

// --------------------------------------------------------
// Validate one full scope projection while retaining cross-instrument aggregate contributions.
[[nodiscard]] model::Result<void>
validate_audit_scope_exposure(const risk::RiskScopeExposure& value) {
  if (value.gross_reserved_quote_notional.coefficient() < 0 ||
      value.reserved_buy_quantity.coefficient() < 0 ||
      value.reserved_sell_quantity.coefficient() < 0 ||
      value.reserved_buy_quote_notional.coefficient() < 0 ||
      value.reserved_sell_quote_notional.coefficient() < 0 ||
      value.worst_case_position_quantity.coefficient() < 0 ||
      value.instrument_worst_case_quote_notional.coefficient() < 0 ||
      value.worst_case_position_quote_notional.coefficient() < 0) {
    return reject_private_audit_field("private_audit.scope_nonnegative");
  }
  const auto worst_quantity = derive_audit_directional_maximum(
      value.confirmed_quantity, value.reserved_buy_quantity, value.reserved_sell_quantity);
  if (!worst_quantity) {
    return model::Result<void>::create_failure(worst_quantity.error());
  }
  const auto worst_notional = derive_audit_directional_maximum(value.confirmed_quote_notional,
                                                               value.reserved_buy_quote_notional,
                                                               value.reserved_sell_quote_notional);
  if (!worst_notional) {
    return model::Result<void>::create_failure(worst_notional.error());
  }
  const auto instrument_gross =
      value.reserved_buy_quote_notional.checked_add(value.reserved_sell_quote_notional);
  if (!instrument_gross) {
    return model::Result<void>::create_failure(instrument_gross.error());
  }
  if (value.worst_case_position_quantity != worst_quantity.value() ||
      value.instrument_worst_case_quote_notional != worst_notional.value() ||
      value.worst_case_position_quote_notional < worst_notional.value() ||
      value.gross_reserved_quote_notional < instrument_gross.value()) {
    return reject_private_audit_field("private_audit.scope_directional_maximum");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Validate all affected residual and directional cells before confirming one seven-scope row.
[[nodiscard]] model::Result<void>
validate_audit_scope_replacement(const InventoryScopeAuditEffect& scope,
                                 const risk::ReservationEvidence& before,
                                 const risk::ReservationEvidence& after) {
  auto valid_before = validate_audit_scope_exposure(scope.before);
  if (!valid_before) {
    return valid_before;
  }
  auto valid_after = validate_audit_scope_exposure(scope.after);
  if (!valid_after) {
    return valid_after;
  }
  const bool buy = before.side == execution::OrderSide::Buy;
  auto quantity = validate_audit_residual_replacement(
      buy ? scope.before.reserved_buy_quantity : scope.before.reserved_sell_quantity,
      before.remaining_exposure.quantity, after.remaining_exposure.quantity,
      buy ? scope.after.reserved_buy_quantity : scope.after.reserved_sell_quantity);
  if (!quantity) {
    return quantity;
  }
  auto notional = validate_audit_residual_replacement(
      buy ? scope.before.reserved_buy_quote_notional : scope.before.reserved_sell_quote_notional,
      before.remaining_exposure.quote_notional, after.remaining_exposure.quote_notional,
      buy ? scope.after.reserved_buy_quote_notional : scope.after.reserved_sell_quote_notional);
  if (!notional) {
    return notional;
  }
  auto gross = validate_audit_residual_replacement(
      scope.before.gross_reserved_quote_notional, before.remaining_exposure.quote_notional,
      after.remaining_exposure.quote_notional, scope.after.gross_reserved_quote_notional);
  if (!gross) {
    return gross;
  }
  if ((buy ? scope.before.reserved_sell_quantity != scope.after.reserved_sell_quantity
           : scope.before.reserved_buy_quantity != scope.after.reserved_buy_quantity) ||
      (buy ? scope.before.reserved_sell_quote_notional != scope.after.reserved_sell_quote_notional
           : scope.before.reserved_buy_quote_notional != scope.after.reserved_buy_quote_notional)) {
    return reject_private_audit_field("private_audit.unchanged_opposite_residual");
  }
  const auto untouched_notional = scope.before.worst_case_position_quote_notional.checked_subtract(
      scope.before.instrument_worst_case_quote_notional);
  if (!untouched_notional) {
    return model::Result<void>::create_failure(untouched_notional.error());
  }
  const auto aggregate_notional =
      untouched_notional.value().checked_add(scope.after.instrument_worst_case_quote_notional);
  if (!aggregate_notional) {
    return model::Result<void>::create_failure(aggregate_notional.error());
  }
  if (aggregate_notional.value() != scope.after.worst_case_position_quote_notional) {
    return reject_private_audit_field("private_audit.aggregate_notional_replacement");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reject the complete invalid currency before constructing a bounded retained value.
model::Result<QuoteCurrency> QuoteCurrency::parse_quote_currency(std::string_view value) {
  if (value.size() < 3U || value.size() > 12U || value.front() < 'A' || value.front() > 'Z') {
    return model::Result<QuoteCurrency>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidMetadata, "private_audit.quote_currency"));
  }
  for (const auto character : value) {
    if ((character < 'A' || character > 'Z') && (character < '0' || character > '9')) {
      return model::Result<QuoteCurrency>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidMetadata, "private_audit.quote_currency"));
    }
  }
  return model::Result<QuoteCurrency>::create_success(QuoteCurrency{value});
}

// --------------------------------------------------------
// Copy only validated currency text into its exact bounded storage.
QuoteCurrency::QuoteCurrency(std::string_view value) noexcept
    : size_{static_cast<std::uint8_t>(value.size())} {
  std::copy(value.begin(), value.end(), bytes_.begin());
}

// --------------------------------------------------------
// Reject an over-bound context rather than retain a truncated or allocation-dependent error.
model::Result<BoundedAuditDomainError>
BoundedAuditDomainError::from_domain_error(const model::DomainError& error) {
  const auto code = static_cast<std::uint16_t>(error.code);
  const bool assigned = (code >= 1U && code <= 11U) || (code >= 100U && code <= 102U) ||
                        (code >= 200U && code <= 204U) || (code >= 300U && code <= 301U) ||
                        (code >= 400U && code <= 407U) || (code >= 500U && code <= 510U) ||
                        (code >= 600U && code <= 603U) || (code >= 700U && code <= 701U) ||
                        (code >= 800U && code <= 803U) || (code >= 900U && code <= 906U) ||
                        (code >= 910U && code <= 915U) || (code >= 920U && code <= 930U);
  if (!assigned) {
    return model::Result<BoundedAuditDomainError>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                            "private_audit.error_code"));
  }
  if (error.context.field.size() > 256U) {
    return model::Result<BoundedAuditDomainError>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateEvent,
                                            "private_audit.error_field_capacity"));
  }
  return model::Result<BoundedAuditDomainError>::create_success(BoundedAuditDomainError{error});
}

// --------------------------------------------------------
// Preserve exact field bytes and optional collection position in inline evidence storage.
BoundedAuditDomainError::BoundedAuditDomainError(const model::DomainError& error) noexcept
    : code_{error.code}, field_size_{static_cast<std::uint16_t>(error.context.field.size())},
      collection_index_{error.context.collection_index} {
  std::copy(error.context.field.begin(), error.context.field.end(), field_.begin());
}

// --------------------------------------------------------
// Resolve assigned scope order directly from the closed nominal alternative order.
risk::RiskScopeKind risk_scope_kind(const RiskScopeAuditSubject& subject) noexcept {
  return static_cast<risk::RiskScopeKind>(subject.index() + 1U);
}

// --------------------------------------------------------
// Borrow the active nominal identifier's spelling without converting it to an allocated string.
std::string_view risk_scope_subject_value(const RiskScopeAuditSubject& subject) noexcept {
  return std::visit([](const auto& value) { return value.value(); }, subject);
}

// --------------------------------------------------------
// Compare explicit semantic key fields rather than variant layout, pointer, or object bytes.
bool is_inventory_scope_audit_effect_less(const InventoryScopeAuditEffect& left,
                                          const InventoryScopeAuditEffect& right) noexcept {
  return std::tuple{left.firm_id.value(), risk_scope_kind(left.subject),
                    risk_scope_subject_value(left.subject), left.instrument_id.value(),
                    left.quote_currency.value()} <
         std::tuple{right.firm_id.value(), risk_scope_kind(right.subject),
                    risk_scope_subject_value(right.subject), right.instrument_id.value(),
                    right.quote_currency.value()};
}

// --------------------------------------------------------
// Preserve all original identity components while deriving the semantic audit origin tag.
OriginatingEventIdentity originating_event_identity_from_normalized_input(
    const oms::NormalizedPrivateOrderInput& input) noexcept {
  return std::visit(
      [](const auto& value) -> OriginatingEventIdentity {
        using Origin = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Origin, oms::LocalPrivateEventOrigin>) {
          return OriginatingEventIdentity::create_from_local_event_id(value.event_id);
        } else if constexpr (std::is_same_v<Origin, oms::VenuePrivateEventOrigin>) {
          return OriginatingEventIdentity::create_from_venue_event_key(value.event_key);
        } else {
          return OriginatingEventIdentity::create_from_reconciliation_row(
              value.reconciliation_epoch_id, value.authoritative_cut_id, value.row_ordinal);
        }
      },
      input.origin_value());
}

// --------------------------------------------------------
// Seal the exact proposed input, equality values, and observational linkage by allocation-free
// copy.
PrivateEventEvidence::PrivateEventEvidence(
    oms::NormalizedPrivateOrderInput input,
    oms::PrivateEventIngressSemanticValue ingress_semantic_value,
    oms::PrivateEventResolution resolution,
    std::optional<oms::PrivateTradeSemanticValue> trade_semantic_value,
    recovery::RuntimeEpochId runtime_epoch_id, model::M4RootProvenance root_provenance,
    recovery::JournalReplayProvenance replay_provenance, oms::PrivateEventDisposition disposition,
    std::optional<recovery::JournalSequence> journal_sequence,
    std::optional<recovery::DiagnosticOrdinal> diagnostic_ordinal) noexcept
    : input_{std::move(input)}, ingress_semantic_value_{std::move(ingress_semantic_value)},
      resolution_{std::move(resolution)}, trade_semantic_value_{std::move(trade_semantic_value)},
      runtime_epoch_id_{runtime_epoch_id}, root_provenance_{std::move(root_provenance)},
      replay_provenance_{replay_provenance}, disposition_{disposition},
      journal_sequence_{journal_sequence}, diagnostic_ordinal_{diagnostic_ordinal} {}

// --------------------------------------------------------
// Derive this observation's exact source identity without adding resolved local ownership.
OriginatingEventIdentity PrivateEventEvidence::originating_event() const noexcept {
  return originating_event_identity_from_normalized_input(input_);
}

// --------------------------------------------------------
// Retain reconciliation epoch presence exactly as declared by the original source envelope.
std::optional<recovery::ReconciliationEpochId>
PrivateEventEvidence::reconciliation_epoch_id() const noexcept {
  const auto* origin = std::get_if<oms::ReconciliationPrivateEventOrigin>(&input_.origin_value());
  return origin == nullptr
             ? std::nullopt
             : std::optional<recovery::ReconciliationEpochId>{origin->reconciliation_epoch_id};
}

// --------------------------------------------------------
// Bind one proposed immutable header to already cold-sized and validated backing prefixes.
OrderAuditRecord::OrderAuditRecord(recovery::RuntimeEpochId runtime_epoch_id,
                                   recovery::AuditOrdinal audit_ordinal, M4AuditKind kind,
                                   OriginatingEventIdentity originating_event,
                                   std::optional<oms::NormalizedPrivateOrderInput> source_input,
                                   model::M4RootProvenance root_provenance,
                                   std::optional<model::M4SubjectProvenance> subject_provenance,
                                   std::optional<recovery::JournalSequence> journal_sequence,
                                   std::optional<oms::PrivateEventDisposition> disposition,
                                   CallbackDecision callback_decision,
                                   std::optional<CallbackOrdinalRange> callback_range,
                                   std::optional<BoundedAuditDomainError> error,
                                   std::shared_ptr<const PrivateOrderAuditEffectBuffer> effects,
                                   std::uint32_t effect_count,
                                   std::shared_ptr<const OrderCallbackAuditBuffer> callbacks,
                                   std::uint32_t callback_count) noexcept
    : runtime_epoch_id_{runtime_epoch_id}, audit_ordinal_{audit_ordinal}, kind_{kind},
      originating_event_{std::move(originating_event)}, source_input_{std::move(source_input)},
      root_provenance_{std::move(root_provenance)},
      subject_provenance_{std::move(subject_provenance)}, journal_sequence_{journal_sequence},
      disposition_{disposition}, callback_decision_{callback_decision},
      callback_range_{std::move(callback_range)}, error_{std::move(error)},
      effects_{std::move(effects)}, effect_count_{effect_count}, callbacks_{std::move(callbacks)},
      callback_count_{callback_count} {}

// --------------------------------------------------------
// Borrow one exact populated effect only while this row retains its immutable buffer lease.
const PrivateOrderAuditEffect* OrderAuditRecord::effect_at(std::uint32_t index) const noexcept {
  if (index >= effect_count() || index >= effects_->values_.size()) {
    return nullptr;
  }
  const auto& value = effects_->values_[index];
  return value ? &*value : nullptr;
}

// --------------------------------------------------------
// Borrow one exact prospective notification only while this row retains its immutable buffer lease.
const OrderCallbackAuditValue* OrderAuditRecord::callback_at(std::uint32_t index) const noexcept {
  if (index >= callback_count() || index >= callbacks_->values_.size()) {
    return nullptr;
  }
  const auto& value = callbacks_->values_[index];
  return value ? &*value : nullptr;
}

// --------------------------------------------------------
// Reject malformed projections, ownership, or arithmetic before retaining any proposed audit row.
model::Result<void> validate_private_order_audit_effect(const PrivateOrderAuditEffect& effect,
                                                        const model::M4RootProvenance& owner_root) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Known-order evidence must retain every local subject and one unchanged reservation identity.
  const auto& subject = effect.provenance.subject();
  if (effect.provenance.root() != owner_root || !subject || !subject->firm_id() ||
      !subject->desk_id() || !subject->bot_id() || !subject->strategy_id() || !subject->route() ||
      !subject->instrument() ||
      effect.reservation_before.reservation_id != effect.reservation_after.reservation_id ||
      effect.reservation_before.side != effect.reservation_after.side ||
      effect.reservation_before.exposure != effect.reservation_after.exposure ||
      (effect.reservation_before.state != risk::ReservationState::Held &&
       effect.reservation_before != effect.reservation_after)) {
    return reject_private_audit_field("private_audit.known_subject");
  }
  auto before = validate_audit_reservation(effect.reservation_before);
  if (!before) {
    return before;
  }
  auto after = validate_audit_reservation(effect.reservation_after);
  if (!after) {
    return after;
  }
  if (!is_audit_oms_projection_consistent(effect.oms_before, effect.reservation_before) ||
      !is_audit_oms_projection_consistent(effect.oms_after, effect.reservation_after) ||
      (effect.oms_before.execution_evidence_observed &&
       !effect.oms_after.execution_evidence_observed) ||
      (effect.oms_before.exchange_mapping_established_by_execution &&
       !effect.oms_after.exchange_mapping_established_by_execution) ||
      (effect.oms_before.exchange_acknowledged && !effect.oms_after.exchange_acknowledged) ||
      (effect.oms_before.exchange_order_id &&
       effect.oms_before.exchange_order_id != effect.oms_after.exchange_order_id)) {
    return reject_private_audit_field("private_audit.oms_reservation_pair");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Optional safety evidence must use assigned reasons and the monotonic ordinary severity rule.
  if (effect.account_safety_transition) {
    const auto& safety = *effect.account_safety_transition;
    const auto reason = static_cast<std::uint8_t>(safety.reason);
    const auto before_state = static_cast<std::uint8_t>(safety.before);
    const auto after_state = static_cast<std::uint8_t>(safety.after);
    const auto minimum_state = reason <= 5U ? risk::AccountSafetyState::ReconciliationRequired
                                            : risk::AccountSafetyState::Quarantined;
    const auto required_state = std::max(before_state, static_cast<std::uint8_t>(minimum_state));
    if (reason < 1U || reason > 19U || before_state < 1U || before_state > 3U || after_state < 1U ||
        after_state > 3U || after_state < before_state || after_state != required_state) {
      return reject_private_audit_field("private_audit.account_safety");
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Confirmed economics can only advance; every scope must retain the same exact signed transfer.
  const auto quantity_increment =
      effect.reservation_after.cumulative_confirmed_exposure.quantity.checked_subtract(
          effect.reservation_before.cumulative_confirmed_exposure.quantity);
  if (!quantity_increment) {
    return model::Result<void>::create_failure(quantity_increment.error());
  }
  const auto notional_increment =
      effect.reservation_after.cumulative_confirmed_exposure.quote_notional.checked_subtract(
          effect.reservation_before.cumulative_confirmed_exposure.quote_notional);
  if (!notional_increment) {
    return model::Result<void>::create_failure(notional_increment.error());
  }
  const auto quantity_delta =
      derive_signed_audit_increment(quantity_increment.value(), effect.reservation_before.side);
  if (!quantity_delta) {
    return model::Result<void>::create_failure(quantity_delta.error());
  }
  const auto notional_delta =
      derive_signed_audit_increment(notional_increment.value(), effect.reservation_before.side);
  if (!notional_delta) {
    return model::Result<void>::create_failure(notional_delta.error());
  }
  if (!effect.inventory_effects) {
    if (effect.reservation_before != effect.reservation_after) {
      return reject_private_audit_field("private_audit.missing_economics");
    }
    return model::Result<void>::create_success();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Seven rows retain complete canonical keys, exact confirmed arithmetic, and closure count
  // change.
  const auto& scopes = *effect.inventory_effects;
  for (std::size_t index = 0U; index < scopes.size(); ++index) {
    const auto& scope = scopes[index];
    if (scope.firm_id != *subject->firm_id() ||
        scope.instrument_id != subject->instrument()->instrument_id ||
        risk_scope_kind(scope.subject) != static_cast<risk::RiskScopeKind>(index + 1U) ||
        !does_scope_subject_match_order(scope.subject, *subject) ||
        scope.quote_currency != scopes.front().quote_currency ||
        scope.signed_quantity_delta != quantity_delta.value() ||
        scope.signed_notional_delta != notional_delta.value() ||
        (index != 0U && !is_inventory_scope_audit_effect_less(scopes[index - 1U], scope))) {
      return reject_private_audit_field("private_audit.inventory_keys");
    }
    auto residuals = validate_audit_scope_replacement(scope, effect.reservation_before,
                                                      effect.reservation_after);
    if (!residuals) {
      return residuals;
    }
    auto quantity =
        validate_audit_confirmed_delta(scope.before.confirmed_quantity, scope.signed_quantity_delta,
                                       scope.after.confirmed_quantity);
    if (!quantity) {
      return quantity;
    }
    auto notional = validate_audit_confirmed_delta(scope.before.confirmed_quote_notional,
                                                   scope.signed_notional_delta,
                                                   scope.after.confirmed_quote_notional);
    if (!notional) {
      return notional;
    }
    const bool closes = effect.reservation_before.state == risk::ReservationState::Held &&
                        effect.reservation_after.state != risk::ReservationState::Held;
    if ((closes && (scope.before.open_order_count == 0U ||
                    scope.after.open_order_count != scope.before.open_order_count - 1U)) ||
        (!closes && scope.before.open_order_count != scope.after.open_order_count)) {
      return reject_private_audit_field("private_audit.open_order_count");
    }
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::trace
