// Purpose: prove strict AEGISMD parsing and policy-backed normalization remain bounded,
// credential-free, atomic, deterministic, and separated by stable failure vocabularies.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/market_data/recorded_fixture.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: requires-expression probes make credential, endpoint, socket, or order
// capabilities compile-time regressions in the recorded public-data vocabulary.
template <typename Value>
concept HasCredentials = requires(Value value) { value.credentials; };

template <typename Value>
concept HasSecret = requires(Value value) { value.secret; };

template <typename Value>
concept HasEndpoint = requires(Value value) { value.endpoint; };

template <typename Value>
concept HasSocket = requires(Value value) { value.socket; };

template <typename Value>
concept HasOrder = requires(Value value) { value.order; };

template <typename Value>
concept HasVenueId = requires(Value value) { value.venue_id; };

static_assert(!HasCredentials<market_data::IngressFrameAttempt>);
static_assert(!HasSecret<market_data::IngressFrameAttempt>);
static_assert(!HasEndpoint<market_data::IngressFrameAttempt>);
static_assert(!HasSocket<market_data::IngressFrameAttempt>);
static_assert(!HasOrder<market_data::IngressFrameAttempt>);
static_assert(!HasVenueId<market_data::IngressFrameAttempt>);
static_assert(!HasCredentials<market_data::RecordedFrame>);
static_assert(!HasSecret<market_data::RecordedFrame>);
static_assert(!HasEndpoint<market_data::RecordedFrame>);
static_assert(!HasSocket<market_data::RecordedFrame>);
static_assert(!HasOrder<market_data::RecordedFrame>);
static_assert(!HasCredentials<market_data::ParsedMarketMessage>);
static_assert(!HasSecret<market_data::ParsedMarketMessage>);
static_assert(!HasEndpoint<market_data::ParsedMarketMessage>);
static_assert(!HasSocket<market_data::ParsedMarketMessage>);
static_assert(!HasOrder<market_data::ParsedMarketMessage>);
static_assert(!std::is_aggregate_v<market_data::IngressFrameAttempt>);
static_assert(!std::is_aggregate_v<market_data::RecordedFrame>);
static_assert(!std::is_aggregate_v<market_data::ParsedMarketMessage>);
static_assert(
    !std::is_constructible_v<market_data::IngressFrameAttempt, std::optional<model::MarketSourceId>,
                             model::SessionEpoch, std::string>);
static_assert(!std::is_constructible_v<
              market_data::RecordedFrame, market_data::MarketSourceIdentity, model::SessionEpoch,
              model::ReceiveSequence, model::ReceiveTimestamp, std::string>);
static_assert(
    std::is_same_v<decltype(std::declval<const market_data::RecordedFrame&>().receive_sequence()),
                   model::ReceiveSequence>);

// ########################################################################

// --------------------------------------------------------
// Invalid typed literals indicate a broken test fixture rather than behavior under test.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in recorded-fixture test"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Unwrap one-based receive identity while rejecting invalid test setup immediately.
[[nodiscard]] model::ReceiveSequence receive_sequence(std::uint64_t value) {
  auto parsed = model::ReceiveSequence::from_value(value);
  if (!parsed) {
    throw std::logic_error{"invalid receive sequence in recorded-fixture test"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build one caller-owned attempt without granting its optional attribution policy authority.
[[nodiscard]] market_data::IngressFrameAttempt
ingress_attempt(std::string frame,
                std::optional<std::string_view> source_id = "source.deribit.public") {
  std::optional<model::MarketSourceId> typed_source;
  if (source_id.has_value()) {
    typed_source = id<model::MarketSourceId>(source_id.value());
  }
  auto result = market_data::IngressFrameAttempt::create(std::move(typed_source),
                                                         model::SessionEpoch{7U}, std::move(frame));
  if (!result) {
    throw std::logic_error{"invalid ingress attempt in recorded-fixture test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Seal one reference runtime policy whose only source may mint the normalized source identity.
[[nodiscard]] runtime::RuntimePolicy
policy_with_limits(std::uint32_t maximum_changes =
                       static_cast<std::uint32_t>(market_data::maximum_changes_per_market_update),
                   std::uint32_t maximum_frame_bytes =
                       static_cast<std::uint32_t>(market_data::maximum_recorded_frame_bytes),
                   std::string_view source_id = "source.deribit.public") {
  auto configuration_result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  if (!configuration_result) {
    throw std::logic_error{"invalid startup configuration in recorded-fixture test"};
  }
  auto configuration = std::move(configuration_result).value();
  runtime::RuntimePolicyParams params{
      runtime::RuntimePolicyLimits{8U, maximum_frame_bytes, maximum_changes, 20U, 5'000'000'000U,
                                   4U, 128U, 256U, 32U, 100'000U},
      {{id<model::MarketSourceId>(source_id), id<model::VenueId>("deribit"),
        id<model::InstrumentId>("BTC-USD-PERPETUAL"), id<model::VenueInstrumentId>("BTC-PERPETUAL"),
        model::InstrumentMetadataRevision::initial()}}};
  auto policy_result = runtime::RuntimePolicy::create(configuration, std::move(params));
  if (!policy_result) {
    throw std::logic_error{"invalid runtime policy in recorded-fixture test"};
  }
  return std::move(policy_result).value();
}

// --------------------------------------------------------
// Reuse one immutable configured policy across ordinary parser and normalization tests.
[[nodiscard]] const runtime::RuntimePolicy& configured_policy() {
  static const auto policy = policy_with_limits();
  return policy;
}

// --------------------------------------------------------
// Mint the immutable post-admission frame from policy and explicitly owner-assigned receipt fields.
[[nodiscard]] market_data::RecordedFrame
recorded_frame(std::string frame, const runtime::RuntimePolicy& policy = configured_policy(),
               std::uint64_t sequence = 19U, std::uint64_t receive_time = 9'000U,
               std::optional<std::string_view> source_id = "source.deribit.public") {
  auto result = market_data::RecordedFrame::create(ingress_attempt(std::move(frame), source_id),
                                                   policy, receive_sequence(sequence),
                                                   model::ReceiveTimestamp{receive_time});
  if (!result) {
    throw std::logic_error{"invalid recorded frame in recorded-fixture test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Assert the complete stable parser failure, including the first offending byte.
void require_error(std::string frame, market_data::RecordedFixtureParseCode code,
                   std::size_t byte_offset,
                   std::string_view attributed_source = "source.deribit.public") {
  const auto result = market_data::parse_recorded_fixture(
      recorded_frame(std::move(frame), configured_policy(), 19U, 9'000U, attributed_source));
  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        market_data::RecordedFixtureParseError{code, static_cast<std::uint32_t>(byte_offset)});
}

// --------------------------------------------------------
// Parse a valid frame and transfer the complete DTO into the policy-backed normalization boundary.
[[nodiscard]] model::Result<market_data::NormalizedRecordedMarketCommand>
normalize_frame(std::string frame, const runtime::RuntimePolicy& policy = configured_policy(),
                std::uint64_t sequence = 19U, std::uint64_t receive_time = 9'000U,
                std::string_view source_id = "source.deribit.public") {
  auto parsed = market_data::parse_recorded_fixture(
      recorded_frame(std::move(frame), policy, sequence, receive_time, source_id));
  if (!parsed) {
    throw std::logic_error{"normalization test supplied malformed AEGISMD bytes"};
  }
  return market_data::normalize_recorded_fixture(std::move(parsed).value(), policy);
}

// --------------------------------------------------------
// Render a fixed digest into the same lowercase compatibility form used by other golden tests.
[[nodiscard]] std::string digest_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Construct a maximum-change frame with unique levels so bound acceptance is independently visible.
[[nodiscard]] std::string maximum_level_frame() {
  std::string frame = "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:max|64";
  for (std::size_t index = 0U; index < market_data::maximum_changes_per_market_update; ++index) {
    frame += "|B," + std::to_string(100U + index) + ",1";
  }
  return frame;
}

// --------------------------------------------------------

// --------------------------------------------------------
// The frame source uses the shared nominal grammar and its exact 64-byte upper boundary.
TEST_CASE("recorded fixture source identity is nominal and exactly bounded",
          "[market_data][recorded_fixture]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A 64-byte source resolves through policy and remains the same opaque identity after parsing.
  const std::string maximum_source =
      "source." + std::string(64U - std::string_view{"source."}.size(), 'a');
  const std::string maximum_frame = "AEGISMD|1|" + maximum_source + "|session-started|7";
  const auto maximum_source_policy = policy_with_limits(
      static_cast<std::uint32_t>(market_data::maximum_changes_per_market_update),
      static_cast<std::uint32_t>(market_data::maximum_recorded_frame_bytes), maximum_source);
  const auto accepted = market_data::parse_recorded_fixture(
      recorded_frame(maximum_frame, maximum_source_policy, 19U, 9'000U, maximum_source));
  REQUIRE(accepted);
  CHECK(accepted.value().source().source_id() == id<model::MarketSourceId>(maximum_source));

  // ++++++++++++++++++++++++++++++++++++++++
  // The shared grammar rejects underscores and any source larger than 64 bytes inside the frame.
  const std::string underscore = "AEGISMD|1|source.deribit_public|session-started|7";
  require_error(underscore, market_data::RecordedFixtureParseCode::InvalidSourceId,
                underscore.find("source."));

  const std::string oversized_source = maximum_source + "a";
  const std::string oversized_frame = "AEGISMD|1|" + oversized_source + "|session-started|7";
  require_error(oversized_frame, market_data::RecordedFixtureParseCode::InvalidSourceId,
                oversized_frame.find("source."));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A valid snapshot publishes policy attribution and exact parser data only after all levels pass.
TEST_CASE("recorded snapshot parsing preserves attribution and exact book data",
          "[market_data][recorded_fixture]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse one complete snapshot whose final zero quantity remains observable pre-domain data.
  const auto result = market_data::parse_recorded_fixture(
      recorded_frame("AEGISMD|1|source.deribit.public|snapshot|100|none|5000000|2|ok:book100|2|"
                     "B,30000.5,2|A,30001.0,0"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify policy-derived attribution and distinct nominal receive and source domains.
  REQUIRE(result);
  CHECK(result.value().kind() == market_data::ParsedMarketMessageKind::Snapshot);
  CHECK(result.value().source().source_id() == id<model::MarketSourceId>("source.deribit.public"));
  CHECK(result.value().source().source_ordinal() == model::MarketSourceOrdinal::initial());
  CHECK(result.value().source().venue_id() == id<model::VenueId>("deribit"));
  CHECK(result.value().source().instrument_id() == id<model::InstrumentId>("BTC-USD-PERPETUAL"));
  CHECK(result.value().source().venue_instrument_id() ==
        id<model::VenueInstrumentId>("BTC-PERPETUAL"));
  CHECK(result.value().session_epoch() == model::SessionEpoch{7U});
  CHECK(result.value().receive_sequence() == receive_sequence(19U));
  CHECK(result.value().receive_timestamp() == model::ReceiveTimestamp{9'000U});

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify exact update identity, integrity, and fixed-point level preservation.
  const auto& update = std::get<market_data::ParsedFixtureBookUpdate>(result.value().payload());
  CHECK(update.kind == market_data::ParsedFixtureBookUpdateKind::Snapshot);
  CHECK(update.source_sequence == model::SequenceNumber{100U});
  CHECK_FALSE(update.predecessor_sequence.has_value());
  CHECK(update.source_timestamp == model::SourceTimestamp{5'000'000U});
  CHECK(update.metadata_revision == model::InstrumentMetadataRevision::from_value(2U).value());
  CHECK(update.integrity.verdict == market_data::ParsedFixtureIntegrityVerdict::Accepted);
  CHECK(update.integrity.token == "book100");
  REQUIRE(update.levels.size() == 2U);
  CHECK(update.levels[0U].side == market_data::ParsedFixtureBookSide::Bid);
  CHECK(update.levels[0U].price == model::Price::parse_ascii("30000.5").value());
  CHECK(update.levels[0U].quantity == model::Quantity::parse_ascii("2").value());
  CHECK(update.levels[1U].side == market_data::ParsedFixtureBookSide::Ask);
  CHECK(update.levels[1U].quantity == model::Quantity::parse_ascii("0").value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Delta parsing preserves predecessor identity, rejected integrity, and deletion intent unchanged.
TEST_CASE("recorded delta parsing retains predecessor and zero quantity",
          "[market_data][recorded_fixture]") {
  const auto result = market_data::parse_recorded_fixture(recorded_frame(
      "AEGISMD|1|source.deribit.public|delta|101|100|5000100|2|bad:fixture-mismatch|1|"
      "B,30000.5,0"));

  REQUIRE(result);
  CHECK(result.value().kind() == market_data::ParsedMarketMessageKind::Delta);
  const auto& update = std::get<market_data::ParsedFixtureBookUpdate>(result.value().payload());
  REQUIRE(update.predecessor_sequence.has_value());
  CHECK(update.predecessor_sequence.value() == model::SequenceNumber{100U});
  CHECK(update.integrity.verdict == market_data::ParsedFixtureIntegrityVerdict::Rejected);
  REQUIRE(update.levels.size() == 1U);
  CHECK(update.levels[0U].quantity.coefficient() == 0);
}

// --------------------------------------------------------
// Control records retain explicit clock domains and normalize without consulting an ambient clock.
TEST_CASE("session and staleness fixture controls normalize deterministically",
          "[market_data][recorded_fixture][normalization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Session start combines source time with recorded-frame session and receive identity.
  const auto session = normalize_frame("AEGISMD|1|source.deribit.public|session-started|7000",
                                       configured_policy(), 21U, 9'100U);
  REQUIRE(session);
  const auto& session_command = std::get<market_data::SessionStarted>(session.value());
  CHECK(session_command.source.source_id() == id<model::MarketSourceId>("source.deribit.public"));
  CHECK(session_command.source.source_ordinal() == model::MarketSourceOrdinal::initial());
  CHECK(session_command.session_epoch == model::SessionEpoch{7U});
  CHECK(session_command.source_timestamp == model::SourceTimestamp{7'000U});
  CHECK(session_command.receive_sequence == receive_sequence(21U));
  CHECK(session_command.receive_timestamp == model::ReceiveTimestamp{9'100U});

  // ++++++++++++++++++++++++++++++++++++++++
  // Staleness carries its scripted processing time alongside the same policy-backed source.
  const auto staleness = normalize_frame("AEGISMD|1|source.deribit.public|staleness-check|12000",
                                         configured_policy(), 22U, 9'200U);
  REQUIRE(staleness);
  const auto& staleness_command = std::get<market_data::StalenessCheck>(staleness.value());
  CHECK(staleness_command.source.source_ordinal() == model::MarketSourceOrdinal::initial());
  CHECK(staleness_command.receive_sequence == receive_sequence(22U));
  CHECK(staleness_command.receive_timestamp == model::ReceiveTimestamp{9'200U});
  CHECK(staleness_command.processing_timestamp == model::ProcessingTimestamp{12'000U});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Closed compatibility identifiers and duplicated source attribution fail at exact token offsets.
TEST_CASE("recorded fixtures reject unsupported contracts and source mismatch",
          "[market_data][recorded_fixture]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Version and message-kind values are closed compatibility identifiers.
  const std::string unsupported_version =
      "AEGISMD|2|source.deribit.public|snapshot|1|none|2|1|ok:x|0";
  require_error(unsupported_version, market_data::RecordedFixtureParseCode::UnsupportedVersion,
                unsupported_version.find('2'));

  const std::string unsupported_type = "AEGISMD|1|source.deribit.public|quote|1";
  require_error(unsupported_type, market_data::RecordedFixtureParseCode::UnsupportedMessageType,
                unsupported_type.find("quote"));

  // ++++++++++++++++++++++++++++++++++++++++
  // A syntactically valid embedded source cannot contradict the policy-derived recorded source.
  const std::string source_mismatch = "AEGISMD|1|source.other.public|session-started|1";
  require_error(source_mismatch, market_data::RecordedFixtureParseCode::SourceMismatch,
                source_mismatch.find("source.other"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Missing common fields and declared levels distinguish truncation from count disagreement.
TEST_CASE("recorded fixtures report truncation and level-count mismatch",
          "[market_data][recorded_fixture]") {
  const std::string truncated = "AEGISMD|1|source.deribit.public|snapshot|1";
  require_error(truncated, market_data::RecordedFixtureParseCode::UnexpectedEnd, truncated.size());

  const std::string missing_level =
      "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:x|2|B,100,1";
  require_error(missing_level, market_data::RecordedFixtureParseCode::LevelCountMismatch,
                missing_level.size());
}

// --------------------------------------------------------
// Strict numeric parsing rejects signs, overflow, invalid revisions, and shifted level components.
TEST_CASE("recorded fixtures reject invalid numeric and level syntax",
          "[market_data][recorded_fixture]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Unsigned counters neither accept a sign nor wrap beyond exact storage width.
  const std::string signed_sequence = "AEGISMD|1|source.deribit.public|snapshot|-1|none|2|1|ok:x|0";
  require_error(signed_sequence, market_data::RecordedFixtureParseCode::InvalidUnsignedInteger,
                signed_sequence.find("-1"));

  const std::string overflow_sequence =
      "AEGISMD|1|source.deribit.public|snapshot|18446744073709551616|none|2|1|ok:x|0";
  require_error(overflow_sequence, market_data::RecordedFixtureParseCode::NumericOverflow,
                overflow_sequence.find("18446744073709551616"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Installed metadata revision zero remains invalid despite integer representability.
  const std::string zero_revision = "AEGISMD|1|source.deribit.public|snapshot|1|none|2|0|ok:x|0";
  require_error(zero_revision, market_data::RecordedFixtureParseCode::InvalidRevision,
                zero_revision.find("|0|ok") + 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Level components retain exact comma alignment and ordinary fixed-point decimal syntax.
  const std::string shifted_level = "AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|1|B,100";
  require_error(shifted_level, market_data::RecordedFixtureParseCode::InvalidLevelSyntax,
                shifted_level.find("B,100"));

  const std::string exponent_price = "AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|1|B,1e3,1";
  require_error(exponent_price, market_data::RecordedFixtureParseCode::InvalidPrice,
                exponent_price.find("1e3"));

  const std::string negative_quantity =
      "AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|1|B,100,-1";
  require_error(negative_quantity, market_data::RecordedFixtureParseCode::InvalidQuantity,
                negative_quantity.find("-1"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every variable-length region honors both its exact accepted edge and first rejected value.
TEST_CASE("recorded fixture frame, level, and integrity bounds are exact",
          "[market_data][recorded_fixture][bounds]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Exactly the compiled ceiling reaches policy mint and grammar validation.
  const std::string boundary_frame(market_data::maximum_recorded_frame_bytes, 'x');
  require_error(boundary_frame, market_data::RecordedFixtureParseCode::InvalidMagic, 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The first byte beyond the compiled ceiling fails before a mutable ingress value is published.
  const std::string oversized_frame(market_data::maximum_recorded_frame_bytes + 1U, 'x');
  const auto oversized_attempt = market_data::IngressFrameAttempt::create(
      id<model::MarketSourceId>("source.deribit.public"), model::SessionEpoch{7U}, oversized_frame);
  REQUIRE_FALSE(oversized_attempt);
  CHECK(oversized_attempt.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketEvent,
                                     "ingress_frame_attempt.frame"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Exactly 64 unique levels publish; the declared 65th fails before allocation or parsing.
  auto maximum_levels = market_data::parse_recorded_fixture(recorded_frame(maximum_level_frame()));
  REQUIRE(maximum_levels);
  CHECK(std::get<market_data::ParsedFixtureBookUpdate>(maximum_levels.value().payload())
            .levels.size() == market_data::maximum_changes_per_market_update);
  const auto maximum_normalized = market_data::normalize_recorded_fixture(
      std::move(maximum_levels).value(), configured_policy());
  REQUIRE(maximum_normalized);
  CHECK(
      std::get<market_data::NormalizedMarketUpdate>(maximum_normalized.value()).changes().size() ==
      market_data::maximum_changes_per_market_update);

  const std::string too_many_levels = "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:x|65";
  require_error(too_many_levels, market_data::RecordedFixtureParseCode::TooManyLevels,
                too_many_levels.rfind("65"));

  // ++++++++++++++++++++++++++++++++++++++++
  // The shared 64-byte integrity edge hashes later; the first oversized token fails in parsing.
  const std::string maximum_token(market_data::maximum_integrity_token_bytes, 'x');
  const std::string maximum_integrity =
      "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:" + maximum_token + "|0";
  REQUIRE(market_data::parse_recorded_fixture(recorded_frame(maximum_integrity)));
  REQUIRE(normalize_frame(maximum_integrity));

  const std::string oversized_token(market_data::maximum_integrity_token_bytes + 1U, 'x');
  const std::string oversized_integrity =
      "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:" + oversized_token + "|0";
  require_error(oversized_integrity, market_data::RecordedFixtureParseCode::InvalidIntegrityToken,
                oversized_integrity.find("ok:"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The immutable runtime policy may impose a stricter frame bound before executor admission.
TEST_CASE("configured frame limit accepts its exact edge and rejects the next byte",
          "[market_data][recorded_fixture][bounds][policy]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A compiled-valid attempt exactly at the configured limit resolves an attributable ordinal.
  constexpr std::uint32_t configured_limit = 128U;
  const auto policy = policy_with_limits(
      static_cast<std::uint32_t>(market_data::maximum_changes_per_market_update), configured_limit);
  auto exact = market_data::IngressFrameAttempt::create(
      id<model::MarketSourceId>("source.deribit.public"), model::SessionEpoch{7U},
      std::string(configured_limit, 'x'));
  REQUIRE(exact);
  const auto exact_source = market_data::resolve_recorded_frame_source(exact.value(), policy);
  REQUIRE(exact_source);
  CHECK(exact_source.value() == model::MarketSourceOrdinal::initial());
  const auto exact_frame = market_data::RecordedFrame::create(
      std::move(exact).value(), policy, receive_sequence(1U), model::ReceiveTimestamp{10U});
  REQUIRE(exact_frame);
  CHECK(exact_frame.value().frame().size() == configured_limit);

  // ++++++++++++++++++++++++++++++++++++++++
  // The next byte is below the compiled ceiling but fails both the pre-admission and mint seams.
  auto over = market_data::IngressFrameAttempt::create(
      id<model::MarketSourceId>("source.deribit.public"), model::SessionEpoch{7U},
      std::string(configured_limit + 1U, 'x'));
  REQUIRE(over);
  const auto rejected_source = market_data::resolve_recorded_frame_source(over.value(), policy);
  REQUIRE_FALSE(rejected_source);
  CHECK(rejected_source.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketEvent,
                                     "ingress_frame_attempt.frame"));
  const auto rejected_frame = market_data::RecordedFrame::create(
      std::move(over).value(), policy, receive_sequence(2U), model::ReceiveTimestamp{11U});
  REQUIRE_FALSE(rejected_frame);
  CHECK(rejected_frame.error() == rejected_source.error());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Extra fields remain observable incompatibilities rather than silently ignored extensions.
TEST_CASE("recorded fixtures reject trailing input", "[market_data][recorded_fixture]") {
  const std::string frame = "AEGISMD|1|source.deribit.public|session-started|7|extra";
  require_error(frame, market_data::RecordedFixtureParseCode::TrailingInput, frame.find("extra"));
}

// --------------------------------------------------------
// Parser and normalization failures use distinct result types and preserve their own authority.
TEST_CASE("syntax failures stay separate from normalized market validation",
          "[market_data][recorded_fixture][normalization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Malformed quantity text never publishes a parser DTO or a DomainError.
  const std::string malformed =
      "AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:x|1|B,100,broken";
  require_error(malformed, market_data::RecordedFixtureParseCode::InvalidQuantity,
                malformed.find("broken"));

  // ++++++++++++++++++++++++++++++++++++++++
  // A syntactically valid zero snapshot quantity is rejected only by normalized update semantics.
  const auto zero_snapshot =
      normalize_frame("AEGISMD|1|source.deribit.public|snapshot|1|none|2|1|ok:x|1|B,100,0");
  REQUIRE_FALSE(zero_snapshot);
  CHECK(zero_snapshot.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidMarketEvent,
                                     "market_update.changes.quantity", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Duplicate side/price keys are valid grammar but one invalid semantic normalized update.
  const auto duplicate =
      normalize_frame("AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|2|B,100,1|B,100,2");
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidMarketEvent,
                                     "market_update.changes.price", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Parser-level 64-change storage does not override a smaller immutable runtime-policy bound.
  const auto constrained_policy = policy_with_limits(1U);
  const auto constrained = normalize_frame(
      "AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|2|B,100,1|A,101,1", constrained_policy);
  REQUIRE_FALSE(constrained);
  CHECK(constrained.error() ==
        model::DomainError::at_field(model::DomainErrorCode::MarketBookCapacityExceeded,
                                     "market_update.changes"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Missing or unconfigured attribution cannot mint an ordinal, recorded frame, or normalized input.
TEST_CASE("policy resolution rejects missing and unconfigured ingress attribution",
          "[market_data][recorded_fixture][ingress][normalization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Missing optional attribution remains a valid caller attempt but cannot name source state.
  auto missing = market_data::IngressFrameAttempt::create(
      std::nullopt, model::SessionEpoch{7U}, "AEGISMD|1|source.deribit.public|session-started|7");
  REQUIRE(missing);
  const auto missing_source =
      market_data::resolve_recorded_frame_source(missing.value(), configured_policy());
  REQUIRE_FALSE(missing_source);
  CHECK(missing_source.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                     "ingress_frame_attempt.source_id"));
  const auto missing_frame =
      market_data::RecordedFrame::create(std::move(missing).value(), configured_policy(),
                                         receive_sequence(1U), model::ReceiveTimestamp{10U});
  REQUIRE_FALSE(missing_frame);
  CHECK(missing_frame.error() == missing_source.error());

  // ++++++++++++++++++++++++++++++++++++++++
  // A typed but absent registry ID fails through the same stable source-resolution contract.
  auto unconfigured =
      ingress_attempt("AEGISMD|1|source.other.public|session-started|7", "source.other.public");
  const auto unconfigured_source =
      market_data::resolve_recorded_frame_source(unconfigured, configured_policy());
  REQUIRE_FALSE(unconfigured_source);
  CHECK(unconfigured_source.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                     "ingress_frame_attempt.source_id"));
  const auto unconfigured_frame =
      market_data::RecordedFrame::create(std::move(unconfigured), configured_policy(),
                                         receive_sequence(2U), model::ReceiveTimestamp{11U});
  REQUIRE_FALSE(unconfigured_frame);
  CHECK(unconfigured_frame.error() == unconfigured_source.error());

  // ++++++++++++++++++++++++++++++++++++++++
  // A valid frame cannot normalize under a different policy that does not contain its source ID.
  auto parsed = market_data::parse_recorded_fixture(
      recorded_frame("AEGISMD|1|source.deribit.public|session-started|7", configured_policy()));
  REQUIRE(parsed);
  const auto other_policy = policy_with_limits(
      static_cast<std::uint32_t>(market_data::maximum_changes_per_market_update),
      static_cast<std::uint32_t>(market_data::maximum_recorded_frame_bytes), "source.other.public");
  const auto wrong_policy =
      market_data::normalize_recorded_fixture(std::move(parsed).value(), other_policy);
  REQUIRE_FALSE(wrong_policy);
  CHECK(wrong_policy.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                     "recorded_fixture.source_id"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Semantic update identity ignores authored order and receive timing after deterministic mapping.
TEST_CASE("fixture normalization canonicalizes changes and reproduces one payload digest",
          "[market_data][recorded_fixture][normalization][canonical]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Present the same semantic delta in opposite level order and distinct receive positions.
  const auto first =
      normalize_frame("AEGISMD|1|source.deribit.public|delta|101|100|5000100|1|ok:book101|2|"
                      "A,30001,2|B,30000,3",
                      configured_policy(), 19U, 9'000U);
  const auto second =
      normalize_frame("AEGISMD|1|source.deribit.public|delta|101|100|5000100|1|ok:book101|2|"
                      "B,30000,3|A,30001,2",
                      configured_policy(), 20U, 9'500U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Policy resolution supplies the same ordinal, canonical order, and timing-independent digest.
  REQUIRE(first);
  REQUIRE(second);
  const auto& first_update = std::get<market_data::NormalizedMarketUpdate>(first.value());
  const auto& second_update = std::get<market_data::NormalizedMarketUpdate>(second.value());
  CHECK(first_update.source().source_id() == id<model::MarketSourceId>("source.deribit.public"));
  CHECK(first_update.source().source_ordinal() == model::MarketSourceOrdinal::initial());
  REQUIRE(first_update.changes().size() == 2U);
  CHECK(first_update.changes()[0U].side == market_data::BookSide::Bid);
  CHECK(first_update.changes()[1U].side == market_data::BookSide::Ask);
  CHECK(first_update.receive_sequence() != second_update.receive_sequence());
  CHECK(first_update.receive_timestamp() != second_update.receive_timestamp());
  CHECK(first_update.payload_digest() == second_update.payload_digest());
  CHECK(digest_hex(first_update.payload_digest()) ==
        "de85fba1820002ae1260678b0e64637a8833f52c24af76434e4cd46f101c8f14");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A late malformed frame cannot retain partial values or contaminate a following valid item.
TEST_CASE("malformed fixture parsing does not corrupt subsequent parsing or normalization",
          "[market_data][recorded_fixture][isolation]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish a valid structural baseline, then fail at the final quantity field.
  const std::string valid = "AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|1|A,101,4";
  const auto before = market_data::parse_recorded_fixture(recorded_frame(valid));
  REQUIRE(before);

  const auto malformed = market_data::parse_recorded_fixture(
      recorded_frame("AEGISMD|1|source.deribit.public|delta|2|1|3|1|ok:x|1|A,101,broken"));
  REQUIRE_FALSE(malformed);
  CHECK(malformed.error().code == market_data::RecordedFixtureParseCode::InvalidQuantity);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reparse and normalize the baseline; neither output may depend on the failed temporary state.
  const auto after = market_data::parse_recorded_fixture(recorded_frame(valid));
  REQUIRE(after);
  CHECK(after.value() == before.value());

  auto first_message = before.value();
  auto second_message = after.value();
  const auto first_normalized =
      market_data::normalize_recorded_fixture(std::move(first_message), configured_policy());
  const auto second_normalized =
      market_data::normalize_recorded_fixture(std::move(second_message), configured_policy());
  REQUIRE(first_normalized);
  REQUIRE(second_normalized);
  CHECK(first_normalized.value() == second_normalized.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
