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

// Assigned compatibility values for the complete M1 trace vocabulary. M2 callback and dispatch
// events require their own later schema decision rather than being smuggled into this enum.
enum class TraceEventKind : std::uint16_t {
  ConfigurationSealed = 1,
  BotAttributed = 2,
  SubscriptionConfigured = 3,
  RouteConfigured = 4,
};

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

  friend bool operator==(const TraceSubjects&, const TraceSubjects&) = default;
};

// Payloads copy at most the assigned bound into inline storage, so records never retain caller
// memory and payload acceptance does not require heap allocation.
class TracePayload final {
public:
  TracePayload() noexcept = default;

  [[nodiscard]] static model::Result<TracePayload> copy_from(std::span<const std::byte> bytes);

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return std::span<const std::byte>{bytes_.data(), size_};
  }

  // Unused array tail bytes are storage, not payload meaning, and are deliberately excluded.
  friend bool operator==(const TracePayload& left, const TracePayload& right) noexcept {
    return left.bytes().size() == right.bytes().size() &&
           std::equal(left.bytes().begin(), left.bytes().end(), right.bytes().begin());
  }

private:
  std::array<std::byte, max_trace_payload_bytes> bytes_{};
  std::size_t size_{0U};
};

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

  [[nodiscard]] static TraceProvenance
  from(const configuration::ConfigurationProvenance& provenance,
       std::optional<model::InstrumentMetadataRevision> metadata_revision = std::nullopt);

  friend bool operator==(const TraceProvenance&, const TraceProvenance&) = default;
};

// Ordinals are one-based append positions issued only after a record passes validation.
class TraceOrdinal final {
public:
  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

  friend constexpr bool operator==(TraceOrdinal, TraceOrdinal) = default;
  friend constexpr auto operator<=>(TraceOrdinal, TraceOrdinal) = default;

private:
  // Interesting syntax: the private constructor plus TraceSink friend prevents callers from forging
  // or skipping positions in an accepted trace prefix.
  friend class TraceSink;

  explicit constexpr TraceOrdinal(std::uint64_t value) noexcept : value_{value} {}

  std::uint64_t value_;
};

// Only the sink can originate a record's validated field combination and ordinal. Public copy/move
// operations preserve that already-validated immutable observation without minting new positions.
class TraceRecord final {
public:
  TraceRecord(const TraceRecord&) = default;
  TraceRecord& operator=(const TraceRecord&) = default;
  TraceRecord(TraceRecord&&) noexcept = default;
  TraceRecord& operator=(TraceRecord&&) noexcept = default;

  [[nodiscard]] constexpr std::uint16_t schema_version() const noexcept {
    return trace_schema_version;
  }
  [[nodiscard]] constexpr TraceOrdinal ordinal() const noexcept { return ordinal_; }
  [[nodiscard]] constexpr TraceEventKind kind() const noexcept { return kind_; }
  [[nodiscard]] const TraceSubjects& subjects() const noexcept { return subjects_; }
  [[nodiscard]] const TraceProvenance& provenance() const noexcept { return provenance_; }
  [[nodiscard]] const TracePayload& payload() const noexcept { return payload_; }

  friend bool operator==(const TraceRecord&, const TraceRecord&) = default;

private:
  friend class TraceSink;

  TraceRecord(TraceOrdinal ordinal, TraceEventKind kind, TraceSubjects subjects,
              TraceProvenance provenance, TracePayload payload)
      : ordinal_{ordinal}, kind_{kind}, subjects_{std::move(subjects)},
        provenance_{std::move(provenance)}, payload_{std::move(payload)} {}

  TraceOrdinal ordinal_;
  TraceEventKind kind_;
  TraceSubjects subjects_;
  TraceProvenance provenance_;
  TracePayload payload_;
};

// This bounded in-memory append authority preserves its accepted prefix on every failure; canonical
// bytes and the digest are pure projections and perform no external I/O.
class TraceSink final {
public:
  explicit TraceSink(std::uint32_t capacity);

  // Interesting syntax: deliberate move-only semantics prevent accidental duplication of append
  // authority while still allowing ownership of a complete trace to transfer.
  TraceSink(const TraceSink&) = delete;
  TraceSink& operator=(const TraceSink&) = delete;
  TraceSink(TraceSink&&) noexcept = default;
  TraceSink& operator=(TraceSink&&) noexcept = default;

  [[nodiscard]] model::Result<void> append(TraceEventKind kind, TraceSubjects subjects,
                                           TraceProvenance provenance, TracePayload payload = {});

  [[nodiscard]] std::span<const TraceRecord> records() const noexcept { return records_; }
  [[nodiscard]] std::uint32_t size() const noexcept {
    return static_cast<std::uint32_t>(records_.size());
  }
  [[nodiscard]] constexpr std::uint32_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] model::Result<std::vector<std::byte>> canonical_bytes() const;
  [[nodiscard]] model::Result<model::Sha256Digest> digest() const;

private:
  std::uint32_t capacity_;
  std::vector<TraceRecord> records_;
};

} // namespace aegis::trace
