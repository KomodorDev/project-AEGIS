// Purpose: retain the first bounded owner-local M2 diagnostic prefix and expose later observation
// loss without allowing noncritical telemetry saturation to alter runtime behavior.

#pragma once

#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/runtime/runtime_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Assigned values describe noncanonical details that supplement, but never replace, admission
// decisions, source-discontinuity state, or the canonical AEGISRTS accepted prefix.
enum class RuntimeDiagnosticKind : std::uint16_t {
  SourceDiscontinuity = 1,
  MalformedInput = 2,
  UnsupportedInput = 3,
  StructuralBookRejected = 4,
  CallbackBudgetExceeded = 5,
  OwnerReentryDetected = 6,
  DispatchReentryDetected = 7,
  EvidenceExhausted = 8,
  CallbackClockRegression = 9,
};

// ########################################################################
// Fixed primitive fields keep owner-turn append allocation-free. Detail code/value meaning is
// selected by kind and never carries raw fixture bytes, free-form text, or ambient identifiers.
struct RuntimeDiagnosticFields {
  std::optional<model::MarketSourceOrdinal> source_ordinal;
  std::optional<model::AdmissionOrdinal> admission_ordinal;
  std::optional<model::TurnOrdinal> turn_ordinal;
  std::optional<model::CallbackOrdinal> callback_ordinal;
  std::uint32_t detail_code{0U};
  std::uint64_t observed_value{0U};
  std::uint64_t limit_value{0U};
  std::uint64_t occurrence_count{1U};

  // --------------------------------------------------------
  // Structural equality compares every bounded diagnostic field.
  friend bool operator==(const RuntimeDiagnosticFields&, const RuntimeDiagnosticFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A record owns one sink-issued ordinal and validated fixed-field observation.
struct RuntimeDiagnosticRecord {
  std::uint64_t ordinal;
  RuntimeDiagnosticKind kind;
  RuntimeDiagnosticFields fields;

  // --------------------------------------------------------
  // Structural equality makes retained chronological details deterministic in tests.
  friend bool operator==(const RuntimeDiagnosticRecord&, const RuntimeDiagnosticRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The diagnostic sink preserves its first accepted prefix forever and counts later valid details as
// dropped. Critical runtime facts remain independently observable through their result, state,
// report, or canonical-trace contracts.
class RuntimeDiagnosticSink final {
public:

  // --------------------------------------------------------
  // Derive the fixed slot count from the same immutable policy used by the runtime.
  explicit RuntimeDiagnosticSink(const RuntimePolicy& policy);

  // --------------------------------------------------------
  // One sink is the sole non-movable authority for its retained prefix and counters.
  RuntimeDiagnosticSink(const RuntimeDiagnosticSink&) = delete;
  RuntimeDiagnosticSink& operator=(const RuntimeDiagnosticSink&) = delete;
  RuntimeDiagnosticSink(RuntimeDiagnosticSink&&) = delete;
  RuntimeDiagnosticSink& operator=(RuntimeDiagnosticSink&&) = delete;

  // --------------------------------------------------------
  // Validate the fixed-field profile and configured source without changing prefix or counters.
  [[nodiscard]] model::Result<void>
  validate_diagnostic(RuntimeDiagnosticKind kind, const RuntimeDiagnosticFields& fields) const;

  // --------------------------------------------------------
  // Validate one observation and retain it only while prefix capacity remains. A valid observation
  // arriving after saturation increments dropped_count and succeeds so telemetry cannot abort a
  // canonical owner turn.
  [[nodiscard]] model::Result<void> append_diagnostic(RuntimeDiagnosticKind kind,
                                                      RuntimeDiagnosticFields fields);

  // --------------------------------------------------------
  // Resolve one retained record in oldest-to-newest order; out-of-range positions return null.
  [[nodiscard]] const RuntimeDiagnosticRecord*
  diagnostic_at(std::size_t chronological_index) const noexcept;

  // --------------------------------------------------------
  // Return the number of records retained in the immutable prefix.
  [[nodiscard]] std::uint32_t diagnostic_count() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  // Return the policy-fixed retained-prefix limit.
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Borrow the sealed startup identity from which this diagnostic boundary was constructed.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow the complete runtime-policy identity that fixed capacity and source membership.
  [[nodiscard]] const RuntimePolicyFingerprint& runtime_policy_fingerprint() const noexcept {
    return runtime_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Report whether the retained prefix has reached its policy-fixed capacity, independently of
  // whether a later observation has already been dropped.
  [[nodiscard]] bool is_saturated() const noexcept { return records_.size() == capacity_; }

  // --------------------------------------------------------
  // Count valid details dropped after saturation; the counter never silently wraps.
  [[nodiscard]] constexpr std::uint64_t dropped_count() const noexcept { return dropped_count_; }

  // --------------------------------------------------------
  // Count records accepted into the immutable retained prefix.
  [[nodiscard]] constexpr std::uint64_t accepted_count() const noexcept { return last_ordinal_; }

  // --------------------------------------------------------
private:
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  RuntimePolicyFingerprint runtime_policy_fingerprint_;
  std::uint32_t capacity_;
  std::size_t source_capacity_;
  std::vector<RuntimeDiagnosticRecord> records_;
  std::uint64_t last_ordinal_{0U};
  std::uint64_t dropped_count_{0U};
};

// ########################################################################

} // namespace aegis::runtime
