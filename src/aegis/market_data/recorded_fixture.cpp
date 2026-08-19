// Purpose: bound and resolve recorded ingress, mint immutable owner-stamped frames, then parse and
// normalize strict AEGISMD bytes without mutating books or reading ambient clocks.

#include "aegis/market_data/recorded_fixture.hpp"

#include "aegis/runtime/runtime_policy.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::market_data {
namespace {

// ########################################################################
// Retain a field's original location so every strict-parser failure names a deterministic byte.
struct FixtureField {
  std::string_view text;
  std::size_t byte_offset;
};

// ########################################################################
// Walk pipe-delimited fields without materializing an unbounded token collection. A trailing pipe
// intentionally produces one final empty field rather than disappearing.
class FixtureFieldCursor final {
public:

  // --------------------------------------------------------
  // Begin before the first field of one already size-bounded frame.
  explicit FixtureFieldCursor(std::string_view frame) noexcept : frame_{frame} {}

  // --------------------------------------------------------
  // Return the next field with its original byte offset, preserving empty fields.
  [[nodiscard]] std::optional<FixtureField> next() noexcept {
    if (finished_) {
      return std::nullopt;
    }

    const auto start = next_offset_;
    const auto delimiter = frame_.find('|', start);
    if (delimiter == std::string_view::npos) {
      finished_ = true;
      next_offset_ = frame_.size();
      return FixtureField{frame_.substr(start), start};
    }

    next_offset_ = delimiter + 1U;
    return FixtureField{frame_.substr(start, delimiter - start), start};
  }

  // --------------------------------------------------------
  // Report where a missing required field would have begun.
  [[nodiscard]] std::size_t next_byte_offset() const noexcept { return next_offset_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  std::string_view frame_;
  std::size_t next_offset_{0U};
  bool finished_{false};
};

// ########################################################################

// --------------------------------------------------------
// Convert a known bounded offset into the stable fixed-width diagnostic representation.
[[nodiscard]] RecordedFixtureParseError parse_error(RecordedFixtureParseCode code,
                                                    std::size_t offset) noexcept {
  return RecordedFixtureParseError{code, static_cast<std::uint32_t>(offset)};
}

// --------------------------------------------------------
// Consume a mandatory field and report truncation at the exact absent-field position.
[[nodiscard]] std::optional<FixtureField>
required_field(FixtureFieldCursor& cursor, RecordedFixtureParseError& error) noexcept {
  auto field = cursor.next();
  if (!field.has_value()) {
    error = parse_error(RecordedFixtureParseCode::UnexpectedEnd, cursor.next_byte_offset());
  }
  return field;
}

// --------------------------------------------------------
// Parse one complete unsigned decimal token, distinguishing lexical failure from overflow.
[[nodiscard]] std::optional<std::uint64_t>
parse_unsigned(FixtureField field, RecordedFixtureParseError& error) noexcept {
  std::uint64_t value = 0U;
  const auto* begin = field.text.data();
  const auto* end = begin + field.text.size();
  const auto conversion = std::from_chars(begin, end, value, 10);

  if (conversion.ec == std::errc::result_out_of_range) {
    error = parse_error(RecordedFixtureParseCode::NumericOverflow, field.byte_offset);
    return std::nullopt;
  }
  if (field.text.empty() || conversion.ec == std::errc::invalid_argument || conversion.ptr != end) {
    error = parse_error(RecordedFixtureParseCode::InvalidUnsignedInteger, field.byte_offset);
    return std::nullopt;
  }
  return value;
}

// --------------------------------------------------------
// Parse an optional predecessor token without conflating absence with sequence zero.
[[nodiscard]] std::optional<std::optional<model::SequenceNumber>>
parse_predecessor(FixtureField field, RecordedFixtureParseError& error) noexcept {
  if (field.text == "none") {
    return std::optional<model::SequenceNumber>{};
  }

  auto value = parse_unsigned(field, error);
  if (!value.has_value()) {
    if (error.code == RecordedFixtureParseCode::InvalidUnsignedInteger) {
      error.code = RecordedFixtureParseCode::InvalidPredecessor;
    }
    return std::nullopt;
  }
  return std::optional<model::SequenceNumber>{model::SequenceNumber{value.value()}};
}

// --------------------------------------------------------
// Parse a positive installed metadata revision through the M1 nominal factory.
[[nodiscard]] std::optional<model::InstrumentMetadataRevision>
parse_metadata_revision(FixtureField field, RecordedFixtureParseError& error) {
  auto value = parse_unsigned(field, error);
  if (!value.has_value()) {
    return std::nullopt;
  }

  auto revision = model::InstrumentMetadataRevision::from_value(value.value());
  if (!revision) {
    error = parse_error(RecordedFixtureParseCode::InvalidRevision, field.byte_offset);
    return std::nullopt;
  }
  return std::move(revision).value();
}

// --------------------------------------------------------
// Decode `ok:` or `bad:` plus a bounded opaque token without assigning it a checksum algorithm.
[[nodiscard]] std::optional<ParsedFixtureIntegrity>
parse_integrity(FixtureField field, RecordedFixtureParseError& error) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Separate the repository-authored verdict from the opaque token without interpreting it.
  ParsedFixtureIntegrityVerdict verdict{};
  std::string_view token;
  if (field.text.starts_with("ok:")) {
    verdict = ParsedFixtureIntegrityVerdict::Accepted;
    token = field.text.substr(3U);
  } else if (field.text.starts_with("bad:")) {
    verdict = ParsedFixtureIntegrityVerdict::Rejected;
    token = field.text.substr(4U);
  } else {
    error = parse_error(RecordedFixtureParseCode::InvalidIntegrityToken, field.byte_offset);
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Enforce a printable algorithm-neutral token grammar and its fixed byte bound.
  if (token.empty() || token.size() > recorded_fixture_maximum_integrity_token_bytes) {
    error = parse_error(RecordedFixtureParseCode::InvalidIntegrityToken, field.byte_offset);
    return std::nullopt;
  }
  for (const char character : token) {
    const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                              (character >= 'A' && character <= 'Z') ||
                              (character >= '0' && character <= '9');
    if (!alphanumeric && character != '.' && character != '-' && character != '_') {
      error = parse_error(RecordedFixtureParseCode::InvalidIntegrityToken, field.byte_offset);
      return std::nullopt;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the exact token only after the entire field has validated.
  return ParsedFixtureIntegrity{verdict, std::string{token}};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Parse exactly one `side,price,quantity` field while retaining zero quantity unchanged.
[[nodiscard]] std::optional<ParsedFixtureBookLevel> parse_level(FixtureField field,
                                                                RecordedFixtureParseError& error) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Locate exactly two component separators before attempting any numeric conversion.
  const auto first_comma = field.text.find(',');
  const auto second_comma = first_comma == std::string_view::npos
                                ? std::string_view::npos
                                : field.text.find(',', first_comma + 1U);
  if (first_comma == std::string_view::npos || second_comma == std::string_view::npos ||
      field.text.find(',', second_comma + 1U) != std::string_view::npos) {
    error = parse_error(RecordedFixtureParseCode::InvalidLevelSyntax, field.byte_offset);
    return std::nullopt;
  }

  const auto side_text = field.text.substr(0U, first_comma);
  const auto price_text = field.text.substr(first_comma + 1U, second_comma - first_comma - 1U);
  const auto quantity_text = field.text.substr(second_comma + 1U);
  const auto price_offset = field.byte_offset + first_comma + 1U;
  const auto quantity_offset = field.byte_offset + second_comma + 1U;

  // ++++++++++++++++++++++++++++++++++++++++
  // Decode the closed side vocabulary independently from book ordering policy.
  ParsedFixtureBookSide side{};
  if (side_text == "B") {
    side = ParsedFixtureBookSide::Bid;
  } else if (side_text == "A") {
    side = ParsedFixtureBookSide::Ask;
  } else {
    error = parse_error(RecordedFixtureParseCode::InvalidSide, field.byte_offset);
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse exact fixed-point components and preserve numeric-overflow identity.
  auto price = model::Price::parse_ascii(price_text);
  if (!price) {
    const auto code = price.error().code == model::DomainErrorCode::ArithmeticOverflow
                          ? RecordedFixtureParseCode::NumericOverflow
                          : RecordedFixtureParseCode::InvalidPrice;
    error = parse_error(code, price_offset);
    return std::nullopt;
  }

  auto quantity = model::Quantity::parse_ascii(quantity_text);
  if (!quantity) {
    const auto code = quantity.error().code == model::DomainErrorCode::ArithmeticOverflow
                          ? RecordedFixtureParseCode::NumericOverflow
                          : RecordedFixtureParseCode::InvalidQuantity;
    error = parse_error(code, quantity_offset);
    return std::nullopt;
  }
  if (quantity.value().coefficient() < 0) {
    error = parse_error(RecordedFixtureParseCode::InvalidQuantity, quantity_offset);
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the level only after nonnegative quantity validation; zero remains unchanged.
  return ParsedFixtureBookLevel{side, std::move(price).value(), std::move(quantity).value()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reject any field beyond a message type's complete grammar.
[[nodiscard]] bool reject_trailing_field(FixtureFieldCursor& cursor,
                                         RecordedFixtureParseError& error) noexcept {
  const auto trailing = cursor.next();
  if (trailing.has_value()) {
    error = parse_error(RecordedFixtureParseCode::TrailingInput, trailing->byte_offset);
    return false;
  }
  return true;
}

// --------------------------------------------------------
// Parse a snapshot or delta into temporary storage and publish only after every declared level.
[[nodiscard]] std::optional<ParsedFixtureBookUpdate>
parse_book_update(FixtureFieldCursor& cursor, ParsedFixtureBookUpdateKind kind,
                  RecordedFixtureParseError& error) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the fixed header shared by both update kinds, preserving optional predecessor identity.
  const auto sequence_field = required_field(cursor, error);
  if (!sequence_field.has_value()) {
    return std::nullopt;
  }
  auto sequence = parse_unsigned(sequence_field.value(), error);
  if (!sequence.has_value()) {
    return std::nullopt;
  }

  const auto predecessor_field = required_field(cursor, error);
  if (!predecessor_field.has_value()) {
    return std::nullopt;
  }
  auto predecessor = parse_predecessor(predecessor_field.value(), error);
  if (!predecessor.has_value()) {
    return std::nullopt;
  }

  const auto source_time_field = required_field(cursor, error);
  if (!source_time_field.has_value()) {
    return std::nullopt;
  }
  auto source_time = parse_unsigned(source_time_field.value(), error);
  if (!source_time.has_value()) {
    return std::nullopt;
  }

  const auto revision_field = required_field(cursor, error);
  if (!revision_field.has_value()) {
    return std::nullopt;
  }
  auto revision = parse_metadata_revision(revision_field.value(), error);
  if (!revision.has_value()) {
    return std::nullopt;
  }

  const auto integrity_field = required_field(cursor, error);
  if (!integrity_field.has_value()) {
    return std::nullopt;
  }
  auto integrity = parse_integrity(integrity_field.value(), error);
  if (!integrity.has_value()) {
    return std::nullopt;
  }

  const auto level_count_field = required_field(cursor, error);
  if (!level_count_field.has_value()) {
    return std::nullopt;
  }
  auto level_count = parse_unsigned(level_count_field.value(), error);
  if (!level_count.has_value()) {
    return std::nullopt;
  }
  if (level_count.value() > recorded_fixture_maximum_levels) {
    error = parse_error(RecordedFixtureParseCode::TooManyLevels, level_count_field->byte_offset);
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse every declared level into a bounded temporary vector before any message is published.
  std::vector<ParsedFixtureBookLevel> levels;
  levels.reserve(static_cast<std::size_t>(level_count.value()));
  for (std::uint64_t index = 0U; index < level_count.value(); ++index) {
    const auto level_field = cursor.next();
    if (!level_field.has_value()) {
      error = parse_error(RecordedFixtureParseCode::LevelCountMismatch, cursor.next_byte_offset());
      return std::nullopt;
    }
    auto level = parse_level(level_field.value(), error);
    if (!level.has_value()) {
      return std::nullopt;
    }
    levels.push_back(std::move(level).value());
  }
  if (!reject_trailing_field(cursor, error)) {
    return std::nullopt;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct the independent parser DTO only after the complete frame has succeeded.
  return ParsedFixtureBookUpdate{
      kind,
      model::SequenceNumber{sequence.value()},
      std::move(predecessor).value(),
      model::SourceTimestamp{source_time.value()},
      std::move(revision).value(),
      std::move(integrity).value(),
      std::move(levels),
  };

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Parse the single timestamp field used by session-started control records.
[[nodiscard]] std::optional<ParsedFixtureSessionStarted>
parse_session_started(FixtureFieldCursor& cursor, RecordedFixtureParseError& error) {
  const auto timestamp_field = required_field(cursor, error);
  if (!timestamp_field.has_value()) {
    return std::nullopt;
  }
  auto timestamp = parse_unsigned(timestamp_field.value(), error);
  if (!timestamp.has_value() || !reject_trailing_field(cursor, error)) {
    return std::nullopt;
  }
  return ParsedFixtureSessionStarted{model::SourceTimestamp{timestamp.value()}};
}

// --------------------------------------------------------
// Parse the explicit deterministic processing timestamp used by a staleness-check turn.
[[nodiscard]] std::optional<ParsedFixtureStalenessCheck>
parse_staleness_check(FixtureFieldCursor& cursor, RecordedFixtureParseError& error) {
  const auto timestamp_field = required_field(cursor, error);
  if (!timestamp_field.has_value()) {
    return std::nullopt;
  }
  auto timestamp = parse_unsigned(timestamp_field.value(), error);
  if (!timestamp.has_value() || !reject_trailing_field(cursor, error)) {
    return std::nullopt;
  }
  return ParsedFixtureStalenessCheck{model::ProcessingTimestamp{timestamp.value()}};
}

// --------------------------------------------------------
// Build one stable domain failure for ingress, policy resolution, or normalized-market validation.
[[nodiscard]] model::DomainError fixture_domain_error(model::DomainErrorCode code,
                                                      std::string field) {
  return model::DomainError::at_field(code, std::move(field));
}

// --------------------------------------------------------
// Validate pre-admission attribution and size, then resolve the sole immutable runtime source.
[[nodiscard]] model::Result<const runtime::RuntimeSource*>
resolve_runtime_source(const IngressFrameAttempt& attempt, const runtime::RuntimePolicy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Missing attribution cannot name arbitrary source state or an attributable overload fence.
  if (!attempt.source_id().has_value()) {
    return model::Result<const runtime::RuntimeSource*>::failure(fixture_domain_error(
        model::DomainErrorCode::RuntimeSourceNotConfigured, "ingress_frame_attempt.source_id"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The runtime-specific bound may be stricter than the compiled ceiling already enforced at mint.
  if (attempt.frame().size() > policy.limits().maximum_frame_bytes) {
    return model::Result<const runtime::RuntimeSource*>::failure(fixture_domain_error(
        model::DomainErrorCode::InvalidMarketEvent, "ingress_frame_attempt.frame"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only an exact lookup in this immutable policy grants source authority.
  const auto* const source = policy.find_source(attempt.source_id().value());
  if (source == nullptr) {
    return model::Result<const runtime::RuntimeSource*>::failure(fixture_domain_error(
        model::DomainErrorCode::RuntimeSourceNotConfigured, "ingress_frame_attempt.source_id"));
  }
  return model::Result<const runtime::RuntimeSource*>::success(source);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Revalidate a parsed frame's opaque source against the policy used by normalization.
[[nodiscard]] model::Result<MarketSourceIdentity>
resolve_source_identity(const ParsedMarketMessage& message, const runtime::RuntimePolicy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Normalization accepts only an identity present in this exact immutable source registry.
  const auto* const source = policy.find_source(message.source().source_id());
  if (source == nullptr) {
    return model::Result<MarketSourceIdentity>::failure(fixture_domain_error(
        model::DomainErrorCode::RuntimeSourceNotConfigured, "recorded_fixture.source_id"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Ordinal and full venue/instrument attribution must still match the same policy entry.
  auto resolved = MarketSourceIdentity::from_runtime_source(*source);
  if (resolved != message.source()) {
    return model::Result<MarketSourceIdentity>::failure(fixture_domain_error(
        model::DomainErrorCode::RuntimeSourceNotConfigured, "recorded_fixture.source_identity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  return model::Result<MarketSourceIdentity>::success(std::move(resolved));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Convert one parsed book update into the normalized contract without weakening either vocabulary.
[[nodiscard]] model::Result<NormalizedRecordedMarketCommand>
normalize_book_update(ParsedMarketMessage message, MarketSourceIdentity source,
                      const runtime::RuntimePolicy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // The parser produces assigned values, while this check also contains directly authored DTOs.
  const auto& parsed = std::get<ParsedFixtureBookUpdate>(message.payload());
  MarketUpdateKind kind{};
  switch (parsed.kind) {
  case ParsedFixtureBookUpdateKind::Snapshot:
    kind = MarketUpdateKind::Snapshot;
    break;
  case ParsedFixtureBookUpdateKind::Delta:
    kind = MarketUpdateKind::Delta;
    break;
  default:
    return model::Result<NormalizedRecordedMarketCommand>::failure(fixture_domain_error(
        model::DomainErrorCode::InvalidMarketEvent, "recorded_fixture.update.kind"));
  }

  IntegrityVerdict verdict{};
  switch (parsed.integrity.verdict) {
  case ParsedFixtureIntegrityVerdict::Accepted:
    verdict = IntegrityVerdict::Accepted;
    break;
  case ParsedFixtureIntegrityVerdict::Rejected:
    verdict = IntegrityVerdict::Rejected;
    break;
  default:
    return model::Result<NormalizedRecordedMarketCommand>::failure(fixture_domain_error(
        model::DomainErrorCode::InvalidMarketEvent, "recorded_fixture.update.integrity_verdict"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Contain directly authored DTOs before reserve; parser output has already passed this ceiling.
  if (parsed.levels.size() > policy.limits().maximum_changes_per_update) {
    return model::Result<NormalizedRecordedMarketCommand>::failure(fixture_domain_error(
        model::DomainErrorCode::MarketBookCapacityExceeded, "market_update.changes"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Hash the bounded token and map levels into temporary normalized storage before publication.
  auto token_identity = IntegrityTokenIdentity::from_token(parsed.integrity.token);
  if (!token_identity) {
    return model::Result<NormalizedRecordedMarketCommand>::failure(token_identity.error());
  }

  std::vector<MarketLevelChange> changes;
  changes.reserve(parsed.levels.size());
  for (std::size_t index = 0U; index < parsed.levels.size(); ++index) {
    const auto& level = parsed.levels[index];
    BookSide side{};
    switch (level.side) {
    case ParsedFixtureBookSide::Bid:
      side = BookSide::Bid;
      break;
    case ParsedFixtureBookSide::Ask:
      side = BookSide::Ask;
      break;
    default:
      return model::Result<NormalizedRecordedMarketCommand>::failure(
          model::DomainError::at_index(model::DomainErrorCode::InvalidMarketEvent,
                                       "recorded_fixture.update.levels.side", index));
    }
    changes.push_back(MarketLevelChange{side, level.price, level.quantity});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Delegate semantic shape, duplicate, and policy-bound checks to the normalized update factory.
  auto normalized = NormalizedMarketUpdate::create(
      NormalizedMarketUpdateFields{
          std::move(source), message.session_epoch(), parsed.source_sequence,
          parsed.predecessor_sequence, parsed.source_timestamp, message.receive_sequence(),
          message.receive_timestamp(), parsed.metadata_revision, kind,
          MarketIntegrity{verdict, std::move(token_identity).value()}, std::move(changes)},
      policy.limits().maximum_changes_per_update);
  if (!normalized) {
    return model::Result<NormalizedRecordedMarketCommand>::failure(normalized.error());
  }
  return model::Result<NormalizedRecordedMarketCommand>::success(NormalizedRecordedMarketCommand{
      std::in_place_type<NormalizedMarketUpdate>, std::move(normalized).value()});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Bound caller-owned bytes at the compiled AEGISMD ceiling without trusting optional attribution.
model::Result<IngressFrameAttempt>
IngressFrameAttempt::create(std::optional<model::MarketSourceId> source_id,
                            model::SessionEpoch session_epoch, std::string frame) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject oversized caller storage before it can become an accepted bounded runtime value.
  if (frame.size() > maximum_recorded_frame_bytes) {
    return model::Result<IngressFrameAttempt>::failure(fixture_domain_error(
        model::DomainErrorCode::InvalidMarketEvent, "ingress_frame_attempt.frame"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish optional attribution unchanged so policy, rather than the caller, grants authority.
  return model::Result<IngressFrameAttempt>::success(
      IngressFrameAttempt{std::move(source_id), session_epoch, std::move(frame)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve the source ordinal and configured frame limit before the executor assigns a receipt.
model::Result<model::MarketSourceOrdinal>
resolve_recorded_frame_source(const IngressFrameAttempt& attempt,
                              const runtime::RuntimePolicy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Apply the same policy resolution that owner-side minting will repeat after admission.
  auto source = resolve_runtime_source(attempt, policy);
  if (!source) {
    return model::Result<model::MarketSourceOrdinal>::failure(source.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Expose only the stable ordinal needed to attribute any capacity-loss fence.
  return model::Result<model::MarketSourceOrdinal>::success(source.value()->ordinal());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Combine a policy-resolved attempt with owner-assigned receive identity after accepted admission.
model::Result<RecordedFrame> RecordedFrame::create(IngressFrameAttempt attempt,
                                                   const runtime::RuntimePolicy& policy,
                                                   model::ReceiveSequence receive_sequence,
                                                   model::ReceiveTimestamp receive_timestamp) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Revalidation is race-free because the runtime policy is immutable for the runtime lifetime.
  auto source = resolve_runtime_source(attempt, policy);
  if (!source) {
    return model::Result<RecordedFrame>::failure(source.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Transfer bounded bytes while adding only policy-derived source and owner receipt identity.
  return model::Result<RecordedFrame>::success(RecordedFrame{
      MarketSourceIdentity::from_runtime_source(*source.value()), attempt.session_epoch_,
      receive_sequence, receive_timestamp, std::move(attempt.frame_)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Report semantic payload kind independently of std::variant alternative indices.
ParsedMarketMessageKind ParsedMarketMessage::kind() const noexcept {
  if (const auto* update = std::get_if<ParsedFixtureBookUpdate>(&payload_); update != nullptr) {
    return update->kind == ParsedFixtureBookUpdateKind::Snapshot ? ParsedMarketMessageKind::Snapshot
                                                                 : ParsedMarketMessageKind::Delta;
  }
  if (std::holds_alternative<ParsedFixtureSessionStarted>(payload_)) {
    return ParsedMarketMessageKind::SessionStarted;
  }
  return ParsedMarketMessageKind::StalenessCheck;
}

// --------------------------------------------------------
// Select the successful parse arm.
RecordedFixtureParseResult RecordedFixtureParseResult::success(ParsedMarketMessage message) {
  return RecordedFixtureParseResult{std::move(message)};
}

// --------------------------------------------------------
// Select the failed parse arm.
RecordedFixtureParseResult RecordedFixtureParseResult::failure(RecordedFixtureParseError error) {
  return RecordedFixtureParseResult{error};
}

// --------------------------------------------------------
// Inspect which parse-result arm is active.
bool RecordedFixtureParseResult::has_value() const noexcept {
  return std::holds_alternative<ParsedMarketMessage>(storage_);
}

// --------------------------------------------------------
// Borrow the mutable successful message.
ParsedMarketMessage& RecordedFixtureParseResult::value() & {
  return std::get<ParsedMarketMessage>(storage_);
}

// --------------------------------------------------------
// Borrow the immutable successful message.
const ParsedMarketMessage& RecordedFixtureParseResult::value() const& {
  return std::get<ParsedMarketMessage>(storage_);
}

// --------------------------------------------------------
// Move the successful message from a temporary result.
ParsedMarketMessage&& RecordedFixtureParseResult::value() && {
  return std::get<ParsedMarketMessage>(std::move(storage_));
}

// --------------------------------------------------------
// Borrow the mutable failed result.
RecordedFixtureParseError& RecordedFixtureParseResult::error() & {
  return std::get<RecordedFixtureParseError>(storage_);
}

// --------------------------------------------------------
// Borrow the immutable failed result.
const RecordedFixtureParseError& RecordedFixtureParseResult::error() const& {
  return std::get<RecordedFixtureParseError>(storage_);
}

// --------------------------------------------------------
// Select the successful variant arm explicitly.
RecordedFixtureParseResult::RecordedFixtureParseResult(ParsedMarketMessage message)
    : storage_{std::in_place_type<ParsedMarketMessage>, std::move(message)} {}

// --------------------------------------------------------
// Select the failed variant arm explicitly.
RecordedFixtureParseResult::RecordedFixtureParseResult(RecordedFixtureParseError error)
    : storage_{std::in_place_type<RecordedFixtureParseError>, error} {}

// --------------------------------------------------------
// Parse one complete versioned frame without exposing partial intermediate state.
RecordedFixtureParseResult parse_recorded_fixture(const RecordedFrame& frame) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Defensively preserve parse dispositions even though RecordedFrame has already enforced bounds.
  if (frame.frame().empty()) {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::EmptyFrame, 0U));
  }
  if (frame.frame().size() > recorded_fixture_maximum_frame_bytes) {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::FrameTooLarge, recorded_fixture_maximum_frame_bytes));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse and verify the repository magic, exact schema version, and duplicated source identity.
  FixtureFieldCursor cursor{frame.frame()};
  auto error = parse_error(RecordedFixtureParseCode::UnexpectedEnd, 0U);

  const auto magic = required_field(cursor, error);
  if (!magic.has_value()) {
    return RecordedFixtureParseResult::failure(error);
  }
  if (magic->text != "AEGISMD") {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::InvalidMagic, magic->byte_offset));
  }

  const auto version = required_field(cursor, error);
  if (!version.has_value()) {
    return RecordedFixtureParseResult::failure(error);
  }
  if (version->text != "1") {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::UnsupportedVersion, version->byte_offset));
  }

  const auto source = required_field(cursor, error);
  if (!source.has_value()) {
    return RecordedFixtureParseResult::failure(error);
  }
  auto source_id = model::MarketSourceId::parse(source->text);
  if (!source_id) {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::InvalidSourceId, source->byte_offset));
  }
  if (source_id.value() != frame.source().source_id()) {
    return RecordedFixtureParseResult::failure(
        parse_error(RecordedFixtureParseCode::SourceMismatch, source->byte_offset));
  }

  const auto type = required_field(cursor, error);
  if (!type.has_value()) {
    return RecordedFixtureParseResult::failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the selected closed payload grammar into temporary storage.
  std::optional<ParsedFixturePayload> payload;
  if (type->text == "snapshot" || type->text == "delta") {
    const auto kind = type->text == "snapshot" ? ParsedFixtureBookUpdateKind::Snapshot
                                               : ParsedFixtureBookUpdateKind::Delta;
    auto update = parse_book_update(cursor, kind, error);
    if (update.has_value()) {
      payload.emplace(std::move(update).value());
    }
  } else if (type->text == "session-started") {
    auto session = parse_session_started(cursor, error);
    if (session.has_value()) {
      payload.emplace(session.value());
    }
  } else if (type->text == "staleness-check") {
    auto check = parse_staleness_check(cursor, error);
    if (check.has_value()) {
      payload.emplace(check.value());
    }
  } else {
    error = parse_error(RecordedFixtureParseCode::UnsupportedMessageType, type->byte_offset);
  }
  if (!payload.has_value()) {
    return RecordedFixtureParseResult::failure(error);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Attach only policy-derived source and executor-assigned receipt identity after full parsing.
  return RecordedFixtureParseResult::success(ParsedMarketMessage{
      frame.source(),
      frame.session_epoch(),
      frame.receive_sequence(),
      frame.receive_timestamp(),
      std::move(payload).value(),
  });

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve parsed attribution and publish exactly one policy-backed normalized command.
model::Result<NormalizedRecordedMarketCommand>
normalize_recorded_fixture(ParsedMarketMessage message, const runtime::RuntimePolicy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the complete configured tuple before interpreting any parsed payload alternative.
  auto source = resolve_source_identity(message, policy);
  if (!source) {
    return model::Result<NormalizedRecordedMarketCommand>::failure(source.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Book updates retain a separate semantic validation path owned by NormalizedMarketUpdate.
  if (std::holds_alternative<ParsedFixtureBookUpdate>(message.payload())) {
    return normalize_book_update(std::move(message), std::move(source).value(), policy);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Control commands carry only their exact recorded and owner-assigned timing domains.
  if (const auto* session = std::get_if<ParsedFixtureSessionStarted>(&message.payload());
      session != nullptr) {
    return model::Result<NormalizedRecordedMarketCommand>::success(NormalizedRecordedMarketCommand{
        std::in_place_type<SessionStarted>, std::move(source).value(), message.session_epoch(),
        session->source_timestamp, message.receive_sequence(), message.receive_timestamp()});
  }

  const auto& staleness = std::get<ParsedFixtureStalenessCheck>(message.payload());
  return model::Result<NormalizedRecordedMarketCommand>::success(NormalizedRecordedMarketCommand{
      std::in_place_type<StalenessCheck>, std::move(source).value(), message.session_epoch(),
      message.receive_sequence(), message.receive_timestamp(), staleness.processing_timestamp});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::market_data
