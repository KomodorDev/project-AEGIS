// Purpose: prove the accepted M1 reference configuration produces one exact bounded trace and that
// capacity failure preserves its accepted prefix.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/trace/trace.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Assigned channel and route-state values are encoded as one-byte event payloads in schema 1.
[[nodiscard]] trace::TracePayload one_byte_payload(std::uint8_t value) {
  const std::array bytes{std::byte{value}};
  auto result = trace::TracePayload::copy_from(bytes);
  if (!result) {
    throw std::logic_error{"invalid M1 reference trace payload"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Replay the accepted rulebook in causal order: seal, derived attribution, observation grant, then
// execution grant. Any append failure is a broken deterministic fixture, not an expected branch.
void append_reference_records(trace::TraceSink& sink,
                              const configuration::StartupConfiguration& configuration) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve every immutable configuration projection needed by the causal trace sequence.
  const auto& attribution = configuration.organization().bot_attributions().front();
  const auto& subscription = configuration.subscriptions().subscriptions().front();
  const auto& route = configuration.routes().routes().front();

  // ++++++++++++++++++++++++++++++++++++++++
  // Both instrument-bearing events must cite the revision sealed into configuration provenance.
  const auto* const metadata_revision =
      configuration.provenance().find_instrument_metadata_revision(subscription.venue_id,
                                                                   subscription.instrument_id);
  if (metadata_revision == nullptr) {
    throw std::logic_error{"reference metadata revision is absent"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The seal is the subject-free root record and therefore carries no instrument revision.
  auto appended = sink.append(trace::TraceEventKind::ConfigurationSealed, {},
                              trace::TraceProvenance::from(configuration.provenance()));
  if (!appended) {
    throw std::logic_error{"failed to append configuration trace record"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Attribution records copy the complete immutable Firm -> Desk -> Bot -> Strategy chain.
  trace::TraceSubjects bot_subjects;
  bot_subjects.firm_id = attribution.firm_id;
  bot_subjects.desk_id = attribution.desk_id;
  bot_subjects.bot_id = attribution.bot_id;
  bot_subjects.strategy_id = attribution.strategy_id;
  appended = sink.append(trace::TraceEventKind::BotAttributed, std::move(bot_subjects),
                         trace::TraceProvenance::from(configuration.provenance()));
  if (!appended) {
    throw std::logic_error{"failed to append bot-attribution trace record"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Subscription evidence couples the explicit data grant to its metadata and channel value.
  trace::TraceSubjects subscription_subjects;
  subscription_subjects.bot_id = subscription.bot_id;
  subscription_subjects.venue_id = subscription.venue_id;
  subscription_subjects.instrument_id = subscription.instrument_id;
  subscription_subjects.subscription_id = subscription.id;
  appended =
      sink.append(trace::TraceEventKind::SubscriptionConfigured, std::move(subscription_subjects),
                  trace::TraceProvenance::from(configuration.provenance(), *metadata_revision),
                  one_byte_payload(static_cast<std::uint8_t>(subscription.channel)));
  if (!appended) {
    throw std::logic_error{"failed to append subscription trace record"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Route evidence adds logical-account authority and preserves the configured disabled state.
  trace::TraceSubjects route_subjects;
  route_subjects.bot_id = route.bot_id;
  route_subjects.venue_id = route.venue_id;
  route_subjects.logical_account_id = route.logical_account_id;
  route_subjects.instrument_id = route.instrument_id;
  route_subjects.route_id = route.id;
  appended =
      sink.append(trace::TraceEventKind::RouteConfigured, std::move(route_subjects),
                  trace::TraceProvenance::from(configuration.provenance(), *metadata_revision),
                  one_byte_payload(static_cast<std::uint8_t>(route.state)));
  if (!appended) {
    throw std::logic_error{"failed to append route trace record"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Render a fixed digest as an owning string for exact scenario assertions.
[[nodiscard]] std::string digest_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Render canonical stream bytes as lowercase hexadecimal without changing their order.
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

// --------------------------------------------------------
// Exercise the canonical collection-order path independently of the ordinary reference builder.
[[nodiscard]] configuration::StartupConfigurationParams reordered_reference_params() {
  auto params = test_support::reference_configuration_params();

  // ++++++++++++++++++++++++++++++++++++++++
  // The accepted reference has one value per collection today. Deliberately take the independent
  // reorder path anyway so this scenario starts exercising canonical collection order as soon as
  // the fixture grows, without changing the exact reference scenario now.
  std::reverse(params.firms.begin(), params.firms.end());
  std::reverse(params.desks.begin(), params.desks.end());
  std::reverse(params.bots.begin(), params.bots.end());
  std::reverse(params.strategy_settings.begin(), params.strategy_settings.end());
  std::reverse(params.venues.begin(), params.venues.end());
  std::reverse(params.logical_accounts.begin(), params.logical_accounts.end());
  std::reverse(params.instrument_metadata.begin(), params.instrument_metadata.end());
  std::reverse(params.subscriptions.begin(), params.subscriptions.end());
  std::reverse(params.routes.begin(), params.routes.end());
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Equivalent authored configurations must produce identical records, provenance, bytes, and digest.
TEST_CASE("the exact reference configuration produces one deterministic bounded M1 trace",
          "[m1][deterministic_scenario][trace]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the singleton fixture through its ordinary and deliberate reorder code paths. Reversal is
  // a no-op today; equality keeps this scenario ready for future multi-value fixture growth.
  const auto first_configuration =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  const auto second_configuration =
      configuration::StartupConfiguration::create(reordered_reference_params());
  REQUIRE(first_configuration);
  REQUIRE(second_configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Replay both accepted snapshots into independently owned bounded sinks.
  trace::TraceSink first_trace{4U};
  trace::TraceSink second_trace{4U};
  append_reference_records(first_trace, first_configuration.value());
  append_reference_records(second_trace, second_configuration.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Record equality proves stable order, ordinal assignment, and copied section provenance.
  REQUIRE(first_trace.records().size() == 4U);
  REQUIRE(second_trace.records().size() == 4U);
  const auto& configuration_provenance = first_configuration.value().provenance();
  for (std::size_t index = 0U; index < first_trace.records().size(); ++index) {
    CHECK(first_trace.records()[index].ordinal().value() == index + 1U);
    CHECK(first_trace.records()[index] == second_trace.records()[index]);
    const auto& record_provenance = first_trace.records()[index].provenance();
    CHECK(record_provenance.configuration_fingerprint == first_configuration.value().fingerprint());
    CHECK(record_provenance.configuration_revision ==
          configuration_provenance.configuration_revision());
    CHECK(record_provenance.organization_revision ==
          configuration_provenance.organization_revision());
    CHECK(record_provenance.strategy_configuration_revision ==
          configuration_provenance.strategy_configuration_revision());
    CHECK(record_provenance.subscription_revision ==
          configuration_provenance.subscription_revision());
    CHECK(record_provenance.route_revision == configuration_provenance.route_revision());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Assigned event vocabulary, metadata applicability, and disabled route state remain explicit.
  CHECK(first_trace.records()[0U].kind() == trace::TraceEventKind::ConfigurationSealed);
  CHECK(first_trace.records()[1U].kind() == trace::TraceEventKind::BotAttributed);
  CHECK(first_trace.records()[2U].kind() == trace::TraceEventKind::SubscriptionConfigured);
  CHECK(first_trace.records()[3U].kind() == trace::TraceEventKind::RouteConfigured);
  CHECK_FALSE(first_trace.records()[0U].provenance().instrument_metadata_revision.has_value());
  CHECK_FALSE(first_trace.records()[1U].provenance().instrument_metadata_revision.has_value());
  CHECK(first_trace.records()[2U].provenance().instrument_metadata_revision ==
        model::InstrumentMetadataRevision::initial());
  CHECK(first_trace.records()[3U].provenance().instrument_metadata_revision ==
        model::InstrumentMetadataRevision::initial());
  CHECK_FALSE(first_trace.records()[2U].subjects().route_id.has_value());
  CHECK(first_trace.records()[3U].payload().bytes().front() == std::byte{0U});
  CHECK(first_configuration.value().routes().routes().front().is_enabled() == false);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical stream identity closes the replay proof across the complete record sequence.
  const auto first_bytes = first_trace.canonical_bytes();
  const auto second_bytes = second_trace.canonical_bytes();
  const auto first_digest = first_trace.digest();
  const auto second_digest = second_trace.digest();
  REQUIRE(first_bytes);
  REQUIRE(second_bytes);
  REQUIRE(first_digest);
  REQUIRE(second_digest);
  CHECK(first_bytes.value() == second_bytes.value());
  CHECK(first_digest.value() == second_digest.value());
  CHECK(first_bytes.value().size() == 1285U);
  REQUIRE(first_bytes.value().size() >= 14U);
  CHECK(hexadecimal(std::span<const std::byte>{first_bytes.value()}.first(14U)) ==
        "4145474953545253000100000004");

  // ++++++++++++++++++++++++++++++++++++++++
  // The compact fixed size, canonical stream prefix, and SHA-256 cover the full 1,285-byte stream
  // without embedding a second multi-kilobyte hexadecimal fixture beside the unit byte vector.
  CHECK(digest_hex(first_digest.value()) ==
        "242691fdfaa6377bf86b0bd3642ece7a8223e52936d3a67d7a2d5b5731033749");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A rejected fourth record must not alter the exact bytes or digest of the accepted three-record
// prefix.
TEST_CASE("reference trace capacity failure preserves every accepted prefix record",
          "[m1][deterministic_scenario][trace]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the complete four-record reference trace as the source of accepted records.
  const auto configured =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  REQUIRE(configured);

  trace::TraceSink complete{4U};
  append_reference_records(complete, configured.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the first three records into a sink whose capacity is now exactly full.
  trace::TraceSink bounded{3U};
  const auto& complete_records = complete.records();
  REQUIRE(bounded.append(complete_records[0U].kind(), complete_records[0U].subjects(),
                         complete_records[0U].provenance(), complete_records[0U].payload()));
  REQUIRE(bounded.append(complete_records[1U].kind(), complete_records[1U].subjects(),
                         complete_records[1U].provenance(), complete_records[1U].payload()));
  REQUIRE(bounded.append(complete_records[2U].kind(), complete_records[2U].subjects(),
                         complete_records[2U].provenance(), complete_records[2U].payload()));
  const auto prefix_bytes = bounded.canonical_bytes();
  const auto prefix_digest = bounded.digest();
  REQUIRE(prefix_bytes);
  REQUIRE(prefix_digest);

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject the fourth record without altering the accepted prefix or consuming state.
  const auto rejected =
      bounded.append(complete_records[3U].kind(), complete_records[3U].subjects(),
                     complete_records[3U].provenance(), complete_records[3U].payload());
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded, "trace.records",
                                     3U));
  CHECK(bounded.size() == 3U);
  REQUIRE(bounded.canonical_bytes());
  REQUIRE(bounded.digest());
  CHECK(bounded.canonical_bytes().value() == prefix_bytes.value());
  CHECK(bounded.digest().value() == prefix_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
