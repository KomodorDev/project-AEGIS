// Purpose: prove M1 startup validation, canonical identity, and multi-firm provenance as one unit.

#include "aegis/configuration/startup_configuration.hpp"
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
// Interesting syntax: requires-expression concepts make selected forbidden public member names on
// the asserted M1 types compile-time test failures without constructing those types.
template <typename Configuration>
concept HasCredentials = requires(Configuration value) { value.credentials; };

template <typename Configuration>
concept HasApiKey = requires(Configuration value) { value.api_key; };

template <typename Configuration>
concept HasVenueAccountId = requires(Configuration value) { value.venue_account_id; };

template <typename Configuration>
concept HasHttpUrl = requires(Configuration value) { value.http_url; };

template <typename Configuration>
concept HasWebsocketUrl = requires(Configuration value) { value.websocket_url; };

template <typename Configuration>
concept HasCompanyParent = requires(Configuration value) { value.parent_company_id; };

template <typename Configuration>
concept HasEndpoint = requires(Configuration value) { value.endpoint; };

template <typename Configuration>
concept HasSecret = requires(Configuration value) { value.secret; };

static_assert(!HasCredentials<configuration::StartupConfigurationParams>);
static_assert(!HasApiKey<configuration::StartupConfigurationParams>);
static_assert(!HasVenueAccountId<configuration::StartupConfigurationParams>);
static_assert(!HasHttpUrl<configuration::StartupConfigurationParams>);
static_assert(!HasWebsocketUrl<configuration::StartupConfigurationParams>);
static_assert(!HasCompanyParent<configuration::StartupConfigurationParams>);
static_assert(!HasCredentials<configuration::VenueDefinition>);
static_assert(!HasApiKey<configuration::VenueDefinition>);
static_assert(!HasHttpUrl<configuration::VenueDefinition>);
static_assert(!HasWebsocketUrl<configuration::VenueDefinition>);
static_assert(!HasEndpoint<configuration::VenueDefinition>);
static_assert(!HasSecret<configuration::VenueDefinition>);
static_assert(!HasVenueAccountId<configuration::LogicalAccountVenueBinding>);

// ########################################################################

// --------------------------------------------------------
// Typed identifier parsers fail fast on broken fixtures rather than creating incidental test cases.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse_identifier(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in configuration test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Typed decimal parsers apply the same fail-fast policy to numeric fixture literals.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view text) {
  auto result = Decimal::parse_ascii(text);
  if (!result) {
    throw std::logic_error{"invalid decimal in configuration test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Hexadecimal rendering makes canonical byte failures exact and reviewable.
[[nodiscard]] std::string bytes_to_hexadecimal(const std::vector<std::byte>& bytes) {
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
// Multiple values in every relevant collection ensure reversal genuinely exercises canonical order.
[[nodiscard]] configuration::StartupConfigurationParams
create_reordered_configuration_params_or_throw() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Begin with two independent firms and define a second venue-owned instrument path.
  auto params = test_support::create_two_firm_configuration_params_or_throw();

  const auto venue_id = parse_identifier_or_throw<model::VenueId>("kraken");
  const auto instrument_id = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.kraken-testnet-subsidiary");
  const auto firm_id = parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary");
  const auto bot_id = parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference");

  // ++++++++++++++++++++++++++++++++++++++++
  // Add the venue and its subsidiary-owned logical account.
  params.venues.push_back(
      configuration::VenueDefinition{venue_id, configuration::VenueEnvironment::Testnet});
  params.logical_accounts.push_back(
      configuration::LogicalAccountVenueBinding{account_id, firm_id, venue_id});

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive a distinct linear ETH metadata record from the accepted inverse BTC fixture.
  auto metadata = params.instrument_metadata.front();
  metadata.venue_id = venue_id;
  metadata.instrument_id = instrument_id;
  metadata.venue_instrument_id =
      parse_identifier_or_throw<model::VenueInstrumentId>("ETH-PERPETUAL");
  metadata.base_currency = "ETH";
  metadata.settlement_currency = "USD";
  metadata.contract_style = model::ContractStyle::Linear;
  metadata.contract_multiplier_unit = model::ContractMultiplierUnit::BaseCurrencyPerContract;
  metadata.price_scale = 2U;
  metadata.tick_size = parse_decimal_or_throw<model::Price>("0.01");
  metadata.contract_multiplier = parse_decimal_or_throw<model::Notional>("0.001");
  params.instrument_metadata.push_back(std::move(metadata));

  // ++++++++++++++++++++++++++++++++++++++++
  // Complete the second path with matching observation and disabled execution grants.
  params.subscriptions.push_back(market_data::Subscription{
      parse_identifier_or_throw<model::SubscriptionId>("subscription.kraken-eth-perpetual-book"),
      bot_id, venue_id, instrument_id, market_data::SubscriptionChannel::OrderBook});
  params.routes.push_back(execution::ExecutionRoute{
      parse_identifier_or_throw<model::RouteId>("route.kraken-testnet-subsidiary-eth-perpetual"),
      bot_id, venue_id, account_id, instrument_id, execution::ExecutionRouteState::Disabled});
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Accepted snapshots prove section coherence, exact revision provenance, and independent peer
// firms.
TEST_CASE("the accepted reference configuration is sealed and carries exact provenance",
          "[configuration][m1]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the complete single-firm reference configuration atomically.
  const auto result = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());

  REQUIRE(result);
  const auto& configured = result.value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify organization, route, subscription, venue, and account projections remain coherent.
  REQUIRE(configured.organization().firms().size() == 1U);
  REQUIRE(configured.organization().bot_attributions().size() == 1U);
  CHECK(configured.organization().bot_attributions().front().firm_id ==
        parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
  REQUIRE(configured.routes().routes().size() == 1U);
  CHECK_FALSE(configured.routes().routes().front().is_enabled());
  REQUIRE(configured.subscriptions().subscriptions().size() == 1U);
  CHECK(configured.find_venue(parse_identifier_or_throw<model::VenueId>("deribit")) != nullptr);
  CHECK(configured
            .find_logical_account(
                parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"))
            ->firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify the sealed fingerprint and every section revision are captured in provenance.
  const auto& provenance = configured.provenance();
  CHECK(provenance.fingerprint() == configured.fingerprint());
  CHECK(provenance.configuration_revision() == model::ConfigurationRevision::create_initial());
  CHECK(provenance.organization_revision() == model::OrganizationRevision::create_initial());
  CHECK(provenance.strategy_configuration_revision() ==
        model::StrategyConfigurationRevision::create_initial());
  CHECK(provenance.subscription_revision() == model::SubscriptionRevision::create_initial());
  CHECK(provenance.route_revision() == model::RouteRevision::create_initial());
  const auto* const metadata_revision = provenance.find_instrument_metadata_revision(
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"));
  REQUIRE(metadata_revision != nullptr);
  CHECK(*metadata_revision == model::InstrumentMetadataRevision::create_initial());
  CHECK(provenance.find_instrument_metadata_revision(
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Peer firms retain independent bot, account, and disabled-route ownership after sealing.
TEST_CASE("peer firms retain independent bot attribution and account ownership",
          "[configuration][m1][organization]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the subsidiary-aware fixture as one immutable configuration.
  const auto result = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_two_firm_configuration_params_or_throw());

  REQUIRE(result);
  CHECK(result.value().organization().firms().size() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the subsidiary bot and account back to the same firm root.
  const auto* const subsidiary = result.value().organization().find_bot(
      parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"));
  REQUIRE(subsidiary != nullptr);
  CHECK(subsidiary->firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary"));
  const auto* const subsidiary_account = result.value().find_logical_account(
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-subsidiary"));
  REQUIRE(subsidiary_account != nullptr);
  CHECK(subsidiary_account->firm_id ==
        parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Both peer-firm routes remain explicitly disabled in the accepted snapshot.
  REQUIRE(result.value().routes().routes().size() == 2U);
  CHECK(std::all_of(result.value().routes().routes().begin(),
                    result.value().routes().routes().end(),
                    [](const execution::ExecutionRoute& route) { return !route.is_enabled(); }));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Account ownership is an authorization boundary even when both firms are valid configuration
// roots.
TEST_CASE("a route cannot cross from its bot firm into a subsidiary account",
          "[configuration][m1][organization]") {
  auto params = test_support::create_two_firm_configuration_params_or_throw();
  params.routes.back().logical_account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis");

  const auto result =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));

  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidRelationship);
  CHECK(result.error().context.field == "routes.account_firm");
}

// --------------------------------------------------------
// Canonical evidence must ignore authoring order, change with every semantic/revision input, and
// match one published byte-and-digest vector.
TEST_CASE("canonical configuration identity is independent of every input collection order",
          "[configuration][canonical]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reverse every collection in a fixture that contains multiple values per relevant section.
  auto ordered_params = create_reordered_configuration_params_or_throw();
  auto reversed_params = ordered_params;
  std::reverse(reversed_params.firms.begin(), reversed_params.firms.end());
  std::reverse(reversed_params.desks.begin(), reversed_params.desks.end());
  std::reverse(reversed_params.bots.begin(), reversed_params.bots.end());
  std::reverse(reversed_params.strategy_settings.begin(), reversed_params.strategy_settings.end());
  std::reverse(reversed_params.venues.begin(), reversed_params.venues.end());
  std::reverse(reversed_params.logical_accounts.begin(), reversed_params.logical_accounts.end());
  std::reverse(reversed_params.instrument_metadata.begin(),
               reversed_params.instrument_metadata.end());
  std::reverse(reversed_params.subscriptions.begin(), reversed_params.subscriptions.end());
  std::reverse(reversed_params.routes.begin(), reversed_params.routes.end());

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal both authoring orders through independent validation paths.
  const auto ordered =
      configuration::StartupConfiguration::create_startup_configuration(std::move(ordered_params));
  const auto reversed =
      configuration::StartupConfiguration::create_startup_configuration(std::move(reversed_params));

  REQUIRE(ordered);
  REQUIRE(reversed);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical bytes and their digest must be identical after sorting all semantic collections.
  CHECK(ordered.value().canonical_bytes() == reversed.value().canonical_bytes());
  CHECK(ordered.value().fingerprint() == reversed.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Each decision-bearing field and revision independently changes the sealed fingerprint.
TEST_CASE("decision semantics and every revision participate in the fingerprint",
          "[configuration][canonical]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish the accepted fingerprint against which every isolated mutation is compared.
  const auto baseline = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());
  REQUIRE(baseline);

  // ++++++++++++++++++++++++++++++++++++++++
  // Route state is decision-bearing even when every identity remains unchanged.
  auto semantic_params = test_support::create_reference_configuration_params_or_throw();
  semantic_params.routes.front().state = execution::ExecutionRouteState::Enabled;
  const auto semantic =
      configuration::StartupConfiguration::create_startup_configuration(std::move(semantic_params));
  REQUIRE(semantic);
  CHECK(semantic.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Configuration revision participates in the canonical evidence.
  auto configuration_revision_params =
      test_support::create_reference_configuration_params_or_throw();
  configuration_revision_params.revision = model::ConfigurationRevision::from_value(2U).value();
  const auto configuration_revision =
      configuration::StartupConfiguration::create_startup_configuration(
          std::move(configuration_revision_params));
  REQUIRE(configuration_revision);
  CHECK(configuration_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Organization revision participates independently in the canonical evidence.
  auto organization_revision_params =
      test_support::create_reference_configuration_params_or_throw();
  organization_revision_params.organization_revision =
      model::OrganizationRevision::from_value(2U).value();
  const auto organization_revision =
      configuration::StartupConfiguration::create_startup_configuration(
          std::move(organization_revision_params));
  REQUIRE(organization_revision);
  CHECK(organization_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Strategy revision participates independently in the canonical evidence.
  auto strategy_revision_params = test_support::create_reference_configuration_params_or_throw();
  strategy_revision_params.strategy_configuration_revision =
      model::StrategyConfigurationRevision::from_value(2U).value();
  const auto strategy_revision = configuration::StartupConfiguration::create_startup_configuration(
      std::move(strategy_revision_params));
  REQUIRE(strategy_revision);
  CHECK(strategy_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Instrument metadata revision participates independently in the canonical evidence.
  auto metadata_revision_params = test_support::create_reference_configuration_params_or_throw();
  metadata_revision_params.instrument_metadata.front().revision =
      model::InstrumentMetadataRevision::from_value(2U).value();
  const auto metadata_revision = configuration::StartupConfiguration::create_startup_configuration(
      std::move(metadata_revision_params));
  REQUIRE(metadata_revision);
  CHECK(metadata_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Subscription revision participates independently in the canonical evidence.
  auto subscription_revision_params =
      test_support::create_reference_configuration_params_or_throw();
  subscription_revision_params.subscription_revision =
      model::SubscriptionRevision::from_value(2U).value();
  const auto subscription_revision =
      configuration::StartupConfiguration::create_startup_configuration(
          std::move(subscription_revision_params));
  REQUIRE(subscription_revision);
  CHECK(subscription_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Route revision completes the set of section revisions covered by the fingerprint.
  auto route_revision_params = test_support::create_reference_configuration_params_or_throw();
  route_revision_params.route_revision = model::RouteRevision::from_value(2U).value();
  const auto route_revision = configuration::StartupConfiguration::create_startup_configuration(
      std::move(route_revision_params));
  REQUIRE(route_revision);
  CHECK(route_revision.value().fingerprint() != baseline.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The published schema-one vector detects any tag, length, ordering, endian, or digest drift.
TEST_CASE("the reference configuration has a published schema-one golden vector",
          "[configuration][canonical][golden]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the exact reference fixture whose canonical bytes are published below.
  const auto result = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());
  REQUIRE(result);

  // ++++++++++++++++++++++++++++++++++++++++
  // This vector locks magic, tags, lengths, order, and endian representation as one schema
  // contract.
  const std::string expected_bytes =
      "4145474953434647000100010000000800000000000000010002000000080000000000000001"
      "00030000001c000000010000001400010000000e6669726d2e61656769732d6c616200040000"
      "0035000000010000002d0001000000136465736b2e6469676974616c2d617373657473000200"
      "00000e6669726d2e61656769732d6c6162000500000070000000010000006800010000002362"
      "6f742e646572696269742d6274632d70657270657475616c2d7265666572656e636500020000"
      "00136465736b2e6469676974616c2d61737365747300030000002073747261746567792e6465"
      "7465726d696e69737469632d7265666572656e63650006000000080000000000000001000700"
      "00005e0000000100000056000100000023626f742e646572696269742d6274632d7065727065"
      "7475616c2d7265666572656e636500020000002073747261746567792e64657465726d696e69"
      "737469632d7265666572656e63650003000000010100080000001c0000000100000014000100"
      "000007646572696269740002000000010100090000004c000000010000004400010000001d61"
      "63636f756e742e646572696269742d746573746e65742d616567697300020000000e6669726d"
      "2e61656769732d6c616200030000000764657269626974000a000000c700000001000000bf00"
      "0100000007646572696269740002000000114254432d5553442d50455250455455414c000300"
      "00000d4254432d50455250455455414c00040000000800000000000000010005000000034254"
      "430006000000035553440007000000034254430008000000010200090000000101000a000000"
      "0102000b0000000101000c0000000100000d00000009000000000000000501000e0000000900"
      "0000000000000100000f00000009000000000000000100001000000009000000000000000a00"
      "000b000000080000000000000001000c00000089000000010000008100010000002773756273"
      "6372697074696f6e2e646572696269742d6274632d70657270657475616c2d626f6f6b000200"
      "000023626f742e646572696269742d6274632d70657270657475616c2d7265666572656e6365"
      "000300000007646572696269740004000000114254432d5553442d50455250455455414c0005"
      "0000000101000d000000080000000000000001000e000000a800000001000000a00001000000"
      "23726f7574652e646572696269742d746573746e65742d6274632d70657270657475616c0002"
      "00000023626f742e646572696269742d6274632d70657270657475616c2d7265666572656e63"
      "650003000000076465726962697400040000001d6163636f756e742e646572696269742d7465"
      "73746e65742d61656769730005000000114254432d5553442d50455250455455414c00060000"
      "000100";
  CHECK(bytes_to_hexadecimal(result.value().canonical_bytes()) == expected_bytes);
  CHECK(result.value().fingerprint().to_hex() ==
        "e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The rejection matrix proves no invalid section escapes and preserves documented section priority.
TEST_CASE("startup validation rejects incomplete and inconsistent sections atomically",
          "[configuration][validation]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Every registered bot requires one matching strategy-settings entry.
  SECTION("missing strategy settings") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.strategy_settings.clear();
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                              "strategy_settings.missing_bot", 0U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A settings entry cannot change the strategy assigned by organization registration.
  SECTION("mismatched bot strategy settings") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.strategy_settings.front().strategy_id =
        parse_identifier_or_throw<model::StrategyId>("strategy.wrong");
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "strategy_settings.strategy_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Each bot can own only one strategy-settings entry.
  SECTION("duplicate bot strategy settings") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.strategy_settings.push_back(params.strategy_settings.front());
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                              "strategy_settings.bot_id", 1U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue environments fail closed when their representation is unassigned.
  SECTION("invalid environment") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.venues.front().environment =
        static_cast<configuration::VenueEnvironment>(std::uint8_t{99U});
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::create_at_index(
                                model::DomainErrorCode::InvalidValue, "venues.environment", 0U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Logical account ownership must resolve to a configured firm root.
  SECTION("dangling account firm") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.logical_accounts.front().firm_id =
        parse_identifier_or_throw<model::FirmId>("firm.unknown");
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "logical_accounts.firm_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue definitions remain unique after canonical sorting.
  SECTION("duplicate venue definition") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.venues.push_back(params.venues.front());
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::create_at_index(
                                model::DomainErrorCode::DuplicateIdentifier, "venues.id", 1U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Logical account identifiers remain unique after canonical sorting.
  SECTION("duplicate logical account") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.logical_accounts.push_back(params.logical_accounts.front());
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                              "logical_accounts.id", 1U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Logical account bindings must resolve to a configured venue.
  SECTION("dangling account venue") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.logical_accounts.front().venue_id = parse_identifier_or_throw<model::VenueId>("unknown");
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "logical_accounts.venue_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Metadata cannot duplicate one normalized instrument at the same venue.
  SECTION("duplicate venue and instrument metadata") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.instrument_metadata.push_back(params.instrument_metadata.front());
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                              "instrument_metadata.venue_instrument", 1U));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Distinct normalized IDs cannot disguise a duplicate venue-native instrument identity.
  SECTION("duplicate venue-native semantic key") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    auto duplicate_native = params.instrument_metadata.front();
    duplicate_native.instrument_id =
        parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
    duplicate_native.base_currency = "ETH";
    params.instrument_metadata.push_back(std::move(duplicate_native));
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "instrument_metadata.venue_native_instrument");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Every metadata record must resolve to a configured venue definition.
  SECTION("metadata references an undefined venue") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.instrument_metadata.front().venue_id =
        parse_identifier_or_throw<model::VenueId>("unknown");
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "instrument_metadata.venue_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Nested metadata failures preserve their field and enclosing collection index atomically.
  SECTION("invalid metadata remains inside the atomic failure") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.instrument_metadata.front().base_currency = "btc";
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidMetadata);
    CHECK(result.error().context.field == "instrument.base_currency");
    CHECK(result.error().context.collection_index == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Subscription validation remains part of the same all-or-nothing startup operation.
  SECTION("existing subscription validation participates in startup") {
    auto params = test_support::create_reference_configuration_params_or_throw();
    params.subscriptions.front().bot_id = parse_identifier_or_throw<model::BotId>("bot.unknown");
    const auto result =
        configuration::StartupConfiguration::create_startup_configuration(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "subscriptions.bot_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// When multiple sections are corrupt, validation reports the documented earliest section.
TEST_CASE("startup sections fail in their documented canonical order",
          "[configuration][validation]") {
  auto params = test_support::create_reference_configuration_params_or_throw();
  params.firms.clear();
  params.venues.front().environment =
      static_cast<configuration::VenueEnvironment>(std::uint8_t{99U});

  const auto result =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::create_at_field(
                              model::DomainErrorCode::EmptyCollection, "organization.firms"));
}

// --------------------------------------------------------

} // namespace
