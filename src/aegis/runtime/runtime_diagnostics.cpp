// Purpose: validate fixed diagnostic profiles, preserve the first bounded owner-local prefix, and
// account for later valid observations without failing ordinary runtime behavior.

#include "aegis/runtime/runtime_diagnostics.hpp"

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
// Keep the persisted kind vocabulary closed at append validation.
[[nodiscard]] bool is_known(RuntimeDiagnosticKind kind) noexcept {
  switch (kind) {
  case RuntimeDiagnosticKind::SourceDiscontinuity:
  case RuntimeDiagnosticKind::MalformedInput:
  case RuntimeDiagnosticKind::UnsupportedInput:
  case RuntimeDiagnosticKind::StructuralBookRejected:
  case RuntimeDiagnosticKind::CallbackBudgetExceeded:
  case RuntimeDiagnosticKind::OwnerReentryDetected:
  case RuntimeDiagnosticKind::DispatchReentryDetected:
  case RuntimeDiagnosticKind::EvidenceExhausted:
  case RuntimeDiagnosticKind::CallbackClockRegression:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Require accepted admission/turn identity without inventing source attribution or a callback.
[[nodiscard]] bool has_accepted_input_context(const RuntimeDiagnosticFields& fields) noexcept {
  return fields.admission_ordinal && fields.turn_ordinal && !fields.callback_ordinal;
}

// --------------------------------------------------------
// Require a configured source in addition to accepted input identity for book/state evidence.
[[nodiscard]] bool has_attributed_input_context(const RuntimeDiagnosticFields& fields) noexcept {
  return fields.source_ordinal && has_accepted_input_context(fields);
}

// --------------------------------------------------------
// Recognize diagnostic profiles that deliberately carry no measured values.
[[nodiscard]] bool has_no_measurement(const RuntimeDiagnosticFields& fields) noexcept {
  return fields.observed_value == 0U && fields.limit_value == 0U;
}

// --------------------------------------------------------
// Enforce the exact fixed-field profile assigned to each stable diagnostic kind.
[[nodiscard]] bool is_valid_profile(RuntimeDiagnosticKind kind,
                                    const RuntimeDiagnosticFields& fields) noexcept {
  if (!is_known(kind) || fields.occurrence_count == 0U) {
    return false;
  }

  switch (kind) {
  case RuntimeDiagnosticKind::SourceDiscontinuity:
    return has_attributed_input_context(fields) && fields.detail_code == 0U &&
           has_no_measurement(fields);
  case RuntimeDiagnosticKind::MalformedInput:
  case RuntimeDiagnosticKind::UnsupportedInput:
    return has_accepted_input_context(fields) && fields.detail_code != 0U;
  case RuntimeDiagnosticKind::StructuralBookRejected:
    return has_attributed_input_context(fields) && fields.detail_code != 0U;
  case RuntimeDiagnosticKind::CallbackBudgetExceeded:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U &&
           fields.observed_value > fields.limit_value && fields.limit_value != 0U &&
           fields.occurrence_count == 1U;
  case RuntimeDiagnosticKind::OwnerReentryDetected:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U && has_no_measurement(fields);
  case RuntimeDiagnosticKind::DispatchReentryDetected:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U && has_no_measurement(fields);
  case RuntimeDiagnosticKind::EvidenceExhausted:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           !fields.callback_ordinal && fields.detail_code == 0U && has_no_measurement(fields) &&
           fields.occurrence_count == 1U;
  case RuntimeDiagnosticKind::CallbackClockRegression:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U &&
           fields.observed_value < fields.limit_value && fields.occurrence_count == 1U;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Return the shared stable failure for a malformed internal diagnostic profile.
[[nodiscard]] model::Result<void> invalid(std::string_view field) {
  return model::Result<void>::failure(
      DomainError::at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Bind capacity and source membership to one immutable policy and reserve storage up front.
RuntimeDiagnosticSink::RuntimeDiagnosticSink(const RuntimePolicy& policy)
    : configuration_fingerprint_{policy.configuration_fingerprint()},
      runtime_policy_fingerprint_{policy.fingerprint()},
      capacity_{policy.limits().diagnostic_capacity}, source_capacity_{policy.source_capacity()} {
  records_.reserve(capacity_);
}

// --------------------------------------------------------
// Validate profile and policy attribution without consuming diagnostic storage or ordinals.
model::Result<void> RuntimeDiagnosticSink::validate(RuntimeDiagnosticKind kind,
                                                    const RuntimeDiagnosticFields& fields) const {
  if (!is_valid_profile(kind, fields)) {
    return invalid("runtime_diagnostic.fields");
  }
  if (fields.source_ordinal && fields.source_ordinal->value() > source_capacity_) {
    return model::Result<void>::failure(DomainError::at_field(
        DomainErrorCode::RuntimeSourceNotConfigured, "runtime_diagnostic.source_ordinal"));
  }
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Validate one detail, retain the accepted prefix, and count later valid arrivals as dropped.
model::Result<void> RuntimeDiagnosticSink::append(RuntimeDiagnosticKind kind,
                                                  RuntimeDiagnosticFields fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Invalid or misattributed observations remain programmer-contract failures even after the
  // retained prefix has filled.
  auto valid = validate(kind, fields);
  if (!valid) {
    return valid;
  }
  if (records_.size() == capacity_) {
    if (dropped_count_ == std::numeric_limits<std::uint64_t>::max()) {
      return model::Result<void>::failure(DomainError::at_field(
          DomainErrorCode::CounterExhausted, "runtime_diagnostic.dropped_count"));
    }
    ++dropped_count_;
    return model::Result<void>::success();
  }
  if (last_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<void>::failure(
        DomainError::at_field(DomainErrorCode::CounterExhausted, "runtime_diagnostic.ordinal"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Append only to the retained prefix; existing records and their ordinals never change.
  const auto record = RuntimeDiagnosticRecord{last_ordinal_ + 1U, kind, std::move(fields)};
  records_.push_back(record);
  ++last_ordinal_;
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Borrow one retained prefix position without exposing mutable storage.
const RuntimeDiagnosticRecord*
RuntimeDiagnosticSink::at(std::size_t chronological_index) const noexcept {
  if (chronological_index >= records_.size()) {
    return nullptr;
  }
  return &records_[chronological_index];
}

// --------------------------------------------------------

} // namespace aegis::runtime
