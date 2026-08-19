// Purpose: retain bounded owner-local M2 diagnostic details with explicit overwrite accounting.

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
// The diagnostic ring overwrites oldest noncritical detail only after recording that loss. Critical
// runtime facts remain independently observable through their owning result/state/trace contracts.
class RuntimeDiagnosticSink final {
public:

  // --------------------------------------------------------
  // Derive the fixed slot count from the same immutable policy used by the runtime.
  explicit RuntimeDiagnosticSink(const RuntimePolicy& policy);

  // --------------------------------------------------------
  RuntimeDiagnosticSink(const RuntimeDiagnosticSink&) = delete;
  RuntimeDiagnosticSink& operator=(const RuntimeDiagnosticSink&) = delete;
  RuntimeDiagnosticSink(RuntimeDiagnosticSink&&) = delete;
  RuntimeDiagnosticSink& operator=(RuntimeDiagnosticSink&&) = delete;

  // --------------------------------------------------------
  // Validate and retain one observation, overwriting only the oldest detail when already full.
  [[nodiscard]] model::Result<void> append(RuntimeDiagnosticKind kind,
                                           RuntimeDiagnosticFields fields);

  // --------------------------------------------------------
  // Resolve one retained record in oldest-to-newest order; out-of-range positions return null.
  [[nodiscard]] const RuntimeDiagnosticRecord* at(std::size_t chronological_index) const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Count details evicted from the retained ring; the counter never silently wraps.
  [[nodiscard]] constexpr std::uint64_t overwritten_count() const noexcept {
    return overwritten_count_;
  }

  // --------------------------------------------------------
  // Count every successfully appended detail, including overwritten records.
  [[nodiscard]] constexpr std::uint64_t accepted_count() const noexcept { return last_ordinal_; }

  // --------------------------------------------------------
private:
  std::uint32_t capacity_;
  std::size_t source_capacity_;
  std::vector<RuntimeDiagnosticRecord> records_;
  std::size_t oldest_index_{0U};
  std::uint64_t last_ordinal_{0U};
  std::uint64_t overwritten_count_{0U};
};

// ########################################################################

} // namespace aegis::runtime
