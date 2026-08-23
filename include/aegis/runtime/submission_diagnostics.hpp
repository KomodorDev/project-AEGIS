// Purpose: retain bounded noncanonical M3 submission diagnostics with exact provenance and
// explicit saturation accounting without altering canonical submission outcomes.

#pragma once

#include "aegis/execution/submit_result.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/risk/risk_scope.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Assigned kinds supplement canonical AEGISSTS records but never replace a submission result,
// reservation transition, or terminal owner fault.
enum class SubmissionDiagnosticKind : std::uint16_t {
  EvidenceCapacityExceeded = 1,
  ReentryDetected = 2,
  ReservationReleased = 3,
  UnknownExposureRetained = 4,
  InternalInvariantFailure = 5,
  MeasurementUnavailable = 6,
};

// ########################################################################

// ########################################################################
// Raw canonical digests keep the diagnostic leaf independent of policy wrapper ownership while
// still binding every retained record to the exact startup, risk, and submission rulebooks.
struct SubmissionDiagnosticProvenance {
  model::Sha256Digest configuration_fingerprint;
  model::Sha256Digest risk_policy_fingerprint;
  model::Sha256Digest submission_policy_fingerprint;

  // --------------------------------------------------------
  // Structural equality compares all three canonical identities.
  friend bool operator==(const SubmissionDiagnosticProvenance&,
                         const SubmissionDiagnosticProvenance&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Optional fixed fields preserve only the submission identity available at the observation point;
// dependency validation prevents partial identity chains and forged free-form context.
struct SubmissionDiagnosticFields {
  std::optional<model::SubmissionAttemptId> attempt_id;
  std::optional<model::TurnOrdinal> owner_turn_ordinal;
  std::optional<model::CallbackOrdinal> callback_ordinal;
  std::optional<model::OrderId> order_id;
  std::optional<model::ReservationId> reservation_id;
  std::optional<execution::SubmissionStage> stage;
  std::optional<execution::SubmissionReason> reason;
  std::optional<risk::RiskScopeKind> risk_scope;
  std::uint64_t occurrence_count{1U};

  // --------------------------------------------------------
  // Structural equality compares every fixed field and optional-presence decision.
  friend bool operator==(const SubmissionDiagnosticFields&,
                         const SubmissionDiagnosticFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One retained observation owns a sink-issued ordinal, exact kind/profile, and all required
// canonical provenance digests.
struct SubmissionDiagnosticRecord {
  std::uint64_t ordinal;
  SubmissionDiagnosticKind kind;
  SubmissionDiagnosticProvenance provenance;
  SubmissionDiagnosticFields fields;

  // --------------------------------------------------------
  // Structural equality makes deterministic retained-prefix comparisons direct.
  friend bool operator==(const SubmissionDiagnosticRecord&,
                         const SubmissionDiagnosticRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The owner-local sink preserves the earliest valid prefix and saturates its dropped count so
// noncritical diagnostic pressure can never change a submission decision.
class SubmissionDiagnosticSink final {
public:

  // --------------------------------------------------------
  // Reserve the complete policy-fixed prefix before any direct-path observation can arrive.
  SubmissionDiagnosticSink(SubmissionDiagnosticProvenance provenance, std::uint32_t capacity);

  // --------------------------------------------------------
  // One non-movable authority owns prefix order and saturation counters.
  SubmissionDiagnosticSink(const SubmissionDiagnosticSink&) = delete;
  SubmissionDiagnosticSink& operator=(const SubmissionDiagnosticSink&) = delete;
  SubmissionDiagnosticSink(SubmissionDiagnosticSink&&) = delete;
  SubmissionDiagnosticSink& operator=(SubmissionDiagnosticSink&&) = delete;

  // --------------------------------------------------------
  // Validate a kind-specific fixed-field profile without changing prefix or counters.
  [[nodiscard]] model::Result<void> validate(SubmissionDiagnosticKind kind,
                                             const SubmissionDiagnosticFields& fields) const;

  // --------------------------------------------------------
  // Retain one valid observation or account for its loss after prefix saturation.
  [[nodiscard]] model::Result<void> append(SubmissionDiagnosticKind kind,
                                           SubmissionDiagnosticFields fields);

  // --------------------------------------------------------
  // Resolve one retained record in oldest-to-newest order; out-of-range positions return null.
  [[nodiscard]] const SubmissionDiagnosticRecord*
  at(std::size_t chronological_index) const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  [[nodiscard]] const SubmissionDiagnosticProvenance& provenance() const noexcept {
    return provenance_;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool saturated() const noexcept { return records_.size() == capacity_; }

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint64_t accepted_count() const noexcept { return accepted_count_; }

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint64_t dropped_count() const noexcept { return dropped_count_; }

  // --------------------------------------------------------
private:
  SubmissionDiagnosticProvenance provenance_;
  std::uint32_t capacity_;
  std::vector<SubmissionDiagnosticRecord> records_;
  std::uint64_t accepted_count_{0U};
  std::uint64_t dropped_count_{0U};
};

// ########################################################################

} // namespace aegis::runtime
