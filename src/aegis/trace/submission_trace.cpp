// Purpose: validate exact M3 submission evidence shapes and causal sequences, then encode the
// accepted bounded prefix as portable positional AEGISSTS schema-one bytes.

#include "aegis/trace/submission_trace.hpp"

#include "aegis/model/domain_error.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::trace {
namespace {

using model::DomainError;
using model::DomainErrorCode;

inline constexpr std::string_view submission_stream_magic = "AEGISSTS";

// ########################################################################
// This schema-local writer exposes only the positional primitives assigned by ADR-0009.
class CanonicalSubmissionTraceWriter final {
public:

  // --------------------------------------------------------
  // Append one byte only after proving that the backing vector can grow.
  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
    return true;
  }

  // --------------------------------------------------------
  // Append fixed schema magic without a length prefix.
  [[nodiscard]] bool append_ascii_raw(std::string_view value) {
    if (!can_grow(value.size())) {
      return false;
    }
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return true;
  }

  // --------------------------------------------------------
  // Append opaque canonical bytes without reinterpretation.
  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  // --------------------------------------------------------
  // Append one unsigned 16-bit value in canonical big-endian byte order.
  [[nodiscard]] bool append_u16(std::uint16_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Append one unsigned 32-bit value in canonical big-endian byte order.
  [[nodiscard]] bool append_u32(std::uint32_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 24U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 16U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Append one unsigned 64-bit value in canonical big-endian byte order.
  [[nodiscard]] bool append_u64(std::uint64_t value) {
    // Interesting syntax: the explicit zero break prevents unsigned loop wrap after byte eight.
    for (unsigned int shift = 56U;; shift -= 8U) {
      if (!append_byte(static_cast<std::uint8_t>((value >> shift) & 0xffU))) {
        return false;
      }
      if (shift == 0U) {
        break;
      }
    }
    return true;
  }

  // --------------------------------------------------------
  // Preserve a signed decimal coefficient's two's-complement bits in big-endian order.
  [[nodiscard]] bool append_i64(std::int64_t value) {
    return append_u64(std::bit_cast<std::uint64_t>(value));
  }

  // --------------------------------------------------------
  // Encode validated identifier text behind the schema's unsigned 16-bit length.
  [[nodiscard]] bool append_string(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
      return false;
    }
    return append_u16(static_cast<std::uint16_t>(value.size())) && append_ascii_raw(value);
  }

  // --------------------------------------------------------
  // Encode any nominal exact decimal as signed coefficient plus one-byte canonical scale.
  template <typename Decimal> [[nodiscard]] bool append_decimal(const Decimal& value) {
    return append_i64(value.coefficient()) && append_byte(value.scale());
  }

  // --------------------------------------------------------
  // Encode the canonical zero used by count-domain risk evidence.
  [[nodiscard]] bool append_zero_decimal() { return append_i64(0) && append_byte(0U); }

  // --------------------------------------------------------
  // Transfer the complete canonical byte prefix out of the consumed writer.
  [[nodiscard]] std::vector<std::byte> take_canonical_bytes() && { return std::move(bytes_); }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Return whether the backing vector can accept the complete additional byte count.
  [[nodiscard]] bool can_grow(std::size_t additional) const noexcept {
    return additional <= bytes_.max_size() - bytes_.size();
  }

  // --------------------------------------------------------
  // Own the canonical prefix until a successful encoder consumes the writer.
  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Return one stable malformed-evidence error without carrying authored values as text.
[[nodiscard]] model::Result<void> create_invalid_submission_trace_result(std::string_view field) {
  return model::Result<void>::create_failure(
      DomainError::create_at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------
// Keep every persisted event on its assigned schema-one number.
[[nodiscard]] bool is_known(SubmissionTraceEventKind kind) noexcept {
  switch (kind) {
  case SubmissionTraceEventKind::Attempt:
  case SubmissionTraceEventKind::RouteAuthorized:
  case SubmissionTraceEventKind::CanonicalValidated:
  case SubmissionTraceEventKind::IdentityGenerated:
  case SubmissionTraceEventKind::RiskReserved:
  case SubmissionTraceEventKind::RiskRejected:
  case SubmissionTraceEventKind::OmsAdmitted:
  case SubmissionTraceEventKind::OmsNonAdmission:
  case SubmissionTraceEventKind::Encoded:
  case SubmissionTraceEventKind::EncodingFailed:
  case SubmissionTraceEventKind::InitiationDefinitelyFailed:
  case SubmissionTraceEventKind::WriteInitiated:
  case SubmissionTraceEventKind::SubmissionUnknown:
  case SubmissionTraceEventKind::ReservationReleased:
  case SubmissionTraceEventKind::ReentryRejected:
  case SubmissionTraceEventKind::SubmissionCompleted:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Recognize only the five M3 OMS states assigned to the schema-one submission-trace byte.
[[nodiscard]] bool is_known_m3_submission_state(oms::OutboundOrderState state) noexcept {
  switch (state) {
  case oms::OutboundOrderState::PendingEncoding:
  case oms::OutboundOrderState::PendingInitiation:
  case oms::OutboundOrderState::WriteInitiated:
  case oms::OutboundOrderState::SubmissionUnknown:
  case oms::OutboundOrderState::LocallyFailed:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Recognize every assigned fake-initiation outcome and its exact accepted-write shape.
[[nodiscard]] bool is_valid(const SubmissionInitiationEvidence& evidence) noexcept {
  switch (evidence.outcome) {
  case execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance:
    return !evidence.accepted_write_ordinal.has_value();
  case execution::FakeInitiationOutcome::AcceptedAndInitiated:
  case execution::FakeInitiationOutcome::AcceptedThenOutcomeLost:
    return evidence.accepted_write_ordinal.has_value();
  default:
    return false;
  }
}

// --------------------------------------------------------
// Keep risk evidence on one assigned scope and exactly one nominal measure representation.
[[nodiscard]] bool is_valid(const execution::RiskLimitEvidence& evidence) noexcept {
  const auto scope_byte = static_cast<std::uint8_t>(evidence.scope());
  if (scope_byte < static_cast<std::uint8_t>(risk::RiskScopeKind::Bot) ||
      scope_byte > static_cast<std::uint8_t>(risk::RiskScopeKind::Venue)) {
    return false;
  }

  switch (evidence.measure_kind()) {
  case execution::RiskMeasureKind::Quantity:
    return evidence.observed_quantity() && evidence.quantity_limit() &&
           !evidence.observed_notional() && !evidence.notional_limit() &&
           !evidence.observed_count() && !evidence.count_limit() &&
           *evidence.observed_quantity() > *evidence.quantity_limit();
  case execution::RiskMeasureKind::QuoteNotional:
    return !evidence.observed_quantity() && !evidence.quantity_limit() &&
           evidence.observed_notional() && evidence.notional_limit() &&
           !evidence.observed_count() && !evidence.count_limit() &&
           *evidence.observed_notional() > *evidence.notional_limit();
  case execution::RiskMeasureKind::OrderCount:
    return !evidence.observed_quantity() && !evidence.quantity_limit() &&
           !evidence.observed_notional() && !evidence.notional_limit() &&
           evidence.observed_count() && evidence.count_limit() &&
           *evidence.observed_count() > *evidence.count_limit();
  default:
    return false;
  }
}

// --------------------------------------------------------
// Validate the closed final-result triple without inventing an acknowledgement state.
[[nodiscard]] bool is_valid(const SubmissionFinalResult& result) noexcept {
  switch (result.disposition) {
  case execution::SubmitDisposition::WriteInitiated:
    return result.stage == execution::SubmissionStage::Initiation &&
           result.reason == execution::SubmissionReason::None;
  case execution::SubmitDisposition::SubmissionUnknown:
    return result.stage == execution::SubmissionStage::Initiation &&
           result.reason == execution::SubmissionReason::InitiationOutcomeUnknown;
  case execution::SubmitDisposition::LocallyRejected:
    return result.reason != execution::SubmissionReason::None &&
           result.reason != execution::SubmissionReason::InitiationOutcomeUnknown;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Route failures are the only ordinary completions immediately after Attempt.
[[nodiscard]] bool is_route_rejection(const SubmissionFinalResult& result) noexcept {
  if (result.disposition != execution::SubmitDisposition::LocallyRejected ||
      result.stage != execution::SubmissionStage::Route) {
    return false;
  }
  switch (result.reason) {
  case execution::SubmissionReason::RouteNotFound:
  case execution::SubmissionReason::RouteNotOwned:
  case execution::SubmissionReason::RouteDisabled:
  case execution::SubmissionReason::RouteInstrumentMismatch:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Canonical-validation failures are closed to the assigned economics reasons.
[[nodiscard]] bool is_canonical_rejection(const SubmissionFinalResult& result) noexcept {
  if (result.disposition != execution::SubmitDisposition::LocallyRejected ||
      result.stage != execution::SubmissionStage::CanonicalValidation) {
    return false;
  }
  switch (result.reason) {
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
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Match each risk reason to the exact optional measure shape persisted at RiskRejected.
[[nodiscard]] bool is_risk_rejection(const SubmissionFinalResult& result,
                                     const std::optional<execution::RiskLimitEvidence>& evidence) {
  if (result.disposition != execution::SubmitDisposition::LocallyRejected ||
      result.stage != execution::SubmissionStage::Risk) {
    return false;
  }
  switch (result.reason) {
  case execution::SubmissionReason::RiskArithmeticFailure:
  case execution::SubmissionReason::ReservationCapacityExceeded:
    return !evidence.has_value();
  case execution::SubmissionReason::SingleOrderQuantityExceeded:
  case execution::SubmissionReason::WorstCasePositionQuantityExceeded:
    return evidence && evidence->measure_kind() == execution::RiskMeasureKind::Quantity;
  case execution::SubmissionReason::SingleOrderNotionalExceeded:
  case execution::SubmissionReason::GrossReservedNotionalExceeded:
  case execution::SubmissionReason::WorstCasePositionNotionalExceeded:
    return evidence && evidence->measure_kind() == execution::RiskMeasureKind::QuoteNotional;
  case execution::SubmissionReason::OpenOrderCountExceeded:
    return evidence && evidence->measure_kind() == execution::RiskMeasureKind::OrderCount;
  default:
    return false;
  }
}

// --------------------------------------------------------
// A current-attempt invariant fault is the only generic early completion escape.
[[nodiscard]] bool is_internal_rejection(const SubmissionFinalResult& result) noexcept {
  return result.disposition == execution::SubmitDisposition::LocallyRejected &&
         result.stage == execution::SubmissionStage::Internal &&
         result.reason == execution::SubmissionReason::SubmissionRuntimeFaulted;
}

// --------------------------------------------------------
// Validate projection and downstream economics relationships that nominal types alone cannot prove.
[[nodiscard]] bool has_valid_relationships(const SubmissionTraceFields& fields) noexcept {
  if (fields.authorized_projection &&
      (fields.authorized_projection->route_id != fields.context.request.route_id ||
       fields.authorized_projection->instrument_id != fields.context.request.instrument_id)) {
    return false;
  }
  if (fields.reservation_id &&
      fields.reservation_id->value() != fields.context.attempt_id.value()) {
    return false;
  }
  if (fields.approved_exposure &&
      (fields.approved_exposure->quantity != fields.context.request.quantity ||
       fields.approved_exposure->quantity.coefficient() <= 0 ||
       fields.approved_exposure->quote_notional.coefficient() <= 0)) {
    return false;
  }
  if (fields.risk_rejection && !is_valid(*fields.risk_rejection)) {
    return false;
  }
  if (fields.oms_state && !is_known_m3_submission_state(*fields.oms_state)) {
    return false;
  }
  if (fields.encoding &&
      (fields.encoding->byte_length == 0U ||
       fields.encoding->byte_length > execution::maximum_encoded_fake_order_bytes)) {
    return false;
  }
  if (fields.initiation && !is_valid(*fields.initiation)) {
    return false;
  }
  switch (fields.release_transition) {
  case SubmissionReleaseTransition::None:
  case SubmissionReleaseTransition::Released:
  case SubmissionReleaseTransition::Retained:
    break;
  default:
    return false;
  }
  return !fields.final_result || is_valid(*fields.final_result);
}

// --------------------------------------------------------
// Enforce the fixed presence profile introduced at each named causal event.
[[nodiscard]] bool has_valid_event_shape(SubmissionTraceEventKind kind,
                                         const SubmissionTraceFields& fields) noexcept {
  if (!is_known(kind) || !has_valid_relationships(fields)) {
    return false;
  }

  const bool projection = fields.authorized_projection.has_value();
  const bool order = fields.order_id.has_value();
  const bool reservation = fields.reservation_id.has_value();
  const bool exposure = fields.approved_exposure.has_value();
  const bool risk_rejection = fields.risk_rejection.has_value();
  const bool oms_state = fields.oms_state.has_value();
  const bool encoding = fields.encoding.has_value();
  const bool initiation = fields.initiation.has_value();
  const bool final_result = fields.final_result.has_value();
  const bool none = fields.release_transition == SubmissionReleaseTransition::None;

  switch (kind) {
  case SubmissionTraceEventKind::Attempt:
    return !projection && !order && !reservation && !exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && !final_result;
  case SubmissionTraceEventKind::RouteAuthorized:
  case SubmissionTraceEventKind::CanonicalValidated:
    return projection && !order && !reservation && !exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && !final_result;
  case SubmissionTraceEventKind::IdentityGenerated:
    return projection && order && !reservation && !exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && !final_result;
  case SubmissionTraceEventKind::RiskReserved:
    return projection && order && reservation && exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && !final_result;
  case SubmissionTraceEventKind::RiskRejected:
    return projection && order && !reservation && !exposure && !oms_state && !encoding &&
           !initiation && none && !final_result;
  case SubmissionTraceEventKind::OmsAdmitted:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::PendingEncoding && !encoding &&
           !initiation && none && !final_result;
  case SubmissionTraceEventKind::OmsNonAdmission:
    return projection && order && reservation && exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && !final_result;
  case SubmissionTraceEventKind::Encoded:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::PendingInitiation && encoding &&
           !initiation && none && !final_result;
  case SubmissionTraceEventKind::EncodingFailed:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::LocallyFailed && !encoding &&
           !initiation && none && !final_result;
  case SubmissionTraceEventKind::InitiationDefinitelyFailed:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::LocallyFailed && encoding && initiation &&
           fields.initiation->outcome ==
               execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance &&
           none && !final_result;
  case SubmissionTraceEventKind::WriteInitiated:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::WriteInitiated && encoding && initiation &&
           fields.initiation->outcome == execution::FakeInitiationOutcome::AcceptedAndInitiated &&
           fields.release_transition == SubmissionReleaseTransition::Retained && !final_result;
  case SubmissionTraceEventKind::SubmissionUnknown:
    return projection && order && reservation && exposure && !risk_rejection && oms_state &&
           *fields.oms_state == oms::OutboundOrderState::SubmissionUnknown && encoding &&
           initiation &&
           fields.initiation->outcome ==
               execution::FakeInitiationOutcome::AcceptedThenOutcomeLost &&
           fields.initiation->accepted_write_ordinal &&
           fields.release_transition == SubmissionReleaseTransition::Retained && !final_result;
  case SubmissionTraceEventKind::ReservationReleased:
    return projection && order && reservation && exposure && !risk_rejection &&
           fields.release_transition == SubmissionReleaseTransition::Released && !final_result;
  case SubmissionTraceEventKind::ReentryRejected:
    return !projection && !order && !reservation && !exposure && !risk_rejection && !oms_state &&
           !encoding && !initiation && none && final_result &&
           fields.final_result->disposition == execution::SubmitDisposition::LocallyRejected &&
           fields.final_result->stage == execution::SubmissionStage::Context &&
           fields.final_result->reason == execution::SubmissionReason::SubmissionReentry;
  case SubmissionTraceEventKind::SubmissionCompleted:
    return final_result;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Ignore the one nested re-entry exception when resolving the ordinary causal predecessor.
[[nodiscard]] const SubmissionTraceRecord*
last_ordinary(std::span<const SubmissionTraceRecord> records) noexcept {
  for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
    if (iterator->kind() != SubmissionTraceEventKind::ReentryRejected) {
      return &*iterator;
    }
  }
  return nullptr;
}

// --------------------------------------------------------
// Resolve the ordinary event immediately before a supplied ordinary record.
[[nodiscard]] const SubmissionTraceRecord*
ordinary_before(std::span<const SubmissionTraceRecord> records,
                const SubmissionTraceRecord& boundary) noexcept {
  const auto boundary_ordinal = boundary.ordinal().value();
  for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
    if (iterator->ordinal().value() >= boundary_ordinal ||
        iterator->kind() == SubmissionTraceEventKind::ReentryRejected) {
      continue;
    }
    return &*iterator;
  }
  return nullptr;
}

// --------------------------------------------------------
// Compare the outer identity fields that a nested request is forbidden to replace.
[[nodiscard]] bool has_same_outer_context(const SubmissionTraceContext& nested,
                                          const SubmissionTraceContext& outer) noexcept {
  return nested.attempt_id == outer.attempt_id &&
         nested.owner_turn_ordinal == outer.owner_turn_ordinal &&
         nested.callback_ordinal == outer.callback_ordinal &&
         nested.callback_processing_nanoseconds == outer.callback_processing_nanoseconds &&
         nested.attribution == outer.attribution;
}

// --------------------------------------------------------
// Prove that at most the first recursive call receives the reserved canonical re-entry slot.
[[nodiscard]] bool has_reentry_for_attempt(std::span<const SubmissionTraceRecord> records,
                                           model::SubmissionAttemptId attempt_id) noexcept {
  for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
    if (iterator->fields().context.attempt_id != attempt_id) {
      break;
    }
    if (iterator->kind() == SubmissionTraceEventKind::ReentryRejected) {
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------
// Define every allowed consecutive ordinary event in the M3 branch table.
[[nodiscard]] bool can_follow_in_submission_trace(SubmissionTraceEventKind previous,
                                                  SubmissionTraceEventKind current) noexcept {
  switch (previous) {
  case SubmissionTraceEventKind::Attempt:
    return current == SubmissionTraceEventKind::RouteAuthorized ||
           current == SubmissionTraceEventKind::SubmissionCompleted;
  case SubmissionTraceEventKind::RouteAuthorized:
    return current == SubmissionTraceEventKind::CanonicalValidated ||
           current == SubmissionTraceEventKind::SubmissionCompleted;
  case SubmissionTraceEventKind::CanonicalValidated:
    return current == SubmissionTraceEventKind::IdentityGenerated ||
           current == SubmissionTraceEventKind::SubmissionCompleted;
  case SubmissionTraceEventKind::IdentityGenerated:
    return current == SubmissionTraceEventKind::RiskReserved ||
           current == SubmissionTraceEventKind::RiskRejected ||
           current == SubmissionTraceEventKind::SubmissionCompleted;
  case SubmissionTraceEventKind::RiskReserved:
    return current == SubmissionTraceEventKind::OmsAdmitted ||
           current == SubmissionTraceEventKind::OmsNonAdmission ||
           current == SubmissionTraceEventKind::ReservationReleased;
  case SubmissionTraceEventKind::RiskRejected:
    return current == SubmissionTraceEventKind::SubmissionCompleted;
  case SubmissionTraceEventKind::OmsAdmitted:
    return current == SubmissionTraceEventKind::Encoded ||
           current == SubmissionTraceEventKind::EncodingFailed ||
           current == SubmissionTraceEventKind::ReservationReleased;
  case SubmissionTraceEventKind::OmsNonAdmission:
  case SubmissionTraceEventKind::EncodingFailed:
  case SubmissionTraceEventKind::InitiationDefinitelyFailed:
    return current == SubmissionTraceEventKind::ReservationReleased;
  case SubmissionTraceEventKind::Encoded:
    return current == SubmissionTraceEventKind::InitiationDefinitelyFailed ||
           current == SubmissionTraceEventKind::WriteInitiated ||
           current == SubmissionTraceEventKind::SubmissionUnknown ||
           current == SubmissionTraceEventKind::ReservationReleased;
  case SubmissionTraceEventKind::WriteInitiated:
  case SubmissionTraceEventKind::SubmissionUnknown:
  case SubmissionTraceEventKind::ReservationReleased:
    return current == SubmissionTraceEventKind::SubmissionCompleted;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Check the final result against the exact causal branch that immediately precedes completion.
[[nodiscard]] bool does_completion_match(std::span<const SubmissionTraceRecord> records,
                                         const SubmissionTraceRecord& previous,
                                         const SubmissionFinalResult& result) {
  if (is_internal_rejection(result)) {
    return previous.kind() != SubmissionTraceEventKind::WriteInitiated &&
           previous.kind() != SubmissionTraceEventKind::SubmissionUnknown;
  }

  switch (previous.kind()) {
  case SubmissionTraceEventKind::Attempt:
    return is_route_rejection(result);
  case SubmissionTraceEventKind::RouteAuthorized:
    return is_canonical_rejection(result);
  case SubmissionTraceEventKind::CanonicalValidated:
    return result.disposition == execution::SubmitDisposition::LocallyRejected &&
           result.stage == execution::SubmissionStage::Identity &&
           result.reason == execution::SubmissionReason::OrderIdentityExhausted;
  case SubmissionTraceEventKind::RiskRejected:
    return is_risk_rejection(result, previous.fields().risk_rejection);
  case SubmissionTraceEventKind::WriteInitiated:
    return result.disposition == execution::SubmitDisposition::WriteInitiated;
  case SubmissionTraceEventKind::SubmissionUnknown:
    return result.disposition == execution::SubmitDisposition::SubmissionUnknown;
  case SubmissionTraceEventKind::ReservationReleased: {
    const auto* failure = ordinary_before(records, previous);
    if (failure == nullptr) {
      return false;
    }
    switch (failure->kind()) {
    case SubmissionTraceEventKind::OmsNonAdmission:
      return result.disposition == execution::SubmitDisposition::LocallyRejected &&
             result.stage == execution::SubmissionStage::Oms &&
             (result.reason == execution::SubmissionReason::DuplicateOrderIdentity ||
              result.reason == execution::SubmissionReason::OmsCapacityExceeded);
    case SubmissionTraceEventKind::EncodingFailed:
      return result.disposition == execution::SubmitDisposition::LocallyRejected &&
             result.stage == execution::SubmissionStage::Encoding &&
             result.reason == execution::SubmissionReason::EncodingFailed;
    case SubmissionTraceEventKind::InitiationDefinitelyFailed:
      return result.disposition == execution::SubmitDisposition::LocallyRejected &&
             result.stage == execution::SubmissionStage::Initiation &&
             result.reason == execution::SubmissionReason::InitiationDefinitelyFailed;
    case SubmissionTraceEventKind::RiskReserved:
    case SubmissionTraceEventKind::OmsAdmitted:
    case SubmissionTraceEventKind::Encoded:
      return is_internal_rejection(result);
    default:
      return false;
    }
  }
  default:
    return false;
  }
}

// --------------------------------------------------------
// Apply only the evidence introduction or state transition assigned to the current event.
[[nodiscard]] SubmissionTraceFields
derive_expected_submission_trace_snapshot(const SubmissionTraceFields& previous,
                                          SubmissionTraceEventKind current,
                                          const SubmissionTraceFields& candidate) {
  SubmissionTraceFields expected = previous;
  switch (current) {
  case SubmissionTraceEventKind::RouteAuthorized:
    expected.authorized_projection = candidate.authorized_projection;
    break;
  case SubmissionTraceEventKind::IdentityGenerated:
    expected.order_id = candidate.order_id;
    break;
  case SubmissionTraceEventKind::RiskReserved:
    expected.reservation_id = candidate.reservation_id;
    expected.approved_exposure = candidate.approved_exposure;
    break;
  case SubmissionTraceEventKind::RiskRejected:
    expected.risk_rejection = candidate.risk_rejection;
    break;
  case SubmissionTraceEventKind::OmsAdmitted:
  case SubmissionTraceEventKind::EncodingFailed:
    expected.oms_state = candidate.oms_state;
    break;
  case SubmissionTraceEventKind::Encoded:
    expected.oms_state = candidate.oms_state;
    expected.encoding = candidate.encoding;
    break;
  case SubmissionTraceEventKind::InitiationDefinitelyFailed:
    expected.oms_state = candidate.oms_state;
    expected.initiation = candidate.initiation;
    break;
  case SubmissionTraceEventKind::WriteInitiated:
  case SubmissionTraceEventKind::SubmissionUnknown:
    expected.oms_state = candidate.oms_state;
    expected.initiation = candidate.initiation;
    expected.release_transition = candidate.release_transition;
    break;
  case SubmissionTraceEventKind::ReservationReleased:
    expected.release_transition = candidate.release_transition;
    break;
  case SubmissionTraceEventKind::SubmissionCompleted:
    expected.final_result = candidate.final_result;
    break;
  default:
    break;
  }
  return expected;
}

// --------------------------------------------------------
// Enforce attempt boundaries, the one re-entry exception, transitions, and cumulative snapshots.
[[nodiscard]] model::Result<void> validate_sequence(std::span<const SubmissionTraceRecord> records,
                                                    SubmissionTraceEventKind kind,
                                                    const SubmissionTraceFields& fields) {
  const auto* previous = last_ordinary(records);

  if (kind == SubmissionTraceEventKind::Attempt) {
    if (previous != nullptr &&
        (previous->kind() != SubmissionTraceEventKind::SubmissionCompleted ||
         fields.context.attempt_id.value() <= previous->fields().context.attempt_id.value())) {
      return create_invalid_submission_trace_result("submission_trace.sequence");
    }
    return model::Result<void>::create_success();
  }

  if (previous == nullptr || previous->kind() == SubmissionTraceEventKind::SubmissionCompleted) {
    return create_invalid_submission_trace_result("submission_trace.sequence");
  }

  if (kind == SubmissionTraceEventKind::ReentryRejected) {
    if (!has_same_outer_context(fields.context, previous->fields().context) ||
        has_reentry_for_attempt(records, fields.context.attempt_id)) {
      return create_invalid_submission_trace_result("submission_trace.reentry");
    }
    return model::Result<void>::create_success();
  }

  if (fields.context != previous->fields().context ||
      !can_follow_in_submission_trace(previous->kind(), kind)) {
    return create_invalid_submission_trace_result("submission_trace.sequence");
  }

  const auto expected = derive_expected_submission_trace_snapshot(previous->fields(), kind, fields);
  if (fields != expected) {
    return create_invalid_submission_trace_result("submission_trace.cumulative_fields");
  }
  if (kind == SubmissionTraceEventKind::SubmissionCompleted &&
      !does_completion_match(records, *previous, *fields.final_result)) {
    return create_invalid_submission_trace_result("submission_trace.final_result");
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Encode one risk evidence measure into its single pair of positional decimal/count slots.
[[nodiscard]] bool append_risk_limit_evidence(CanonicalSubmissionTraceWriter& writer,
                                              const execution::RiskLimitEvidence& evidence) {
  if (!writer.append_byte(static_cast<std::uint8_t>(evidence.scope())) ||
      !writer.append_byte(static_cast<std::uint8_t>(evidence.measure_kind()))) {
    return false;
  }

  switch (evidence.measure_kind()) {
  case execution::RiskMeasureKind::Quantity:
    return writer.append_decimal(*evidence.observed_quantity()) &&
           writer.append_decimal(*evidence.quantity_limit()) && writer.append_u64(0U) &&
           writer.append_u64(0U);
  case execution::RiskMeasureKind::QuoteNotional:
    return writer.append_decimal(*evidence.observed_notional()) &&
           writer.append_decimal(*evidence.notional_limit()) && writer.append_u64(0U) &&
           writer.append_u64(0U);
  case execution::RiskMeasureKind::OrderCount:
    return writer.append_zero_decimal() && writer.append_zero_decimal() &&
           writer.append_u64(*evidence.observed_count()) &&
           writer.append_u64(*evidence.count_limit());
  default:
    return false;
  }
}

// --------------------------------------------------------
// Encode one complete positional record without a native-layout or per-record envelope.
[[nodiscard]] bool append_submission_trace_record(CanonicalSubmissionTraceWriter& writer,
                                                  const SubmissionTraceRecord& record) {
  const auto& fields = record.fields();
  const auto& context = fields.context;
  const auto& request = context.request;
  const auto& provenance = record.provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit fixed identity, callback, request, and startup provenance positions first.
  if (!writer.append_u64(record.ordinal().value()) ||
      !writer.append_u64(context.attempt_id.value()) ||
      !writer.append_u16(static_cast<std::uint16_t>(record.kind())) ||
      !writer.append_u64(context.owner_turn_ordinal.value()) ||
      !writer.append_u64(context.callback_ordinal.value()) ||
      !writer.append_u64(context.callback_processing_nanoseconds) ||
      !writer.append_string(context.attribution.firm_id.value()) ||
      !writer.append_string(context.attribution.desk_id.value()) ||
      !writer.append_string(context.attribution.bot_id.value()) ||
      !writer.append_string(context.attribution.strategy_id.value()) ||
      !writer.append_string(request.route_id.value()) ||
      !writer.append_string(request.instrument_id.value()) ||
      !writer.append_byte(static_cast<std::uint8_t>(request.side)) ||
      !writer.append_byte(static_cast<std::uint8_t>(request.type)) ||
      !writer.append_byte(static_cast<std::uint8_t>(request.time_in_force)) ||
      !writer.append_decimal(request.price) || !writer.append_decimal(request.quantity) ||
      !writer.append_bytes(provenance.configuration_fingerprint) ||
      !writer.append_u64(provenance.configuration_revision.value()) ||
      !writer.append_u64(provenance.organization_revision.value()) ||
      !writer.append_u64(provenance.route_revision.value()) ||
      !writer.append_bytes(provenance.runtime_policy_fingerprint) ||
      !writer.append_bytes(provenance.risk_policy_fingerprint) ||
      !writer.append_u64(provenance.risk_policy_revision.value()) ||
      !writer.append_bytes(provenance.submission_policy_fingerprint)) {
    return false;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit authorization, local identity, reservation, and approved economics with explicit presence.
  if (!writer.append_byte(fields.authorized_projection ? 1U : 0U)) {
    return false;
  }
  if (fields.authorized_projection) {
    const auto& projection = *fields.authorized_projection;
    if (!writer.append_string(projection.route_id.value()) ||
        !writer.append_string(projection.venue_id.value()) ||
        !writer.append_string(projection.logical_account_id.value()) ||
        !writer.append_string(projection.instrument_id.value()) ||
        !writer.append_string(projection.venue_instrument_id.value()) ||
        !writer.append_u64(projection.metadata_revision.value())) {
      return false;
    }
  }
  if (!writer.append_byte(fields.order_id ? 1U : 0U) ||
      (fields.order_id &&
       !writer.append_bytes(std::as_bytes(std::span{fields.order_id->bytes()}))) ||
      !writer.append_byte(fields.reservation_id ? 1U : 0U) ||
      (fields.reservation_id && !writer.append_u64(fields.reservation_id->value())) ||
      !writer.append_byte(fields.approved_exposure ? 1U : 0U) ||
      (fields.approved_exposure &&
       (!writer.append_decimal(fields.approved_exposure->quantity) ||
        !writer.append_decimal(fields.approved_exposure->quote_notional)))) {
    return false;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit rejection, OMS, encoding, and initiation evidence only in their assigned optional slots.
  if (!writer.append_byte(fields.risk_rejection ? 1U : 0U) ||
      (fields.risk_rejection && !append_risk_limit_evidence(writer, *fields.risk_rejection)) ||
      !writer.append_byte(fields.oms_state ? 1U : 0U) ||
      (fields.oms_state && !writer.append_byte(static_cast<std::uint8_t>(*fields.oms_state))) ||
      !writer.append_byte(fields.encoding ? 1U : 0U)) {
    return false;
  }
  if (fields.encoding && (!writer.append_u64(fields.encoding->invocation_ordinal.value()) ||
                          !writer.append_u16(fields.encoding->byte_length) ||
                          !writer.append_bytes(fields.encoding->encoded_order_fingerprint))) {
    return false;
  }
  if (!writer.append_byte(fields.initiation ? 1U : 0U)) {
    return false;
  }
  if (fields.initiation &&
      (!writer.append_u64(fields.initiation->invocation_ordinal.value()) ||
       !writer.append_byte(static_cast<std::uint8_t>(fields.initiation->outcome)) ||
       !writer.append_byte(fields.initiation->accepted_write_ordinal ? 1U : 0U) ||
       (fields.initiation->accepted_write_ordinal &&
        !writer.append_u64(fields.initiation->accepted_write_ordinal->value())))) {
    return false;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Finish with release and final synchronous-local result positions.
  if (!writer.append_byte(static_cast<std::uint8_t>(fields.release_transition)) ||
      !writer.append_byte(fields.final_result ? 1U : 0U)) {
    return false;
  }
  return !fields.final_result ||
         (writer.append_byte(static_cast<std::uint8_t>(fields.final_result->disposition)) &&
          writer.append_byte(static_cast<std::uint8_t>(fields.final_result->stage)) &&
          writer.append_u16(static_cast<std::uint16_t>(fields.final_result->reason)));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Bind immutable provenance and allocate the complete accepted-prefix vector up front.
SubmissionTraceSink::SubmissionTraceSink(SubmissionTraceProvenance provenance,
                                         std::uint32_t capacity)
    : provenance_{std::move(provenance)}, capacity_{capacity} {
  records_.reserve(capacity_);
}

// --------------------------------------------------------
// Reject insufficient headroom before identity generation or any risk reservation can occur.
model::Result<void>
SubmissionTraceSink::preflight_trace_append(std::uint32_t additional_records) const {
  if (additional_records > remaining_capacity()) {
    return model::Result<void>::create_failure(DomainError::create_at_index(
        DomainErrorCode::SubmissionEvidenceExhausted, "submission_trace.capacity",
        static_cast<std::size_t>(capacity_)));
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Validate shape, causal order, and cumulative fields without consuming capacity or an ordinal.
model::Result<void>
SubmissionTraceSink::validate_trace_record(SubmissionTraceEventKind kind,
                                           const SubmissionTraceFields& fields) const {
  if (!has_valid_event_shape(kind, fields)) {
    return create_invalid_submission_trace_result("submission_trace.fields");
  }
  return validate_sequence(records_, kind, fields);
}

// --------------------------------------------------------
// Capacity and all deterministic validation precede mutation of the accepted canonical prefix.
model::Result<void> SubmissionTraceSink::append_trace_record(SubmissionTraceEventKind kind,
                                                             SubmissionTraceFields fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity precedence keeps a full sink's behavior independent of attempted field shape.
  auto capacity_check = preflight_trace_append(1U);
  if (!capacity_check) {
    return capacity_check;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the entire record before assigning its one-based accepted-prefix ordinal.
  auto validation = validate_trace_record(kind, fields);
  if (!validation) {
    return validation;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preallocated storage makes this append allocation-free under the sealed policy capacity.
  const auto ordinal = SubmissionTraceOrdinal{static_cast<std::uint64_t>(records_.size()) + 1U};
  records_.push_back(SubmissionTraceRecord{ordinal, kind, provenance_, std::move(fields)});
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Emit magic, version, count, and every complete positional record in accepted order.
model::Result<std::vector<std::byte>> SubmissionTraceSink::encode_canonical_bytes() const {
  CanonicalSubmissionTraceWriter writer;
  bool success = writer.append_ascii_raw(submission_stream_magic) &&
                 writer.append_u16(submission_trace_schema_version) &&
                 writer.append_u32(record_count());
  for (const auto& record : records_) {
    if (!success || !append_submission_trace_record(writer, record)) {
      success = false;
      break;
    }
  }
  if (!success) {
    return model::Result<std::vector<std::byte>>::create_failure(
        DomainError::create_at_field(DomainErrorCode::EncodingOverflow, "submission_trace"));
  }
  return model::Result<std::vector<std::byte>>::create_success(
      std::move(writer).take_canonical_bytes());
}

// --------------------------------------------------------
// Hash exactly the complete AEGISSTS bytes without embedding the digest back into the stream.
model::Result<model::Sha256Digest> SubmissionTraceSink::derive_digest() const {
  auto encoded = encode_canonical_bytes();
  if (!encoded) {
    return model::Result<model::Sha256Digest>::create_failure(std::move(encoded).error());
  }
  return model::Result<model::Sha256Digest>::create_success(
      model::calculate_sha256_digest(encoded.value()));
}

// --------------------------------------------------------

} // namespace aegis::trace
