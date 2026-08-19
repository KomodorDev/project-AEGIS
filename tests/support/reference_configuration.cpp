// Purpose: construct accepted single-firm and subsidiary-aware M1 startup fixtures.

#include "reference_configuration.hpp"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace aegis::test_support {
namespace {

// --------------------------------------------------------
// Invalid literals are fixture-authoring defects, so typed identifier values fail immediately.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in reference configuration"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid decimal literals are fixture-authoring defects and use the same fail-fast policy.
template <typename Decimal> [[nodiscard]] Decimal decimal(std::string_view text) {
  auto result = Decimal::parse_ascii(text);
  if (!result) {
    throw std::logic_error{"invalid decimal in reference configuration"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// These are the assigned inverse Deribit semantics consumed by canonical startup and trace
// evidence.
[[nodiscard]] model::InstrumentMetadataParams reference_metadata() {
  return model::InstrumentMetadataParams{
      id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::initial(),
      "BTC",
      "USD",
      "BTC",
      model::ContractStyle::Inverse,
      model::QuantityUnit::Contracts,
      model::ContractMultiplierUnit::QuoteCurrencyPerContract,
      1U,
      0U,
      decimal<model::Price>("0.5"),
      decimal<model::Quantity>("1"),
      decimal<model::Quantity>("1"),
      decimal<model::Notional>("10"),
  };
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// The baseline intentionally enables observation while leaving the independent execution route off.
configuration::StartupConfigurationParams reference_configuration_params() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the canonical identities once so every dependent fixture section reuses exact values.
  const auto firm_id = id<model::FirmId>("firm.aegis-lab");
  const auto desk_id = id<model::DeskId>("desk.digital-assets");
  const auto bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  const auto strategy_id = id<model::StrategyId>("strategy.deterministic-reference");
  const auto venue_id = id<model::VenueId>("deribit");
  const auto account_id = id<model::LogicalAccountId>("account.deribit-testnet-aegis");
  const auto instrument_id = id<model::InstrumentId>("BTC-USD-PERPETUAL");

  // ++++++++++++++++++++++++++++++++++++++++
  // Assemble the accepted configuration with observation enabled and execution disabled.
  return configuration::StartupConfigurationParams{
      model::ConfigurationRevision::initial(),
      model::OrganizationRevision::initial(),
      {organization::Firm{firm_id}},
      {organization::Desk{desk_id, firm_id}},
      {organization::BotRegistration{bot_id, desk_id, strategy_id}},
      model::StrategyConfigurationRevision::initial(),
      {configuration::BotStrategySettings{bot_id, strategy_id,
                                          configuration::StrategyMode::ObserveOnly}},
      {configuration::VenueDefinition{venue_id, configuration::VenueEnvironment::Testnet}},
      {configuration::LogicalAccountVenueBinding{account_id, firm_id, venue_id}},
      {reference_metadata()},
      model::SubscriptionRevision::initial(),
      {market_data::Subscription{
          id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book"), bot_id, venue_id,
          instrument_id, market_data::SubscriptionChannel::OrderBook}},
      model::RouteRevision::initial(),
      {execution::ExecutionRoute{id<model::RouteId>("route.deribit-testnet-btc-perpetual"), bot_id,
                                 venue_id, account_id, instrument_id,
                                 execution::ExecutionRouteState::Disabled}},
  };

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Extend the baseline with an independently owned bot, account, and route at the shared venue.
configuration::StartupConfigurationParams two_firm_configuration_params() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Start from the accepted baseline so the extension proves only peer-firm isolation.
  auto params = reference_configuration_params();

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the subsidiary-owned identities while deliberately reusing the shared venue/instrument.
  const auto firm_id = id<model::FirmId>("firm.aegis-subsidiary");
  const auto desk_id = id<model::DeskId>("desk.subsidiary-digital-assets");
  const auto bot_id = id<model::BotId>("bot.subsidiary-reference");
  const auto strategy_id = id<model::StrategyId>("strategy.subsidiary-reference");
  const auto venue_id = id<model::VenueId>("deribit");
  const auto account_id = id<model::LogicalAccountId>("account.deribit-testnet-subsidiary");
  const auto instrument_id = id<model::InstrumentId>("BTC-USD-PERPETUAL");

  // ++++++++++++++++++++++++++++++++++++++++
  // Add a complete ownership chain so no subsidiary object depends on the baseline firm.
  params.firms.push_back(organization::Firm{firm_id});
  params.desks.push_back(organization::Desk{desk_id, firm_id});
  params.bots.push_back(organization::BotRegistration{bot_id, desk_id, strategy_id});
  params.strategy_settings.push_back(configuration::BotStrategySettings{
      bot_id, strategy_id, configuration::StrategyMode::ObserveOnly});
  params.logical_accounts.push_back(
      configuration::LogicalAccountVenueBinding{account_id, firm_id, venue_id});
  params.routes.push_back(execution::ExecutionRoute{
      id<model::RouteId>("route.deribit-testnet-subsidiary-btc-perpetual"), bot_id, venue_id,
      account_id, instrument_id, execution::ExecutionRouteState::Disabled});
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::test_support
