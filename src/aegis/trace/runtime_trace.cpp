// Purpose: validate fixed-field M2 runtime evidence and encode accepted records as canonical
// AEGISRTS schema-one bytes.

#include "aegis/trace/runtime_trace.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::trace {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// Stream and record magic are independent from AEGISTRS/AEGISTRC, making accidental schema mixing
// visible before any tagged field is interpreted.
inline constexpr std::string_view runtime_stream_magic = "AEGISRTS";
inline constexpr std::string_view runtime_record_magic = "AEGISRTR";

// ########################################################################
// Every number is a schema-one compatibility tag; fields are always emitted in this order even
// when their optional values are absent.
enum class RuntimeTraceRecordTag : std::uint16_t {
  Ordinal = 0x0001,
  EventKind = 0x0002,
  AdmissionOrdinal = 0x0003,
  TurnOrdinal = 0x0004,
  CallbackOrdinal = 0x0005,
  SourceOrdinal = 0x0006,
  VenueId = 0x0010,
  InstrumentId = 0x0011,
  VenueInstrumentId = 0x0012,
  BotId = 0x0013,
  SubscriptionId = 0x0014,
  ConfigurationFingerprint = 0x0018,
  RuntimePolicyFingerprint = 0x0019,
  SessionEpoch = 0x0020,
  SourceSequence = 0x0021,
  ReceiveSequence = 0x0022,
  MetadataRevision = 0x0023,
  BookGeneration = 0x0024,
  BookRevision = 0x0025,
  InputDisposition = 0x0030,
  PreviousState = 0x0031,
  State = 0x0032,
  FailureReason = 0x0033,
  BestBid = 0x0040,
  BestAsk = 0x0041,
};

// ########################################################################

// --------------------------------------------------------
// Convert a stable tag to its portable fixed-width representation.
[[nodiscard]] constexpr std::uint16_t
runtime_trace_record_tag_code(RuntimeTraceRecordTag value) noexcept {
  return static_cast<std::uint16_t>(value);
}

// --------------------------------------------------------

// ########################################################################
// This schema-local writer emits portable big-endian primitives and explicit optional-presence
// markers; it deliberately exposes no decoder or general serialization surface.
class CanonicalRuntimeTraceWriter final {
public:

  // --------------------------------------------------------
  // Append one byte only after proving the backing buffer can grow.
  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
    return true;
  }

  // --------------------------------------------------------
  // Append schema-owned ASCII magic without a second length marker.
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
  // Append opaque already-canonical bytes while preserving their order.
  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  // --------------------------------------------------------
  // Encode a portable unsigned 16-bit integer in big-endian order.
  [[nodiscard]] bool append_u16(std::uint16_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Encode a portable unsigned 32-bit integer in big-endian order.
  [[nodiscard]] bool append_u32(std::uint32_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 24U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 16U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  // Encode a portable unsigned 64-bit integer in big-endian order.
  [[nodiscard]] bool append_u64(std::uint64_t value) {
    // Interesting syntax: an explicit zero break prevents the unsigned shift from wrapping after
    // the eighth byte and fixes the representation to exactly eight bytes.
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
  // Preserve a signed coefficient's exact two's-complement bit pattern in portable byte order.
  [[nodiscard]] bool append_i64(std::int64_t value) {
    // Interesting syntax: bit_cast names the exact representation contract and avoids assigning
    // arithmetic meaning while the signed coefficient is serialized.
    return append_u64(std::bit_cast<std::uint64_t>(value));
  }

  // --------------------------------------------------------
  // Encode one field as a tag, portable 32-bit payload length, and payload bytes.
  [[nodiscard]] bool append_field(std::uint16_t field_tag, std::span<const std::byte> payload) {
    if (payload.size() > maximum_u32_size) {
      return false;
    }
    return append_u16(field_tag) && append_u32(static_cast<std::uint32_t>(payload.size())) &&
           append_bytes(payload);
  }

  // --------------------------------------------------------
  // Encode one unsigned 16-bit value as a tagged field.
  [[nodiscard]] bool append_u16_field(std::uint16_t field_tag, std::uint16_t value) {
    CanonicalRuntimeTraceWriter payload;
    return payload.append_u16(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode one unsigned 64-bit value as a tagged field.
  [[nodiscard]] bool append_u64_field(std::uint16_t field_tag, std::uint64_t value) {
    CanonicalRuntimeTraceWriter payload;
    return payload.append_u64(value) && append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode an optional unsigned value with an explicit presence byte.
  [[nodiscard]] bool append_optional_u64_field(std::uint16_t field_tag,
                                               const std::optional<std::uint64_t>& value) {
    CanonicalRuntimeTraceWriter payload;
    if (!payload.append_byte(value.has_value() ? 1U : 0U)) {
      return false;
    }
    if (value.has_value() && !payload.append_u64(*value)) {
      return false;
    }
    return append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Encode an optional nominal identifier without constructing a temporary owning optional.
  template <typename Identifier>
  [[nodiscard]] bool append_optional_identifier_field(std::uint16_t field_tag,
                                                      const Identifier* identifier) {

    // ++++++++++++++++++++++++++++++++++++++++
    // Emit absence explicitly so later fields cannot shift into the identifier's position.
    CanonicalRuntimeTraceWriter payload;
    if (!payload.append_byte(identifier != nullptr ? 1U : 0U)) {
      return false;
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // A present identifier retains its validated byte spelling behind a portable length.
    if (identifier != nullptr) {
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
  // Encode an optional exact price as presence, signed coefficient bits, and canonical scale.
  [[nodiscard]] bool append_optional_price_field(std::uint16_t field_tag,
                                                 const std::optional<model::Price>& price) {
    CanonicalRuntimeTraceWriter payload;
    if (!payload.append_byte(price.has_value() ? 1U : 0U)) {
      return false;
    }
    if (price.has_value() &&
        (!payload.append_i64(price->coefficient()) || !payload.append_byte(price->scale()))) {
      return false;
    }
    return append_field(field_tag, payload.bytes());
  }

  // --------------------------------------------------------
  // Prefix a nested record with its portable 32-bit byte length.
  [[nodiscard]] bool append_length_prefixed(std::span<const std::byte> value) {
    if (value.size() > maximum_u32_size) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(value.size())) && append_bytes(value);
  }

  // --------------------------------------------------------
  // Borrow accumulated bytes for composing nested canonical writers.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Interesting syntax: destructive extraction is ref-qualified to disposable writers, preventing
  // accidental reuse after ownership transfer.
  [[nodiscard]] std::vector<std::byte> take_bytes() && { return std::move(bytes_); }

  // --------------------------------------------------------
private:
  static constexpr std::size_t maximum_u32_size =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());

  // --------------------------------------------------------
  // Subtraction-form checking cannot overflow while deciding whether the vector can grow.
  [[nodiscard]] bool can_grow(std::size_t additional_size) const noexcept {
    return additional_size <= bytes_.max_size() - bytes_.size();
  }

  // --------------------------------------------------------

  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Only assigned disposition values may enter schema-one records.
[[nodiscard]] bool is_known(RuntimeInputDisposition value) noexcept {
  switch (value) {
  case RuntimeInputDisposition::Unspecified:
  case RuntimeInputDisposition::SnapshotApplied:
  case RuntimeInputDisposition::DeltaApplied:
  case RuntimeInputDisposition::ExactDuplicateIgnored:
  case RuntimeInputDisposition::OlderInputIgnored:
  case RuntimeInputDisposition::GapRejected:
  case RuntimeInputDisposition::SequenceConflictRejected:
  case RuntimeInputDisposition::ChecksumRejected:
  case RuntimeInputDisposition::MetadataRevisionRejected:
  case RuntimeInputDisposition::MalformedRejected:
  case RuntimeInputDisposition::UnsupportedRejected:
  case RuntimeInputDisposition::NonReadyDeltaRejected:
  case RuntimeInputDisposition::SessionReset:
  case RuntimeInputDisposition::StalenessChecked:
  case RuntimeInputDisposition::SourceDiscontinuity:
  case RuntimeInputDisposition::StructuralBookRejected:
  case RuntimeInputDisposition::SessionIgnored:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Only the four explicit M2 readiness states and their absence sentinel are canonical.
[[nodiscard]] bool is_known(RuntimeMarketState value) noexcept {
  switch (value) {
  case RuntimeMarketState::Unspecified:
  case RuntimeMarketState::Synchronizing:
  case RuntimeMarketState::Ready:
  case RuntimeMarketState::Stale:
  case RuntimeMarketState::Invalid:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Only assigned re-entry reasons may enter schema-one records.
[[nodiscard]] bool is_known(RuntimeTraceFailureReason value) noexcept {
  switch (value) {
  case RuntimeTraceFailureReason::None:
  case RuntimeTraceFailureReason::OwnerDriveReentry:
  case RuntimeTraceFailureReason::StrategyDispatchReentry:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// A configured source identity requires the nominal ordinal and its complete instrument tuple.
[[nodiscard]] bool has_valid_source_identity(const RuntimeTraceFields& fields) noexcept {
  return fields.source.has_value();
}

// --------------------------------------------------------
// Session, source sequence, local receive sequence, and metadata revision form one atomic context.
[[nodiscard]] bool has_complete_market_context(const RuntimeTraceFields& fields) noexcept {
  return fields.session_epoch.has_value() && fields.source_sequence.has_value() &&
         fields.receive_sequence.has_value() && fields.metadata_revision.has_value();
}

// --------------------------------------------------------
// Envelope/control input retains session and receive identity but has no parsed update counters.
[[nodiscard]] bool has_envelope_market_context(const RuntimeTraceFields& fields) noexcept {
  return fields.session_epoch.has_value() && !fields.source_sequence &&
         fields.receive_sequence.has_value() && !fields.metadata_revision;
}

// --------------------------------------------------------
// Events without active input context must omit all four related fields together.
[[nodiscard]] bool has_no_market_context(const RuntimeTraceFields& fields) noexcept {
  return !fields.session_epoch && !fields.source_sequence && !fields.receive_sequence &&
         !fields.metadata_revision;
}

// --------------------------------------------------------
// State observations bind receive identity to its accepted attempt, while startup/owner controls
// omit both and discontinuity fences carry only their failed attempt ordinal.
[[nodiscard]] bool has_supported_state_context(const RuntimeTraceFields& fields) noexcept {
  const bool accepted_input =
      fields.admission_ordinal.has_value() &&
      (has_complete_market_context(fields) || has_envelope_market_context(fields));
  const bool discontinuity = fields.admission_ordinal.has_value() && has_no_market_context(fields);
  const bool owner_control = !fields.admission_ordinal && has_no_market_context(fields);
  return accepted_input || discontinuity || owner_control;
}

// --------------------------------------------------------
// Name each complete state-input profile so transition validation can mirror ADR-0007 exactly.
[[nodiscard]] bool has_update_state_context(const RuntimeTraceFields& fields) noexcept {
  return fields.admission_ordinal.has_value() && has_complete_market_context(fields);
}

// --------------------------------------------------------
// Report whether an input-disposition record carries only accepted-envelope context.
[[nodiscard]] bool has_envelope_state_context(const RuntimeTraceFields& fields) noexcept {
  return fields.admission_ordinal.has_value() && has_envelope_market_context(fields);
}

// --------------------------------------------------------
// Report whether a source-discontinuity record carries only its failed admission identity.
[[nodiscard]] bool has_discontinuity_state_context(const RuntimeTraceFields& fields) noexcept {
  return fields.admission_ordinal.has_value() && has_no_market_context(fields);
}

// --------------------------------------------------------
// Report whether an owner-authored state observation correctly omits admission and market context.
[[nodiscard]] bool has_owner_state_context(const RuntimeTraceFields& fields) noexcept {
  return !fields.admission_ordinal && has_no_market_context(fields);
}

// --------------------------------------------------------
// Book generation and revision are either both absent before the first commit or both present after
// one; no record may identify half a book version.
[[nodiscard]] bool has_supported_book_identity(const RuntimeTraceFields& fields) noexcept {
  if (fields.book_generation.has_value() != fields.book_revision.has_value()) {
    return false;
  }
  return !fields.book_generation ||
         fields.book_revision->value() >= fields.book_generation->value();
}

// --------------------------------------------------------
// A committed identity is the supported pair's present case, independent of current readiness.
[[nodiscard]] bool has_committed_book_identity(const RuntimeTraceFields& fields) noexcept {
  return fields.book_generation.has_value() && fields.book_revision.has_value();
}

// --------------------------------------------------------
// Current or previous Ready/Stale state proves that at least one book commit already exists.
[[nodiscard]] bool
does_runtime_trace_event_require_book_identity(const RuntimeTraceFields& fields) noexcept {
  return fields.state == RuntimeMarketState::Ready || fields.state == RuntimeMarketState::Stale ||
         fields.previous_state == RuntimeMarketState::Ready ||
         fields.previous_state == RuntimeMarketState::Stale;
}

// --------------------------------------------------------
// Callback ordinal, bot, and subscription are an all-or-none attribution group.
[[nodiscard]] bool has_complete_callback_identity(const RuntimeTraceFields& fields) noexcept {
  return fields.callback_ordinal.has_value() && fields.bot_id.has_value() &&
         fields.subscription_id.has_value();
}

// --------------------------------------------------------
// Absence of every callback field is distinct from an incomplete attribution group.
[[nodiscard]] bool has_no_callback_identity(const RuntimeTraceFields& fields) noexcept {
  return !fields.callback_ordinal && !fields.bot_id && !fields.subscription_id;
}

// --------------------------------------------------------
// A Ready observation must identify a nonzero committed generation and revision.
[[nodiscard]] bool has_ready_book_identity(const RuntimeTraceFields& fields) noexcept {
  return has_committed_book_identity(fields) && fields.state == RuntimeMarketState::Ready;
}

// --------------------------------------------------------
// Startup omits a previous state; explicit owner resynchronization alone may repeat Synchronizing.
[[nodiscard]] bool has_valid_transition_states(const RuntimeTraceFields& fields) noexcept {
  if (fields.state == RuntimeMarketState::Unspecified) {
    return false;
  }
  if (fields.previous_state == RuntimeMarketState::Unspecified) {
    return fields.state == RuntimeMarketState::Synchronizing;
  }
  return fields.previous_state != fields.state ||
         (fields.state == RuntimeMarketState::Synchronizing && has_owner_state_context(fields));
}

// --------------------------------------------------------
// Enforce the same destination/profile matrix as the sanitized MarketStateEvent boundary.
[[nodiscard]] bool has_valid_state_transition_profile(const RuntimeTraceFields& fields) noexcept {
  if (fields.previous_state == RuntimeMarketState::Unspecified) {
    return fields.state == RuntimeMarketState::Synchronizing && has_owner_state_context(fields) &&
           !fields.book_generation;
  }

  switch (fields.state) {
  case RuntimeMarketState::Ready:
    return has_update_state_context(fields);
  case RuntimeMarketState::Stale:
    return fields.previous_state == RuntimeMarketState::Ready && has_envelope_state_context(fields);
  case RuntimeMarketState::Synchronizing:
    return has_owner_state_context(fields) || has_envelope_state_context(fields);
  case RuntimeMarketState::Invalid:
    return has_update_state_context(fields) || has_envelope_state_context(fields) ||
           has_discontinuity_state_context(fields);
  case RuntimeMarketState::Unspecified:
  default:
    return false;
  }
}

// --------------------------------------------------------
// Each side may be absent on a coherent one-sided book; when both are present they must be strictly
// uncrossed.
[[nodiscard]] bool has_valid_top_of_book(const RuntimeTraceFields& fields) noexcept {
  const bool bid_is_positive = !fields.best_bid || fields.best_bid->coefficient() > 0;
  const bool ask_is_positive = !fields.best_ask || fields.best_ask->coefficient() > 0;
  const bool uncrossed =
      !fields.best_bid || !fields.best_ask || *fields.best_bid < *fields.best_ask;
  return bid_is_positive && ask_is_positive && uncrossed;
}

// --------------------------------------------------------
// A disposition's resulting readiness is part of its stable semantic meaning.
[[nodiscard]] bool does_disposition_match_state(RuntimeInputDisposition disposition,
                                                RuntimeMarketState state) noexcept {
  switch (disposition) {
  case RuntimeInputDisposition::SnapshotApplied:
  case RuntimeInputDisposition::DeltaApplied:
    return state == RuntimeMarketState::Ready;
  case RuntimeInputDisposition::ExactDuplicateIgnored:
  case RuntimeInputDisposition::OlderInputIgnored:
  case RuntimeInputDisposition::SessionIgnored:
  case RuntimeInputDisposition::StalenessChecked:
    return state != RuntimeMarketState::Unspecified;
  case RuntimeInputDisposition::GapRejected:
  case RuntimeInputDisposition::SequenceConflictRejected:
  case RuntimeInputDisposition::ChecksumRejected:
  case RuntimeInputDisposition::MetadataRevisionRejected:
  case RuntimeInputDisposition::StructuralBookRejected:
  case RuntimeInputDisposition::MalformedRejected:
  case RuntimeInputDisposition::UnsupportedRejected:
  case RuntimeInputDisposition::SourceDiscontinuity:
    return state == RuntimeMarketState::Invalid;
  case RuntimeInputDisposition::SessionReset:
    return state == RuntimeMarketState::Synchronizing;
  case RuntimeInputDisposition::NonReadyDeltaRejected:
    return state == RuntimeMarketState::Synchronizing || state == RuntimeMarketState::Stale ||
           state == RuntimeMarketState::Invalid;
  case RuntimeInputDisposition::Unspecified:
  default:
    return false;
  }
}

// --------------------------------------------------------
// Each disposition selects exactly one parsed, envelope-only, or discontinuity context profile.
[[nodiscard]] bool does_input_context_match_disposition(const RuntimeTraceFields& fields) noexcept {
  switch (fields.input_disposition) {
  case RuntimeInputDisposition::SnapshotApplied:
  case RuntimeInputDisposition::DeltaApplied:
  case RuntimeInputDisposition::ExactDuplicateIgnored:
  case RuntimeInputDisposition::OlderInputIgnored:
  case RuntimeInputDisposition::GapRejected:
  case RuntimeInputDisposition::SequenceConflictRejected:
  case RuntimeInputDisposition::ChecksumRejected:
  case RuntimeInputDisposition::MetadataRevisionRejected:
  case RuntimeInputDisposition::NonReadyDeltaRejected:
  case RuntimeInputDisposition::StructuralBookRejected:
    return has_complete_market_context(fields);
  case RuntimeInputDisposition::MalformedRejected:
  case RuntimeInputDisposition::UnsupportedRejected:
  case RuntimeInputDisposition::SessionReset:
  case RuntimeInputDisposition::SessionIgnored:
  case RuntimeInputDisposition::StalenessChecked:
    return has_envelope_market_context(fields);
  case RuntimeInputDisposition::SourceDiscontinuity:
    return has_no_market_context(fields);
  case RuntimeInputDisposition::Unspecified:
  default:
    return false;
  }
}

// --------------------------------------------------------
// Input records require the exact context profile selected by their assigned disposition.
[[nodiscard]] bool is_valid_input_disposition(const RuntimeTraceFields& fields) noexcept {
  return has_valid_source_identity(fields) && fields.admission_ordinal.has_value() &&
         fields.turn_ordinal.has_value() && does_input_context_match_disposition(fields) &&
         has_supported_book_identity(fields) && has_no_callback_identity(fields) &&
         is_known(fields.input_disposition) && is_known(fields.state) &&
         fields.input_disposition != RuntimeInputDisposition::Unspecified &&
         fields.previous_state == RuntimeMarketState::Unspecified &&
         fields.failure_reason == RuntimeTraceFailureReason::None && !fields.best_bid &&
         !fields.best_ask && does_disposition_match_state(fields.input_disposition, fields.state) &&
         (!does_runtime_trace_event_require_book_identity(fields) ||
          has_committed_book_identity(fields));
}

// --------------------------------------------------------
// State transitions support startup/timer turns without input context but reject partial context.
[[nodiscard]] bool is_valid_state_transition(const RuntimeTraceFields& fields) noexcept {
  return has_valid_source_identity(fields) && fields.turn_ordinal.has_value() &&
         has_supported_state_context(fields) && has_supported_book_identity(fields) &&
         has_no_callback_identity(fields) &&
         fields.input_disposition == RuntimeInputDisposition::Unspecified &&
         is_known(fields.previous_state) && is_known(fields.state) &&
         has_valid_transition_states(fields) && has_valid_state_transition_profile(fields) &&
         fields.failure_reason == RuntimeTraceFailureReason::None && !fields.best_bid &&
         !fields.best_ask &&
         (!does_runtime_trace_event_require_book_identity(fields) ||
          has_committed_book_identity(fields));
}

// --------------------------------------------------------
// Market callbacks require a complete Ready observation and permit only a valid optional top book.
[[nodiscard]] bool is_valid_market_callback(const RuntimeTraceFields& fields) noexcept {
  return has_valid_source_identity(fields) && fields.admission_ordinal.has_value() &&
         fields.turn_ordinal.has_value() && has_complete_callback_identity(fields) &&
         has_complete_market_context(fields) && has_ready_book_identity(fields) &&
         fields.input_disposition == RuntimeInputDisposition::Unspecified &&
         fields.previous_state == RuntimeMarketState::Unspecified &&
         fields.state == RuntimeMarketState::Ready &&
         fields.failure_reason == RuntimeTraceFailureReason::None && has_valid_top_of_book(fields);
}

// --------------------------------------------------------
// State callbacks carry complete bot attribution but never expose a retained top-of-book view.
[[nodiscard]] bool is_valid_state_callback(const RuntimeTraceFields& fields) noexcept {
  return has_valid_source_identity(fields) && fields.turn_ordinal.has_value() &&
         has_complete_callback_identity(fields) && has_supported_state_context(fields) &&
         has_supported_book_identity(fields) &&
         fields.input_disposition == RuntimeInputDisposition::Unspecified &&
         is_known(fields.previous_state) && is_known(fields.state) &&
         has_valid_transition_states(fields) && has_valid_state_transition_profile(fields) &&
         fields.failure_reason == RuntimeTraceFailureReason::None && !fields.best_bid &&
         !fields.best_ask &&
         (!does_runtime_trace_event_require_book_identity(fields) ||
          has_committed_book_identity(fields));
}

// --------------------------------------------------------
// Re-entry evidence identifies the active turn and carries callback identity whenever recursion was
// attempted from inside a callback, including nested owner drive.
[[nodiscard]] bool is_valid_reentry(const RuntimeTraceFields& fields) noexcept {
  const bool owner_reentry =
      fields.failure_reason == RuntimeTraceFailureReason::OwnerDriveReentry &&
      (has_no_callback_identity(fields) || has_complete_callback_identity(fields));
  const bool dispatch_reentry =
      fields.failure_reason == RuntimeTraceFailureReason::StrategyDispatchReentry &&
      has_complete_callback_identity(fields);
  return !fields.admission_ordinal && fields.turn_ordinal.has_value() &&
         (owner_reentry || dispatch_reentry) && !fields.source && has_no_market_context(fields) &&
         !fields.book_generation && !fields.book_revision &&
         fields.input_disposition == RuntimeInputDisposition::Unspecified &&
         fields.previous_state == RuntimeMarketState::Unspecified &&
         fields.state == RuntimeMarketState::Unspecified && !fields.best_bid && !fields.best_ask;
}

// --------------------------------------------------------
// Construct a stable schema failure without retaining caller text in canonical evidence.
[[nodiscard]] model::Result<void>
create_invalid_runtime_trace_record_result(std::string_view field) {
  return model::Result<void>::create_failure(
      DomainError::create_at_field(DomainErrorCode::InvalidValue, std::string{field}));
}

// --------------------------------------------------------
// Bind every event kind to one exact fixed-field shape before an ordinal is assigned.
[[nodiscard]] model::Result<void> validate_event(RuntimeTraceEventKind kind,
                                                 const RuntimeTraceFields& fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Unknown enum representations and kind-specific partial groups fail before sink mutation.
  if (!is_known(fields.input_disposition) || !is_known(fields.previous_state) ||
      !is_known(fields.state) || !is_known(fields.failure_reason)) {
    return create_invalid_runtime_trace_record_result("runtime_trace.enum");
  }
  switch (kind) {
  case RuntimeTraceEventKind::InputDisposition:
    return is_valid_input_disposition(fields)
               ? model::Result<void>::create_success()
               : create_invalid_runtime_trace_record_result("runtime_trace.input_disposition");
  case RuntimeTraceEventKind::MarketStateTransition:
    return is_valid_state_transition(fields) ? model::Result<void>::create_success()
                                             : create_invalid_runtime_trace_record_result(
                                                   "runtime_trace.market_state_transition");
  case RuntimeTraceEventKind::MarketCallback:
    return is_valid_market_callback(fields)
               ? model::Result<void>::create_success()
               : create_invalid_runtime_trace_record_result("runtime_trace.market_callback");
  case RuntimeTraceEventKind::StateCallback:
    return is_valid_state_callback(fields)
               ? model::Result<void>::create_success()
               : create_invalid_runtime_trace_record_result("runtime_trace.state_callback");
  case RuntimeTraceEventKind::ReentryDetected:
    return is_valid_reentry(fields)
               ? model::Result<void>::create_success()
               : create_invalid_runtime_trace_record_result("runtime_trace.reentry_detected");
  default:
    return create_invalid_runtime_trace_record_result("runtime_trace.kind");
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Convert a nominal optional counter into the primitive form assigned to schema-one bytes.
template <typename Counter>
[[nodiscard]] std::optional<std::uint64_t>
optional_uint64_from_optional_counter(const std::optional<Counter>& value) noexcept {
  return value.has_value() ? std::optional<std::uint64_t>{value->value()} : std::nullopt;
}

// --------------------------------------------------------
// Encode every tagged field in fixed order so absence changes only its explicit presence payload.
[[nodiscard]] model::Result<std::vector<std::byte>>
encode_record(const RuntimeTraceRecord& record, const RuntimeTraceProvenance& provenance) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Borrow optional identifier values without allocating temporary nominal values.
  CanonicalRuntimeTraceWriter writer;
  const auto& fields = record.fields();
  const auto* source = fields.source.has_value() ? &*fields.source : nullptr;
  const auto source_ordinal = source != nullptr
                                  ? std::optional<std::uint64_t>{source->source_ordinal().value()}
                                  : std::nullopt;
  const auto* venue_id = source != nullptr ? &source->venue_id() : nullptr;
  const auto* instrument_id = source != nullptr ? &source->instrument_id() : nullptr;
  const auto* venue_instrument_id = source != nullptr ? &source->venue_instrument_id() : nullptr;
  const auto* bot_id = fields.bot_id.has_value() ? &*fields.bot_id : nullptr;
  const auto* subscription_id =
      fields.subscription_id.has_value() ? &*fields.subscription_id : nullptr;

  // ++++++++++++++++++++++++++++++++++++++++
  // Project nominal counters into stable unsigned fields without changing their type at the API.
  const auto session_epoch = optional_uint64_from_optional_counter(fields.session_epoch);
  const auto source_sequence = optional_uint64_from_optional_counter(fields.source_sequence);
  const auto admission_ordinal = optional_uint64_from_optional_counter(fields.admission_ordinal);
  const auto turn_ordinal = optional_uint64_from_optional_counter(fields.turn_ordinal);
  const auto callback_ordinal = optional_uint64_from_optional_counter(fields.callback_ordinal);
  const auto receive_sequence = optional_uint64_from_optional_counter(fields.receive_sequence);
  const auto metadata_revision = optional_uint64_from_optional_counter(fields.metadata_revision);
  const auto book_generation = optional_uint64_from_optional_counter(fields.book_generation);
  const auto book_revision = optional_uint64_from_optional_counter(fields.book_revision);

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit the independent record envelope and all tagged values in compatibility order.
  const bool success =
      writer.append_ascii(runtime_record_magic) && writer.append_u16(record.schema_version()) &&
      writer.append_u64_field(runtime_trace_record_tag_code(RuntimeTraceRecordTag::Ordinal),
                              record.ordinal().value()) &&
      writer.append_u16_field(runtime_trace_record_tag_code(RuntimeTraceRecordTag::EventKind),
                              static_cast<std::uint16_t>(record.kind())) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::AdmissionOrdinal),
          admission_ordinal) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::TurnOrdinal), turn_ordinal) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::CallbackOrdinal),
          callback_ordinal) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::SourceOrdinal), source_ordinal) &&
      writer.append_optional_identifier_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::VenueId), venue_id) &&
      writer.append_optional_identifier_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::InstrumentId), instrument_id) &&
      writer.append_optional_identifier_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::VenueInstrumentId),
          venue_instrument_id) &&
      writer.append_optional_identifier_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::BotId), bot_id) &&
      writer.append_optional_identifier_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::SubscriptionId), subscription_id) &&
      writer.append_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::ConfigurationFingerprint),
          provenance.configuration_fingerprint().bytes()) &&
      writer.append_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::RuntimePolicyFingerprint),
          provenance.runtime_policy_fingerprint().bytes()) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::SessionEpoch), session_epoch) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::SourceSequence), source_sequence) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::ReceiveSequence),
          receive_sequence) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::MetadataRevision),
          metadata_revision) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::BookGeneration), book_generation) &&
      writer.append_optional_u64_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::BookRevision), book_revision) &&
      writer.append_u16_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::InputDisposition),
          static_cast<std::uint16_t>(fields.input_disposition)) &&
      writer.append_u16_field(runtime_trace_record_tag_code(RuntimeTraceRecordTag::PreviousState),
                              static_cast<std::uint16_t>(fields.previous_state)) &&
      writer.append_u16_field(runtime_trace_record_tag_code(RuntimeTraceRecordTag::State),
                              static_cast<std::uint16_t>(fields.state)) &&
      writer.append_u16_field(runtime_trace_record_tag_code(RuntimeTraceRecordTag::FailureReason),
                              static_cast<std::uint16_t>(fields.failure_reason)) &&
      writer.append_optional_price_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::BestBid), fields.best_bid) &&
      writer.append_optional_price_field(
          runtime_trace_record_tag_code(RuntimeTraceRecordTag::BestAsk), fields.best_ask);

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish no partial bytes when any nested writer reaches an encoding limit.
  if (!success) {
    return model::Result<std::vector<std::byte>>::create_failure(DomainError::create_at_field(
        DomainErrorCode::EncodingOverflow, "runtime_trace.record_encoding"));
  }
  return model::Result<std::vector<std::byte>>::create_success(std::move(writer).take_bytes());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Preserve only one source tuple and ordinal already validated by the immutable runtime policy.
RuntimeTraceSource RuntimeTraceSource::from_runtime_source(const runtime::RuntimeSource& source) {
  const auto& definition = source.definition();
  return RuntimeTraceSource{definition.source_id, source.ordinal(), definition.venue_id,
                            definition.instrument_id, definition.venue_instrument_id};
}

// --------------------------------------------------------
// Derive both stream identities from the same immutable policy to prevent mixed-provenance records.
RuntimeTraceProvenance
RuntimeTraceProvenance::from_runtime_policy(const runtime::RuntimePolicy& policy) {
  return RuntimeTraceProvenance{policy.configuration_fingerprint(), policy.fingerprint()};
}

// --------------------------------------------------------
// Bind all stream policy before reserving complete fixed-capacity owner storage.
RuntimeTraceSink::RuntimeTraceSink(const runtime::RuntimePolicy& policy)
    : capacity_{policy.limits().runtime_trace_capacity},
      provenance_{RuntimeTraceProvenance::from_runtime_policy(policy)} {
  sources_.reserve(policy.source_capacity());
  for (const auto& source : policy.sources()) {
    sources_.push_back(RuntimeTraceSource::from_runtime_source(source));
  }
  records_.reserve(capacity_);
}

// --------------------------------------------------------
// Reject the first record beyond the bound without reserving ordinals or changing accepted bytes.
model::Result<void>
RuntimeTraceSink::preflight_trace_append(std::uint32_t additional_records) const {
  if (additional_records > remaining_capacity()) {
    return model::Result<void>::create_failure(
        DomainError::create_at_index(DomainErrorCode::TraceCapacityExceeded,
                                     "runtime_trace.records", static_cast<std::size_t>(capacity_)));
  }
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Expose shape validation so a turn can build and prove every record before committing domain
// state.
model::Result<void>
RuntimeTraceSink::validate_trace_record(RuntimeTraceEventKind kind,
                                        const RuntimeTraceFields& fields) const {
  auto shape = validate_event(kind, fields);
  if (!shape) {
    return shape;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve a present opaque source proof against the same policy that fixed stream provenance.
  if (fields.source) {
    const auto ordinal = fields.source->source_ordinal().value();
    if (ordinal > sources_.size() ||
        sources_[static_cast<std::size_t>(ordinal - 1U)] != fields.source.value()) {
      return model::Result<void>::create_failure(DomainError::create_at_field(
          DomainErrorCode::RuntimeSourceNotConfigured, "runtime_trace.source"));
    }
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Capacity and schema validation both complete before the accepted prefix mutates.
model::Result<void> RuntimeTraceSink::append_trace_record(RuntimeTraceEventKind kind,
                                                          RuntimeTraceFields fields) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity has deterministic precedence and makes a full sink reject every attempted shape alike.
  auto capacity_check = preflight_trace_append(1U);
  if (!capacity_check) {
    return capacity_check;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate all fixed-field groups before assigning the next one-based ordinal.
  auto validation = validate_trace_record(kind, fields);
  if (!validation) {
    return validation;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Append exactly one complete record after every fallible domain check has succeeded.
  const auto ordinal = RuntimeTraceOrdinal{static_cast<std::uint64_t>(records_.size()) + 1U};
  records_.push_back(RuntimeTraceRecord{ordinal, kind, std::move(fields)});
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A stream is magic, version, count, and length-prefixed canonical records in accepted order.
model::Result<std::vector<std::byte>> RuntimeTraceSink::encode_canonical_bytes() const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Emit the fixed stream envelope before traversing the immutable accepted prefix.
  CanonicalRuntimeTraceWriter writer;
  bool success = writer.append_ascii(runtime_stream_magic) &&
                 writer.append_u16(runtime_trace_schema_version) &&
                 writer.append_u32(record_count());
  for (const auto& record : records_) {
    auto encoded = encode_record(record, provenance_);
    if (!encoded || !writer.append_length_prefixed(encoded.value())) {
      success = false;
      break;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Return either the complete stream or one stable encoding failure, never a partial projection.
  if (!success) {
    return model::Result<std::vector<std::byte>>::create_failure(DomainError::create_at_field(
        DomainErrorCode::EncodingOverflow, "runtime_trace.stream_encoding"));
  }
  return model::Result<std::vector<std::byte>>::create_success(std::move(writer).take_bytes());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Digest identity is exactly SHA-256 over canonical bytes and contains no ambient timing input.
model::Result<model::Sha256Digest> RuntimeTraceSink::derive_digest() const {
  auto encoded = encode_canonical_bytes();
  if (!encoded) {
    return model::Result<model::Sha256Digest>::create_failure(std::move(encoded).error());
  }
  return model::Result<model::Sha256Digest>::create_success(
      model::calculate_sha256_digest(encoded.value()));
}

// --------------------------------------------------------

} // namespace aegis::trace
