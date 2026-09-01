// Purpose: construct frozen M1/M2 startup fixtures plus separately enabled M3 configuration and
// complete fixed-risk authoring values without changing earlier golden bytes.

#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::test_support {
namespace {

// --------------------------------------------------------
// Invalid literals are fixture-authoring defects, so typed identifier values fail immediately.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto result = Identifier::parse_identifier(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in reference configuration"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid decimal literals are fixture-authoring defects and use the same fail-fast policy.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view text) {
  auto result = Decimal::parse_ascii(text);
  if (!result) {
    throw std::logic_error{"invalid decimal in reference configuration"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// These are the assigned inverse Deribit semantics consumed by canonical startup and trace
// evidence.
[[nodiscard]] model::InstrumentMetadataParams create_reference_metadata_or_throw() {
  return model::InstrumentMetadataParams{
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial(),
      "BTC",
      "USD",
      "BTC",
      model::ContractStyle::Inverse,
      model::QuantityUnit::Contracts,
      model::ContractMultiplierUnit::QuoteCurrencyPerContract,
      1U,
      0U,
      parse_decimal_or_throw<model::Price>("0.5"),
      parse_decimal_or_throw<model::Quantity>("1"),
      parse_decimal_or_throw<model::Quantity>("1"),
      parse_decimal_or_throw<model::Notional>("10"),
  };
}

// --------------------------------------------------------
// Spell one heterogeneous scope subject from the same sealed startup authority used by risk.
[[nodiscard]] std::string derive_risk_scope_subject_or_throw(
    const execution::ExecutionRoute& route, const organization::BotAttribution& attribution,
    const model::InstrumentMetadata& metadata, risk::RiskScopeKind scope) {
  switch (scope) {
  case risk::RiskScopeKind::Bot:
    return std::string{attribution.bot_id.value()};
  case risk::RiskScopeKind::Desk:
    return std::string{attribution.desk_id.value()};
  case risk::RiskScopeKind::Firm:
    return std::string{attribution.firm_id.value()};
  case risk::RiskScopeKind::Account:
    return std::string{route.logical_account_id.value()};
  case risk::RiskScopeKind::Route:
    return std::string{route.id.value()};
  case risk::RiskScopeKind::Instrument:
    return std::string{metadata.instrument_id().value()};
  case risk::RiskScopeKind::Venue:
    return std::string{metadata.venue_id().value()};
  default:
    throw std::logic_error{"invalid M3 reference risk scope"};
  }
}

// --------------------------------------------------------
// Author one positive complete row whose aggregate headroom safely retains 10,000 open orders.
[[nodiscard]] risk::RiskLimitSetParams create_risk_limit_row_or_throw(
    const execution::ExecutionRoute& route, const organization::BotAttribution& attribution,
    const model::InstrumentMetadata& metadata, risk::RiskScopeKind scope) {
  return risk::RiskLimitSetParams{
      attribution.firm_id,
      scope,
      derive_risk_scope_subject_or_throw(route, attribution, metadata, scope),
      metadata.instrument_id(),
      std::string{metadata.quote_currency()},
      parse_decimal_or_throw<model::Quantity>("1000000"),
      parse_decimal_or_throw<model::Notional>("10000000"),
      20'000U,
      parse_decimal_or_throw<model::Notional>("100000000"),
      parse_decimal_or_throw<model::Quantity>("1000000"),
      parse_decimal_or_throw<model::Notional>("100000000"),
  };
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// The baseline intentionally enables observation while leaving the independent execution route off.
configuration::StartupConfigurationParams create_reference_configuration_params_or_throw() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the canonical identities once so every dependent fixture section reuses exact values.
  const auto firm_id = parse_identifier_or_throw<model::FirmId>("firm.aegis-lab");
  const auto desk_id = parse_identifier_or_throw<model::DeskId>("desk.digital-assets");
  const auto bot_id =
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference");
  const auto strategy_id =
      parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference");
  const auto venue_id = parse_identifier_or_throw<model::VenueId>("deribit");
  const auto account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis");
  const auto instrument_id = parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");

  // ++++++++++++++++++++++++++++++++++++++++
  // Assemble the accepted configuration with observation enabled and execution disabled.
  return configuration::StartupConfigurationParams{
      model::ConfigurationRevision::create_initial(),
      model::OrganizationRevision::create_initial(),
      {organization::Firm{firm_id}},
      {organization::Desk{desk_id, firm_id}},
      {organization::BotRegistration{bot_id, desk_id, strategy_id}},
      model::StrategyConfigurationRevision::create_initial(),
      {configuration::BotStrategySettings{bot_id, strategy_id,
                                          configuration::StrategyMode::ObserveOnly}},
      {configuration::VenueDefinition{venue_id, configuration::VenueEnvironment::Testnet}},
      {configuration::LogicalAccountVenueBinding{account_id, firm_id, venue_id}},
      {create_reference_metadata_or_throw()},
      model::SubscriptionRevision::create_initial(),
      {market_data::Subscription{parse_identifier_or_throw<model::SubscriptionId>(
                                     "subscription.deribit-btc-perpetual-book"),
                                 bot_id, venue_id, instrument_id,
                                 market_data::SubscriptionChannel::OrderBook}},
      model::RouteRevision::create_initial(),
      {execution::ExecutionRoute{
          parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"), bot_id,
          venue_id, account_id, instrument_id, execution::ExecutionRouteState::Disabled}},
  };

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Extend the baseline with an independently owned bot, account, and route at the shared venue.
configuration::StartupConfigurationParams create_two_firm_configuration_params_or_throw() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Start from the accepted baseline so the extension proves only peer-firm isolation.
  auto params = create_reference_configuration_params_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse the subsidiary-owned identities while deliberately reusing the shared venue/instrument.
  const auto firm_id = parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary");
  const auto desk_id = parse_identifier_or_throw<model::DeskId>("desk.subsidiary-digital-assets");
  const auto bot_id = parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference");
  const auto strategy_id =
      parse_identifier_or_throw<model::StrategyId>("strategy.subsidiary-reference");
  const auto venue_id = parse_identifier_or_throw<model::VenueId>("deribit");
  const auto account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-subsidiary");
  const auto instrument_id = parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");

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
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-subsidiary-btc-perpetual"),
      bot_id, venue_id, account_id, instrument_id, execution::ExecutionRouteState::Disabled});
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Derive the separately fingerprinted M3 fixture while preserving every accepted M1/M2 byte.
configuration::StartupConfigurationParams
create_m3_enabled_two_firm_configuration_params_or_throw() {

  // ++++++++++++++++++++++++++++++++++++++++
  // Enable only already explicit owner-validated routes; subscriptions remain observation-only.
  auto params = create_two_firm_configuration_params_or_throw();
  for (auto& route : params.routes) {
    route.state = execution::ExecutionRouteState::Enabled;
  }
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Derive complete seven-scope rows and metadata provenance from every enabled sealed route.
risk::RiskPolicyParams create_m3_reference_risk_policy_params_or_throw(
    const configuration::StartupConfiguration& configuration) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Gather route-derived authority only; missing projections are fixture-authoring failures.
  constexpr std::array scopes{
      risk::RiskScopeKind::Bot,     risk::RiskScopeKind::Desk,  risk::RiskScopeKind::Firm,
      risk::RiskScopeKind::Account, risk::RiskScopeKind::Route, risk::RiskScopeKind::Instrument,
      risk::RiskScopeKind::Venue,
  };
  std::vector<configuration::InstrumentMetadataRevisionEntry> metadata_revisions;
  std::vector<risk::RiskLimitSetParams> limits;
  for (const auto& route : configuration.routes().routes()) {
    if (!route.is_enabled()) {
      continue;
    }
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"incomplete M3 reference risk authority"};
    }
    metadata_revisions.push_back(configuration::InstrumentMetadataRevisionEntry{
        metadata->venue_id(), metadata->instrument_id(), metadata->revision()});
    for (const auto scope : scopes) {
      limits.push_back(create_risk_limit_row_or_throw(route, *attribution, *metadata, scope));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonicalize shared metadata and shared scope keys before policy validation rejects duplicates.
  std::sort(metadata_revisions.begin(), metadata_revisions.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.venue_id, left.instrument_id) <
                     std::tie(right.venue_id, right.instrument_id);
            });
  metadata_revisions.erase(std::unique(metadata_revisions.begin(), metadata_revisions.end()),
                           metadata_revisions.end());
  const auto limit_key = [](const auto& row) {
    return std::tuple{row.firm_id.value(), static_cast<std::uint8_t>(row.scope),
                      std::string_view{row.scope_subject}, row.instrument_id.value(),
                      std::string_view{row.quote_currency}};
  };
  std::sort(limits.begin(), limits.end(), [&](const auto& left, const auto& right) {
    return limit_key(left) < limit_key(right);
  });
  limits.erase(std::unique(limits.begin(), limits.end(),
                           [&](const auto& left, const auto& right) {
                             return limit_key(left) == limit_key(right);
                           }),
               limits.end());

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind exact startup revisions and conservative away-from-zero quote-notional rounding.
  return risk::RiskPolicyParams{
      model::RiskPolicyRevision::create_initial(),
      configuration.fingerprint(),
      configuration.revision(),
      configuration.organization().revision(),
      configuration.routes().revision(),
      2U,
      model::RoundingMode::AwayFromZero,
      std::move(metadata_revisions),
      std::move(limits),
  };

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Tighten only the baseline bot's first canonical quantity limit for the paired reject workload.
risk::RiskPolicyParams create_m3_rejecting_risk_policy_params_or_throw(
    const configuration::StartupConfiguration& configuration) {
  auto params = create_m3_reference_risk_policy_params_or_throw(configuration);
  const auto baseline_bot =
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference");
  const auto found =
      std::find_if(params.limit_sets.begin(), params.limit_sets.end(), [&](const auto& row) {
        return row.scope == risk::RiskScopeKind::Bot && row.scope_subject == baseline_bot.value();
      });
  if (found == params.limit_sets.end()) {
    throw std::logic_error{"missing baseline bot risk limit"};
  }
  found->maximum_single_order_quantity = parse_decimal_or_throw<model::Quantity>("1");
  return params;
}

// --------------------------------------------------------

} // namespace aegis::test_support
