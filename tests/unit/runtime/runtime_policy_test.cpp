// Purpose: prove M2 runtime-policy bounds, source coherence, canonical identity, and
// credential-free publication against sealed M1 startup configuration.

#include "aegis/runtime/runtime_policy.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
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

// ########################################################################
// Interesting syntax: requires-expression probes turn accidental credential or endpoint additions
// to the public policy vocabulary into compile-time regression failures.
template <typename Value>
concept HasCredentials = requires(Value value) { value.credentials; };

template <typename Value>
concept HasApiKey = requires(Value value) { value.api_key; };

template <typename Value>
concept HasSecret = requires(Value value) { value.secret; };

template <typename Value>
concept HasEndpoint = requires(Value value) { value.endpoint; };

template <typename Value>
concept HasSocket = requires(Value value) { value.socket; };

static_assert(!HasCredentials<runtime::RuntimePolicyParams>);
static_assert(!HasApiKey<runtime::RuntimePolicyParams>);
static_assert(!HasSecret<runtime::RuntimePolicyParams>);
static_assert(!HasEndpoint<runtime::RuntimePolicyParams>);
static_assert(!HasSocket<runtime::RuntimePolicyParams>);
static_assert(!HasCredentials<runtime::RuntimeSourceDefinition>);
static_assert(!HasApiKey<runtime::RuntimeSourceDefinition>);
static_assert(!HasSecret<runtime::RuntimeSourceDefinition>);
static_assert(!HasEndpoint<runtime::RuntimeSourceDefinition>);
static_assert(!HasSocket<runtime::RuntimeSourceDefinition>);

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture-authoring defects, not policy behaviors under test.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse_identifier(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in runtime-policy test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Seal a configuration fixture or fail immediately when test setup violates an M1 contract.
[[nodiscard]] configuration::StartupConfiguration
create_configuration_from_params_or_throw(configuration::StartupConfigurationParams params) {
  auto result =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!result) {
    throw std::logic_error{"invalid startup configuration in runtime-policy test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Reference limits exercise every canonical field while remaining small enough for unit tests.
[[nodiscard]] runtime::RuntimePolicyLimits create_reference_limits() {
  return runtime::RuntimePolicyLimits{
      8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 128U, 256U, 32U, 100'000U,
  };
}

// --------------------------------------------------------
// Build the canonical Deribit BTC source from exact identifiers in the M1 reference fixture.
[[nodiscard]] runtime::RuntimeSourceDefinition
create_reference_source_or_throw(std::string_view source_id = "source.deribit-btc-perpetual") {
  return runtime::RuntimeSourceDefinition{
      parse_identifier_or_throw<model::MarketSourceId>(source_id),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial(),
  };
}

// --------------------------------------------------------
// Couple reference limits and one source into the ordinary single-source policy input.
[[nodiscard]] runtime::RuntimePolicyParams create_reference_policy_params_or_throw() {
  return runtime::RuntimePolicyParams{create_reference_limits(),
                                      {create_reference_source_or_throw()}};
}

// --------------------------------------------------------
// Extend the sealed configuration authoring input with an independent venue/instrument path.
[[nodiscard]] configuration::StartupConfigurationParams
create_two_source_configuration_params_or_throw() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reuse validated economic metadata while assigning a distinct venue and instrument identity.
  auto params = test_support::create_reference_configuration_params_or_throw();
  const auto venue_id = parse_identifier_or_throw<model::VenueId>("kraken");
  const auto instrument_id = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  auto metadata = params.instrument_metadata.front();
  metadata.venue_id = venue_id;
  metadata.instrument_id = instrument_id;
  metadata.venue_instrument_id =
      parse_identifier_or_throw<model::VenueInstrumentId>("ETH-PERPETUAL");

  // ++++++++++++++++++++++++++++++++++++++++
  // Add only public venue, metadata, and observation records; no account or route is required.
  params.venues.push_back(
      configuration::VenueDefinition{venue_id, configuration::VenueEnvironment::Testnet});
  params.instrument_metadata.push_back(std::move(metadata));
  params.subscriptions.push_back(market_data::Subscription{
      parse_identifier_or_throw<model::SubscriptionId>("subscription.kraken-eth-perpetual-book"),
      params.bots.front().id, venue_id, instrument_id,
      market_data::SubscriptionChannel::OrderBook});
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Build both source definitions in intentionally noncanonical caller order.
[[nodiscard]] runtime::RuntimePolicyParams create_two_source_policy_params_or_throw() {
  auto params = create_reference_policy_params_or_throw();
  params.sources.front().source_id =
      parse_identifier_or_throw<model::MarketSourceId>("source.z-deribit-btc");
  params.sources.push_back(runtime::RuntimeSourceDefinition{
      parse_identifier_or_throw<model::MarketSourceId>("source.a-kraken-eth"),
      parse_identifier_or_throw<model::VenueId>("kraken"),
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("ETH-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial(),
  });
  return params;
}

// --------------------------------------------------------
// Add a second bot grant for the same book; two callbacks per grant make recovery fanout four.
[[nodiscard]] configuration::StartupConfigurationParams
create_two_callback_configuration_params_or_throw() {
  auto params = test_support::create_two_firm_configuration_params_or_throw();
  params.subscriptions.push_back(market_data::Subscription{
      parse_identifier_or_throw<model::SubscriptionId>("subscription.subsidiary-deribit-btc-book"),
      parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      market_data::SubscriptionChannel::OrderBook});
  return params;
}

// --------------------------------------------------------

// --------------------------------------------------------
// An accepted policy must bind every limit and source ordinal to the sealed startup identity.
TEST_CASE("runtime policy seals positive bounds and configured source provenance",
          "[runtime][policy][m2]") {
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());
  const auto result = runtime::RuntimePolicy::create_runtime_policy(
      configuration, create_reference_policy_params_or_throw());

  REQUIRE(result);
  const auto& policy = result.value();
  CHECK(policy.configuration_fingerprint() == configuration.fingerprint());
  CHECK(policy.limits() == create_reference_limits());
  CHECK(policy.source_capacity() == 1U);
  REQUIRE(policy.sources().size() == 1U);
  CHECK(policy.sources().front().ordinal() == model::MarketSourceOrdinal::create_initial());
  CHECK(policy.sources().front().matching_subscription_count() == 1U);
  CHECK(policy.sources().front().definition() == create_reference_source_or_throw());

  // ++++++++++++++++++++++++++++++++++++++++
  // Both nominal ID and complete order-book key resolve the same immutable published record.
  const auto* const by_id = policy.find_source(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"));
  REQUIRE(by_id != nullptr);
  const auto key =
      runtime::RuntimeSourceKey{parse_identifier_or_throw<model::VenueId>("deribit"),
                                parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
                                market_data::SubscriptionChannel::OrderBook};
  CHECK(policy.find_source(key) == by_id);
  CHECK(policy.find_source(parse_identifier_or_throw<model::MarketSourceId>("source.missing")) ==
        nullptr);
  CHECK(policy.find_source(runtime::RuntimeSourceKey{
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL"),
            market_data::SubscriptionChannel::OrderBook}) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical bytes expose the independent AEGISRTP magic and big-endian schema version.
  REQUIRE(policy.canonical_bytes().size() > 10U);
  const std::string magic{reinterpret_cast<const char*>(policy.canonical_bytes().data()), 8U};
  CHECK(magic == "AEGISRTP");
  CHECK(policy.canonical_bytes()[8U] == std::byte{0U});
  CHECK(policy.canonical_bytes()[9U] == std::byte{1U});
  CHECK(policy.fingerprint().to_hex().size() == model::sha256_hex_size);

  // ++++++++++++++++++++++++++++++++++++++++
  // An otherwise identical policy under a different sealed organization must have a new identity.
  const auto peer_configuration = create_configuration_from_params_or_throw(
      test_support::create_two_firm_configuration_params_or_throw());
  const auto peer_policy = runtime::RuntimePolicy::create_runtime_policy(
      peer_configuration, create_reference_policy_params_or_throw());
  REQUIRE(peer_policy);
  CHECK(peer_policy.value().fingerprint() != policy.fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reordering author input must preserve canonical source order, ordinals, bytes, and golden digest.
TEST_CASE("runtime policy source order is canonical and fingerprinted stably",
          "[runtime][policy][canonical][m2]") {
  const auto configuration =
      create_configuration_from_params_or_throw(create_two_source_configuration_params_or_throw());
  auto first_params = create_two_source_policy_params_or_throw();
  auto second_params = first_params;
  std::reverse(second_params.sources.begin(), second_params.sources.end());

  const auto first =
      runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(first_params));
  const auto second =
      runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(second_params));

  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value() == second.value());
  REQUIRE(first.value().sources().size() == 2U);
  CHECK(first.value().sources()[0U].definition().source_id ==
        parse_identifier_or_throw<model::MarketSourceId>("source.a-kraken-eth"));
  CHECK(first.value().sources()[0U].ordinal() == model::MarketSourceOrdinal::create_initial());
  CHECK(first.value().sources()[1U].definition().source_id ==
        parse_identifier_or_throw<model::MarketSourceId>("source.z-deribit-btc"));
  CHECK(first.value().sources()[1U].ordinal() ==
        model::MarketSourceOrdinal::from_value(2U).value());
  CHECK(first.value().fingerprint().to_hex() ==
        "8e82863c6ec43e1393bef732c401706de2fb394ef3803104d68d8e649b9b876a");
}

// --------------------------------------------------------
// Every authored limit has its own canonical field, so an isolated valid change must alter both
// the bytes and their digest without relying on another policy difference.
TEST_CASE("every runtime policy limit participates independently in canonical identity",
          "[runtime][policy][canonical][sensitivity][m2]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Hold the sealed configuration and source registry constant across the complete limit table.
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());
  const auto baseline = runtime::RuntimePolicy::create_runtime_policy(
      configuration, create_reference_policy_params_or_throw());
  REQUIRE(baseline);

  const auto check_changed_limit = [&](runtime::RuntimePolicyLimits limits) {
    const auto changed = runtime::RuntimePolicy::create_runtime_policy(
        configuration, runtime::RuntimePolicyParams{limits, {create_reference_source_or_throw()}});
    REQUIRE(changed);
    CHECK(changed.value().canonical_bytes() != baseline.value().canonical_bytes());
    CHECK(changed.value().fingerprint() != baseline.value().fingerprint());
  };

  // ++++++++++++++++++++++++++++++++++++++++
  // Each section starts from the baseline and mutates exactly one valid scheduling or storage
  // bound.
  SECTION("ingress capacity") {
    auto limits = create_reference_limits();
    limits.ingress_capacity += 1U;
    check_changed_limit(limits);
  }
  SECTION("maximum frame bytes") {
    auto limits = create_reference_limits();
    limits.maximum_frame_bytes -= 1U;
    check_changed_limit(limits);
  }
  SECTION("maximum changes per update") {
    auto limits = create_reference_limits();
    limits.maximum_changes_per_update -= 1U;
    check_changed_limit(limits);
  }
  SECTION("retained book depth") {
    auto limits = create_reference_limits();
    limits.retained_book_depth += 1U;
    check_changed_limit(limits);
  }
  SECTION("stale threshold") {
    auto limits = create_reference_limits();
    limits.stale_threshold_nanoseconds += 1U;
    check_changed_limit(limits);
  }
  SECTION("maximum callbacks per turn") {
    auto limits = create_reference_limits();
    limits.maximum_callbacks_per_turn += 1U;
    check_changed_limit(limits);
  }
  SECTION("diagnostic capacity") {
    auto limits = create_reference_limits();
    limits.diagnostic_capacity += 1U;
    check_changed_limit(limits);
  }
  SECTION("runtime trace capacity") {
    auto limits = create_reference_limits();
    limits.runtime_trace_capacity += 1U;
    check_changed_limit(limits);
  }
  SECTION("maximum drive turns") {
    auto limits = create_reference_limits();
    limits.maximum_drive_turns += 1U;
    check_changed_limit(limits);
  }
  SECTION("callback budget") {
    auto limits = create_reference_limits();
    limits.callback_budget_nanoseconds += 1U;
    check_changed_limit(limits);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Every source-definition member must alter the source record. Configuration-bound identities are
// changed in lockstep with their sealed catalog so each variant remains an accepted policy.
TEST_CASE("every runtime source member participates in canonical identity",
          "[runtime][policy][canonical][sensitivity][m2]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one source baseline and the fixed end of the leading configuration-fingerprint TLV.
  const auto baseline_configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());
  const auto baseline = runtime::RuntimePolicy::create_runtime_policy(
      baseline_configuration, create_reference_policy_params_or_throw());
  REQUIRE(baseline);
  constexpr std::size_t configuration_field_end = 8U + sizeof(std::uint16_t) +
                                                  sizeof(std::uint16_t) + sizeof(std::uint32_t) +
                                                  model::sha256_digest_size;
  REQUIRE(baseline.value().canonical_bytes().size() > configuration_field_end);

  const auto check_changed_source =
      [&](configuration::StartupConfigurationParams configuration_params,
          const runtime::RuntimeSourceDefinition& source) {
        const auto changed_configuration =
            create_configuration_from_params_or_throw(std::move(configuration_params));
        const auto changed = runtime::RuntimePolicy::create_runtime_policy(
            changed_configuration,
            runtime::RuntimePolicyParams{create_reference_limits(), {source}});
        REQUIRE(changed);
        REQUIRE(changed.value().sources().size() == 1U);
        CHECK(changed.value().sources().front().definition() == source);
        CHECK(changed.value().sources().front().definition() != create_reference_source_or_throw());
        CHECK(changed.value().canonical_bytes() != baseline.value().canonical_bytes());
        CHECK(changed.value().fingerprint() != baseline.value().fingerprint());

        // The suffix excludes the potentially changed startup fingerprint, isolating sensitivity
        // to the source record while all runtime limits remain identical.
        const auto& baseline_bytes = baseline.value().canonical_bytes();
        const auto& changed_bytes = changed.value().canonical_bytes();
        REQUIRE(changed_bytes.size() > configuration_field_end);
        CHECK_FALSE(
            std::equal(baseline_bytes.begin() + configuration_field_end, baseline_bytes.end(),
                       changed_bytes.begin() + configuration_field_end, changed_bytes.end()));
      };

  // ++++++++++++++++++++++++++++++++++++++++
  // The nominal source ID is policy-local and does not require a startup-catalog mutation.
  SECTION("source ID") {
    auto source = create_reference_source_or_throw();
    source.source_id =
        parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual-secondary");
    check_changed_source(test_support::create_reference_configuration_params_or_throw(), source);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue identity must stay coherent across metadata, observation, account, and route catalogs.
  SECTION("venue ID") {
    auto configuration_params = test_support::create_reference_configuration_params_or_throw();
    const auto venue_id = parse_identifier_or_throw<model::VenueId>("kraken");
    configuration_params.venues.front().id = venue_id;
    configuration_params.logical_accounts.front().venue_id = venue_id;
    configuration_params.instrument_metadata.front().venue_id = venue_id;
    configuration_params.subscriptions.front().venue_id = venue_id;
    configuration_params.routes.front().venue_id = venue_id;
    auto source = create_reference_source_or_throw();
    source.venue_id = venue_id;
    check_changed_source(std::move(configuration_params), source);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Normalized instrument identity must stay coherent across metadata, observation, and routes.
  SECTION("instrument ID") {
    auto configuration_params = test_support::create_reference_configuration_params_or_throw();
    const auto instrument_id = parse_identifier_or_throw<model::InstrumentId>("XBT-USD-PERPETUAL");
    configuration_params.instrument_metadata.front().instrument_id = instrument_id;
    configuration_params.subscriptions.front().instrument_id = instrument_id;
    configuration_params.routes.front().instrument_id = instrument_id;
    auto source = create_reference_source_or_throw();
    source.instrument_id = instrument_id;
    check_changed_source(std::move(configuration_params), source);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The venue-native instrument spelling is validated against the exact metadata snapshot.
  SECTION("venue instrument ID") {
    auto configuration_params = test_support::create_reference_configuration_params_or_throw();
    const auto venue_instrument_id =
        parse_identifier_or_throw<model::VenueInstrumentId>("XBT-PERPETUAL");
    configuration_params.instrument_metadata.front().venue_instrument_id = venue_instrument_id;
    auto source = create_reference_source_or_throw();
    source.venue_instrument_id = venue_instrument_id;
    check_changed_source(std::move(configuration_params), source);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Metadata revision changes are accepted only when source and catalog advance together.
  SECTION("metadata revision") {
    auto configuration_params = test_support::create_reference_configuration_params_or_throw();
    const auto revision = model::InstrumentMetadataRevision::from_value(2U).value();
    configuration_params.instrument_metadata.front().revision = revision;
    auto source = create_reference_source_or_throw();
    source.metadata_revision = revision;
    check_changed_source(std::move(configuration_params), source);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Source IDs and normalized observation keys must each remain unique after canonical sorting.
TEST_CASE("runtime policy rejects duplicate source identities and keys", "[runtime][policy][m2]") {
  const auto configuration =
      create_configuration_from_params_or_throw(create_two_source_configuration_params_or_throw());

  SECTION("duplicate source identity") {
    auto params = create_two_source_policy_params_or_throw();
    params.sources[1U].source_id = params.sources[0U].source_id;
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                              "runtime_policy.sources.source_id", 1U));
  }

  SECTION("duplicate observation key") {
    auto params = create_reference_policy_params_or_throw();
    auto duplicate = params.sources.front();
    duplicate.source_id =
        parse_identifier_or_throw<model::MarketSourceId>("source.second-deribit-btc");
    params.sources.push_back(std::move(duplicate));
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                              "runtime_policy.sources.key", 1U));
  }
}

// --------------------------------------------------------
// Sources must resolve through venue, metadata, native symbol, revision, and subscription catalogs.
TEST_CASE("runtime policy rejects dangling or mismatched source definitions",
          "[runtime][policy][m2]") {
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());

  SECTION("venue is not configured") {
    auto params = create_reference_policy_params_or_throw();
    params.sources.front().venue_id = parse_identifier_or_throw<model::VenueId>("kraken");
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                              "runtime_policy.sources.venue_id", 0U));
  }

  SECTION("instrument is not configured at the venue") {
    auto params = create_reference_policy_params_or_throw();
    params.sources.front().instrument_id =
        parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                              "runtime_policy.sources.instrument_id", 0U));
  }

  SECTION("venue-native instrument identity disagrees") {
    auto params = create_reference_policy_params_or_throw();
    params.sources.front().venue_instrument_id =
        parse_identifier_or_throw<model::VenueInstrumentId>("BTC-29AUG26");
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "runtime_policy.sources.venue_instrument_id", 0U));
  }

  SECTION("metadata revision disagrees") {
    auto params = create_reference_policy_params_or_throw();
    params.sources.front().metadata_revision =
        model::InstrumentMetadataRevision::from_value(2U).value();
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "runtime_policy.sources.metadata_revision", 0U));
  }

  SECTION("no order-book subscription grants observation") {
    auto configuration_params = test_support::create_reference_configuration_params_or_throw();
    configuration_params.subscriptions.clear();
    const auto unsubscribed_configuration =
        create_configuration_from_params_or_throw(std::move(configuration_params));
    const auto result = runtime::RuntimePolicy::create_runtime_policy(
        unsubscribed_configuration, create_reference_policy_params_or_throw());
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                              "runtime_policy.sources.subscription", 0U));
  }
}

// --------------------------------------------------------
// Every capacity or timing value is positive and parser/book bounds cannot exceed M2 ceilings.
TEST_CASE("runtime policy rejects zero and over-limit bounds", "[runtime][policy][m2]") {
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());

  const auto check_failure = [&](runtime::RuntimePolicyLimits limits, std::string_view field) {
    const auto result = runtime::RuntimePolicy::create_runtime_policy(
        configuration, runtime::RuntimePolicyParams{limits, {create_reference_source_or_throw()}});
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::create_at_field(
                                model::DomainErrorCode::InvalidRuntimePolicy, std::string{field}));
  };

  SECTION("zero ingress capacity") {
    auto limits = create_reference_limits();
    limits.ingress_capacity = 0U;
    check_failure(limits, "runtime_policy.ingress_capacity");
  }
  SECTION("frame bytes are zero") {
    auto limits = create_reference_limits();
    limits.maximum_frame_bytes = 0U;
    check_failure(limits, "runtime_policy.maximum_frame_bytes");
  }
  SECTION("frame bytes exceed the fixture ceiling") {
    auto limits = create_reference_limits();
    limits.maximum_frame_bytes = runtime::maximum_runtime_frame_bytes + 1U;
    check_failure(limits, "runtime_policy.maximum_frame_bytes");
  }
  SECTION("changes per update are zero") {
    auto limits = create_reference_limits();
    limits.maximum_changes_per_update = 0U;
    check_failure(limits, "runtime_policy.maximum_changes_per_update");
  }
  SECTION("changes exceed the normalized-update ceiling") {
    auto limits = create_reference_limits();
    limits.maximum_changes_per_update = runtime::maximum_runtime_changes_per_update + 1U;
    check_failure(limits, "runtime_policy.maximum_changes_per_update");
  }
  SECTION("retained depth is zero") {
    auto limits = create_reference_limits();
    limits.retained_book_depth = 0U;
    check_failure(limits, "runtime_policy.retained_book_depth");
  }
  SECTION("retained depth exceeds the compiled book ceiling") {
    auto limits = create_reference_limits();
    limits.retained_book_depth = runtime::maximum_runtime_retained_book_depth + 1U;
    check_failure(limits, "runtime_policy.retained_book_depth");
  }
  SECTION("zero stale threshold") {
    auto limits = create_reference_limits();
    limits.stale_threshold_nanoseconds = 0U;
    check_failure(limits, "runtime_policy.stale_threshold_nanoseconds");
  }
  SECTION("zero callback fanout") {
    auto limits = create_reference_limits();
    limits.maximum_callbacks_per_turn = 0U;
    check_failure(limits, "runtime_policy.maximum_callbacks_per_turn");
  }
  SECTION("zero diagnostic capacity") {
    auto limits = create_reference_limits();
    limits.diagnostic_capacity = 0U;
    check_failure(limits, "runtime_policy.diagnostic_capacity");
  }
  SECTION("zero trace capacity") {
    auto limits = create_reference_limits();
    limits.runtime_trace_capacity = 0U;
    check_failure(limits, "runtime_policy.runtime_trace_capacity");
  }
  SECTION("zero drive bound") {
    auto limits = create_reference_limits();
    limits.maximum_drive_turns = 0U;
    check_failure(limits, "runtime_policy.maximum_drive_turns");
  }
  SECTION("zero callback budget") {
    auto limits = create_reference_limits();
    limits.callback_budget_nanoseconds = 0U;
    check_failure(limits, "runtime_policy.callback_budget_nanoseconds");
  }
}

// --------------------------------------------------------
// Empty registries and callback fanout beyond policy capacity must fail before runtime
// construction.
TEST_CASE("runtime policy requires sources and sufficient callback fanout capacity",
          "[runtime][policy][m2]") {
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_reference_configuration_params_or_throw());

  SECTION("source registry is empty") {
    auto params = create_reference_policy_params_or_throw();
    params.sources.clear();
    const auto result =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::create_at_field(
                                model::DomainErrorCode::EmptyCollection, "runtime_policy.sources"));
  }

  SECTION("recovery snapshot callback fanout exceeds policy capacity") {
    const auto multi_callback_configuration = create_configuration_from_params_or_throw(
        create_two_callback_configuration_params_or_throw());
    auto params = create_reference_policy_params_or_throw();
    params.limits.maximum_callbacks_per_turn = 2U;
    const auto result = runtime::RuntimePolicy::create_runtime_policy(multi_callback_configuration,
                                                                      std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "runtime_policy.sources.fanout", 0U));
  }

  SECTION("recovery snapshot callback fanout may equal policy capacity") {
    const auto multi_callback_configuration = create_configuration_from_params_or_throw(
        create_two_callback_configuration_params_or_throw());
    auto params = create_reference_policy_params_or_throw();
    params.limits.maximum_callbacks_per_turn = 4U;
    const auto result = runtime::RuntimePolicy::create_runtime_policy(multi_callback_configuration,
                                                                      std::move(params));
    REQUIRE(result);
    CHECK(result.value().limits().maximum_callbacks_per_turn == 4U);
  }

  SECTION("single-grant trace capacity must cover the maximum recovery turn") {
    auto params = create_reference_policy_params_or_throw();
    params.limits.runtime_trace_capacity = 5U;
    const auto insufficient = runtime::RuntimePolicy::create_runtime_policy(configuration, params);
    REQUIRE_FALSE(insufficient);
    CHECK(insufficient.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "runtime_policy.runtime_trace_capacity"));

    params.limits.runtime_trace_capacity = 6U;
    const auto boundary =
        runtime::RuntimePolicy::create_runtime_policy(configuration, std::move(params));
    REQUIRE(boundary);
    CHECK(boundary.value().limits().runtime_trace_capacity == 6U);
  }

  SECTION("multi-grant trace capacity uses overflow-safe exact fanout") {
    const auto multi_callback_configuration = create_configuration_from_params_or_throw(
        create_two_callback_configuration_params_or_throw());
    auto params = create_reference_policy_params_or_throw();
    params.limits.runtime_trace_capacity = 9U;
    const auto insufficient =
        runtime::RuntimePolicy::create_runtime_policy(multi_callback_configuration, params);
    REQUIRE_FALSE(insufficient);
    CHECK(insufficient.error() ==
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidRuntimePolicy,
                                              "runtime_policy.runtime_trace_capacity"));

    params.limits.runtime_trace_capacity = 10U;
    const auto boundary = runtime::RuntimePolicy::create_runtime_policy(
        multi_callback_configuration, std::move(params));
    REQUIRE(boundary);
    CHECK(boundary.value().limits().runtime_trace_capacity == 10U);
  }
}

// --------------------------------------------------------

} // namespace
