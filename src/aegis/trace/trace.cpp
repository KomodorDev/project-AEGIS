// Purpose: validate M1 trace events and encode their canonical bounded in-memory sequence.

#include "aegis/trace/trace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::trace {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// Stream/record magic and every assigned tag are schema-1 compatibility values. Optional fields are
// still emitted with presence markers, so absence never changes the record layout.
inline constexpr std::string_view stream_magic = "AEGISTRS";
inline constexpr std::string_view record_magic = "AEGISTRC";

// ########################################################################
// Assign every record field its stable schema-1 numeric tag.
enum class TraceRecordTag : std::uint16_t {
  Ordinal = 0x0001,
  EventKind = 0x0002,
  FirmId = 0x0010,
  DeskId = 0x0011,
  BotId = 0x0012,
  StrategyId = 0x0013,
  VenueId = 0x0014,
  LogicalAccountId = 0x0015,
  InstrumentId = 0x0016,
  SubscriptionId = 0x0017,
  RouteId = 0x0018,
  ConfigurationFingerprint = 0x0020,
  ConfigurationRevision = 0x0021,
  OrganizationRevision = 0x0022,
  StrategyConfigurationRevision = 0x0023,
  SubscriptionRevision = 0x0024,
  RouteRevision = 0x0025,
  InstrumentMetadataRevision = 0x0026,
  Payload = 0x0030,
};

// ########################################################################

// --------------------------------------------------------
// Convert a record tag to its fixed-width encoded representation without implicit enum conversion.
[[nodiscard]] constexpr std::uint16_t trace_record_tag_code(TraceRecordTag value) noexcept {
  return static_cast<std::uint16_t>(value);
}

// --------------------------------------------------------

// ########################################################################
// This schema-local writer emits fixed-width big-endian primitives and fails closed on size growth;
// it is intentionally not a general serialization API.
class CanonicalTraceWriter final {
public:

  // --------------------------------------------------------
  // Append one raw byte after proving the backing buffer can grow.
  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
    return true;
  }

  // --------------------------------------------------------
  // Append already validated ASCII bytes without a length prefix.
  [[nodiscard]] bool append_ascii(std::string_view value) {
    if (!can_grow(value.size())) {
      return false;
    }
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return true;
  }

  // --------------------------------------------------------
  // Append opaque canonical bytes without assigning new field meaning.
  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  // --------------------------------------------------------
  // Encode one unsigned 16-bit integer in portable big-endian order.
  [[nodiscard]] bool append_u16(std::uint16_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Encode one unsigned 32-bit integer in portable big-endian order.
  [[nodiscard]] bool append_u32(std::uint32_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 24U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 16U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Encode one unsigned 64-bit integer in portable big-endian order.
  [[nodiscard]] bool append_u64(std::uint64_t value) {
    // Interesting syntax: the explicit zero break prevents unsigned shift wrap after emitting the
    // eighth byte, fixing every integer to portable big-endian width.
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
  // Encode a tagged field as tag, 32-bit payload length, and payload bytes.
  [[nodiscard]] bool append_field(std::uint16_t field_tag, std::span<const std::byte> payload) {
    if (payload.size() > maximum_u32_size) {
      return false;
    }
    return append_u16(field_tag) && append_u32(static_cast<std::uint32_t>(payload.size())) &&
           append_bytes(payload);
  }

  // --------------------------------------------------------
  // Encode one big-endian 16-bit integer as a tagged field.
  [[nodiscard]] bool append_u16_field(std::uint16_t field_tag, std::uint16_t value) {
    CanonicalTraceWriter payload;
    return payload.append_u16(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode one big-endian 64-bit integer as a tagged field.
  [[nodiscard]] bool append_u64_field(std::uint16_t field_tag, std::uint64_t value) {
    CanonicalTraceWriter payload;
    return payload.append_u64(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Optional values always carry an explicit presence byte to preserve canonical field shape.
  [[nodiscard]] bool append_optional_u64_field(std::uint16_t field_tag,
                                               const std::optional<std::uint64_t>& value) {
    CanonicalTraceWriter payload;
    if (!payload.append_byte(value.has_value() ? 1U : 0U)) {
      return false;
    }
    if (value.has_value() && !payload.append_u64(*value)) {
      return false;
    }
    return append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode an optional identifier with explicit presence and byte-length markers.
  template <typename Identifier>
  [[nodiscard]] bool append_optional_identifier_field(std::uint16_t field_tag,
                                                      const std::optional<Identifier>& identifier) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Emit presence independently so absent and present identifiers retain the same field shape.
    CanonicalTraceWriter payload;
    if (!payload.append_byte(identifier.has_value() ? 1U : 0U)) {
      return false;
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Encode a present identifier as a checked length plus its validated ASCII bytes.
    if (identifier.has_value()) {
      const auto value = identifier->value();
      if (value.size() > maximum_u32_size ||
          !payload.append_u32(static_cast<std::uint32_t>(value.size())) ||
          !payload.append_ascii(value)) {
        return false;
      }
    }
    return append_field(field_tag, payload.bytes());

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Prefix one encoded record with its portable 32-bit byte length.
  [[nodiscard]] bool append_length_prefixed(std::span<const std::byte> value) {
    if (value.size() > maximum_u32_size) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(value.size())) && append_bytes(value);
  }

  // --------------------------------------------------------
  // Expose a read-only view for composing nested writers without transferring ownership.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Interesting syntax: the && qualifier permits destructive extraction only from a disposable
  // writer, so an encoder cannot accidentally drain a writer it intends to keep using.
  [[nodiscard]] std::vector<std::byte> take_bytes() && { return std::move(bytes_); }

  // --------------------------------------------------------

private:
  static constexpr std::size_t maximum_u32_size =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

  // --------------------------------------------------------
  // Subtraction-form bounds checking avoids overflowing size_t while testing the requested growth.
  [[nodiscard]] bool can_grow(std::size_t additional_size) const noexcept {
    return additional_size <= bytes_.max_size() - bytes_.size();
  }

  // --------------------------------------------------------

  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Exact-shape predicates reject unexpected subjects as well as omissions, preventing event kinds
// from acquiring unassigned meaning through surplus identifiers.
[[nodiscard]] bool has_no_subjects(const TraceSubjects& subjects) noexcept {
  return !subjects.firm_id && !subjects.desk_id && !subjects.bot_id && !subjects.strategy_id &&
         !subjects.venue_id && !subjects.logical_account_id && !subjects.instrument_id &&
         !subjects.subscription_id && !subjects.route_id;
}

// --------------------------------------------------------
// Require exactly the organization and strategy subjects carried by bot attribution events.
[[nodiscard]] bool is_exact_bot_attribution(const TraceSubjects& subjects) noexcept {
  return subjects.firm_id && subjects.desk_id && subjects.bot_id && subjects.strategy_id &&
         !subjects.venue_id && !subjects.logical_account_id && !subjects.instrument_id &&
         !subjects.subscription_id && !subjects.route_id;
}

// --------------------------------------------------------
// Require exactly the bot, venue, instrument, and subscription subjects for observation grants.
[[nodiscard]] bool is_exact_subscription(const TraceSubjects& subjects) noexcept {
  return !subjects.firm_id && !subjects.desk_id && subjects.bot_id && !subjects.strategy_id &&
         subjects.venue_id && !subjects.logical_account_id && subjects.instrument_id &&
         subjects.subscription_id && !subjects.route_id;
}

// --------------------------------------------------------
// Require exactly the bot, venue, account, instrument, and route subjects for execution grants.
[[nodiscard]] bool is_exact_route(const TraceSubjects& subjects) noexcept {
  return !subjects.firm_id && !subjects.desk_id && subjects.bot_id && !subjects.strategy_id &&
         subjects.venue_id && subjects.logical_account_id && subjects.instrument_id &&
         !subjects.subscription_id && subjects.route_id;
}

// --------------------------------------------------------
// Construct the stable trace-schema error used by exact-shape validation failures.
[[nodiscard]] model::Result<void> create_invalid_trace_record_result(std::string_view field) {
  return model::Result<void>::create_failure(
      DomainError::create_at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------
// M1 binds each kind to one subject/provenance/payload schema: subscription byte 1 means OrderBook,
// while route byte 0/1 carries the assigned disabled/enabled state.
[[nodiscard]] model::Result<void> validate_event(TraceEventKind kind, const TraceSubjects& subjects,
                                                 const TraceProvenance& provenance,
                                                 const TracePayload& payload) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Dispatch each assigned event kind to its exact subject, provenance, and payload shape.
  switch (kind) {
  case TraceEventKind::ConfigurationSealed:
    if (!has_no_subjects(subjects) || !payload.bytes().empty() ||
        provenance.instrument_metadata_revision.has_value()) {
      return create_invalid_trace_record_result("trace.configuration_sealed");
    }
    break;
  case TraceEventKind::BotAttributed:
    if (!is_exact_bot_attribution(subjects) || !payload.bytes().empty() ||
        provenance.instrument_metadata_revision.has_value()) {
      return create_invalid_trace_record_result("trace.bot_attributed");
    }
    break;
  case TraceEventKind::SubscriptionConfigured:
    if (!is_exact_subscription(subjects) || !provenance.instrument_metadata_revision.has_value() ||
        payload.bytes().size() != 1U || payload.bytes().front() != std::byte{1U}) {
      return create_invalid_trace_record_result("trace.subscription_configured");
    }
    break;
  case TraceEventKind::RouteConfigured:
    if (!is_exact_route(subjects) || !provenance.instrument_metadata_revision.has_value() ||
        payload.bytes().size() != 1U ||
        (payload.bytes().front() != std::byte{0U} && payload.bytes().front() != std::byte{1U})) {
      return create_invalid_trace_record_result("trace.route_configured");
    }
    break;
  default:
    return create_invalid_trace_record_result("trace.kind");
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every record emits every tag in fixed order, including absent optionals, before any payload
// bytes.
[[nodiscard]] model::Result<std::vector<std::byte>> encode_record(const TraceRecord& record) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Project record state into the optional primitive forms used by schema-1 fields.
  CanonicalTraceWriter writer;
  const auto& subjects = record.subjects();
  const auto& provenance = record.provenance();
  const auto metadata_revision =
      provenance.instrument_metadata_revision.has_value()
          ? std::optional<std::uint64_t>{provenance.instrument_metadata_revision->value()}
          : std::nullopt;
  const auto& fingerprint = provenance.configuration_fingerprint.bytes();

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit magic, version, identity, provenance, and payload fields in their assigned tag order.
  const bool success =
      writer.append_ascii(record_magic) && writer.append_u16(record.schema_version()) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::Ordinal),
                              record.ordinal().value()) &&
      writer.append_u16_field(trace_record_tag_code(TraceRecordTag::EventKind),
                              static_cast<std::uint16_t>(record.kind())) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::FirmId),
                                              subjects.firm_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::DeskId),
                                              subjects.desk_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::BotId),
                                              subjects.bot_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::StrategyId),
                                              subjects.strategy_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::VenueId),
                                              subjects.venue_id) &&
      writer.append_optional_identifier_field(
          trace_record_tag_code(TraceRecordTag::LogicalAccountId), subjects.logical_account_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::InstrumentId),
                                              subjects.instrument_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::SubscriptionId),
                                              subjects.subscription_id) &&
      writer.append_optional_identifier_field(trace_record_tag_code(TraceRecordTag::RouteId),
                                              subjects.route_id) &&
      writer.append_field(trace_record_tag_code(TraceRecordTag::ConfigurationFingerprint),
                          fingerprint) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::ConfigurationRevision),
                              provenance.configuration_revision.value()) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::OrganizationRevision),
                              provenance.organization_revision.value()) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::StrategyConfigurationRevision),
                              provenance.strategy_configuration_revision.value()) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::SubscriptionRevision),
                              provenance.subscription_revision.value()) &&
      writer.append_u64_field(trace_record_tag_code(TraceRecordTag::RouteRevision),
                              provenance.route_revision.value()) &&
      writer.append_optional_u64_field(
          trace_record_tag_code(TraceRecordTag::InstrumentMetadataRevision), metadata_revision) &&
      writer.append_field(trace_record_tag_code(TraceRecordTag::Payload), record.payload().bytes());

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish a record only when the complete canonical encoding fits its bounded writer.
  if (!success) {
    return model::Result<std::vector<std::byte>>::create_failure(
        DomainError::create_at_field(DomainErrorCode::EncodingOverflow, "trace.record_encoding"));
  }
  return model::Result<std::vector<std::byte>>::create_success(std::move(writer).take_bytes());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reject excessive input before copying so failure cannot expose a partially initialized payload.
model::Result<TracePayload> TracePayload::copy_from(std::span<const std::byte> bytes) {
  if (bytes.size() > max_trace_payload_bytes) {
    return model::Result<TracePayload>::create_failure(
        DomainError::create_at_field(DomainErrorCode::EncodingOverflow, "trace.payload"));
  }
  TracePayload payload;
  std::copy(bytes.begin(), bytes.end(), payload.bytes_.begin());
  payload.size_ = bytes.size();
  return model::Result<TracePayload>::create_success(std::move(payload));
}

// --------------------------------------------------------
// The caller supplies any already-resolved instrument revision; this projection performs no lookup.
TraceProvenance TraceProvenance::trace_provenance_from_configuration(
    const configuration::ConfigurationProvenance& provenance,
    std::optional<model::InstrumentMetadataRevision> metadata_revision) {
  return TraceProvenance{provenance.fingerprint(),
                         provenance.configuration_revision(),
                         provenance.organization_revision(),
                         provenance.strategy_configuration_revision(),
                         provenance.subscription_revision(),
                         provenance.route_revision(),
                         metadata_revision};
}

// --------------------------------------------------------
// Reserve the declared hard bound once so accepted appends do not grow storage beyond it.
TraceSink::TraceSink(std::uint32_t capacity) : capacity_{capacity} { records_.reserve(capacity_); }

// --------------------------------------------------------
// Validate and append one record atomically while preserving the accepted prefix on failure.
model::Result<void> TraceSink::append_trace_record(TraceEventKind kind, TraceSubjects subjects,
                                                   TraceProvenance provenance,
                                                   TracePayload payload) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity has deterministic precedence and leaves the existing prefix byte-for-byte unchanged.
  if (records_.size() >= capacity_) {
    return model::Result<void>::create_failure(DomainError::create_at_index(
        DomainErrorCode::TraceCapacityExceeded, "trace.records", records_.size()));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Schema validation happens before ordinal assignment or mutation.
  auto validation = validate_event(kind, subjects, provenance, payload);
  if (!validation) {
    return validation;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Commit a one-based ordinal only for the record that is about to join the accepted prefix.
  const auto ordinal = TraceOrdinal{static_cast<std::uint64_t>(records_.size()) + 1U};
  records_.push_back(
      TraceRecord{ordinal, kind, std::move(subjects), std::move(provenance), std::move(payload)});
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A stream is magic + version + count followed by length-prefixed canonical records in append
// order.
model::Result<std::vector<std::byte>> TraceSink::encode_canonical_bytes() const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit the stream envelope before appending each accepted record in ordinal order.
  CanonicalTraceWriter writer;
  bool success = writer.append_ascii(stream_magic) && writer.append_u16(trace_schema_version) &&
                 writer.append_u32(record_count());
  for (const auto& record : records_) {
    auto encoded = encode_record(record);
    if (!encoded || !writer.append_length_prefixed(encoded.value())) {
      success = false;
      break;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish bytes only when both the envelope and every nested record encoded completely.
  if (!success) {
    return model::Result<std::vector<std::byte>>::create_failure(
        DomainError::create_at_field(DomainErrorCode::EncodingOverflow, "trace.stream_encoding"));
  }
  return model::Result<std::vector<std::byte>>::create_success(std::move(writer).take_bytes());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The trace identity is exactly SHA-256 over encode_canonical_bytes(); encoding failures propagate
// unchanged.
model::Result<model::Sha256Digest> TraceSink::derive_digest() const {
  auto encoded = encode_canonical_bytes();
  if (!encoded) {
    return model::Result<model::Sha256Digest>::create_failure(std::move(encoded).error());
  }
  return model::Result<model::Sha256Digest>::create_success(
      model::calculate_sha256_digest(encoded.value()));
}

// --------------------------------------------------------

} // namespace aegis::trace
