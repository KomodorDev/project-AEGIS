// Purpose: define the bounded canonical M3 submission evidence stream, including exact causal
// shapes, cumulative owner-local snapshots, positional AEGISSTS bytes, and digest identity.

#pragma once

#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/risk/exposure.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace aegis::trace {

// AEGISSTS is independent from AEGISTRS and AEGISRTS. Its fixed maximum includes the one possible
// first re-entry record in addition to the longest ten-record ordinary path.
inline constexpr std::uint16_t submission_trace_schema_version = 1U;
inline constexpr std::uint32_t maximum_submission_trace_records_per_attempt = 11U;

// ########################################################################
// Assigned kinds name every causal M3 boundary without treating local initiation as
// acknowledgement.
enum class SubmissionTraceEventKind : std::uint16_t {
  Attempt = 1,
  RouteAuthorized = 2,
  CanonicalValidated = 3,
  IdentityGenerated = 4,
  RiskReserved = 5,
  RiskRejected = 6,
  OmsAdmitted = 7,
  OmsNonAdmission = 8,
  Encoded = 9,
  EncodingFailed = 10,
  InitiationDefinitelyFailed = 11,
  WriteInitiated = 12,
  SubmissionUnknown = 13,
  ReservationReleased = 14,
  ReentryRejected = 15,
  SubmissionCompleted = 16,
};

// ########################################################################

// ########################################################################
// Release values distinguish no transition, exact rollback, and conservative retained exposure.
enum class SubmissionReleaseTransition : std::uint8_t {
  None = 0,
  Released = 1,
  Retained = 2,
};

// ########################################################################

// ########################################################################
// Runtime-minted attribution is repeated in every record so the request cannot forge organizational
// identity and a standalone canonical stream remains interpretable.
struct SubmissionTraceAttribution {
  model::FirmId firm_id;
  model::DeskId desk_id;
  model::BotId bot_id;
  model::StrategyId strategy_id;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionTraceAttribution&,
                         const SubmissionTraceAttribution&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Raw digests and exact revisions bind one sink to the complete startup and immutable M2/M3 policy
// authority without reversing dependencies into their owning wrapper types.
struct SubmissionTraceProvenance {
  model::Sha256Digest configuration_fingerprint;
  model::ConfigurationRevision configuration_revision;
  model::OrganizationRevision organization_revision;
  model::RouteRevision route_revision;
  model::Sha256Digest runtime_policy_fingerprint;
  model::Sha256Digest risk_policy_fingerprint;
  model::RiskPolicyRevision risk_policy_revision;
  model::Sha256Digest submission_policy_fingerprint;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionTraceProvenance&,
                         const SubmissionTraceProvenance&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One outer-attempt context fixes callback identity, runtime attribution, and exact caller request;
// ReentryRejected deliberately substitutes only the nested request under the same outer identity.
struct SubmissionTraceContext {
  model::SubmissionAttemptId attempt_id;
  model::TurnOrdinal owner_turn_ordinal;
  model::CallbackOrdinal callback_ordinal;
  std::uint64_t callback_processing_nanoseconds;
  SubmissionTraceAttribution attribution;
  execution::OrderRequest request;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionTraceContext&, const SubmissionTraceContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Authorization copies the complete route/account/venue/instrument projection and exact metadata
// revision that later economics and encoding used.
struct AuthorizedSubmissionProjection {
  model::RouteId route_id;
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  model::InstrumentId instrument_id;
  model::VenueInstrumentId venue_instrument_id;
  model::InstrumentMetadataRevision metadata_revision;

  // --------------------------------------------------------
  friend bool operator==(const AuthorizedSubmissionProjection&,
                         const AuthorizedSubmissionProjection&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Successful encoding evidence identifies the consumed invocation and hashes the exact AEGISFOE
// bytes without copying the bounded byte payload into every later cumulative record.
struct SubmissionEncodingEvidence {
  model::EncoderInvocationOrdinal invocation_ordinal;
  std::uint16_t byte_length;
  model::Sha256Digest encoded_order_fingerprint;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionEncodingEvidence&,
                         const SubmissionEncodingEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Initiation evidence records the exact selected boundary outcome and accepted-write identity only
// when the fixed slot copy definitely completed.
struct SubmissionInitiationEvidence {
  model::InitiatorInvocationOrdinal invocation_ordinal;
  execution::FakeInitiationOutcome outcome;
  std::optional<model::FakeWriteOrdinal> accepted_write_ordinal;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionInitiationEvidence&,
                         const SubmissionInitiationEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Only the three canonical SubmitResult fields are persisted; identities and risk evidence already
// occupy their earlier cumulative schema positions and local duration is deliberately excluded.
struct SubmissionFinalResult {
  execution::SubmitDisposition disposition;
  execution::SubmissionStage stage;
  execution::SubmissionReason reason;

  // --------------------------------------------------------
  // Project exactly the canonical subset of one synchronous local result.
  [[nodiscard]] static SubmissionFinalResult
  from_submit_result(const execution::SubmitResult& result) noexcept {
    return SubmissionFinalResult{result.disposition(), result.stage(), result.reason()};
  }

  // --------------------------------------------------------
  friend bool operator==(const SubmissionFinalResult&, const SubmissionFinalResult&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Optional groups become present only at their named causal event and then remain available in all
// later outer-attempt snapshots; shape validation rejects partial or premature evidence.
struct SubmissionTraceFields {
  SubmissionTraceContext context;
  std::optional<AuthorizedSubmissionProjection> authorized_projection;
  std::optional<model::OrderId> order_id;
  std::optional<model::ReservationId> reservation_id;
  std::optional<risk::OrderExposure> approved_exposure;
  std::optional<execution::RiskLimitEvidence> risk_rejection;
  std::optional<oms::OutboundOrderState> oms_state;
  std::optional<SubmissionEncodingEvidence> encoding;
  std::optional<SubmissionInitiationEvidence> initiation;
  SubmissionReleaseTransition release_transition{SubmissionReleaseTransition::None};
  std::optional<SubmissionFinalResult> final_result;

  // --------------------------------------------------------
  friend bool operator==(const SubmissionTraceFields&, const SubmissionTraceFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Ordinals are one-based accepted-prefix positions issued only by the submission trace sink.
class SubmissionTraceOrdinal final {
public:

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  friend constexpr bool operator==(SubmissionTraceOrdinal, SubmissionTraceOrdinal) = default;
  friend constexpr auto operator<=>(SubmissionTraceOrdinal, SubmissionTraceOrdinal) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the bounded sink may mint accepted-prefix positions.
  friend class SubmissionTraceSink;

  // ########################################################################

  // --------------------------------------------------------
  explicit constexpr SubmissionTraceOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  std::uint64_t value_;
};

// ########################################################################

// ########################################################################
// One immutable record owns its sink-issued ordinal, assigned event, policy provenance, and
// complete validated cumulative snapshot.
class SubmissionTraceRecord final {
public:

  // --------------------------------------------------------
  SubmissionTraceRecord(const SubmissionTraceRecord&) = default;
  SubmissionTraceRecord& operator=(const SubmissionTraceRecord&) = default;
  SubmissionTraceRecord(SubmissionTraceRecord&&) noexcept = default;
  SubmissionTraceRecord& operator=(SubmissionTraceRecord&&) noexcept = default;

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint16_t schema_version() const noexcept {
    return submission_trace_schema_version;
  }

  // --------------------------------------------------------
  [[nodiscard]] constexpr SubmissionTraceOrdinal ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  [[nodiscard]] constexpr SubmissionTraceEventKind kind() const noexcept { return kind_; }

  // --------------------------------------------------------
  [[nodiscard]] const SubmissionTraceProvenance& provenance() const noexcept { return provenance_; }

  // --------------------------------------------------------
  [[nodiscard]] const SubmissionTraceFields& fields() const noexcept { return fields_; }

  // --------------------------------------------------------
  friend bool operator==(const SubmissionTraceRecord&, const SubmissionTraceRecord&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the sink may combine caller fields with policy provenance and a canonical ordinal.
  friend class SubmissionTraceSink;

  // ########################################################################

  // --------------------------------------------------------
  SubmissionTraceRecord(SubmissionTraceOrdinal ordinal, SubmissionTraceEventKind kind,
                        SubmissionTraceProvenance provenance, SubmissionTraceFields fields)
      : ordinal_{ordinal}, kind_{kind}, provenance_{std::move(provenance)},
        fields_{std::move(fields)} {}

  // --------------------------------------------------------
  SubmissionTraceOrdinal ordinal_;
  SubmissionTraceEventKind kind_;
  SubmissionTraceProvenance provenance_;
  SubmissionTraceFields fields_;
};

// ########################################################################

// ########################################################################
// The sole append authority reserves fixed storage, validates event profiles and exact sequence,
// and projects the immutable accepted prefix into positional AEGISSTS bytes.
class SubmissionTraceSink final {
public:

  // --------------------------------------------------------
  // Reserve the complete positive policy capacity before any submission attempt begins.
  SubmissionTraceSink(SubmissionTraceProvenance provenance, std::uint32_t capacity);

  // --------------------------------------------------------
  SubmissionTraceSink(const SubmissionTraceSink&) = delete;
  SubmissionTraceSink& operator=(const SubmissionTraceSink&) = delete;
  SubmissionTraceSink(SubmissionTraceSink&&) = delete;
  SubmissionTraceSink& operator=(SubmissionTraceSink&&) = delete;

  // --------------------------------------------------------
  // Prove a complete attempt's maximum evidence fits before identity or risk mutation.
  [[nodiscard]] model::Result<void> preflight(std::uint32_t additional_records) const;

  // --------------------------------------------------------
  // Validate profile, cumulative snapshot, and causal sequence without assigning an ordinal.
  [[nodiscard]] model::Result<void> validate(SubmissionTraceEventKind kind,
                                             const SubmissionTraceFields& fields) const;

  // --------------------------------------------------------
  // Append one valid event to the immutable prefix or leave the prefix unchanged on failure.
  [[nodiscard]] model::Result<void> append(SubmissionTraceEventKind kind,
                                           SubmissionTraceFields fields);

  // --------------------------------------------------------
  [[nodiscard]] std::span<const SubmissionTraceRecord> records() const noexcept { return records_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t remaining_capacity() const noexcept { return capacity_ - size(); }

  // --------------------------------------------------------
  [[nodiscard]] const SubmissionTraceProvenance& provenance() const noexcept { return provenance_; }

  // --------------------------------------------------------
  [[nodiscard]] model::Result<std::vector<std::byte>> canonical_bytes() const;

  // --------------------------------------------------------
  [[nodiscard]] model::Result<model::Sha256Digest> digest() const;

  // --------------------------------------------------------
private:
  SubmissionTraceProvenance provenance_;
  std::uint32_t capacity_;
  std::vector<SubmissionTraceRecord> records_;
};

// ########################################################################

} // namespace aegis::trace
