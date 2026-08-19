// Purpose: validate, canonically encode, and fingerprint immutable M2 runtime policies.

#include "aegis/runtime/runtime_policy.hpp"

#include "aegis/configuration/startup_configuration.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::runtime {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// Top-level tags are persisted AEGISRTP schema-one values and must never be inferred from field
// declaration order.
enum class RuntimePolicyTag : std::uint16_t {
  ConfigurationFingerprint = 1,
  IngressCapacity = 2,
  MaximumFrameBytes = 3,
  MaximumChangesPerUpdate = 4,
  RetainedBookDepth = 5,
  StaleThresholdNanoseconds = 6,
  MaximumCallbacksPerTurn = 7,
  DiagnosticCapacity = 8,
  RuntimeTraceCapacity = 9,
  MaximumDriveTurns = 10,
  CallbackBudgetNanoseconds = 11,
  Sources = 12,
};

// ########################################################################

// ########################################################################
// Nested source tags restart at one and explicitly include the M2-fixed order-book channel so a
// future channel extension cannot reinterpret existing bytes.
enum class RuntimeSourceTag : std::uint16_t {
  SourceId = 1,
  Ordinal = 2,
  VenueId = 3,
  InstrumentId = 4,
  VenueInstrumentId = 5,
  Channel = 6,
  MetadataRevision = 7,
};

// ########################################################################

// --------------------------------------------------------
// Convert a persisted top-level enum to its fixed-width tag without implicit narrowing.
[[nodiscard]] constexpr std::uint16_t tag(RuntimePolicyTag value) noexcept {
  return static_cast<std::uint16_t>(value);
}

// --------------------------------------------------------
// Convert a persisted source-record enum to its fixed-width tag without implicit narrowing.
[[nodiscard]] constexpr std::uint16_t tag(RuntimeSourceTag value) noexcept {
  return static_cast<std::uint16_t>(value);
}

// --------------------------------------------------------

// ########################################################################
// This private writer implements only the fixed AEGISRTP schema-one primitives. It is deliberately
// not a general serialization API.
class CanonicalRuntimePolicyWriter final {
public:

  // --------------------------------------------------------
  // Append validated ASCII without a length prefix when writing the fixed stream magic.
  [[nodiscard]] bool append_ascii_raw(std::string_view value) {
    if (!can_grow(value.size())) {
      return false;
    }
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return true;
  }

  // --------------------------------------------------------
  // Append one raw byte after proving the backing vector can grow.
  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
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
    // Interesting syntax: the explicit zero break prevents unsigned shift wrap after the eighth
    // byte and fixes the integer representation to big-endian width.
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
  // Append an opaque fixed or nested byte sequence without assigning it another schema meaning.
  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  // --------------------------------------------------------
  // Encode a field as fixed tag, 32-bit payload length, and exact payload bytes.
  [[nodiscard]] bool append_field(std::uint16_t field_tag, std::span<const std::byte> payload) {
    if (payload.size() > maximum_u32_size) {
      return false;
    }
    return append_u16(field_tag) && append_u32(static_cast<std::uint32_t>(payload.size())) &&
           append_bytes(payload);
  }

  // --------------------------------------------------------
  // Encode one already validated identifier spelling as a tagged ASCII payload.
  [[nodiscard]] bool append_ascii_field(std::uint16_t field_tag, std::string_view value) {
    if (value.size() > maximum_u32_size || !append_u16(field_tag) ||
        !append_u32(static_cast<std::uint32_t>(value.size()))) {
      return false;
    }
    return append_ascii_raw(value);
  }

  // --------------------------------------------------------
  // Encode one byte as a tagged fixed-width payload.
  [[nodiscard]] bool append_u8_field(std::uint16_t field_tag, std::uint8_t value) {
    const std::array payload{std::byte{value}};
    return append_field(field_tag, payload);
  }

  // --------------------------------------------------------
  // Encode one unsigned 32-bit value as a tagged big-endian payload.
  [[nodiscard]] bool append_u32_field(std::uint16_t field_tag, std::uint32_t value) {
    CanonicalRuntimePolicyWriter payload;
    return payload.append_u32(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode one unsigned 64-bit value as a tagged big-endian payload.
  [[nodiscard]] bool append_u64_field(std::uint16_t field_tag, std::uint64_t value) {
    CanonicalRuntimePolicyWriter payload;
    return payload.append_u64(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Prefix one nested canonical record with a portable 32-bit byte length.
  [[nodiscard]] bool append_length_prefixed(std::span<const std::byte> value) {
    if (value.size() > maximum_u32_size) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(value.size())) && append_bytes(value);
  }

  // --------------------------------------------------------
  // Borrow current bytes while composing a nested record or field.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Interesting syntax: the rvalue qualifier permits destructive extraction only from a writer
  // whose contents cannot be needed again by its caller.
  [[nodiscard]] std::vector<std::byte> take_bytes() && { return std::move(bytes_); }

  // --------------------------------------------------------
private:
  static constexpr std::size_t maximum_u32_size =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

  // --------------------------------------------------------
  // Subtraction-form bounds checking avoids overflow in size plus additional bytes.
  [[nodiscard]] bool can_grow(std::size_t additional) const noexcept {
    return additional <= maximum_u32_size && bytes_.size() <= maximum_u32_size - additional;
  }

  // --------------------------------------------------------
  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Return one stable policy error without exposing prose or caller-dependent values.
[[nodiscard]] model::Result<void> invalid_policy(std::string field) {
  return model::Result<void>::failure(
      DomainError::at_field(DomainErrorCode::InvalidRuntimePolicy, std::move(field)));
}

// --------------------------------------------------------
// Validate every positive limit and compile-time ceiling before source relationships are inspected.
[[nodiscard]] model::Result<void> validate_limits(const RuntimePolicyLimits& limits) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absence in the fixed order that the limits are encoded and exposed.
  if (limits.ingress_capacity == 0U) {
    return invalid_policy("runtime_policy.ingress_capacity");
  }
  if (limits.maximum_frame_bytes == 0U ||
      limits.maximum_frame_bytes > maximum_runtime_frame_bytes) {
    return invalid_policy("runtime_policy.maximum_frame_bytes");
  }
  if (limits.maximum_changes_per_update == 0U ||
      limits.maximum_changes_per_update > maximum_runtime_changes_per_update) {
    return invalid_policy("runtime_policy.maximum_changes_per_update");
  }
  if (limits.retained_book_depth == 0U ||
      limits.retained_book_depth > maximum_runtime_retained_book_depth) {
    return invalid_policy("runtime_policy.retained_book_depth");
  }
  if (limits.stale_threshold_nanoseconds == 0U) {
    return invalid_policy("runtime_policy.stale_threshold_nanoseconds");
  }
  if (limits.maximum_callbacks_per_turn == 0U) {
    return invalid_policy("runtime_policy.maximum_callbacks_per_turn");
  }
  if (limits.diagnostic_capacity == 0U) {
    return invalid_policy("runtime_policy.diagnostic_capacity");
  }
  if (limits.runtime_trace_capacity == 0U) {
    return invalid_policy("runtime_policy.runtime_trace_capacity");
  }
  if (limits.maximum_drive_turns == 0U) {
    return invalid_policy("runtime_policy.maximum_drive_turns");
  }
  if (limits.callback_budget_nanoseconds == 0U) {
    return invalid_policy("runtime_policy.callback_budget_nanoseconds");
  }
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate one source against sealed configuration catalogs and compute its exact callback fanout.
[[nodiscard]] model::Result<std::uint64_t>
validate_source(const configuration::StartupConfiguration& configuration,
                const RuntimeSourceDefinition& source, std::size_t index,
                std::uint32_t maximum_callbacks_per_turn) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the configured venue and venue/instrument metadata without accepting partial keys.
  if (configuration.find_venue(source.venue_id) == nullptr) {
    return model::Result<std::uint64_t>::failure(DomainError::at_index(
        DomainErrorCode::RuntimeSourceNotConfigured, "runtime_policy.sources.venue_id", index));
  }
  const auto* const metadata =
      configuration.find_instrument_metadata(source.venue_id, source.instrument_id);
  if (metadata == nullptr) {
    return model::Result<std::uint64_t>::failure(
        DomainError::at_index(DomainErrorCode::RuntimeSourceNotConfigured,
                              "runtime_policy.sources.instrument_id", index));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the venue-native name and metadata revision to match the immutable M1 catalog exactly.
  if (metadata->venue_instrument_id() != source.venue_instrument_id) {
    return model::Result<std::uint64_t>::failure(
        DomainError::at_index(DomainErrorCode::InvalidRuntimePolicy,
                              "runtime_policy.sources.venue_instrument_id", index));
  }
  if (metadata->revision() != source.metadata_revision) {
    return model::Result<std::uint64_t>::failure(DomainError::at_index(
        DomainErrorCode::InvalidRuntimePolicy, "runtime_policy.sources.metadata_revision", index));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Count only exact order-book grants. A recovery snapshot emits a Ready state callback followed
  // by a market callback for every grant, which is the maximum callback fanout of one owner turn.
  std::uint64_t matching_subscriptions = 0U;
  for (const auto& subscription : configuration.subscriptions().subscriptions()) {
    if (subscription.venue_id == source.venue_id &&
        subscription.instrument_id == source.instrument_id &&
        subscription.channel == market_data::SubscriptionChannel::OrderBook) {
      ++matching_subscriptions;
    }
  }
  if (matching_subscriptions == 0U) {
    return model::Result<std::uint64_t>::failure(DomainError::at_index(
        DomainErrorCode::RuntimeSourceNotConfigured, "runtime_policy.sources.subscription", index));
  }
  if (matching_subscriptions > static_cast<std::uint64_t>(maximum_callbacks_per_turn / 2U)) {
    return model::Result<std::uint64_t>::failure(DomainError::at_index(
        DomainErrorCode::InvalidRuntimePolicy, "runtime_policy.sources.fanout", index));
  }
  return model::Result<std::uint64_t>::success(matching_subscriptions);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Encode one already validated configured source including its fixed M2 observation channel.
[[nodiscard]] model::Result<std::vector<std::byte>> encode_source(const RuntimeSource& source) {
  CanonicalRuntimePolicyWriter writer;
  const bool encoded =
      writer.append_ascii_field(tag(RuntimeSourceTag::SourceId),
                                source.definition().source_id.value()) &&
      writer.append_u64_field(tag(RuntimeSourceTag::Ordinal), source.ordinal().value()) &&
      writer.append_ascii_field(tag(RuntimeSourceTag::VenueId),
                                source.definition().venue_id.value()) &&
      writer.append_ascii_field(tag(RuntimeSourceTag::InstrumentId),
                                source.definition().instrument_id.value()) &&
      writer.append_ascii_field(tag(RuntimeSourceTag::VenueInstrumentId),
                                source.definition().venue_instrument_id.value()) &&
      writer.append_u8_field(tag(RuntimeSourceTag::Channel),
                             static_cast<std::uint8_t>(RuntimeSource::channel())) &&
      writer.append_u64_field(tag(RuntimeSourceTag::MetadataRevision),
                              source.definition().metadata_revision.value());
  if (!encoded) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "runtime_policy.sources"));
  }
  return model::Result<std::vector<std::byte>>::success(std::move(writer).take_bytes());
}

// --------------------------------------------------------
// Encode the canonical source sequence as count plus individually length-prefixed records.
[[nodiscard]] model::Result<std::vector<std::byte>>
encode_sources(const std::vector<RuntimeSource>& sources) {
  if (sources.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "runtime_policy.sources"));
  }

  CanonicalRuntimePolicyWriter writer;
  if (!writer.append_u32(static_cast<std::uint32_t>(sources.size()))) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "runtime_policy.sources"));
  }
  for (const auto& source : sources) {
    auto encoded = encode_source(source);
    if (!encoded || !writer.append_length_prefixed(encoded.value())) {
      return model::Result<std::vector<std::byte>>::failure(
          DomainError::at_field(DomainErrorCode::EncodingOverflow, "runtime_policy.sources"));
    }
  }
  return model::Result<std::vector<std::byte>>::success(std::move(writer).take_bytes());
}

// --------------------------------------------------------
// Encode the complete tagged schema after all values and relationships are known valid.
[[nodiscard]] model::Result<std::vector<std::byte>>
encode_policy(const configuration::ConfigurationFingerprint& configuration_fingerprint,
              const RuntimePolicyLimits& limits, const std::vector<RuntimeSource>& sources) {
  auto source_bytes = encode_sources(sources);
  if (!source_bytes) {
    return model::Result<std::vector<std::byte>>::failure(source_bytes.error());
  }

  CanonicalRuntimePolicyWriter writer;
  const bool encoded =
      writer.append_ascii_raw("AEGISRTP") &&
      writer.append_u16(canonical_runtime_policy_schema_version) &&
      writer.append_field(tag(RuntimePolicyTag::ConfigurationFingerprint),
                          configuration_fingerprint.bytes()) &&
      writer.append_u32_field(tag(RuntimePolicyTag::IngressCapacity), limits.ingress_capacity) &&
      writer.append_u32_field(tag(RuntimePolicyTag::MaximumFrameBytes),
                              limits.maximum_frame_bytes) &&
      writer.append_u32_field(tag(RuntimePolicyTag::MaximumChangesPerUpdate),
                              limits.maximum_changes_per_update) &&
      writer.append_u32_field(tag(RuntimePolicyTag::RetainedBookDepth),
                              limits.retained_book_depth) &&
      writer.append_u64_field(tag(RuntimePolicyTag::StaleThresholdNanoseconds),
                              limits.stale_threshold_nanoseconds) &&
      writer.append_u32_field(tag(RuntimePolicyTag::MaximumCallbacksPerTurn),
                              limits.maximum_callbacks_per_turn) &&
      writer.append_u32_field(tag(RuntimePolicyTag::DiagnosticCapacity),
                              limits.diagnostic_capacity) &&
      writer.append_u32_field(tag(RuntimePolicyTag::RuntimeTraceCapacity),
                              limits.runtime_trace_capacity) &&
      writer.append_u32_field(tag(RuntimePolicyTag::MaximumDriveTurns),
                              limits.maximum_drive_turns) &&
      writer.append_u64_field(tag(RuntimePolicyTag::CallbackBudgetNanoseconds),
                              limits.callback_budget_nanoseconds) &&
      writer.append_field(tag(RuntimePolicyTag::Sources), source_bytes.value());
  if (!encoded) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "runtime_policy"));
  }
  return model::Result<std::vector<std::byte>>::success(std::move(writer).take_bytes());
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Render the binary identity without changing or recomputing it.
std::string RuntimePolicyFingerprint::to_hex() const {
  const model::Sha256Hex hex = model::sha256_hex(bytes_);
  return std::string{hex.begin(), hex.end()};
}

// --------------------------------------------------------
// Validate the complete authored policy before publishing any canonicalized or derived state.
model::Result<RuntimePolicy>
RuntimePolicy::create(const configuration::StartupConfiguration& configuration,
                      RuntimePolicyParams params) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject limit defects before source canonicalization can obscure the primary authoring error.
  const auto limits_validation = validate_limits(params.limits);
  if (!limits_validation) {
    return model::Result<RuntimePolicy>::failure(limits_validation.error());
  }
  if (params.sources.empty()) {
    return model::Result<RuntimePolicy>::failure(
        DomainError::at_field(DomainErrorCode::EmptyCollection, "runtime_policy.sources"));
  }
  if (params.sources.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return model::Result<RuntimePolicy>::failure(
        DomainError::at_field(DomainErrorCode::InvalidRuntimePolicy, "runtime_policy.sources"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical source-ID order stabilizes duplicate positions, ordinals, bytes, and lookup behavior.
  std::sort(params.sources.begin(), params.sources.end(),
            [](const RuntimeSourceDefinition& lhs, const RuntimeSourceDefinition& rhs) {
              return lhs.source_id < rhs.source_id;
            });
  for (std::size_t index = 1U; index < params.sources.size(); ++index) {
    if (params.sources[index - 1U].source_id == params.sources[index].source_id) {
      return model::Result<RuntimePolicy>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "runtime_policy.sources.source_id", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject redundant-feed keys before dependency lookup so one key always resolves unambiguously.
  using SourceKey =
      std::tuple<model::VenueId, model::InstrumentId, market_data::SubscriptionChannel>;
  std::set<SourceKey> source_keys;
  for (std::size_t index = 0U; index < params.sources.size(); ++index) {
    const auto& source = params.sources[index];
    if (!source_keys
             .emplace(source.venue_id, source.instrument_id,
                      market_data::SubscriptionChannel::OrderBook)
             .second) {
      return model::Result<RuntimePolicy>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "runtime_policy.sources.key", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate dependencies and assign ordinals only in the final canonical source order.
  std::vector<RuntimeSource> sources;
  sources.reserve(params.sources.size());
  std::uint64_t maximum_matching_subscriptions = 0U;
  for (std::size_t index = 0U; index < params.sources.size(); ++index) {
    const auto validation = validate_source(configuration, params.sources[index], index,
                                            params.limits.maximum_callbacks_per_turn);
    if (!validation) {
      return model::Result<RuntimePolicy>::failure(validation.error());
    }
    maximum_matching_subscriptions = std::max(maximum_matching_subscriptions, validation.value());
    const auto ordinal = model::MarketSourceOrdinal::from_value(index + 1U);
    if (!ordinal) {
      return model::Result<RuntimePolicy>::failure(ordinal.error());
    }
    sources.push_back(RuntimeSource{std::move(params.sources[index]), ordinal.value(),
                                    static_cast<std::uint32_t>(validation.value())});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // One recovery-snapshot turn needs an input record, a state transition, two callbacks per grant,
  // and one reserved first re-entry record for every callback. Reject policies unable to represent
  // even that maximum single-turn accepted prefix.
  const std::uint64_t minimum_trace_capacity = 2U + (4U * maximum_matching_subscriptions);
  if (static_cast<std::uint64_t>(params.limits.runtime_trace_capacity) < minimum_trace_capacity) {
    return model::Result<RuntimePolicy>::failure(DomainError::at_field(
        DomainErrorCode::InvalidRuntimePolicy, "runtime_policy.runtime_trace_capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive bytes and fingerprint from the same validated values before atomically publishing.
  const auto configuration_fingerprint = configuration.fingerprint();
  auto canonical_bytes = encode_policy(configuration_fingerprint, params.limits, sources);
  if (!canonical_bytes) {
    return model::Result<RuntimePolicy>::failure(canonical_bytes.error());
  }
  RuntimePolicyFingerprint fingerprint{model::sha256(canonical_bytes.value())};
  return model::Result<RuntimePolicy>::success(
      RuntimePolicy{configuration_fingerprint, params.limits, std::move(sources),
                    std::move(canonical_bytes).value(), std::move(fingerprint)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Canonical source-ID order makes binary search sufficient without a mutable secondary index.
const RuntimeSource*
RuntimePolicy::find_source(const model::MarketSourceId& source_id) const noexcept {
  const auto found =
      std::lower_bound(sources_.begin(), sources_.end(), source_id,
                       [](const RuntimeSource& source, const model::MarketSourceId& target) {
                         return source.definition().source_id < target;
                       });
  if (found == sources_.end() || found->definition().source_id != source_id) {
    return nullptr;
  }
  return &*found;
}

// --------------------------------------------------------
// M2's single-source-per-key invariant makes a bounded linear lookup unambiguous and immutable.
const RuntimeSource* RuntimePolicy::find_source(const RuntimeSourceKey& key) const noexcept {
  const auto found =
      std::find_if(sources_.begin(), sources_.end(), [&](const RuntimeSource& source) {
        return source.definition().venue_id == key.venue_id &&
               source.definition().instrument_id == key.instrument_id &&
               RuntimeSource::channel() == key.channel;
      });
  return found == sources_.end() ? nullptr : &*found;
}

// --------------------------------------------------------

} // namespace aegis::runtime
