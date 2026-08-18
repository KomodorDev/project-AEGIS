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

// Typed parsers fail fast on broken fixtures; hexadecimal output makes canonical byte failures
// exact.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in configuration test"};
  }
  return std::move(result).value();
}

template <typename Decimal> [[nodiscard]] Decimal decimal(std::string_view text) {
  auto result = Decimal::parse_ascii(text);
  if (!result) {
    throw std::logic_error{"invalid decimal in configuration test"};
  }
  return std::move(result).value();
}

[[nodiscard]] std::string hexadecimal(const std::vector<std::byte>& bytes) {
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

// Multiple values in every relevant collection ensure reversal genuinely exercises canonical order.
[[nodiscard]] configuration::StartupConfigurationParams reordered_configuration_params() {
  auto params = test_support::two_firm_configuration_params();

  const auto venue_id = id<model::VenueId>("kraken");
  const auto instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto account_id = id<model::LogicalAccountId>("account.kraken-testnet-subsidiary");
  const auto firm_id = id<model::FirmId>("firm.aegis-subsidiary");
  const auto bot_id = id<model::BotId>("bot.subsidiary-reference");

  params.venues.push_back(
      configuration::VenueDefinition{venue_id, configuration::VenueEnvironment::Testnet});
  params.logical_accounts.push_back(
      configuration::LogicalAccountVenueBinding{account_id, firm_id, venue_id});

  auto metadata = params.instrument_metadata.front();
  metadata.venue_id = venue_id;
  metadata.instrument_id = instrument_id;
  metadata.venue_instrument_id = id<model::VenueInstrumentId>("ETH-PERPETUAL");
  metadata.base_currency = "ETH";
  metadata.settlement_currency = "USD";
  metadata.contract_style = model::ContractStyle::Linear;
  metadata.contract_multiplier_unit = model::ContractMultiplierUnit::BaseCurrencyPerContract;
  metadata.price_scale = 2U;
  metadata.tick_size = decimal<model::Price>("0.01");
  metadata.contract_multiplier = decimal<model::Notional>("0.001");
  params.instrument_metadata.push_back(std::move(metadata));

  params.subscriptions.push_back(market_data::Subscription{
      id<model::SubscriptionId>("subscription.kraken-eth-perpetual-book"), bot_id, venue_id,
      instrument_id, market_data::SubscriptionChannel::OrderBook});
  params.routes.push_back(execution::ExecutionRoute{
      id<model::RouteId>("route.kraken-testnet-subsidiary-eth-perpetual"), bot_id, venue_id,
      account_id, instrument_id, execution::ExecutionRouteState::Disabled});
  return params;
}

// Accepted snapshots prove section coherence, exact revision provenance, and independent peer
// firms.
TEST_CASE("the accepted reference configuration is sealed and carries exact provenance",
          "[configuration][m1]") {
  const auto result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());

  REQUIRE(result);
  const auto& configured = result.value();
  REQUIRE(configured.organization().firms().size() == 1U);
  REQUIRE(configured.organization().bot_attributions().size() == 1U);
  CHECK(configured.organization().bot_attributions().front().firm_id ==
        id<model::FirmId>("firm.aegis-lab"));
  REQUIRE(configured.routes().routes().size() == 1U);
  CHECK_FALSE(configured.routes().routes().front().is_enabled());
  REQUIRE(configured.subscriptions().subscriptions().size() == 1U);
  CHECK(configured.find_venue(id<model::VenueId>("deribit")) != nullptr);
  CHECK(
      configured.find_logical_account(id<model::LogicalAccountId>("account.deribit-testnet-aegis"))
          ->firm_id == id<model::FirmId>("firm.aegis-lab"));

  const auto& provenance = configured.provenance();
  CHECK(provenance.fingerprint() == configured.fingerprint());
  CHECK(provenance.configuration_revision() == model::ConfigurationRevision::initial());
  CHECK(provenance.organization_revision() == model::OrganizationRevision::initial());
  CHECK(provenance.strategy_configuration_revision() ==
        model::StrategyConfigurationRevision::initial());
  CHECK(provenance.subscription_revision() == model::SubscriptionRevision::initial());
  CHECK(provenance.route_revision() == model::RouteRevision::initial());
  const auto* const metadata_revision = provenance.find_instrument_metadata_revision(
      id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL"));
  REQUIRE(metadata_revision != nullptr);
  CHECK(*metadata_revision == model::InstrumentMetadataRevision::initial());
  CHECK(provenance.find_instrument_metadata_revision(
            id<model::VenueId>("deribit"), id<model::InstrumentId>("ETH-USD-PERPETUAL")) ==
        nullptr);
}

TEST_CASE("peer firms retain independent bot attribution and account ownership",
          "[configuration][m1][organization]") {
  const auto result =
      configuration::StartupConfiguration::create(test_support::two_firm_configuration_params());

  REQUIRE(result);
  CHECK(result.value().organization().firms().size() == 2U);
  const auto* const subsidiary =
      result.value().organization().find_bot(id<model::BotId>("bot.subsidiary-reference"));
  REQUIRE(subsidiary != nullptr);
  CHECK(subsidiary->firm_id == id<model::FirmId>("firm.aegis-subsidiary"));
  const auto* const subsidiary_account = result.value().find_logical_account(
      id<model::LogicalAccountId>("account.deribit-testnet-subsidiary"));
  REQUIRE(subsidiary_account != nullptr);
  CHECK(subsidiary_account->firm_id == id<model::FirmId>("firm.aegis-subsidiary"));
  REQUIRE(result.value().routes().routes().size() == 2U);
  CHECK(std::all_of(result.value().routes().routes().begin(),
                    result.value().routes().routes().end(),
                    [](const execution::ExecutionRoute& route) { return !route.is_enabled(); }));
}

// Account ownership is an authorization boundary even when both firms are valid configuration
// roots.
TEST_CASE("a route cannot cross from its bot firm into a subsidiary account",
          "[configuration][m1][organization]") {
  auto params = test_support::two_firm_configuration_params();
  params.routes.back().logical_account_id =
      id<model::LogicalAccountId>("account.deribit-testnet-aegis");

  const auto result = configuration::StartupConfiguration::create(std::move(params));

  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidRelationship);
  CHECK(result.error().context.field == "routes.account_firm");
}

// Canonical evidence must ignore authoring order, change with every semantic/revision input, and
// match one published byte-and-digest vector.
TEST_CASE("canonical configuration identity is independent of every input collection order",
          "[configuration][canonical]") {
  auto ordered_params = reordered_configuration_params();
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

  const auto ordered = configuration::StartupConfiguration::create(std::move(ordered_params));
  const auto reversed = configuration::StartupConfiguration::create(std::move(reversed_params));

  REQUIRE(ordered);
  REQUIRE(reversed);
  CHECK(ordered.value().canonical_bytes() == reversed.value().canonical_bytes());
  CHECK(ordered.value().fingerprint() == reversed.value().fingerprint());
}

TEST_CASE("decision semantics and every revision participate in the fingerprint",
          "[configuration][canonical]") {
  const auto baseline =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  REQUIRE(baseline);

  auto semantic_params = test_support::reference_configuration_params();
  semantic_params.routes.front().state = execution::ExecutionRouteState::Enabled;
  const auto semantic = configuration::StartupConfiguration::create(std::move(semantic_params));
  REQUIRE(semantic);
  CHECK(semantic.value().fingerprint() != baseline.value().fingerprint());

  auto configuration_revision_params = test_support::reference_configuration_params();
  configuration_revision_params.revision = model::ConfigurationRevision::from_value(2U).value();
  const auto configuration_revision =
      configuration::StartupConfiguration::create(std::move(configuration_revision_params));
  REQUIRE(configuration_revision);
  CHECK(configuration_revision.value().fingerprint() != baseline.value().fingerprint());

  auto organization_revision_params = test_support::reference_configuration_params();
  organization_revision_params.organization_revision =
      model::OrganizationRevision::from_value(2U).value();
  const auto organization_revision =
      configuration::StartupConfiguration::create(std::move(organization_revision_params));
  REQUIRE(organization_revision);
  CHECK(organization_revision.value().fingerprint() != baseline.value().fingerprint());

  auto strategy_revision_params = test_support::reference_configuration_params();
  strategy_revision_params.strategy_configuration_revision =
      model::StrategyConfigurationRevision::from_value(2U).value();
  const auto strategy_revision =
      configuration::StartupConfiguration::create(std::move(strategy_revision_params));
  REQUIRE(strategy_revision);
  CHECK(strategy_revision.value().fingerprint() != baseline.value().fingerprint());

  auto metadata_revision_params = test_support::reference_configuration_params();
  metadata_revision_params.instrument_metadata.front().revision =
      model::InstrumentMetadataRevision::from_value(2U).value();
  const auto metadata_revision =
      configuration::StartupConfiguration::create(std::move(metadata_revision_params));
  REQUIRE(metadata_revision);
  CHECK(metadata_revision.value().fingerprint() != baseline.value().fingerprint());

  auto subscription_revision_params = test_support::reference_configuration_params();
  subscription_revision_params.subscription_revision =
      model::SubscriptionRevision::from_value(2U).value();
  const auto subscription_revision =
      configuration::StartupConfiguration::create(std::move(subscription_revision_params));
  REQUIRE(subscription_revision);
  CHECK(subscription_revision.value().fingerprint() != baseline.value().fingerprint());

  auto route_revision_params = test_support::reference_configuration_params();
  route_revision_params.route_revision = model::RouteRevision::from_value(2U).value();
  const auto route_revision =
      configuration::StartupConfiguration::create(std::move(route_revision_params));
  REQUIRE(route_revision);
  CHECK(route_revision.value().fingerprint() != baseline.value().fingerprint());
}

TEST_CASE("the reference configuration has a published schema-one golden vector",
          "[configuration][canonical][golden]") {
  const auto result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  REQUIRE(result);

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
  CHECK(hexadecimal(result.value().canonical_bytes()) == expected_bytes);
  CHECK(result.value().fingerprint().to_hex() ==
        "e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310");
}

// The rejection matrix proves no invalid section escapes and preserves documented section priority.
TEST_CASE("startup validation rejects incomplete and inconsistent sections atomically",
          "[configuration][validation]") {
  SECTION("missing strategy settings") {
    auto params = test_support::reference_configuration_params();
    params.strategy_settings.clear();
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                       "strategy_settings.missing_bot", 0U));
  }

  SECTION("mismatched bot strategy settings") {
    auto params = test_support::reference_configuration_params();
    params.strategy_settings.front().strategy_id = id<model::StrategyId>("strategy.wrong");
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "strategy_settings.strategy_id");
  }

  SECTION("duplicate bot strategy settings") {
    auto params = test_support::reference_configuration_params();
    params.strategy_settings.push_back(params.strategy_settings.front());
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                       "strategy_settings.bot_id", 1U));
  }

  SECTION("invalid environment") {
    auto params = test_support::reference_configuration_params();
    params.venues.front().environment =
        static_cast<configuration::VenueEnvironment>(std::uint8_t{99U});
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::at_index(model::DomainErrorCode::InvalidValue,
                                                         "venues.environment", 0U));
  }

  SECTION("dangling account firm") {
    auto params = test_support::reference_configuration_params();
    params.logical_accounts.front().firm_id = id<model::FirmId>("firm.unknown");
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "logical_accounts.firm_id");
  }

  SECTION("duplicate venue definition") {
    auto params = test_support::reference_configuration_params();
    params.venues.push_back(params.venues.front());
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() == model::DomainError::at_index(
                                model::DomainErrorCode::DuplicateIdentifier, "venues.id", 1U));
  }

  SECTION("duplicate logical account") {
    auto params = test_support::reference_configuration_params();
    params.logical_accounts.push_back(params.logical_accounts.front());
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                       "logical_accounts.id", 1U));
  }

  SECTION("dangling account venue") {
    auto params = test_support::reference_configuration_params();
    params.logical_accounts.front().venue_id = id<model::VenueId>("unknown");
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "logical_accounts.venue_id");
  }

  SECTION("duplicate venue and instrument metadata") {
    auto params = test_support::reference_configuration_params();
    params.instrument_metadata.push_back(params.instrument_metadata.front());
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error() ==
          model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                       "instrument_metadata.venue_instrument", 1U));
  }

  SECTION("duplicate venue-native semantic key") {
    auto params = test_support::reference_configuration_params();
    auto duplicate_native = params.instrument_metadata.front();
    duplicate_native.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
    duplicate_native.base_currency = "ETH";
    params.instrument_metadata.push_back(std::move(duplicate_native));
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "instrument_metadata.venue_native_instrument");
  }

  SECTION("metadata references an undefined venue") {
    auto params = test_support::reference_configuration_params();
    params.instrument_metadata.front().venue_id = id<model::VenueId>("unknown");
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "instrument_metadata.venue_id");
  }

  SECTION("invalid metadata remains inside the atomic failure") {
    auto params = test_support::reference_configuration_params();
    params.instrument_metadata.front().base_currency = "btc";
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidMetadata);
    CHECK(result.error().context.field == "instrument.base_currency");
    CHECK(result.error().context.collection_index == 0U);
  }

  SECTION("existing subscription validation participates in startup") {
    auto params = test_support::reference_configuration_params();
    params.subscriptions.front().bot_id = id<model::BotId>("bot.unknown");
    const auto result = configuration::StartupConfiguration::create(std::move(params));
    REQUIRE_FALSE(result);
    CHECK(result.error().context.field == "subscriptions.bot_id");
  }
}

TEST_CASE("startup sections fail in their documented canonical order",
          "[configuration][validation]") {
  auto params = test_support::reference_configuration_params();
  params.firms.clear();
  params.venues.front().environment =
      static_cast<configuration::VenueEnvironment>(std::uint8_t{99U});

  const auto result = configuration::StartupConfiguration::create(std::move(params));

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_field(model::DomainErrorCode::EmptyCollection,
                                                       "organization.firms"));
}

} // namespace
