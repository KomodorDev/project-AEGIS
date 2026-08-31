// Purpose: define the bounded, canonical M2 runtime evidence contract without changing the M1
// provenance trace schema or exposing live timing measurements.

#pragma once

#include "aegis/configuration/configuration_provenance.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/runtime/runtime_policy.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace aegis::trace {

// This version belongs only to the AEGISRTS stream. AEGISTRS schema 1 remains an independent M1
// compatibility contract.
inline constexpr std::uint16_t runtime_trace_schema_version = 1U;

// ########################################################################
// Assigned compatibility values describe the five deterministic runtime observations required by
// M2; extending this vocabulary requires an explicit schema decision.
enum class RuntimeTraceEventKind : std::uint16_t {
  InputDisposition = 1,
  MarketStateTransition = 2,
  MarketCallback = 3,
  StateCallback = 4,
  ReentryDetected = 5,
};

// ########################################################################
// Input dispositions encode normalized processing outcomes without retaining malformed bytes or
// free-form diagnostic text in the canonical stream.
enum class RuntimeInputDisposition : std::uint16_t {
  Unspecified = 0,
  SnapshotApplied = 1,
  DeltaApplied = 2,
  ExactDuplicateIgnored = 3,
  OlderInputIgnored = 4,
  GapRejected = 5,
  SequenceConflictRejected = 6,
  ChecksumRejected = 7,
  MetadataRevisionRejected = 8,
  MalformedRejected = 9,
  UnsupportedRejected = 10,
  NonReadyDeltaRejected = 11,
  SessionReset = 12,
  StalenessChecked = 13,
  SourceDiscontinuity = 14,
  StructuralBookRejected = 15,
  SessionIgnored = 16,
};

// ########################################################################
// Readiness values mirror the M2 strategy-visible state contract while remaining trace-local until
// the market-state implementation supplies an explicit conversion boundary.
enum class RuntimeMarketState : std::uint16_t {
  Unspecified = 0,
  Synchronizing = 1,
  Ready = 2,
  Stale = 3,
  Invalid = 4,
};

// ########################################################################
// Failure reasons distinguish recursive execution paths without unstable exception messages or
// platform-dependent prose.
enum class RuntimeTraceFailureReason : std::uint16_t {
  None = 0,
  OwnerDriveReentry = 1,
  StrategyDispatchReentry = 2,
};

// ########################################################################
// A complete source identity prevents venue, normalized instrument, and venue instrument from
// being independently omitted. The nonzero ordinal binds the tuple to one entry interpreted under
// the record's runtime-policy fingerprint.
class RuntimeTraceSource final {
public:

  // --------------------------------------------------------
  // Copy source evidence only from one source validated and ordered by RuntimePolicy.
  [[nodiscard]] static RuntimeTraceSource from_runtime_source(const runtime::RuntimeSource& source);

  // --------------------------------------------------------
  // Borrow the configured source identity represented by this trace value.
  [[nodiscard]] const model::MarketSourceId& source_id() const noexcept { return source_id_; }

  // --------------------------------------------------------
  // Return the policy-assigned one-based source position encoded by runtime records.
  [[nodiscard]] model::MarketSourceOrdinal source_ordinal() const noexcept {
    return source_ordinal_;
  }

  // --------------------------------------------------------
  // Borrow the venue component of the complete configured source identity.
  [[nodiscard]] const model::VenueId& venue_id() const noexcept { return venue_id_; }

  // --------------------------------------------------------
  // Borrow the normalized instrument component of the configured source identity.
  [[nodiscard]] const model::InstrumentId& instrument_id() const noexcept { return instrument_id_; }

  // --------------------------------------------------------
  // Borrow the venue-native instrument component of the configured source identity.
  [[nodiscard]] const model::VenueInstrumentId& venue_instrument_id() const noexcept {
    return venue_instrument_id_;
  }

  // --------------------------------------------------------
  // Structural equality compares the complete normalized and venue-native source identity.
  friend bool operator==(const RuntimeTraceSource&, const RuntimeTraceSource&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain one complete identity copied from a validated runtime-policy source.
  RuntimeTraceSource(model::MarketSourceId source_id, model::MarketSourceOrdinal source_ordinal,
                     model::VenueId venue_id, model::InstrumentId instrument_id,
                     model::VenueInstrumentId venue_instrument_id)
      : source_id_{std::move(source_id)}, source_ordinal_{source_ordinal},
        venue_id_{std::move(venue_id)}, instrument_id_{std::move(instrument_id)},
        venue_instrument_id_{std::move(venue_instrument_id)} {}

  // --------------------------------------------------------
  // The source ID is retained as an opaque registry-membership proof. Canonical records encode the
  // policy-bound ordinal because the policy fingerprint already fixes the ordinal-to-ID mapping.
  model::MarketSourceId source_id_;
  model::MarketSourceOrdinal source_ordinal_;
  model::VenueId venue_id_;
  model::InstrumentId instrument_id_;
  model::VenueInstrumentId venue_instrument_id_;
};

// ########################################################################
// Fixed-width fingerprints bind each record to the sealed M1 configuration and immutable M2
// runtime policy without copying either rulebook into hot-path evidence.
class RuntimeTraceProvenance final {
public:

  // --------------------------------------------------------
  // Derive both identities from the same immutable validated runtime policy.
  [[nodiscard]] static RuntimeTraceProvenance
  from_runtime_policy(const runtime::RuntimePolicy& policy);

  // --------------------------------------------------------
  // Borrow the sealed startup-configuration identity attached to every runtime record.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow the immutable runtime-policy identity attached to every runtime record.
  [[nodiscard]] const runtime::RuntimePolicyFingerprint&
  runtime_policy_fingerprint() const noexcept {
    return runtime_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Structural equality compares both complete canonical identities.
  friend bool operator==(const RuntimeTraceProvenance&, const RuntimeTraceProvenance&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain the two identities derived together from one validated runtime policy.
  RuntimeTraceProvenance(configuration::ConfigurationFingerprint configuration_fingerprint,
                         runtime::RuntimePolicyFingerprint runtime_policy_fingerprint)
      : configuration_fingerprint_{std::move(configuration_fingerprint)},
        runtime_policy_fingerprint_{std::move(runtime_policy_fingerprint)} {}

  // --------------------------------------------------------
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  runtime::RuntimePolicyFingerprint runtime_policy_fingerprint_;
};

// ########################################################################
// This fixed field set owns every canonical value. Optional groups support events without an
// active market input while append-time validation rejects ambiguous partial groups.
struct RuntimeTraceFields {
  std::optional<RuntimeTraceSource> source;
  std::optional<model::BotId> bot_id;
  std::optional<model::SubscriptionId> subscription_id;

  // Attempt ordinals count every ordinary admission attempt, including failures. Receive sequence
  // exists only after accepted source input obtains its owner-visible envelope identity.
  std::optional<model::AdmissionOrdinal> admission_ordinal;
  std::optional<model::TurnOrdinal> turn_ordinal;
  std::optional<model::CallbackOrdinal> callback_ordinal;
  std::optional<model::SessionEpoch> session_epoch;
  std::optional<model::SequenceNumber> source_sequence;
  std::optional<model::ReceiveSequence> receive_sequence;
  std::optional<model::InstrumentMetadataRevision> metadata_revision;
  std::optional<model::BookGeneration> book_generation;
  std::optional<model::BookRevision> book_revision;
  RuntimeInputDisposition input_disposition{RuntimeInputDisposition::Unspecified};
  RuntimeMarketState previous_state{RuntimeMarketState::Unspecified};
  RuntimeMarketState state{RuntimeMarketState::Unspecified};
  RuntimeTraceFailureReason failure_reason{RuntimeTraceFailureReason::None};
  std::optional<model::Price> best_bid;
  std::optional<model::Price> best_ask;

  // --------------------------------------------------------
  // Structural equality compares every canonical runtime field and optional-presence decision.
  friend bool operator==(const RuntimeTraceFields&, const RuntimeTraceFields&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Ordinals are one-based accepted-prefix positions issued only by the runtime trace sink.
class RuntimeTraceOrdinal final {
public:

  // --------------------------------------------------------
  // Return the sink-issued one-based record position.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Compare sink-issued positions structurally and numerically.
  friend constexpr bool operator==(RuntimeTraceOrdinal, RuntimeTraceOrdinal) = default;
  friend constexpr auto operator<=>(RuntimeTraceOrdinal, RuntimeTraceOrdinal) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Interesting syntax: the private constructor and narrow friendship make skipped or forged
  // accepted-prefix positions unrepresentable outside the sink.
  friend class RuntimeTraceSink;

  // ########################################################################

  // --------------------------------------------------------
  // Restrict raw ordinal construction to the append authority.
  explicit constexpr RuntimeTraceOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  std::uint64_t value_;
};

// ########################################################################
// Only the sink originates validated runtime records; copies preserve an already accepted
// observation and cannot mint a new ordinal.
class RuntimeTraceRecord final {
public:

  // --------------------------------------------------------
  // Preserve an immutable accepted observation through explicit copy or move.
  RuntimeTraceRecord(const RuntimeTraceRecord&) = default;
  RuntimeTraceRecord& operator=(const RuntimeTraceRecord&) = default;
  RuntimeTraceRecord(RuntimeTraceRecord&&) noexcept = default;
  RuntimeTraceRecord& operator=(RuntimeTraceRecord&&) noexcept = default;

  // --------------------------------------------------------
  // Return the independent AEGISRTS schema version.
  [[nodiscard]] constexpr std::uint16_t schema_version() const noexcept {
    return runtime_trace_schema_version;
  }

  // --------------------------------------------------------
  // Return the sink-issued accepted-prefix position.
  [[nodiscard]] constexpr RuntimeTraceOrdinal ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  // Return the stable runtime observation kind.
  [[nodiscard]] constexpr RuntimeTraceEventKind kind() const noexcept { return kind_; }

  // --------------------------------------------------------
  // Borrow the complete validated fixed-field record.
  [[nodiscard]] const RuntimeTraceFields& fields() const noexcept { return fields_; }

  // --------------------------------------------------------
  // Structural equality compares the schema-significant record state.
  friend bool operator==(const RuntimeTraceRecord&, const RuntimeTraceRecord&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the bounded append authority may pair caller fields with an accepted-prefix ordinal.
  friend class RuntimeTraceSink;

  // ########################################################################

  // --------------------------------------------------------
  // Assemble a record only after the sink has completed capacity and shape validation.
  RuntimeTraceRecord(RuntimeTraceOrdinal ordinal, RuntimeTraceEventKind kind,
                     RuntimeTraceFields fields)
      : ordinal_{ordinal}, kind_{kind}, fields_{std::move(fields)} {}

  // --------------------------------------------------------
  RuntimeTraceOrdinal ordinal_;
  RuntimeTraceEventKind kind_;
  RuntimeTraceFields fields_;
};

// ########################################################################
// The M2 append authority reserves a fixed record count, never overwrites, and exposes preflight so
// a turn can reserve all of its critical evidence before mutating data-plane state.
class RuntimeTraceSink final {
public:

  // --------------------------------------------------------
  // Bind capacity, provenance, and source membership to one immutable runtime policy, then reserve
  // all record and source-registry storage before any owner turn begins.
  explicit RuntimeTraceSink(const runtime::RuntimePolicy& policy);

  // --------------------------------------------------------
  // Non-copyable and non-movable ownership prevents two authorities, including a moved-from sink,
  // from appending to divergent copies of what appears to be one canonical trace.
  RuntimeTraceSink(const RuntimeTraceSink&) = delete;
  RuntimeTraceSink& operator=(const RuntimeTraceSink&) = delete;
  RuntimeTraceSink(RuntimeTraceSink&&) = delete;
  RuntimeTraceSink& operator=(RuntimeTraceSink&&) = delete;

  // --------------------------------------------------------
  // Prove that a complete turn's critical record count fits before any external state commit.
  [[nodiscard]] model::Result<void> preflight_trace_append(std::uint32_t additional_records) const;

  // --------------------------------------------------------
  // Validate one fully constructed record shape without assigning an ordinal or changing capacity.
  [[nodiscard]] model::Result<void> validate_trace_record(RuntimeTraceEventKind kind,
                                                          const RuntimeTraceFields& fields) const;

  // --------------------------------------------------------
  // Validate and append one fixed-field observation without changing the prefix on failure.
  [[nodiscard]] model::Result<void> append_trace_record(RuntimeTraceEventKind kind,
                                                        RuntimeTraceFields fields);

  // --------------------------------------------------------
  // Borrow the complete accepted prefix in canonical append order.
  [[nodiscard]] std::span<const RuntimeTraceRecord> records() const noexcept { return records_; }

  // --------------------------------------------------------
  // Return the number of accepted records.
  [[nodiscard]] std::uint32_t record_count() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  // Return the immutable accepted-record limit.
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Borrow the one immutable provenance pair encoded into every record in this stream.
  [[nodiscard]] const RuntimeTraceProvenance& provenance() const noexcept { return provenance_; }

  // --------------------------------------------------------
  // Return the number of records that a successful preflight could still reserve.
  [[nodiscard]] std::uint32_t remaining_capacity() const noexcept {
    return capacity_ - record_count();
  }

  // --------------------------------------------------------
  // Project the accepted prefix into the independent AEGISRTS schema-1 byte stream.
  [[nodiscard]] model::Result<std::vector<std::byte>> encode_canonical_bytes() const;

  // --------------------------------------------------------
  // Hash exactly the canonical AEGISRTS byte stream with the M1 SHA-256 implementation.
  [[nodiscard]] model::Result<model::Sha256Digest> derive_digest() const;

  // --------------------------------------------------------
private:
  std::uint32_t capacity_;
  RuntimeTraceProvenance provenance_;
  std::vector<RuntimeTraceSource> sources_;
  std::vector<RuntimeTraceRecord> records_;
};

// ########################################################################
} // namespace aegis::trace
