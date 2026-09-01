// Purpose: record bounded, deterministic M1 provenance without performing external I/O.

#pragma once

#include "aegis/configuration/configuration_provenance.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace aegis::trace {

// Both values are serialization contracts: the version identifies field semantics and the payload
// bound keeps every accepted record's caller-supplied bytes fixed and reviewable.
inline constexpr std::uint16_t trace_schema_version = 1U;
inline constexpr std::size_t max_trace_payload_bytes = 64U;

// ########################################################################
// Assigned compatibility values for the complete M1 trace vocabulary. M2 callback and dispatch
// events require their own later schema decision rather than being smuggled into this enum.
enum class TraceEventKind : std::uint16_t {
  ConfigurationSealed = 1,
  BotAttributed = 2,
  SubscriptionConfigured = 3,
  RouteConfigured = 4,
};

// ########################################################################
// One optional member per nominal identifier kind makes subject cardinality and encoding order
// intrinsic. A record owns these values and therefore never retains pointers into configuration.
struct TraceSubjects {
  std::optional<model::FirmId> firm_id;
  std::optional<model::DeskId> desk_id;
  std::optional<model::BotId> bot_id;
  std::optional<model::StrategyId> strategy_id;
  std::optional<model::VenueId> venue_id;
  std::optional<model::LogicalAccountId> logical_account_id;
  std::optional<model::InstrumentId> instrument_id;
  std::optional<model::SubscriptionId> subscription_id;
  std::optional<model::RouteId> route_id;

  // --------------------------------------------------------
  // Structural equality compares every optional nominal subject.
  friend bool operator==(const TraceSubjects&, const TraceSubjects&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Payloads copy at most the assigned bound into inline storage, so records never retain caller
// memory and payload acceptance does not require heap allocation.
class TracePayload final {
public:

  // --------------------------------------------------------
  // Construct an empty inline payload without allocation.
  TracePayload() noexcept = default;

  // --------------------------------------------------------
  // Copy a bounded byte range into record-owned inline storage.
  [[nodiscard]] static model::Result<TracePayload> copy_from(std::span<const std::byte> bytes);

  // --------------------------------------------------------
  // Borrow only the accepted payload prefix, excluding unused inline storage.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::span<const std::byte>{bytes_.data(), size_};
  }

  // --------------------------------------------------------
  // Unused array tail bytes are storage, not payload meaning, and are deliberately excluded.
  friend bool operator==(const TracePayload& left, const TracePayload& right) noexcept {
    return left.bytes().size() == right.bytes().size() &&
           std::equal(left.bytes().begin(), left.bytes().end(), right.bytes().begin());
  }

  // --------------------------------------------------------
private:
  std::array<std::byte, max_trace_payload_bytes> bytes_{};
  std::size_t size_{0U};
};

// ########################################################################
// This deliberately copies only the common configuration identity plus at most one metadata
// revision applicable to the record's venue/instrument subject.
struct TraceProvenance {
  configuration::ConfigurationFingerprint configuration_fingerprint;
  model::ConfigurationRevision configuration_revision;
  model::OrganizationRevision organization_revision;
  model::StrategyConfigurationRevision strategy_configuration_revision;
  model::SubscriptionRevision subscription_revision;
  model::RouteRevision route_revision;
  std::optional<model::InstrumentMetadataRevision> instrument_metadata_revision;

  // --------------------------------------------------------
  // Copy common provenance plus the optional instrument-specific revision into one trace value.
  [[nodiscard]] static TraceProvenance trace_provenance_from_configuration(
      const configuration::ConfigurationProvenance& provenance,
      std::optional<model::InstrumentMetadataRevision> metadata_revision = std::nullopt);

  // --------------------------------------------------------
  // Structural equality compares the complete copied provenance contract.
  friend bool operator==(const TraceProvenance&, const TraceProvenance&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Ordinals are one-based append positions issued only after a record passes validation.
class TraceOrdinal final {
public:

  // --------------------------------------------------------
  // Return the sink-issued one-based append position.
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  // --------------------------------------------------------
  // Compare sink-issued positions structurally and numerically.
  friend constexpr bool operator==(TraceOrdinal, TraceOrdinal) = default;
  friend constexpr auto operator<=>(TraceOrdinal, TraceOrdinal) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Interesting syntax: the private constructor plus TraceSink friend prevents callers from forging
  // or skipping positions in an accepted trace prefix.
  friend class TraceSink;

  // ########################################################################

  // --------------------------------------------------------
  // Restrict raw ordinal construction to the append authority.
  explicit constexpr TraceOrdinal(std::uint64_t value) noexcept : value_{value} {}

  // --------------------------------------------------------
  std::uint64_t value_;
};

// ########################################################################
// Only the sink can originate a record's validated field combination and ordinal. Public copy/move
// operations preserve that already-validated immutable observation without minting new positions.
class TraceRecord final {
public:

  // --------------------------------------------------------
  // Preserve an already validated immutable observation through explicit copy or move.
  TraceRecord(const TraceRecord&) = default;
  TraceRecord& operator=(const TraceRecord&) = default;
  TraceRecord(TraceRecord&&) noexcept = default;
  TraceRecord& operator=(TraceRecord&&) noexcept = default;

  // --------------------------------------------------------
  // Return the assigned trace schema version.
  [[nodiscard]] constexpr std::uint16_t schema_version() const noexcept {
    return trace_schema_version;
  }

  // --------------------------------------------------------
  // Return the sink-issued append position.
  [[nodiscard]] constexpr TraceOrdinal ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  // Return the assigned event kind.
  [[nodiscard]] constexpr TraceEventKind kind() const noexcept { return kind_; }

  // --------------------------------------------------------
  // Borrow the record-owned nominal subjects.
  [[nodiscard]] const TraceSubjects& subjects() const noexcept { return subjects_; }

  // --------------------------------------------------------
  // Borrow the record-owned configuration provenance.
  [[nodiscard]] const TraceProvenance& provenance() const noexcept { return provenance_; }

  // --------------------------------------------------------
  // Borrow the record-owned bounded payload.
  [[nodiscard]] const TracePayload& payload() const noexcept { return payload_; }

  // --------------------------------------------------------
  // Structural equality compares the complete immutable observation.
  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only the append authority may originate a validated record and ordinal.
  friend class TraceSink;

  // ########################################################################

  // --------------------------------------------------------
  // Assemble only a field combination and ordinal already validated by the sink.
  TraceRecord(TraceOrdinal ordinal, TraceEventKind kind, TraceSubjects subjects,
              TraceProvenance provenance, TracePayload payload)
      : ordinal_{ordinal}, kind_{kind}, subjects_{std::move(subjects)},
        provenance_{std::move(provenance)}, payload_{std::move(payload)} {}

  // --------------------------------------------------------
  TraceOrdinal ordinal_;
  TraceEventKind kind_;
  TraceSubjects subjects_;
  TraceProvenance provenance_;
  TracePayload payload_;
};

// ########################################################################
// This bounded in-memory append authority preserves its accepted prefix on every failure; canonical
// bytes and the digest are pure projections and perform no external I/O.
class TraceSink final {
public:

  // --------------------------------------------------------
  // Reserve bounded append storage for the assigned record capacity.
  explicit TraceSink(std::uint32_t capacity);

  // --------------------------------------------------------
  // Interesting syntax: deliberate move-only semantics prevent accidental duplication of append
  // authority while still allowing ownership of a complete trace to transfer.
  TraceSink(const TraceSink&) = delete;
  TraceSink& operator=(const TraceSink&) = delete;
  TraceSink(TraceSink&&) noexcept = default;
  TraceSink& operator=(TraceSink&&) noexcept = default;

  // --------------------------------------------------------
  // Validate and append one complete observation without damaging the accepted prefix on failure.
  [[nodiscard]] model::Result<void> append_trace_record(TraceEventKind kind, TraceSubjects subjects,
                                                        TraceProvenance provenance,
                                                        TracePayload payload = {});

  // --------------------------------------------------------
  // Borrow the complete accepted record prefix.
  [[nodiscard]] std::span<const TraceRecord> records() const noexcept { return records_; }

  // --------------------------------------------------------
  // Return the number of accepted records.
  [[nodiscard]] std::uint32_t record_count() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }

  // --------------------------------------------------------
  // Return the immutable maximum accepted-record count.
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Project accepted records into schema-versioned canonical bytes.
  [[nodiscard]] model::Result<std::vector<std::byte>> encode_canonical_bytes() const;

  // --------------------------------------------------------
  // Hash the same canonical byte stream with SHA-256.
  [[nodiscard]] model::Result<model::Sha256Digest> derive_digest() const;

  // --------------------------------------------------------
private:
  std::uint32_t capacity_;
  std::vector<TraceRecord> records_;
};

// ########################################################################
} // namespace aegis::trace
