// Purpose: prove subscriptions are canonical data grants, not execution permissions.

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
// Build the valid organization on which every observation grant depends.
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
// Build one canonical order-book observation grant for the reference bot and instrument.
[[nodiscard]] market_data::Subscription
create_subscription_or_throw(std::string_view subscription_id) {
  return market_data::Subscription{
      parse_identifier_or_throw<model::SubscriptionId>(subscription_id),
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      market_data::SubscriptionChannel::OrderBook};
}

// --------------------------------------------------------
// Supply the complete venue-instrument catalog required by the reference grant.
[[nodiscard]] std::vector<market_data::VenueInstrumentPair>
create_venue_instrument_catalog_or_throw() {
  return {{parse_identifier_or_throw<model::VenueId>("deribit"),
           parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")}};
}

// --------------------------------------------------------
// Accepted cases lock canonical grants while preserving the deliberate validity of an empty
// section.
TEST_CASE("subscriptions are sorted by ID and preserve explicit order-book grants",
          "[market_data][subscription]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Author two valid grants out of order against a complete dependency catalog.
  const auto organization = create_reference_organization_or_throw();
  auto second = create_subscription_or_throw("subscription.z-book");
  second.instrument_id = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  auto first = create_subscription_or_throw("subscription.a-book");

  const auto result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(), {second, first}, organization,
      {{parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")},
       {parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")}});

  // ++++++++++++++++++++++++++++++++++++++++
  // Verify canonical publication, revision ownership, and indexed lookup behavior.
  REQUIRE(result);
  REQUIRE(result.value().subscriptions().size() == 2U);
  CHECK(result.value().subscriptions()[0U].id ==
        parse_identifier_or_throw<model::SubscriptionId>("subscription.a-book"));
  CHECK(result.value().subscriptions()[1U].id ==
        parse_identifier_or_throw<model::SubscriptionId>("subscription.z-book"));
  CHECK(result.value().revision() == model::SubscriptionRevision::create_initial());
  CHECK(result.value().find_subscription(
            parse_identifier_or_throw<model::SubscriptionId>("subscription.a-book")) != nullptr);
  CHECK(result.value().find_subscription(
            parse_identifier_or_throw<model::SubscriptionId>("subscription.missing")) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Observation is optional, so an explicitly empty section remains a valid configuration.
TEST_CASE("an empty subscription section is valid", "[market_data][subscription]") {
  const auto organization = create_reference_organization_or_throw();
  const auto result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(), {}, organization, {});

  REQUIRE(result);
  CHECK(result.value().subscriptions().empty());
}

// --------------------------------------------------------
// Subscription IDs and observation meaning are independent uniqueness contracts.
TEST_CASE("subscription IDs and semantic keys must both be unique", "[market_data][subscription]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Duplicate authored identifiers fail at the canonical duplicate position.
  const auto organization = create_reference_organization_or_throw();
  const auto existing = create_subscription_or_throw("subscription.a-book");

  const auto duplicate_id =
      market_data::SubscriptionConfiguration::create_subscription_configuration(
          model::SubscriptionRevision::create_initial(), {existing, existing}, organization,
          create_venue_instrument_catalog_or_throw());
  REQUIRE_FALSE(duplicate_id);
  CHECK(duplicate_id.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "subscriptions.id", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Distinct IDs cannot disguise duplicate bot/venue/instrument/channel meaning.
  const auto duplicate_key =
      market_data::SubscriptionConfiguration::create_subscription_configuration(
          model::SubscriptionRevision::create_initial(),
          {create_subscription_or_throw("subscription.z-book"),
           create_subscription_or_throw("subscription.a-book")},
          organization, create_venue_instrument_catalog_or_throw());
  REQUIRE_FALSE(duplicate_key);
  CHECK(duplicate_key.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "subscriptions.semantic_key", 1U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Permission catalogs must fail with canonical indices before any subscription is evaluated.
TEST_CASE("subscription dependency pairs reject duplicates after canonical sorting",
          "[market_data][subscription]") {
  const auto organization = create_reference_organization_or_throw();
  const market_data::VenueInstrumentPair duplicate{
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")};

  const auto result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(),
      {create_subscription_or_throw("subscription.a-book")}, organization,
      {duplicate,
       {parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")},
       duplicate});

  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::DuplicateIdentifier,
                                            "subscriptions.known_venue_instruments", 2U));
}

// --------------------------------------------------------
// Reference, relationship, and channel failures remain distinct for actionable startup diagnostics.
TEST_CASE("subscriptions reject every dangling reference", "[market_data][subscription]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A grant cannot name a bot absent from the validated organization.
  const auto organization = create_reference_organization_or_throw();
  const auto pairs = create_venue_instrument_catalog_or_throw();

  auto unknown_bot = create_subscription_or_throw("subscription.unknown-bot");
  unknown_bot.bot_id = parse_identifier_or_throw<model::BotId>("bot.unknown");
  const auto bot_result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(), {unknown_bot}, organization, pairs);
  REQUIRE_FALSE(bot_result);
  CHECK(bot_result.error().context.field == "subscriptions.bot_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // A grant cannot name a venue absent from the known pair catalog.
  auto unknown_venue = create_subscription_or_throw("subscription.unknown-venue");
  unknown_venue.venue_id = parse_identifier_or_throw<model::VenueId>("unknown");
  const auto venue_result =
      market_data::SubscriptionConfiguration::create_subscription_configuration(
          model::SubscriptionRevision::create_initial(), {unknown_venue}, organization, pairs);
  REQUIRE_FALSE(venue_result);
  CHECK(venue_result.error().context.field == "subscriptions.venue_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // A grant cannot name an instrument absent from the known pair catalog.
  auto unknown_instrument = create_subscription_or_throw("subscription.unknown-instrument");
  unknown_instrument.instrument_id =
      parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto instrument_result =
      market_data::SubscriptionConfiguration::create_subscription_configuration(
          model::SubscriptionRevision::create_initial(), {unknown_instrument}, organization, pairs);
  REQUIRE_FALSE(instrument_result);
  CHECK(instrument_result.error().context.field == "subscriptions.instrument_id");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Independently known venues and instruments do not authorize an undeclared pair.
TEST_CASE("subscriptions require an explicitly known venue and instrument pair",
          "[market_data][subscription]") {
  const auto organization = create_reference_organization_or_throw();
  auto mismatched = create_subscription_or_throw("subscription.mismatched-pair");
  mismatched.instrument_id = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");

  const auto result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(), {mismatched}, organization,
      {{parse_identifier_or_throw<model::VenueId>("deribit"),
        parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL")},
       {parse_identifier_or_throw<model::VenueId>("coinbase"),
        parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL")}});

  REQUIRE_FALSE(result);
  CHECK(result.error() ==
        model::DomainError::create_at_index(model::DomainErrorCode::InvalidRelationship,
                                            "subscriptions.venue_instrument", 0U));
}

// --------------------------------------------------------
// Unassigned channel values fail closed instead of silently acquiring observation authority.
TEST_CASE("M1 accepts only the assigned order-book channel", "[market_data][subscription]") {
  const auto organization = create_reference_organization_or_throw();
  auto invalid = create_subscription_or_throw("subscription.invalid-channel");
  invalid.channel = static_cast<market_data::SubscriptionChannel>(std::uint8_t{99U});

  const auto result = market_data::SubscriptionConfiguration::create_subscription_configuration(
      model::SubscriptionRevision::create_initial(), {invalid}, organization,
      create_venue_instrument_catalog_or_throw());

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::create_at_index(model::DomainErrorCode::InvalidValue,
                                                              "subscriptions.channel", 0U));
}

// --------------------------------------------------------

} // namespace
