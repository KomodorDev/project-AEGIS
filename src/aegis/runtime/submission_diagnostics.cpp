// Purpose: validate M3 submission diagnostic profiles and preserve their bounded owner-local
// prefix with explicit saturating loss accounting.

#include "aegis/runtime/submission_diagnostics.hpp"

#include "aegis/model/domain_error.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::runtime {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// --------------------------------------------------------
// Keep the persisted diagnostic vocabulary closed at the append boundary.
[[nodiscard]] bool is_known(SubmissionDiagnosticKind kind) noexcept {
  switch (kind) {
  case SubmissionDiagnosticKind::EvidenceCapacityExceeded:
  case SubmissionDiagnosticKind::ReentryDetected:
  case SubmissionDiagnosticKind::ReservationReleased:
  case SubmissionDiagnosticKind::UnknownExposureRetained:
  case SubmissionDiagnosticKind::InternalInvariantFailure:
  case SubmissionDiagnosticKind::MeasurementUnavailable:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Reject unassigned stage values before they can enter retained diagnostics.
[[nodiscard]] bool is_known(execution::SubmissionStage stage) noexcept {
  switch (stage) {
  case execution::SubmissionStage::Context:
  case execution::SubmissionStage::Evidence:
  case execution::SubmissionStage::Route:
  case execution::SubmissionStage::CanonicalValidation:
  case execution::SubmissionStage::Identity:
  case execution::SubmissionStage::Policy:
  case execution::SubmissionStage::Risk:
  case execution::SubmissionStage::Oms:
  case execution::SubmissionStage::Encoding:
  case execution::SubmissionStage::Initiation:
  case execution::SubmissionStage::Internal:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Reject unassigned persisted reasons without duplicating higher-level result construction.
[[nodiscard]] bool is_known(execution::SubmissionReason reason) noexcept {
  switch (reason) {
  case execution::SubmissionReason::None:
  case execution::SubmissionReason::ContextInactive:
  case execution::SubmissionReason::WrongOwner:
  case execution::SubmissionReason::SubmissionReentry:
  case execution::SubmissionReason::EvidenceCapacityExceeded:
  case execution::SubmissionReason::SubmissionAttemptExhausted:
  case execution::SubmissionReason::SubmissionCapabilityUnavailable:
  case execution::SubmissionReason::RouteNotFound:
  case execution::SubmissionReason::RouteNotOwned:
  case execution::SubmissionReason::RouteDisabled:
  case execution::SubmissionReason::RouteInstrumentMismatch:
  case execution::SubmissionReason::UnsupportedSide:
  case execution::SubmissionReason::UnsupportedOrderType:
  case execution::SubmissionReason::UnsupportedTimeInForce:
  case execution::SubmissionReason::PriceNotPositive:
  case execution::SubmissionReason::PriceScaleExceeded:
  case execution::SubmissionReason::PriceTickMismatch:
  case execution::SubmissionReason::QuantityNotPositive:
  case execution::SubmissionReason::QuantityScaleExceeded:
  case execution::SubmissionReason::QuantityBelowMinimum:
  case execution::SubmissionReason::QuantityStepMismatch:
  case execution::SubmissionReason::UnsupportedContractEconomics:
  case execution::SubmissionReason::OrderIdentityExhausted:
  case execution::SubmissionReason::RiskArithmeticFailure:
  case execution::SubmissionReason::SingleOrderQuantityExceeded:
  case execution::SubmissionReason::SingleOrderNotionalExceeded:
  case execution::SubmissionReason::OpenOrderCountExceeded:
  case execution::SubmissionReason::GrossReservedNotionalExceeded:
  case execution::SubmissionReason::WorstCasePositionQuantityExceeded:
  case execution::SubmissionReason::WorstCasePositionNotionalExceeded:
  case execution::SubmissionReason::ReservationCapacityExceeded:
  case execution::SubmissionReason::DuplicateOrderIdentity:
  case execution::SubmissionReason::OmsCapacityExceeded:
  case execution::SubmissionReason::EncodingFailed:
  case execution::SubmissionReason::InitiationDefinitelyFailed:
  case execution::SubmissionReason::InitiationOutcomeUnknown:
  case execution::SubmissionReason::SubmissionRuntimeFaulted:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Keep risk-scope bytes on the assigned seven-scope vocabulary.
[[nodiscard]] bool is_known(risk::RiskScopeKind scope) noexcept {
  switch (scope) {
  case risk::RiskScopeKind::Bot:
  case risk::RiskScopeKind::Desk:
  case risk::RiskScopeKind::Firm:
  case risk::RiskScopeKind::Account:
  case risk::RiskScopeKind::Route:
  case risk::RiskScopeKind::Instrument:
  case risk::RiskScopeKind::Venue:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Enforce common optional-field dependencies before applying a kind-specific profile.
[[nodiscard]] bool has_valid_identity_chain(const SubmissionDiagnosticFields& fields) noexcept {
  if (fields.callback_ordinal && !fields.owner_turn_ordinal) {
    return false;
  }
  if (fields.order_id && !fields.attempt_id) {
    return false;
  }
  if (fields.reservation_id && (!fields.attempt_id || !fields.order_id)) {
    return false;
  }
  if (fields.stage && !is_known(*fields.stage)) {
    return false;
  }
  if (fields.reason && (!fields.stage || !is_known(*fields.reason))) {
    return false;
  }
  if (fields.risk_scope && (!fields.stage || *fields.stage != execution::SubmissionStage::Risk ||
                            !is_known(*fields.risk_scope))) {
    return false;
  }
  return fields.occurrence_count != 0U;
}

// --------------------------------------------------------
// Recognize the complete callback-bound identity available after evidence preflight succeeds.
[[nodiscard]] bool has_attempt_context(const SubmissionDiagnosticFields& fields) noexcept {
  return fields.attempt_id && fields.owner_turn_ordinal && fields.callback_ordinal;
}

// --------------------------------------------------------
// Require exact values for fields whose kind semantics are fixed by ADR-0009.
[[nodiscard]] bool has_stage_reason(const SubmissionDiagnosticFields& fields,
                                    execution::SubmissionStage stage,
                                    execution::SubmissionReason reason) noexcept {
  return fields.stage == stage && fields.reason == reason;
}

// --------------------------------------------------------
// Apply the narrowest stable profile without inventing optional evidence unavailable at a fault.
[[nodiscard]] bool is_valid_profile(SubmissionDiagnosticKind kind,
                                    const SubmissionDiagnosticFields& fields) noexcept {
  if (!is_known(kind) || !has_valid_identity_chain(fields)) {
    return false;
  }

  switch (kind) {
  case SubmissionDiagnosticKind::EvidenceCapacityExceeded:
    return has_attempt_context(fields) && !fields.order_id && !fields.reservation_id &&
           !fields.risk_scope && fields.occurrence_count == 1U &&
           has_stage_reason(fields, execution::SubmissionStage::Evidence,
                            execution::SubmissionReason::EvidenceCapacityExceeded);
  case SubmissionDiagnosticKind::ReentryDetected:
    return has_attempt_context(fields) && !fields.risk_scope &&
           has_stage_reason(fields, execution::SubmissionStage::Context,
                            execution::SubmissionReason::SubmissionReentry);
  case SubmissionDiagnosticKind::ReservationReleased:
    return has_attempt_context(fields) && fields.order_id && fields.reservation_id &&
           fields.stage && fields.reason && *fields.reason != execution::SubmissionReason::None &&
           *fields.reason != execution::SubmissionReason::InitiationOutcomeUnknown &&
           !fields.risk_scope && fields.occurrence_count == 1U;
  case SubmissionDiagnosticKind::UnknownExposureRetained:
    return has_attempt_context(fields) && fields.order_id && fields.reservation_id &&
           !fields.risk_scope && fields.occurrence_count == 1U &&
           has_stage_reason(fields, execution::SubmissionStage::Initiation,
                            execution::SubmissionReason::InitiationOutcomeUnknown);
  case SubmissionDiagnosticKind::InternalInvariantFailure:
    return !fields.risk_scope && fields.occurrence_count == 1U &&
           (!fields.stage || *fields.stage == execution::SubmissionStage::Internal) &&
           (!fields.reason ||
            *fields.reason == execution::SubmissionReason::SubmissionRuntimeFaulted);
  case SubmissionDiagnosticKind::MeasurementUnavailable:
    return has_attempt_context(fields) && !fields.risk_scope && fields.occurrence_count == 1U;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Return the stable programmer-contract failure for a malformed internal profile.
[[nodiscard]] model::Result<void> invalid(std::string_view field) {
  return model::Result<void>::failure(
      DomainError::at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------
// Match the immutable identity and classification of repeated callback-local submission re-entry;
// occurrence count is deliberately excluded because it is the saturating aggregate.
[[nodiscard]] bool same_reentry_observation(const SubmissionDiagnosticFields& lhs,
                                            const SubmissionDiagnosticFields& rhs) noexcept {
  return lhs.attempt_id == rhs.attempt_id && lhs.owner_turn_ordinal == rhs.owner_turn_ordinal &&
         lhs.callback_ordinal == rhs.callback_ordinal && lhs.order_id == rhs.order_id &&
         lhs.reservation_id == rhs.reservation_id && lhs.stage == rhs.stage &&
         lhs.reason == rhs.reason && lhs.risk_scope == rhs.risk_scope;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reserve all record storage at construction so valid hot-path appends cannot allocate.
SubmissionDiagnosticSink::SubmissionDiagnosticSink(SubmissionDiagnosticProvenance provenance,
                                                   std::uint32_t capacity)
    : provenance_{std::move(provenance)}, capacity_{capacity} {
  records_.reserve(capacity_);
}

// --------------------------------------------------------
// Validate the common dependency graph and the selected kind profile without mutation.
model::Result<void>
SubmissionDiagnosticSink::validate(SubmissionDiagnosticKind kind,
                                   const SubmissionDiagnosticFields& fields) const {
  if (!is_valid_profile(kind, fields)) {
    return invalid("submission_diagnostic.fields");
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Preserve the earliest valid observations and account for all later observations without failure.
model::Result<void> SubmissionDiagnosticSink::append(SubmissionDiagnosticKind kind,
                                                     SubmissionDiagnosticFields fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Invalid observations remain programmer errors even when diagnostic storage is saturated.
  auto valid = validate(kind, fields);
  if (!valid) {
    return valid;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Repeated nested calls retain one observation and saturate its count without allocating or
  // consuming another prefix position.
  if (kind == SubmissionDiagnosticKind::ReentryDetected) {
    for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
      if (record->kind == kind && same_reentry_observation(record->fields, fields)) {
        if (fields.occurrence_count >
            std::numeric_limits<std::uint64_t>::max() - record->fields.occurrence_count) {
          record->fields.occurrence_count = std::numeric_limits<std::uint64_t>::max();
        } else {
          record->fields.occurrence_count += fields.occurrence_count;
        }
        return model::Result<void>::success();
      }
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Once no aggregate accepts this observation, retain only the configured earliest prefix.
  if (records_.size() == capacity_) {
    if (dropped_count_ != std::numeric_limits<std::uint64_t>::max()) {
      ++dropped_count_;
    }
    return model::Result<void>::success();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity is uint32-bounded, so the next one-based uint64 ordinal cannot overflow.
  ++accepted_count_;
  records_.push_back(
      SubmissionDiagnosticRecord{accepted_count_, kind, provenance_, std::move(fields)});
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Borrow one retained prefix position without exposing mutable storage.
const SubmissionDiagnosticRecord*
SubmissionDiagnosticSink::at(std::size_t chronological_index) const noexcept {
  if (chronological_index >= records_.size()) {
    return nullptr;
  }
  return &records_[chronological_index];
}

// --------------------------------------------------------

} // namespace aegis::runtime
