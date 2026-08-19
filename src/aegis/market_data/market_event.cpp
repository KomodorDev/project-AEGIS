// Purpose: validate normalized market contracts and derive timing-independent canonical payload
// identities before owner-local books or strategies can observe input.

#include "aegis/market_data/market_event.hpp"

#include "aegis/runtime/runtime_policy.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::market_data {
namespace {

// ########################################################################
// Stream explicitly sized network-order values into SHA-256 so canonical payload identity never
// depends on host layout, endianness, padding, or standard-library serialization.
class CanonicalDigestWriter final {
public:

  // --------------------------------------------------------
  // Append one byte without exposing the hash implementation to callers.
  void append_u8(std::uint8_t value) noexcept {
    const std::array bytes{static_cast<std::byte>(value)};
    hash_.update(bytes);
  }

  // --------------------------------------------------------
  // Append a 16-bit integer in network byte order.
  void append_u16(std::uint16_t value) noexcept {
    const std::array bytes{static_cast<std::byte>((value >> 8U) & 0xffU),
                           static_cast<std::byte>(value & 0xffU)};
    hash_.update(bytes);
  }

  // --------------------------------------------------------
  // Append a 64-bit integer in network byte order.
  void append_u64(std::uint64_t value) noexcept {
    std::array<std::byte, 8U> bytes{};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
      const auto shift = static_cast<unsigned int>((bytes.size() - 1U - index) * 8U);
      bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
    hash_.update(bytes);
  }

  // --------------------------------------------------------
  // Prefix variable text with a fixed-width length so adjacent identifier fields cannot collide.
  void append_string(std::string_view value) noexcept {
    append_u64(static_cast<std::uint64_t>(value.size()));
    hash_.update(std::as_bytes(std::span<const char>{value.data(), value.size()}));
  }

  // --------------------------------------------------------
  // Preserve exact signed coefficient bits plus canonical decimal scale for either nominal value.
  template <typename Decimal> void append_decimal(Decimal value) noexcept {
    append_u64(static_cast<std::uint64_t>(value.coefficient()));
    append_u8(value.scale());
  }

  // --------------------------------------------------------
  // Append already canonical fixed-width bytes without another length prefix.
  void append_digest(const model::Sha256Digest& digest) noexcept { hash_.update(digest); }

  // --------------------------------------------------------
  // Finalize a copy of the streaming state into the complete canonical identity.
  [[nodiscard]] model::Sha256Digest finalize() const noexcept { return hash_.finalize(); }

  // --------------------------------------------------------
private:
  model::Sha256 hash_;
};

// ########################################################################

// --------------------------------------------------------
// Recognize only assigned update-kind values before branching or canonical encoding.
[[nodiscard]] constexpr bool is_valid(MarketUpdateKind kind) noexcept {
  return kind == MarketUpdateKind::Snapshot || kind == MarketUpdateKind::Delta;
}

// --------------------------------------------------------
// Recognize only assigned book-side values before canonical sorting.
[[nodiscard]] constexpr bool is_valid(BookSide side) noexcept {
  return side == BookSide::Bid || side == BookSide::Ask;
}

// --------------------------------------------------------
// Recognize both integrity outcomes because rejected input must still reach deterministic validity
// classification without becoming a Ready market event.
[[nodiscard]] constexpr bool is_valid(IntegrityVerdict verdict) noexcept {
  return verdict == IntegrityVerdict::Accepted || verdict == IntegrityVerdict::Rejected;
}

// --------------------------------------------------------
// Recognize all and only strategy-visible readiness states.
[[nodiscard]] constexpr bool is_valid(MarketReadiness readiness) noexcept {
  return readiness == MarketReadiness::Synchronizing || readiness == MarketReadiness::Ready ||
         readiness == MarketReadiness::Stale || readiness == MarketReadiness::Invalid;
}

// --------------------------------------------------------
// Sort semantic changes independently from authored order: bids precede asks and exact price is
// ascending within each side.
[[nodiscard]] bool canonical_change_less(const MarketLevelChange& left,
                                         const MarketLevelChange& right) noexcept {
  if (left.side != right.side) {
    return static_cast<std::uint8_t>(left.side) < static_cast<std::uint8_t>(right.side);
  }
  return left.price < right.price;
}

// --------------------------------------------------------
// Hash every semantic field in an explicit schema order while excluding receive and processing
// identity so equivalent feed payloads retain one duplicate key.
[[nodiscard]] model::Sha256Digest
canonical_payload_digest(const NormalizedMarketUpdateFields& fields) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Domain-separate normalized-market bytes and bind them to the assigned schema.
  CanonicalDigestWriter writer;
  writer.append_string("AEGISNMD");
  writer.append_u16(normalized_market_schema_version);
  writer.append_u8(static_cast<std::uint8_t>(fields.kind));

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode the complete configured source and source-owned sequence identity.
  writer.append_string(fields.source.source_id().value());
  writer.append_u64(fields.source.source_ordinal().value());
  writer.append_string(fields.source.venue_id().value());
  writer.append_string(fields.source.instrument_id().value());
  writer.append_string(fields.source.venue_instrument_id().value());
  writer.append_u64(fields.session_epoch.value());
  writer.append_u64(fields.source_sequence.value());
  writer.append_u8(fields.predecessor_sequence.has_value() ? 1U : 0U);
  if (fields.predecessor_sequence.has_value()) {
    writer.append_u64(fields.predecessor_sequence->value());
  }
  writer.append_u64(fields.source_timestamp.nanoseconds());

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind metadata and algorithm-neutral integrity identity before level content.
  writer.append_u64(fields.metadata_revision.value());
  writer.append_u8(static_cast<std::uint8_t>(fields.integrity.verdict));
  writer.append_digest(fields.integrity.token_identity.digest());

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode the factory-canonicalized change sequence with exact nominal decimal values.
  writer.append_u64(static_cast<std::uint64_t>(fields.changes.size()));
  for (const auto& change : fields.changes) {
    writer.append_u8(static_cast<std::uint8_t>(change.side));
    writer.append_decimal(change.price);
    writer.append_decimal(change.quantity);
  }
  return writer.finalize();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Create one stable field-level market-contract failure.
[[nodiscard]] model::DomainError invalid_event(model::DomainErrorCode code, std::string field) {
  return model::DomainError::at_field(code, std::move(field));
}

// --------------------------------------------------------
// Create one stable indexed market-contract failure after canonical ordering.
[[nodiscard]] model::DomainError invalid_change(model::DomainErrorCode code, std::string field,
                                                std::size_t index) {
  return model::DomainError::at_index(code, std::move(field), index);
}

// --------------------------------------------------------
// Startup and owner-local resynchronization have no accepted ingress identity.
[[nodiscard]] bool has_owner_transition_context(const MarketStateEventFields& fields) noexcept {
  return !fields.session_epoch && !fields.source_sequence && !fields.receive_sequence &&
         !fields.receive_timestamp && !fields.admission_ordinal && !fields.metadata_revision;
}

// --------------------------------------------------------
// A parsed market update retains every source, receive, attempt, and metadata field together.
[[nodiscard]] bool has_update_transition_context(const MarketStateEventFields& fields) noexcept {
  return fields.session_epoch.has_value() && fields.source_sequence.has_value() &&
         fields.receive_sequence.has_value() && fields.receive_timestamp.has_value() &&
         fields.admission_ordinal.has_value() && fields.metadata_revision.has_value();
}

// --------------------------------------------------------
// Parsed controls and attributable malformed frames retain their trusted accepted envelope only.
[[nodiscard]] bool has_envelope_transition_context(const MarketStateEventFields& fields) noexcept {
  return fields.session_epoch.has_value() && !fields.source_sequence &&
         fields.receive_sequence.has_value() && fields.receive_timestamp.has_value() &&
         fields.admission_ordinal.has_value() && !fields.metadata_revision;
}

// --------------------------------------------------------
// A rejected-admission discontinuity has an attempt position but no accepted receive envelope.
[[nodiscard]] bool
has_discontinuity_transition_context(const MarketStateEventFields& fields) noexcept {
  return !fields.session_epoch && !fields.source_sequence && !fields.receive_sequence &&
         !fields.receive_timestamp && fields.admission_ordinal.has_value() &&
         !fields.metadata_revision;
}

// --------------------------------------------------------
// A book identity is either absent before any commit or a coherent generation/revision pair.
[[nodiscard]] bool has_supported_book_identity(const MarketStateEventFields& fields) noexcept {
  if (fields.book_generation.has_value() != fields.book_revision.has_value()) {
    return false;
  }
  return !fields.book_generation ||
         fields.book_revision->value() >= fields.book_generation->value();
}

// --------------------------------------------------------
// States that prove prior or current readiness cannot omit the last committed book identity.
[[nodiscard]] bool requires_book_identity(const MarketStateEventFields& fields) noexcept {
  return fields.readiness == MarketReadiness::Ready || fields.readiness == MarketReadiness::Stale ||
         fields.previous_readiness == MarketReadiness::Ready ||
         fields.previous_readiness == MarketReadiness::Stale;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Preserve the exact configured tuple and canonical ordinal published by RuntimePolicy.
MarketSourceIdentity
MarketSourceIdentity::from_runtime_source(const runtime::RuntimeSource& source) {
  const auto& definition = source.definition();
  return MarketSourceIdentity{definition.source_id, source.ordinal(), definition.venue_id,
                              definition.instrument_id, definition.venue_instrument_id};
}

// --------------------------------------------------------
// Hash a bounded opaque token into the fixed-width normalized integrity identity.
model::Result<IntegrityTokenIdentity> IntegrityTokenIdentity::from_token(std::string_view token) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absence and oversized adapter input before any normalized identity is published.
  if (token.empty() || token.size() > maximum_integrity_token_bytes) {
    return model::Result<IntegrityTokenIdentity>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketEvent, "market_update.integrity_token"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Hash exact token bytes; lexical grammar belongs to the parser that supplied them.
  const auto bytes = std::as_bytes(std::span<const char>{token.data(), token.size()});
  return model::Result<IntegrityTokenIdentity>::success(from_digest(model::sha256(bytes)));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Validate and canonicalize a complete pre-book update without publishing partial output.
model::Result<NormalizedMarketUpdate>
NormalizedMarketUpdate::create(NormalizedMarketUpdateFields fields, std::size_t maximum_changes) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate closed enums and the immutable bound before interpreting any change content.
  if (!is_valid(fields.kind)) {
    return model::Result<NormalizedMarketUpdate>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketEvent, "market_update.kind"));
  }
  if (!is_valid(fields.integrity.verdict)) {
    return model::Result<NormalizedMarketUpdate>::failure(invalid_event(
        model::DomainErrorCode::InvalidMarketEvent, "market_update.integrity_verdict"));
  }
  if (maximum_changes == 0U || maximum_changes > maximum_changes_per_market_update) {
    return model::Result<NormalizedMarketUpdate>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketEvent, "market_update.maximum_changes"));
  }
  if (fields.changes.size() > maximum_changes) {
    return model::Result<NormalizedMarketUpdate>::failure(
        invalid_event(model::DomainErrorCode::MarketBookCapacityExceeded, "market_update.changes"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject invalid sides and financial shapes before sorting could assign them meaning.
  for (std::size_t index = 0U; index < fields.changes.size(); ++index) {
    const auto& change = fields.changes[index];
    if (!is_valid(change.side)) {
      return model::Result<NormalizedMarketUpdate>::failure(invalid_change(
          model::DomainErrorCode::InvalidMarketEvent, "market_update.changes.side", index));
    }
    if (change.price.coefficient() <= 0) {
      return model::Result<NormalizedMarketUpdate>::failure(invalid_change(
          model::DomainErrorCode::InvalidMarketEvent, "market_update.changes.price", index));
    }
    if (change.quantity.coefficient() < 0 ||
        (fields.kind == MarketUpdateKind::Snapshot && change.quantity.coefficient() == 0)) {
      return model::Result<NormalizedMarketUpdate>::failure(invalid_change(
          model::DomainErrorCode::InvalidMarketEvent, "market_update.changes.quantity", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonicalize independent of fixture order, then detect duplicate side/price keys exactly.
  std::sort(fields.changes.begin(), fields.changes.end(), canonical_change_less);
  for (std::size_t index = 1U; index < fields.changes.size(); ++index) {
    const auto& previous = fields.changes[index - 1U];
    const auto& current = fields.changes[index];
    if (previous.side == current.side && previous.price == current.price) {
      return model::Result<NormalizedMarketUpdate>::failure(invalid_change(
          model::DomainErrorCode::InvalidMarketEvent, "market_update.changes.price", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive semantic identity only from the completely validated canonical field set.
  const auto payload_digest = canonical_payload_digest(fields);
  return model::Result<NormalizedMarketUpdate>::success(
      NormalizedMarketUpdate{std::move(fields), payload_digest});

  // ++++++++++++++++++++++++++++++++++++++++
}

// Validate one sanitized transition profile without granting strategy-publication authority.
model::Result<void> validate_market_state_transition(const MarketStateEventFields& fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate assigned states and the unique initial-transition destination first.
  if (!is_valid(fields.readiness) ||
      (fields.previous_readiness.has_value() && !is_valid(fields.previous_readiness.value()))) {
    return model::Result<void>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketState, "market_state.readiness"));
  }
  if (!fields.previous_readiness.has_value() &&
      fields.readiness != MarketReadiness::Synchronizing) {
    return model::Result<void>::failure(invalid_event(model::DomainErrorCode::InvalidMarketState,
                                                      "market_state.previous_readiness"));
  }
  // Exactly one complete owner, update, envelope, or discontinuity profile must be present.
  const bool owner_context = has_owner_transition_context(fields);
  const bool update_context = has_update_transition_context(fields);
  const bool envelope_context = has_envelope_transition_context(fields);
  const bool discontinuity_context = has_discontinuity_transition_context(fields);
  if (!owner_context && !update_context && !envelope_context && !discontinuity_context) {
    return model::Result<void>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketState, "market_state.input_context"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit owner resynchronization is the sole observable same-state publication.
  if (fields.previous_readiness.has_value() &&
      fields.previous_readiness.value() == fields.readiness &&
      !(fields.readiness == MarketReadiness::Synchronizing && owner_context)) {
    return model::Result<void>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketState, "market_state.transition"));
  }
  if (fields.receive_timestamp.has_value()) {
    const auto delay =
        model::processing_delay(fields.processing_timestamp, fields.receive_timestamp.value());
    if (!delay) {
      return model::Result<void>::failure(invalid_event(model::DomainErrorCode::InvalidMarketState,
                                                        "market_state.processing_timestamp"));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Initial publication has no fabricated session, ingress, or committed-book identity.
  if (!fields.previous_readiness && (!owner_context || fields.book_generation)) {
    return model::Result<void>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketState, "market_state.initial_context"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Enforce the input profiles that can actually cause each transition-table destination.
  if (fields.previous_readiness) {
    const auto previous = *fields.previous_readiness;
    bool transition_profile = false;
    switch (fields.readiness) {
    case MarketReadiness::Ready:
      transition_profile = update_context;
      break;
    case MarketReadiness::Stale:
      transition_profile = previous == MarketReadiness::Ready && envelope_context;
      break;
    case MarketReadiness::Synchronizing:
      transition_profile = owner_context || envelope_context;
      break;
    case MarketReadiness::Invalid:
      transition_profile = update_context || envelope_context || discontinuity_context;
      break;
    default:
      transition_profile = false;
      break;
    }
    if (!transition_profile) {
      return model::Result<void>::failure(invalid_event(model::DomainErrorCode::InvalidMarketState,
                                                        "market_state.transition_profile"));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Book identities remain paired, ordered, and present after any known Ready history.
  if (!has_supported_book_identity(fields)) {
    return model::Result<void>::failure(
        invalid_event(model::DomainErrorCode::InvalidMarketState, "market_state.book_identity"));
  }
  if (requires_book_identity(fields) && !fields.book_generation) {
    return model::Result<void>::failure(invalid_event(model::DomainErrorCode::InvalidMarketState,
                                                      "market_state.required_book_identity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::market_data
