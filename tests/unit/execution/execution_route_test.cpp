// Purpose: prove execution grants are canonical, dependency-checked, and independent of market-data
// subscriptions without selecting or transmitting orders.

#include "aegis/execution/execution_route.hpp"
#include "aegis/market_data/subscription.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// Invalid literals fail fast because they indicate a broken typed test fixture, not a domain case.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in test fixture"};
  }
  return std::move(parsed).value();
}

// The base fixtures describe one valid disabled route and its complete dependency catalogs.
[[nodiscard]] organization::Organization reference_organization() {
  auto result = organization::Organization::create(
      model::OrganizationRevision::initial(),
      {organization::Firm{id<model::FirmId>("firm.aegis-lab")}},
      {organization::Desk{id<model::DeskId>("desk.digital-assets"),
                          id<model::FirmId>("firm.aegis-lab")}},
      {organization::BotRegistration{id<model::BotId>("bot.deribit-btc-perpetual-reference"),
                                     id<model::DeskId>("desk.digital-assets"),
                                     id<model::StrategyId>("strategy.deterministic-reference")}});
  if (!result) {
    throw std::logic_error{"invalid organization in test fixture"};
  }
  return std::move(result).value();
}

[[nodiscard]] organization::Organization two_firm_organization() {
  auto result = organization::Organization::create(
      model::OrganizationRevision::initial(),
      {organization::Firm{id<model::FirmId>("firm.aegis-lab")},
       organization::Firm{id<model::FirmId>("firm.aegis-subsidiary")}},
      {organization::Desk{id<model::DeskId>("desk.digital-assets"),
                          id<model::FirmId>("firm.aegis-lab")},
       organization::Desk{id<model::DeskId>("desk.subsidiary"),
                          id<model::FirmId>("firm.aegis-subsidiary")}},
      {organization::BotRegistration{id<model::BotId>("bot.deribit-btc-perpetual-reference"),
                                     id<model::DeskId>("desk.digital-assets"),
                                     id<model::StrategyId>("strategy.deterministic-reference")},
       organization::BotRegistration{id<model::BotId>("bot.subsidiary-reference"),
                                     id<model::DeskId>("desk.subsidiary"),
                                     id<model::StrategyId>("strategy.subsidiary-reference")}});
  if (!result) {
    throw std::logic_error{"invalid two-firm organization in test fixture"};
  }
  return std::move(result).value();
}

[[nodiscard]] execution::ExecutionRoute route(std::string_view route_id) {
  return execution::ExecutionRoute{id<model::RouteId>(route_id),
                                   id<model::BotId>("bot.deribit-btc-perpetual-reference"),
                                   id<model::VenueId>("deribit"),
                                   id<model::LogicalAccountId>("account.deribit-testnet-aegis"),
                                   id<model::InstrumentId>("BTC-USD-PERPETUAL"),
                                   execution::ExecutionRouteState::Disabled};
}

[[nodiscard]] std::vector<execution::VenueInstrumentPair> venue_instruments() {
  return {{id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")}};
}

[[nodiscard]] std::vector<execution::LogicalAccountVenueBinding> account_bindings() {
  return {{id<model::LogicalAccountId>("account.deribit-testnet-aegis"),
           id<model::FirmId>("firm.aegis-lab"), id<model::VenueId>("deribit")}};
}

// Accepted cases lock canonical publication and prove subscriptions cannot create route authority.
TEST_CASE("the reference route is explicit, canonical, and disabled by default",
          "[execution][route]") {
  const auto organization = reference_organization();
  auto second = route("route.z-disabled");
  second.state = execution::ExecutionRouteState::Enabled;
  const auto first = route("route.a-disabled");
  second.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");

  const auto result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {second, first}, organization,
      {{id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")},
       {id<model::VenueId>("deribit"), id<model::InstrumentId>("ETH-USD-PERPETUAL")}},
      account_bindings());

  REQUIRE(result);
  REQUIRE(result.value().routes().size() == 2U);
  CHECK(result.value().routes()[0U].id == id<model::RouteId>("route.a-disabled"));
  CHECK_FALSE(result.value().routes()[0U].is_enabled());
  CHECK(result.value().routes()[1U].is_enabled());
  CHECK(result.value().revision() == model::RouteRevision::initial());
  CHECK(result.value().find(id<model::RouteId>("route.a-disabled")) != nullptr);
  CHECK(result.value().find(id<model::RouteId>("route.missing")) == nullptr);
}

TEST_CASE("a subscription never creates or enables an execution route", "[execution][route]") {
  const auto organization = reference_organization();
  const auto subscriptions = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(),
      {market_data::Subscription{
          id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book"),
          id<model::BotId>("bot.deribit-btc-perpetual-reference"), id<model::VenueId>("deribit"),
          id<model::InstrumentId>("BTC-USD-PERPETUAL"),
          market_data::SubscriptionChannel::OrderBook}},
      organization, venue_instruments());
  REQUIRE(subscriptions);

  const auto routes = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {}, organization, venue_instruments(), account_bindings());
  REQUIRE(routes);
  CHECK(routes.value().routes().empty());
}

// Route IDs and authorization meaning are independently unique; state cannot duplicate a grant.
TEST_CASE("route IDs and semantic keys must both be unique", "[execution][route]") {
  const auto organization = reference_organization();
  const auto existing = route("route.a");

  const auto duplicate_id = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {existing, existing}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(duplicate_id);
  CHECK(duplicate_id.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier, "routes.id", 1U));

  auto same_key = route("route.z");
  same_key.state = execution::ExecutionRouteState::Enabled;
  const auto duplicate_key = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {same_key, existing}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(duplicate_key);
  CHECK(duplicate_key.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                     "routes.semantic_key", 1U));
}

TEST_CASE("route dependency catalogs reject duplicates after canonical sorting",
          "[execution][route]") {
  const auto organization = reference_organization();
  const execution::VenueInstrumentPair duplicate_instrument{
      id<model::VenueId>("deribit"), id<model::InstrumentId>("ETH-USD-PERPETUAL")};

  const auto duplicate_instrument_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {route("route.a")}, organization,
      {duplicate_instrument,
       {id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")},
       duplicate_instrument},
      account_bindings());
  REQUIRE_FALSE(duplicate_instrument_result);
  CHECK(duplicate_instrument_result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                     "routes.known_venue_instruments", 2U));

  const execution::LogicalAccountVenueBinding duplicate_account{
      id<model::LogicalAccountId>("account.z"), id<model::FirmId>("firm.aegis-lab"),
      id<model::VenueId>("deribit")};
  auto conflicting_owner = duplicate_account;
  conflicting_owner.firm_id = id<model::FirmId>("firm.aegis-subsidiary");
  const auto duplicate_account_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {route("route.a")}, organization, venue_instruments(),
      {duplicate_account,
       {id<model::LogicalAccountId>("account.deribit-testnet-aegis"),
        id<model::FirmId>("firm.aegis-lab"), id<model::VenueId>("deribit")},
       conflicting_owner});
  REQUIRE_FALSE(duplicate_account_result);
  CHECK(duplicate_account_result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                     "routes.known_account_bindings", 2U));
}

// Reference, relationship, and enum failures remain distinct so configuration defects are
// actionable.
TEST_CASE("execution routes reject every dangling reference", "[execution][route]") {
  const auto organization = reference_organization();

  auto unknown_bot = route("route.unknown-bot");
  unknown_bot.bot_id = id<model::BotId>("bot.unknown");
  const auto bot_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {unknown_bot}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(bot_result);
  CHECK(bot_result.error().context.field == "routes.bot_id");

  auto unknown_venue = route("route.unknown-venue");
  unknown_venue.venue_id = id<model::VenueId>("unknown");
  const auto venue_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {unknown_venue}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(venue_result);
  CHECK(venue_result.error().context.field == "routes.venue_id");

  auto unknown_account = route("route.unknown-account");
  unknown_account.logical_account_id = id<model::LogicalAccountId>("account.unknown");
  const auto account_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {unknown_account}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(account_result);
  CHECK(account_result.error().context.field == "routes.logical_account_id");

  auto unknown_instrument = route("route.unknown-instrument");
  unknown_instrument.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto instrument_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {unknown_instrument}, organization, venue_instruments(),
      account_bindings());
  REQUIRE_FALSE(instrument_result);
  CHECK(instrument_result.error().context.field == "routes.instrument_id");
}

TEST_CASE("routes require an explicit venue-instrument pair and account-venue binding",
          "[execution][route]") {
  const auto organization = reference_organization();
  auto mismatched_instrument = route("route.mismatched-instrument");
  mismatched_instrument.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto pair_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {mismatched_instrument}, organization,
      {{id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")},
       {id<model::VenueId>("coinbase"), id<model::InstrumentId>("ETH-USD-PERPETUAL")}},
      account_bindings());
  REQUIRE_FALSE(pair_result);
  CHECK(pair_result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                     "routes.venue_instrument", 0U));

  const auto account_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {route("route.mismatched-account")}, organization,
      venue_instruments(),
      {{id<model::LogicalAccountId>("account.deribit-testnet-aegis"),
        id<model::FirmId>("firm.aegis-lab"), id<model::VenueId>("coinbase")}});
  REQUIRE_FALSE(account_result);
  CHECK(account_result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                     "routes.account_venue", 0U));

  const auto multi_firm = two_firm_organization();
  const auto firm_result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {route("route.mismatched-firm")}, multi_firm,
      venue_instruments(),
      {{id<model::LogicalAccountId>("account.deribit-testnet-aegis"),
        id<model::FirmId>("firm.aegis-subsidiary"), id<model::VenueId>("deribit")}});
  REQUIRE_FALSE(firm_result);
  CHECK(firm_result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                     "routes.account_firm", 0U));
}

TEST_CASE("execution routes reject unassigned state values", "[execution][route]") {
  const auto organization = reference_organization();
  auto invalid = route("route.invalid-state");
  invalid.state = static_cast<execution::ExecutionRouteState>(std::uint8_t{99U});

  const auto result = execution::ExecutionRouteConfiguration::create(
      model::RouteRevision::initial(), {invalid}, organization, venue_instruments(),
      account_bindings());

  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        model::DomainError::at_index(model::DomainErrorCode::InvalidValue, "routes.state", 0U));
}

} // namespace
