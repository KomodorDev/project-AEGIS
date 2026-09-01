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

// --------------------------------------------------------
// Invalid literals fail fast because they indicate a broken typed test fixture, not a domain case.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build the single-firm organization used by ordinary route-validation cases.
[[nodiscard]] organization::Organization create_reference_organization_or_throw() {
  auto result = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(),
      {organization::Firm{parse_identifier_or_throw<model::FirmId>("firm.aegis-lab")}},
      {organization::Desk{parse_identifier_or_throw<model::DeskId>("desk.digital-assets"),
                          parse_identifier_or_throw<model::FirmId>("firm.aegis-lab")}},
      {organization::BotRegistration{
          parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
          parse_identifier_or_throw<model::DeskId>("desk.digital-assets"),
          parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference")}});
  if (!result) {
    throw std::logic_error{"invalid organization in test fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Both owners are valid peer firms, ensuring ownership rejection is not a dangling-reference
// shortcut.
[[nodiscard]] organization::Organization create_two_firm_organization_or_throw() {
  auto result = organization::Organization::create_organization(
      model::OrganizationRevision::create_initial(),
      {organization::Firm{parse_identifier_or_throw<model::FirmId>("firm.aegis-lab")},
       organization::Firm{parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary")}},
      {organization::Desk{parse_identifier_or_throw<model::DeskId>("desk.digital-assets"),
                          parse_identifier_or_throw<model::FirmId>("firm.aegis-lab")},
       organization::Desk{parse_identifier_or_throw<model::DeskId>("desk.subsidiary"),
                          parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary")}},
      {organization::BotRegistration{
           parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
           parse_identifier_or_throw<model::DeskId>("desk.digital-assets"),
           parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference")},
       organization::BotRegistration{
           parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"),
           parse_identifier_or_throw<model::DeskId>("desk.subsidiary"),
           parse_identifier_or_throw<model::StrategyId>("strategy.subsidiary-reference")}});
  if (!result) {
    throw std::logic_error{"invalid two-firm organization in test fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build one disabled reference route whose ID can vary without changing authorization meaning.
[[nodiscard]] execution::ExecutionRoute create_execution_route_or_throw(std::string_view route_id) {
  return execution::ExecutionRoute{
      parse_identifier_or_throw<model::RouteId>(route_id),
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::ExecutionRouteState::Disabled};
}

// --------------------------------------------------------
// Supply the complete venue-instrument dependency catalog for the reference route.
[[nodiscard]] std::vector<execution::VenueInstrumentPair>
create_venue_instrument_catalog_or_throw() {
  return {{parse_identifier_or_throw<model::VenueId>("deribit"),
           parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")}};
}

// --------------------------------------------------------
// Supply the firm-owned account binding required by the reference route.
[[nodiscard]] std::vector<execution::LogicalAccountVenueBinding>
create_account_binding_catalog_or_throw() {
  return {{parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
           parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"),
           parse_identifier_or_throw<model::VenueId>("deribit")}};
}

// --------------------------------------------------------
// Accepted cases lock canonical publication and prove subscriptions cannot create route authority.
TEST_CASE("the reference route is explicit, canonical, and disabled by default",
          "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Author two valid routes out of order with deliberately different state and instrument.
  const auto organization = create_reference_organization_or_throw();
  auto second = create_execution_route_or_throw("route.z-disabled");
  second.state = execution::ExecutionRouteState::Enabled;
  const auto first = create_execution_route_or_throw("route.a-disabled");
  second.instrument_id = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");

  const auto result = execution::ExecutionRouteConfiguration::create_execution_route_configuration(
      model::RouteRevision::create_initial(), {second, first}, organization,
      {{parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")},
       {parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")}},
      create_account_binding_catalog_or_throw());

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify canonical publication, explicit state, revision ownership, and indexed lookup.
  REQUIRE(result);
  REQUIRE(result.value().routes().size() == 2U);
  CHECK(result.value().routes()[0U].id ==
        parse_identifier_or_throw<model::RouteId>("route.a-disabled"));
  CHECK_FALSE(result.value().routes()[0U].is_enabled());
  CHECK(result.value().routes()[1U].is_enabled());
  CHECK(result.value().revision() == model::RouteRevision::create_initial());
  CHECK(result.value().find_route(parse_identifier_or_throw<model::RouteId>("route.a-disabled")) !=
        nullptr);
  CHECK(result.value().find_route(parse_identifier_or_throw<model::RouteId>("route.missing")) ==
        nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A valid observation grant leaves the independently configured execution route set empty.
TEST_CASE("a subscription never creates or enables an execution route", "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish an accepted subscription for the same bot, venue, and instrument.
  const auto organization = create_reference_organization_or_throw();
  const auto subscriptions =
      market_data::SubscriptionConfiguration::create_subscription_configuration(
          model::SubscriptionRevision::create_initial(),
          {market_data::Subscription{
              parse_identifier_or_throw<model::SubscriptionId>(
                  "subscription.deribit-btc-perpetual-book"),
              parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
              parse_identifier_or_throw<model::VenueId>("deribit"),
              parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
              market_data::SubscriptionChannel::OrderBook}},
          organization, create_venue_instrument_catalog_or_throw());
  REQUIRE(subscriptions);

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove route publication remains empty without an explicit execution grant.
  const auto routes = execution::ExecutionRouteConfiguration::create_execution_route_configuration(
      model::RouteRevision::create_initial(), {}, organization,
      create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE(routes);
  CHECK(routes.value().routes().empty());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Route IDs and authorization meaning are independently unique; state cannot duplicate a grant.
TEST_CASE("route IDs and semantic keys must both be unique", "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject an authored route identifier repeated at the canonical duplicate position.
  const auto organization = create_reference_organization_or_throw();
  const auto existing = create_execution_route_or_throw("route.a");

  const auto duplicate_id =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {existing, existing}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(duplicate_id);
  CHECK(duplicate_id.error() == model::DomainError::create_at_index(
                                    model::DomainErrorCode::DuplicateIdentifier, "routes.id", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
  // State differences cannot disguise duplicate bot/venue/account/instrument authorization.
  auto same_key = create_execution_route_or_throw("route.z");
  same_key.state = execution::ExecutionRouteState::Enabled;
  const auto duplicate_key =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {same_key, existing}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(duplicate_key);
  CHECK(duplicate_key.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "routes.semantic_key", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Permission catalogs must fail with canonical indices before any individual route is evaluated.
TEST_CASE("route dependency catalogs reject duplicates after canonical sorting",
          "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject a duplicate venue-instrument dependency at its canonical sorted position.
  const auto organization = create_reference_organization_or_throw();
  const execution::VenueInstrumentPair duplicate_instrument{
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")};

  const auto duplicate_instrument_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {create_execution_route_or_throw("route.a")},
          organization,
          {duplicate_instrument,
           {parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")},
           duplicate_instrument},
          create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(duplicate_instrument_result);
  CHECK(duplicate_instrument_result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "routes.known_venue_instruments", 2U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject one account ID with conflicting owners before lookup could choose either binding.
  const execution::LogicalAccountVenueBinding duplicate_account{
      parse_identifier_or_throw<model::LogicalAccountId>("account.z"),
      parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"),
      parse_identifier_or_throw<model::VenueId>("deribit")};
  auto conflicting_owner = duplicate_account;
  conflicting_owner.firm_id = parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary");
  const auto duplicate_account_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {create_execution_route_or_throw("route.a")},
          organization, create_venue_instrument_catalog_or_throw(),
          {duplicate_account,
           {parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
            parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"),
            parse_identifier_or_throw<model::VenueId>("deribit")},
           conflicting_owner});
  REQUIRE_FALSE(duplicate_account_result);
  CHECK(duplicate_account_result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "routes.known_account_bindings", 2U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Reference, relationship, and enum failures remain distinct so configuration defects are
// actionable.
TEST_CASE("execution routes reject every dangling reference", "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A route cannot name a bot absent from the validated organization.
  const auto organization = create_reference_organization_or_throw();

  auto unknown_bot = create_execution_route_or_throw("route.unknown-bot");
  unknown_bot.bot_id = parse_identifier_or_throw<model::BotId>("bot.unknown");
  const auto bot_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {unknown_bot}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(bot_result);
  CHECK(bot_result.error().context.field == "routes.bot_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // A route cannot name a venue absent from its dependency catalogs.
  auto unknown_venue = create_execution_route_or_throw("route.unknown-venue");
  unknown_venue.venue_id = parse_identifier_or_throw<model::VenueId>("unknown");
  const auto venue_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {unknown_venue}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(venue_result);
  CHECK(venue_result.error().context.field == "routes.venue_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // A route cannot name a logical account absent from the binding catalog.
  auto unknown_account = create_execution_route_or_throw("route.unknown-account");
  unknown_account.logical_account_id =
      parse_identifier_or_throw<model::LogicalAccountId>("account.unknown");
  const auto account_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {unknown_account}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(account_result);
  CHECK(account_result.error().context.field == "routes.logical_account_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // A route cannot name an instrument absent from its dependency catalogs.
  auto unknown_instrument = create_execution_route_or_throw("route.unknown-instrument");
  unknown_instrument.instrument_id =
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto instrument_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {unknown_instrument}, organization,
          create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(instrument_result);
  CHECK(instrument_result.error().context.field == "routes.instrument_id");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Known objects confer authority only through explicitly declared pair and ownership relations.
TEST_CASE("routes require an explicit venue-instrument pair and account-venue binding",
          "[execution][route]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Independently known venues and instruments cannot authorize an undeclared pair.
  const auto organization = create_reference_organization_or_throw();
  auto mismatched_instrument = create_execution_route_or_throw("route.mismatched-instrument");
  mismatched_instrument.instrument_id =
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto pair_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(), {mismatched_instrument}, organization,
          {{parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")},
           {parse_identifier_or_throw<model::VenueId>("coinbase"),
            parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")}},
          create_account_binding_catalog_or_throw());
  REQUIRE_FALSE(pair_result);
  CHECK(pair_result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "routes.venue_instrument", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
  // An account bound to another venue cannot authorize this route.
  const auto account_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(),
          {create_execution_route_or_throw("route.mismatched-account")}, organization,
          create_venue_instrument_catalog_or_throw(),
          {{parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
            parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"),
            parse_identifier_or_throw<model::VenueId>("coinbase")}});
  REQUIRE_FALSE(account_result);
  CHECK(account_result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "routes.account_venue", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact account_firm failure proves the public route factory owns the authorization decision.
  const auto multi_firm = create_two_firm_organization_or_throw();
  const auto firm_result =
      execution::ExecutionRouteConfiguration::create_execution_route_configuration(
          model::RouteRevision::create_initial(),
          {create_execution_route_or_throw("route.mismatched-firm")}, multi_firm,
          create_venue_instrument_catalog_or_throw(),
          {{parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
            parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary"),
            parse_identifier_or_throw<model::VenueId>("deribit")}});
  REQUIRE_FALSE(firm_result);
  CHECK(firm_result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "routes.account_firm", 0U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Unassigned route-state values fail closed instead of silently acquiring execution authority.
TEST_CASE("execution routes reject unassigned state values", "[execution][route]") {
  const auto organization = create_reference_organization_or_throw();
  auto invalid = create_execution_route_or_throw("route.invalid-state");
  invalid.state = static_cast<execution::ExecutionRouteState>(std::uint8_t{99U});

  const auto result = execution::ExecutionRouteConfiguration::create_execution_route_configuration(
      model::RouteRevision::create_initial(), {invalid}, organization,
      create_venue_instrument_catalog_or_throw(), create_account_binding_catalog_or_throw());

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::create_at_index(model::DomainErrorCode::InvalidValue,
                                                              "routes.state", 0U));
}

// --------------------------------------------------------

} // namespace
