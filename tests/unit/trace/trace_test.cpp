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

// Invalid typed literals or revisions are fixture defects and therefore fail immediately.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in trace test"};
  }
  return std::move(result).value();
}

template <typename Revision> [[nodiscard]] Revision revision(std::uint64_t value) {
  auto result = Revision::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid revision in trace test"};
  }
  return std::move(result).value();
}

// Sequential fingerprint bytes make field order and byte preservation visible in the golden vector.
[[nodiscard]] trace::TraceProvenance
provenance(std::optional<model::InstrumentMetadataRevision> metadata_revision = std::nullopt) {
  model::Sha256Digest fingerprint{};
  for (std::size_t index = 0U; index < fingerprint.size(); ++index) {
    fingerprint[index] = std::byte{static_cast<unsigned char>(index)};
  }
  return trace::TraceProvenance{
      configuration::ConfigurationFingerprint{fingerprint},
      revision<model::ConfigurationRevision>(1U),
      revision<model::OrganizationRevision>(2U),
      revision<model::StrategyConfigurationRevision>(3U),
      revision<model::SubscriptionRevision>(4U),
      revision<model::RouteRevision>(5U),
      metadata_revision,
  };
}

// Subject builders mirror the exact accepted schema for each subject-bearing M1 event.
[[nodiscard]] trace::TraceSubjects bot_subjects() {
  trace::TraceSubjects subjects;
  subjects.firm_id = id<model::FirmId>("firm.aegis-lab");
  subjects.desk_id = id<model::DeskId>("desk.digital-assets");
  subjects.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.strategy_id = id<model::StrategyId>("strategy.deterministic-reference");
  return subjects;
}

[[nodiscard]] trace::TraceSubjects subscription_subjects() {
  trace::TraceSubjects subjects;
  subjects.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.venue_id = id<model::VenueId>("deribit");
  subjects.instrument_id = id<model::InstrumentId>("BTC-USD-PERPETUAL");
  subjects.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  return subjects;
}

[[nodiscard]] trace::TraceSubjects route_subjects() {
  trace::TraceSubjects subjects;
  subjects.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  subjects.venue_id = id<model::VenueId>("deribit");
  subjects.logical_account_id = id<model::LogicalAccountId>("account.deribit-testnet-aegis");
  subjects.instrument_id = id<model::InstrumentId>("BTC-USD-PERPETUAL");
  subjects.route_id = id<model::RouteId>("route.deribit-testnet-btc-perpetual");
  return subjects;
}

// Payload and hexadecimal helpers keep schema-byte assertions compact without weakening exactness.
[[nodiscard]] trace::TracePayload one_byte_payload(std::byte value) {
  const std::array bytes{value};
  auto result = trace::TracePayload::copy_from(bytes);
  if (!result) {
    throw std::logic_error{"invalid one-byte trace payload"};
  }
  return std::move(result).value();
}

[[nodiscard]] std::string hexadecimal(std::span<const std::byte> bytes) {
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

[[nodiscard]] std::string digest_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// Boundary and event-schema tests prove malformed input is rejected before a record becomes
// visible.
TEST_CASE("trace payloads have an exact fixed upper bound", "[trace][unit]") {
  const std::array<std::byte, trace::max_trace_payload_bytes> maximum{};
  const auto accepted = trace::TracePayload::copy_from(maximum);
  REQUIRE(accepted);
  CHECK(accepted.value().bytes().size() == trace::max_trace_payload_bytes);

  const std::array<std::byte, trace::max_trace_payload_bytes + 1U> excessive{};
  const auto rejected = trace::TracePayload::copy_from(excessive);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() ==
        model::DomainError::at_field(model::DomainErrorCode::EncodingOverflow, "trace.payload"));
}

TEST_CASE("each M1 event kind enforces its exact subject and payload schema", "[trace][unit]") {
  trace::TraceSink sink{8U};

  auto unexpected_subject = trace::TraceSubjects{};
  unexpected_subject.bot_id = id<model::BotId>("bot.unexpected");
  const auto malformed_configuration = sink.append(trace::TraceEventKind::ConfigurationSealed,
                                                   std::move(unexpected_subject), provenance());
  REQUIRE_FALSE(malformed_configuration);
  CHECK(malformed_configuration.error().context.field == "trace.configuration_sealed");

  const auto unknown_kind = sink.append(static_cast<trace::TraceEventKind>(999U), {}, provenance());
  REQUIRE_FALSE(unknown_kind);
  CHECK(unknown_kind.error().context.field == "trace.kind");

  const auto subscription_without_metadata =
      sink.append(trace::TraceEventKind::SubscriptionConfigured, subscription_subjects(),
                  provenance(), one_byte_payload(std::byte{1U}));
  REQUIRE_FALSE(subscription_without_metadata);
  CHECK(subscription_without_metadata.error().context.field == "trace.subscription_configured");

  const auto route_with_invalid_state = sink.append(
      trace::TraceEventKind::RouteConfigured, route_subjects(),
      provenance(model::InstrumentMetadataRevision::initial()), one_byte_payload(std::byte{2U}));
  REQUIRE_FALSE(route_with_invalid_state);
  CHECK(route_with_invalid_state.error().context.field == "trace.route_configured");
  CHECK(sink.size() == 0U);

  REQUIRE(sink.append(trace::TraceEventKind::BotAttributed, bot_subjects(), provenance()));
  CHECK(sink.records().front().ordinal().value() == 1U);
}

// Capacity failures must preserve the accepted prefix, its ordinals, bytes, and digest atomically.
TEST_CASE("capacity failure cannot drop records or consume an ordinal", "[trace][unit]") {
  trace::TraceSink zero_capacity{0U};
  const auto empty_bytes = zero_capacity.canonical_bytes();
  const auto empty_digest = zero_capacity.digest();
  REQUIRE(empty_bytes);
  REQUIRE(empty_digest);
  const auto zero_rejected =
      zero_capacity.append(trace::TraceEventKind::ConfigurationSealed, {}, provenance());
  REQUIRE_FALSE(zero_rejected);
  CHECK(zero_rejected.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded, "trace.records",
                                     0U));
  CHECK(zero_capacity.size() == 0U);
  REQUIRE(zero_capacity.canonical_bytes());
  REQUIRE(zero_capacity.digest());
  CHECK(zero_capacity.canonical_bytes().value() == empty_bytes.value());
  CHECK(zero_capacity.digest().value() == empty_digest.value());

  trace::TraceSink sink{1U};
  REQUIRE(sink.append(trace::TraceEventKind::ConfigurationSealed, {}, provenance()));
  const auto bytes_before = sink.canonical_bytes();
  const auto digest_before = sink.digest();
  REQUIRE(bytes_before);
  REQUIRE(digest_before);

  const auto rejected =
      sink.append(trace::TraceEventKind::BotAttributed, bot_subjects(), provenance());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded, "trace.records",
                                     1U));
  CHECK(sink.size() == 1U);
  CHECK(sink.records().front().ordinal().value() == 1U);
  REQUIRE(sink.canonical_bytes());
  REQUIRE(sink.digest());
  CHECK(sink.canonical_bytes().value() == bytes_before.value());
  CHECK(sink.digest().value() == digest_before.value());
}

// The golden record locks field tags, lengths, optional markers, endian order, and final SHA-256.
TEST_CASE("one canonical trace record has stable tags lengths and SHA-256", "[trace][golden]") {
  trace::TraceSink sink{1U};
  REQUIRE(sink.append(trace::TraceEventKind::ConfigurationSealed, {}, provenance()));

  const auto bytes = sink.canonical_bytes();
  const auto digest = sink.digest();
  REQUIRE(bytes);
  REQUIRE(digest);

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
  CHECK(hexadecimal(bytes.value()) == expected_bytes);
  CHECK(digest_hex(digest.value()) == expected_digest);
}

} // namespace
