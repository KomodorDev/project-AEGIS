// Purpose: define the bounded credential-free AEGISMD fixture DTO, strict parser result, and
// policy-backed normalization boundary used by deterministic market-data replay.

#pragma once

#include "aegis/market_data/market_event.hpp"
#include "aegis/market_data/market_limits.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// RuntimePolicy is the only registry that may resolve ingress attribution or mint source identity.
class RuntimePolicy;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::market_data {

// ########################################################################
// AEGISMD schema one derives every variable-size ceiling from the shared M2 market-data contract.
// These aliases keep fixture call sites readable without creating independent limits that can
// drift.
inline constexpr std::size_t recorded_fixture_maximum_frame_bytes = maximum_recorded_frame_bytes;
inline constexpr std::size_t recorded_fixture_maximum_levels = maximum_changes_per_market_update;
inline constexpr std::size_t recorded_fixture_maximum_integrity_token_bytes =
    maximum_integrity_token_bytes;

// ########################################################################
// The post-admission recorded frame is a distinct immutable product rather than an ingress alias.
class RecordedFrame;

// ########################################################################
// Own caller-supplied bytes and optional untrusted attribution before executor admission. The
// factory bounds allocation exposure but deliberately grants no configured-source authority.
class IngressFrameAttempt final {
public:

  // --------------------------------------------------------
  // Own one attempt only when its complete bytes fit the compiled AEGISMD schema-one ceiling.
  [[nodiscard]] static model::Result<IngressFrameAttempt>
  create_ingress_frame_attempt(std::optional<model::MarketSourceId> source_id,
                               model::SessionEpoch session_epoch, std::string frame);

  // --------------------------------------------------------
  // Borrow optional caller attribution without claiming that the runtime policy contains it.
  [[nodiscard]] const std::optional<model::MarketSourceId>& source_id() const noexcept {
    return source_id_;
  }

  // --------------------------------------------------------
  // Return the caller-supplied session in which any parsed source sequence will be interpreted.
  [[nodiscard]] model::SessionEpoch session_epoch() const noexcept { return session_epoch_; }

  // --------------------------------------------------------
  // Borrow the compiled-bounded bytes for policy validation and eventual owner-side parsing.
  [[nodiscard]] std::string_view frame() const noexcept { return frame_; }

  // --------------------------------------------------------
  // Structural equality compares complete untrusted replay input without adding authority.
  friend bool operator==(const IngressFrameAttempt&, const IngressFrameAttempt&) = default;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Interesting syntax: friendship permits only the post-admission factory to transfer owned bytes
  // without another allocation after the executor has assigned receive identity.
  friend class RecordedFrame;

  // ########################################################################

  // --------------------------------------------------------
  // Restrict publication to the compiled-size validation path.
  IngressFrameAttempt(std::optional<model::MarketSourceId> source_id,
                      model::SessionEpoch session_epoch, std::string frame)
      : source_id_{std::move(source_id)}, session_epoch_{session_epoch}, frame_{std::move(frame)} {}

  // --------------------------------------------------------
  std::optional<model::MarketSourceId> source_id_;
  model::SessionEpoch session_epoch_;
  std::string frame_;
};

// ########################################################################

// --------------------------------------------------------
// Validate optional attribution and the configured frame limit before admission, returning the
// stable source ordinal needed for attributable capacity-loss handling.
[[nodiscard]] model::Result<model::MarketSourceOrdinal>
resolve_recorded_frame_source(const IngressFrameAttempt& attempt,
                              const runtime::RuntimePolicy& policy);

// --------------------------------------------------------

// ########################################################################
// Own one policy-resolved public frame after admission. Venue and instrument identity are derived
// solely from RuntimeSource, while receive sequence and time come solely from the executor receipt.
class RecordedFrame final {
public:

  // --------------------------------------------------------
  // Revalidate the attempt against the immutable policy and mint its complete post-admission value.
  [[nodiscard]] static model::Result<RecordedFrame>
  create_recorded_frame(IngressFrameAttempt attempt, const runtime::RuntimePolicy& policy,
                        model::ReceiveSequence receive_sequence,
                        model::ReceiveTimestamp receive_timestamp);

  // --------------------------------------------------------
  // Borrow the opaque configured identity copied from exactly one RuntimeSource.
  [[nodiscard]] const MarketSourceIdentity& source() const noexcept { return source_; }

  // --------------------------------------------------------
  // Return recorded session and executor-assigned receive identity without crossing clock domains.
  [[nodiscard]] model::SessionEpoch session_epoch() const noexcept { return session_epoch_; }
  [[nodiscard]] model::ReceiveSequence receive_sequence() const noexcept {
    return receive_sequence_;
  }
  [[nodiscard]] model::ReceiveTimestamp receive_timestamp() const noexcept {
    return receive_timestamp_;
  }

  // --------------------------------------------------------
  // Borrow policy-bounded bytes; only the strict parser may assign them fixture meaning.
  [[nodiscard]] std::string_view frame() const noexcept { return frame_; }

  // --------------------------------------------------------
  // Structural equality compares exact configured attribution, receipt identity, and bytes.
  friend bool operator==(const RecordedFrame&, const RecordedFrame&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Assemble only values already resolved through policy and stamped by accepted admission.
  RecordedFrame(MarketSourceIdentity source, model::SessionEpoch session_epoch,
                model::ReceiveSequence receive_sequence, model::ReceiveTimestamp receive_timestamp,
                std::string frame)
      : source_{std::move(source)}, session_epoch_{session_epoch},
        receive_sequence_{receive_sequence}, receive_timestamp_{receive_timestamp},
        frame_{std::move(frame)} {}

  // --------------------------------------------------------
  MarketSourceIdentity source_;
  model::SessionEpoch session_epoch_;
  model::ReceiveSequence receive_sequence_;
  model::ReceiveTimestamp receive_timestamp_;
  std::string frame_;
};

// ########################################################################
// Stable numeric values persist parser dispositions without depending on prose or implementation-
// specific standard-library categories. They describe syntax and compatibility, not book validity.
enum class RecordedFixtureParseCode : std::uint16_t {
  EmptyFrame = 1,
  FrameTooLarge = 2,
  UnexpectedEnd = 3,
  InvalidMagic = 4,
  UnsupportedVersion = 5,
  InvalidSourceId = 6,
  SourceMismatch = 7,
  UnsupportedMessageType = 8,
  InvalidUnsignedInteger = 9,
  NumericOverflow = 10,
  InvalidRevision = 11,
  InvalidPredecessor = 12,
  InvalidIntegrityToken = 13,
  TooManyLevels = 14,
  InvalidLevelSyntax = 15,
  InvalidSide = 16,
  InvalidPrice = 17,
  InvalidQuantity = 18,
  LevelCountMismatch = 19,
  TrailingInput = 20,
};

// ########################################################################
// Keep parser failure transport a trivial fixed-layout value containing only an assigned code and
// the deterministic byte offset of the first offending field.
struct RecordedFixtureParseError {
  RecordedFixtureParseCode code;
  std::uint32_t byte_offset;

  // --------------------------------------------------------
  // Structural equality pins the complete deterministic failure output.
  friend bool operator==(RecordedFixtureParseError, RecordedFixtureParseError) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The parse failure representation can be copied directly into fixed-layout bounded diagnostics.
static_assert(std::is_trivial_v<RecordedFixtureParseError>);
static_assert(std::is_standard_layout_v<RecordedFixtureParseError>);

// ########################################################################
// Sides remain explicit in parsed input; canonical order and mutation belong to later boundaries.
enum class ParsedFixtureBookSide : std::uint8_t {
  Bid = 1,
  Ask = 2,
};

// ########################################################################
// Preserve one exact price/quantity pair. A zero quantity remains parser data so normalized update
// policy can distinguish permitted delta deletion from an invalid snapshot entry.
struct ParsedFixtureBookLevel {
  ParsedFixtureBookSide side;
  model::Price price;
  model::Quantity quantity;

  // --------------------------------------------------------
  // Structural equality supports exact fixture replay comparisons.
  friend bool operator==(const ParsedFixtureBookLevel&, const ParsedFixtureBookLevel&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Snapshot and delta remain distinguishable before either can reach mutable market state.
enum class ParsedFixtureBookUpdateKind : std::uint8_t {
  Snapshot = 1,
  Delta = 2,
};

// ########################################################################
// Fixtures declare an integrity verdict while retaining an algorithm-neutral token.
enum class ParsedFixtureIntegrityVerdict : std::uint8_t {
  Accepted = 1,
  Rejected = 2,
};

// ########################################################################
// Pair the authored verdict with its already syntax-checked bounded token for later hashing.
struct ParsedFixtureIntegrity {
  ParsedFixtureIntegrityVerdict verdict;
  std::string token;

  // --------------------------------------------------------
  // Structural equality preserves the complete authored integrity assertion.
  friend bool operator==(const ParsedFixtureIntegrity&, const ParsedFixtureIntegrity&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Own every syntactically valid book-update field in temporary storage. Domain validation remains
// deliberately deferred to NormalizedMarketUpdate so the two failure vocabularies cannot blur.
struct ParsedFixtureBookUpdate {
  ParsedFixtureBookUpdateKind kind;
  model::SequenceNumber source_sequence;
  std::optional<model::SequenceNumber> predecessor_sequence;
  model::SourceTimestamp source_timestamp;
  model::InstrumentMetadataRevision metadata_revision;
  ParsedFixtureIntegrity integrity;
  std::vector<ParsedFixtureBookLevel> levels;

  // --------------------------------------------------------
  // Structural equality makes parser output replay-comparable.
  friend bool operator==(const ParsedFixtureBookUpdate&, const ParsedFixtureBookUpdate&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Session-started input carries recorded source time while its session and receive identity remain
// in the immutable policy-minted recorded frame.
struct ParsedFixtureSessionStarted {
  model::SourceTimestamp source_timestamp;

  // --------------------------------------------------------
  // Structural equality supports exact control-fixture comparisons.
  friend bool operator==(ParsedFixtureSessionStarted, ParsedFixtureSessionStarted) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Staleness input carries explicit processing time and never reads an ambient clock during replay.
struct ParsedFixtureStalenessCheck {
  model::ProcessingTimestamp processing_timestamp;

  // --------------------------------------------------------
  // Structural equality pins the explicit deterministic timer input.
  friend bool operator==(ParsedFixtureStalenessCheck, ParsedFixtureStalenessCheck) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The closed parsed payload union keeps controls separate from book updates.
using ParsedFixturePayload =
    std::variant<ParsedFixtureBookUpdate, ParsedFixtureSessionStarted, ParsedFixtureStalenessCheck>;

// ########################################################################
// A stable semantic kind avoids exposing std::variant indices as compatibility identifiers.
enum class ParsedMarketMessageKind : std::uint8_t {
  Snapshot = 1,
  Delta = 2,
  SessionStarted = 3,
  StalenessCheck = 4,
};

// ########################################################################
// Forward-declare the closed parse result so only the parser function can mint its message arm.
class RecordedFixtureParseResult;

// ########################################################################
// Combine policy-derived recorded-frame attribution with one completely parsed payload. Immutable
// publication prevents callers from changing source identity between parsing and normalization.
class ParsedMarketMessage final {
public:

  // --------------------------------------------------------
  // Borrow the policy-derived identity whose source ID matched the embedded bounded token.
  [[nodiscard]] const MarketSourceIdentity& source() const noexcept { return source_; }

  // --------------------------------------------------------
  // Return the exact recorded session and owner-assigned receive identity.
  [[nodiscard]] model::SessionEpoch session_epoch() const noexcept { return session_epoch_; }
  [[nodiscard]] model::ReceiveSequence receive_sequence() const noexcept {
    return receive_sequence_;
  }
  [[nodiscard]] model::ReceiveTimestamp receive_timestamp() const noexcept {
    return receive_timestamp_;
  }

  // --------------------------------------------------------
  // Borrow the closed parsed payload without exposing mutable partial state.
  [[nodiscard]] const ParsedFixturePayload& payload() const noexcept { return payload_; }

  // --------------------------------------------------------
  // Report semantic payload kind without leaking variant alternative ordering.
  [[nodiscard]] ParsedMarketMessageKind kind() const noexcept;

  // --------------------------------------------------------
  // Structural equality supports exact parser replay assertions.
  friend bool operator==(const ParsedMarketMessage&, const ParsedMarketMessage&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Grant construction only to the all-or-error parser after every frame field has succeeded.
  friend RecordedFixtureParseResult parse_recorded_fixture(const RecordedFrame& frame);

  // --------------------------------------------------------
  // Assemble one immutable DTO from the verified recorded frame and complete temporary payload.
  ParsedMarketMessage(MarketSourceIdentity source, model::SessionEpoch session_epoch,
                      model::ReceiveSequence receive_sequence,
                      model::ReceiveTimestamp receive_timestamp, ParsedFixturePayload payload)
      : source_{std::move(source)}, session_epoch_{session_epoch},
        receive_sequence_{receive_sequence}, receive_timestamp_{receive_timestamp},
        payload_{std::move(payload)} {}

  // --------------------------------------------------------
  MarketSourceIdentity source_;
  model::SessionEpoch session_epoch_;
  model::ReceiveSequence receive_sequence_;
  model::ReceiveTimestamp receive_timestamp_;
  ParsedFixturePayload payload_;
};

// ########################################################################
// Return either one fully constructed message or one fixed-layout parse failure. Accessing an
// inactive arm remains a programming error, matching the repository Result convention.
class [[nodiscard]] RecordedFixtureParseResult final {
public:

  // --------------------------------------------------------
  // Publish one completely parsed message.
  [[nodiscard]] static RecordedFixtureParseResult create_success(ParsedMarketMessage message);

  // --------------------------------------------------------
  // Publish one stable syntax or compatibility failure without a partial message.
  [[nodiscard]] static RecordedFixtureParseResult create_failure(RecordedFixtureParseError error);

  // --------------------------------------------------------
  // Inspect which result arm is active.
  [[nodiscard]] bool has_value() const noexcept;

  // --------------------------------------------------------
  // Support concise branches while retaining explicit conversion semantics.
  explicit operator bool() const noexcept { return has_value(); }

  // --------------------------------------------------------
  // Borrow or move the successful message while preserving value category.
  [[nodiscard]] ParsedMarketMessage& value() &;
  [[nodiscard]] const ParsedMarketMessage& value() const&;
  [[nodiscard]] ParsedMarketMessage&& value() &&;

  // --------------------------------------------------------
  // Borrow the fixed-layout error from a failed result.
  [[nodiscard]] RecordedFixtureParseError& error() &;
  [[nodiscard]] const RecordedFixtureParseError& error() const&;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Select the successful result arm explicitly.
  explicit RecordedFixtureParseResult(ParsedMarketMessage message);

  // --------------------------------------------------------
  // Select the failed result arm explicitly.
  explicit RecordedFixtureParseResult(RecordedFixtureParseError error);

  // --------------------------------------------------------
  std::variant<ParsedMarketMessage, RecordedFixtureParseError> storage_;
};

// ########################################################################

// --------------------------------------------------------
// Parse one complete strict AEGISMD schema-one frame without exposing partial intermediate state.
[[nodiscard]] RecordedFixtureParseResult parse_recorded_fixture(const RecordedFrame& frame);

// --------------------------------------------------------
// Resolve the parsed source in one immutable runtime policy, then map the complete DTO into the
// normalized command vocabulary. Domain validation failures remain model::DomainError values.
[[nodiscard]] model::Result<NormalizedRecordedMarketCommand>
normalize_recorded_fixture(ParsedMarketMessage message, const runtime::RuntimePolicy& policy);

// --------------------------------------------------------

} // namespace aegis::market_data
