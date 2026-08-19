// Purpose: validate fixed diagnostic profiles and retain bounded owner-local details in ring order.

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
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
[[nodiscard]] bool has_input_context(const RuntimeDiagnosticFields& fields) noexcept {
  return fields.source_ordinal && fields.admission_ordinal && fields.turn_ordinal &&
         !fields.callback_ordinal;
}

// --------------------------------------------------------
[[nodiscard]] bool has_no_measurement(const RuntimeDiagnosticFields& fields) noexcept {
  return fields.observed_value == 0U && fields.limit_value == 0U;
}

// --------------------------------------------------------
[[nodiscard]] bool is_valid_profile(RuntimeDiagnosticKind kind,
                                    const RuntimeDiagnosticFields& fields) noexcept {
  if (!is_known(kind) || fields.occurrence_count == 0U) {
    return false;
  }

  switch (kind) {
  case RuntimeDiagnosticKind::SourceDiscontinuity:
    return has_input_context(fields) && fields.detail_code == 0U && has_no_measurement(fields);
  case RuntimeDiagnosticKind::MalformedInput:
  case RuntimeDiagnosticKind::UnsupportedInput:
  case RuntimeDiagnosticKind::StructuralBookRejected:
    return has_input_context(fields) && fields.detail_code != 0U;
  case RuntimeDiagnosticKind::CallbackBudgetExceeded:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U &&
           fields.observed_value > fields.limit_value && fields.limit_value != 0U &&
           fields.occurrence_count == 1U;
  case RuntimeDiagnosticKind::OwnerReentryDetected:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.detail_code == 0U && has_no_measurement(fields);
  case RuntimeDiagnosticKind::DispatchReentryDetected:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           fields.callback_ordinal && fields.detail_code == 0U && has_no_measurement(fields);
  case RuntimeDiagnosticKind::EvidenceExhausted:
    return !fields.source_ordinal && !fields.admission_ordinal && fields.turn_ordinal &&
           !fields.callback_ordinal && fields.detail_code == 0U && has_no_measurement(fields) &&
           fields.occurrence_count == 1U;
  default:
    return false;
  }
}

// --------------------------------------------------------
[[nodiscard]] model::Result<void> invalid(std::string_view field) {
  return model::Result<void>::failure(
      DomainError::at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
RuntimeDiagnosticSink::RuntimeDiagnosticSink(const RuntimePolicy& policy)
    : capacity_{policy.limits().diagnostic_capacity}, source_capacity_{policy.source_capacity()} {
  records_.reserve(capacity_);
}

// --------------------------------------------------------
model::Result<void> RuntimeDiagnosticSink::append(RuntimeDiagnosticKind kind,
                                                  RuntimeDiagnosticFields fields) {
  if (!is_valid_profile(kind, fields)) {
    return invalid("runtime_diagnostic.fields");
  }
  if (fields.source_ordinal && fields.source_ordinal->value() > source_capacity_) {
    return model::Result<void>::failure(DomainError::at_field(
        DomainErrorCode::RuntimeSourceNotConfigured, "runtime_diagnostic.source_ordinal"));
  }
  if (last_ordinal_ == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<void>::failure(
        DomainError::at_field(DomainErrorCode::CounterExhausted, "runtime_diagnostic.ordinal"));
  }
  if (records_.size() == capacity_ &&
      overwritten_count_ == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<void>::failure(DomainError::at_field(
        DomainErrorCode::CounterExhausted, "runtime_diagnostic.overwritten_count"));
  }

  const auto record = RuntimeDiagnosticRecord{last_ordinal_ + 1U, kind, std::move(fields)};
  if (records_.size() < capacity_) {
    records_.push_back(record);
  } else {
    records_[oldest_index_] = record;
    oldest_index_ = (oldest_index_ + 1U) % records_.size();
    ++overwritten_count_;
  }
  ++last_ordinal_;
  return model::Result<void>::success();
}

// --------------------------------------------------------
const RuntimeDiagnosticRecord*
RuntimeDiagnosticSink::at(std::size_t chronological_index) const noexcept {
  if (chronological_index >= records_.size()) {
    return nullptr;
  }
  const auto physical_index = (oldest_index_ + chronological_index) % records_.size();
  return &records_[physical_index];
}

// --------------------------------------------------------

} // namespace aegis::runtime
