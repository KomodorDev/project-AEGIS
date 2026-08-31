// Purpose: prove M1 trace validation, capacity behavior, and canonical encoding boundaries.

#include "aegis/trace/trace.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid typed literals are fixture defects and therefore fail immediately.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse_identifier(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid revision literals use the same fail-fast fixture policy.
template <typename Revision> [[nodiscard]] Revision create_revision_or_throw(std::uint64_t value) {
  auto result = Revision::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid revision in trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Sequential fingerprint bytes make field order and byte preservation visible in the golden vector.
[[nodiscard]] trace::TraceProvenance create_provenance_or_throw(
    std::optional<model::InstrumentMetadataRevision> metadata_revision = std::nullopt) {
  model::Sha256Digest fingerprint{};
  for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
    fingerprint[index] = std::byte{static_cast<unsigned char>(index)};
  }
  return trace::TraceProvenance{
      configuration::ConfigurationFingerprint{fingerprint},
      create_revision_or_throw<model::ConfigurationRevision>(1U),
      create_revision_or_throw<model::OrganizationRevision>(2U),
      create_revision_or_throw<model::StrategyConfigurationRevision>(3U),
      create_revision_or_throw<model::SubscriptionRevision>(4U),
      create_revision_or_throw<model::RouteRevision>(5U),
      metadata_revision,
  };
}

// --------------------------------------------------------
// Build the complete subject schema required by a bot-attribution event.
[[nodiscard]] trace::TraceSubjects create_bot_subjects_or_throw() {
  trace::TraceSubjects subjects;
  subjects.firm_id = parse_identifier_or_throw<model::FirmId>("firm.aegis-lab");
  subjects.desk_id = parse_identifier_or_throw<model::DeskId>("desk.digital-assets");
  subjects.bot_id = parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.strategy_id =
      parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference");
  return subjects;
}

// --------------------------------------------------------
// Build the complete subject schema required by a subscription event.
[[nodiscard]] trace::TraceSubjects create_subscription_subjects_or_throw() {
  trace::TraceSubjects subjects;
  subjects.bot_id = parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.venue_id = parse_identifier_or_throw<model::VenueId>("deribit");
  subjects.instrument_id = parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");
  subjects.subscription_id =
      parse_identifier_or_throw<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  return subjects;
}

// --------------------------------------------------------
// Build the complete subject schema required by an execution-route event.
[[nodiscard]] trace::TraceSubjects create_route_subjects_or_throw() {
  trace::TraceSubjects subjects;
  subjects.bot_id = parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.venue_id = parse_identifier_or_throw<model::VenueId>("deribit");
  subjects.logical_account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis");
  subjects.instrument_id = parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");
  subjects.route_id =
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual");
  return subjects;
}

// --------------------------------------------------------
// Copy one byte through the bounded production payload factory for compact schema tests.
[[nodiscard]] trace::TracePayload create_one_byte_payload_or_throw(std::byte value) {
  const std::array bytes{value};
  auto result = trace::TracePayload::copy_from(bytes);
  if (!result) {
    throw std::logic_error{"invalid one-byte trace payload"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Render canonical record bytes as lowercase hexadecimal without changing their ordering.
[[nodiscard]] std::string bytes_to_hexadecimal(std::span<const std::byte> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const std::byte value : bytes) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(digits[(byte >> 4U) & 0xfU]);
    result.push_back(digits[byte & 0xfU]);
  }
  return result;
}

// --------------------------------------------------------
// Render a fixed SHA-256 digest as an owning string for golden-vector comparison.
[[nodiscard]] std::string digest_to_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex_from_digest(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Boundary and event-schema tests prove malformed input is rejected before a record becomes
// visible.
TEST_CASE("trace payloads have an exact fixed upper bound", "[trace][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Accept a payload exactly at the fixed inline-storage boundary.
  const std::array<std::byte, trace::max_trace_payload_bytes> maximum{};
  const auto accepted = trace::TracePayload::copy_from(maximum);
  REQUIRE(accepted);
  CHECK(accepted.value().bytes().size() == trace::max_trace_payload_bytes);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject the first excessive size with a stable encoding-field error.
  const std::array<std::byte, trace::max_trace_payload_bytes + 1U> excessive{};
  const auto rejected = trace::TracePayload::copy_from(excessive);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == model::DomainError::create_at_field(
                                model::DomainErrorCode::EncodingOverflow, "trace.payload"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every event kind enforces its exact subjects, provenance, and payload contract before append.
TEST_CASE("each M1 event kind enforces its exact subject and payload schema", "[trace][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject extra subjects on a configuration-sealed event.
  trace::TraceSink sink{8U};

  auto unexpected_subject = trace::TraceSubjects{};
  unexpected_subject.bot_id = parse_identifier_or_throw<model::BotId>("bot.unexpected");
  const auto malformed_configuration =
      sink.append_trace_record(trace::TraceEventKind::ConfigurationSealed,
                               std::move(unexpected_subject), create_provenance_or_throw());
  REQUIRE_FALSE(malformed_configuration);
  CHECK(malformed_configuration.error().context.field == "trace.configuration_sealed");

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject an event kind outside the assigned schema vocabulary.
  const auto unknown_kind = sink.append_trace_record(static_cast<trace::TraceEventKind>(999U), {},
                                                     create_provenance_or_throw());
  REQUIRE_FALSE(unknown_kind);
  CHECK(unknown_kind.error().context.field == "trace.kind");

  // ++++++++++++++++++++++++++++++++++++++++
  // Subscription evidence requires the metadata revision that defined its instrument semantics.
  const auto subscription_without_metadata = sink.append_trace_record(
      trace::TraceEventKind::SubscriptionConfigured, create_subscription_subjects_or_throw(),
      create_provenance_or_throw(), create_one_byte_payload_or_throw(std::byte{1U}));
  REQUIRE_FALSE(subscription_without_metadata);
  CHECK(subscription_without_metadata.error().context.field == "trace.subscription_configured");

  // ++++++++++++++++++++++++++++++++++++++++
  // Route payload state is checked before any malformed record can become visible.
  const auto route_with_invalid_state = sink.append_trace_record(
      trace::TraceEventKind::RouteConfigured, create_route_subjects_or_throw(),
      create_provenance_or_throw(model::InstrumentMetadataRevision::create_initial()),
      create_one_byte_payload_or_throw(std::byte{2U}));
  REQUIRE_FALSE(route_with_invalid_state);
  CHECK(route_with_invalid_state.error().context.field == "trace.route_configured");
  CHECK(sink.record_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A valid first append starts the sink-owned ordinal sequence at one.
  REQUIRE(sink.append_trace_record(trace::TraceEventKind::BotAttributed,
                                   create_bot_subjects_or_throw(), create_provenance_or_throw()));
  CHECK(sink.records().front().ordinal().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Capacity failures must preserve the accepted prefix, its ordinals, bytes, and digest atomically.
TEST_CASE("capacity failure cannot drop records or consume an ordinal", "[trace][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A zero-capacity sink rejects its first append without changing empty-stream evidence.
  trace::TraceSink zero_capacity{0U};
  const auto empty_bytes = zero_capacity.encode_canonical_bytes();
  const auto empty_digest = zero_capacity.derive_digest();
  REQUIRE(empty_bytes);
  REQUIRE(empty_digest);
  const auto zero_rejected = zero_capacity.append_trace_record(
      trace::TraceEventKind::ConfigurationSealed, {}, create_provenance_or_throw());
  REQUIRE_FALSE(zero_rejected);
  CHECK(zero_rejected.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::TraceCapacityExceeded,
                                            "trace.records", 0U));
  CHECK(zero_capacity.record_count() == 0U);
  REQUIRE(zero_capacity.encode_canonical_bytes());
  REQUIRE(zero_capacity.derive_digest());
  CHECK(zero_capacity.encode_canonical_bytes().value() == empty_bytes.value());
  CHECK(zero_capacity.derive_digest().value() == empty_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A full sink preserves its one-record prefix and next ordinal after a rejected append.
  trace::TraceSink sink{1U};
  REQUIRE(sink.append_trace_record(trace::TraceEventKind::ConfigurationSealed, {},
                                   create_provenance_or_throw()));
  const auto bytes_before = sink.encode_canonical_bytes();
  const auto digest_before = sink.derive_digest();
  REQUIRE(bytes_before);
  REQUIRE(digest_before);

  const auto rejected =
      sink.append_trace_record(trace::TraceEventKind::BotAttributed, create_bot_subjects_or_throw(),
                               create_provenance_or_throw());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::TraceCapacityExceeded,
                                            "trace.records", 1U));
  CHECK(sink.record_count() == 1U);
  CHECK(sink.records().front().ordinal().value() == 1U);
  REQUIRE(sink.encode_canonical_bytes());
  REQUIRE(sink.derive_digest());
  CHECK(sink.encode_canonical_bytes().value() == bytes_before.value());
  CHECK(sink.derive_digest().value() == digest_before.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The golden record locks field tags, lengths, optional markers, endian order, and final SHA-256.
TEST_CASE("one canonical trace record has stable tags lengths and SHA-256", "[trace][golden]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Create and encode the smallest complete trace stream.
  trace::TraceSink sink{1U};
  REQUIRE(sink.append_trace_record(trace::TraceEventKind::ConfigurationSealed, {},
                                   create_provenance_or_throw()));

  const auto bytes = sink.encode_canonical_bytes();
  const auto digest = sink.derive_digest();
  REQUIRE(bytes);
  REQUIRE(digest);

  // ++++++++++++++++++++++++++++++++++++++++
  // A complete one-record stream catches layout drift more precisely than field-by-field
  // assertions.
  const std::string expected_bytes =
      "4145474953545253000100000001000000d8414547495354524300010001000000080000000000"
      "000001000200000002000100100000000100001100000001000012000000010000130000000100"
      "001400000001000015000000010000160000000100001700000001000018000000010000200000"
      "0020000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f0021000000"
      "080000000000000001002200000008000000000000000200230000000800000000000000030024"
      "000000080000000000000004002500000008000000000000000500260000000100003000000000";
  const std::string expected_digest =
      "2abefab5ebcd43b63cc4401a4c7bd916af1da855b5ceff0d601fc3a6df50b4ba";
  CHECK(bytes_to_hexadecimal(bytes.value()) == expected_bytes);
  CHECK(digest_to_hex(digest.value()) == expected_digest);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
