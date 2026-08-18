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

// Invalid literals fail fast because they indicate a broken typed test fixture, not a domain case.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in test fixture"};
  }
  return std::move(parsed).value();
}

// The base fixtures describe one valid observation grant and its complete dependency catalog.
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

[[nodiscard]] market_data::Subscription subscription(std::string_view subscription_id) {
  return market_data::Subscription{
      id<model::SubscriptionId>(subscription_id),
      id<model::BotId>("bot.deribit-btc-perpetual-reference"), id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"), market_data::SubscriptionChannel::OrderBook};
}

[[nodiscard]] std::vector<market_data::VenueInstrumentPair> venue_instruments() {
  return {{id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")}};
}

// Accepted cases lock canonical grants while preserving the deliberate validity of an empty
// section.
TEST_CASE("subscriptions are sorted by ID and preserve explicit order-book grants",
          "[market_data][subscription]") {
  const auto organization = reference_organization();
  auto second = subscription("subscription.z-book");
  second.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  auto first = subscription("subscription.a-book");

  const auto result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {second, first}, organization,
      {{id<model::VenueId>("deribit"), id<model::InstrumentId>("ETH-USD-PERPETUAL")},
       {id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")}});

  REQUIRE(result);
  REQUIRE(result.value().subscriptions().size() == 2U);
  CHECK(result.value().subscriptions()[0U].id == id<model::SubscriptionId>("subscription.a-book"));
  CHECK(result.value().subscriptions()[1U].id == id<model::SubscriptionId>("subscription.z-book"));
  CHECK(result.value().revision() == model::SubscriptionRevision::initial());
  CHECK(result.value().find(id<model::SubscriptionId>("subscription.a-book")) != nullptr);
  CHECK(result.value().find(id<model::SubscriptionId>("subscription.missing")) == nullptr);
}

TEST_CASE("an empty subscription section is valid", "[market_data][subscription]") {
  const auto organization = reference_organization();
  const auto result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {}, organization, {});

  REQUIRE(result);
  CHECK(result.value().subscriptions().empty());
}

// Subscription IDs and observation meaning are independent uniqueness contracts.
TEST_CASE("subscription IDs and semantic keys must both be unique", "[market_data][subscription]") {
  const auto organization = reference_organization();
  const auto existing = subscription("subscription.a-book");

  const auto duplicate_id = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {existing, existing}, organization,
      venue_instruments());
  REQUIRE_FALSE(duplicate_id);
  CHECK(duplicate_id.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                     "subscriptions.id", 1U));

  const auto duplicate_key = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(),
      {subscription("subscription.z-book"), subscription("subscription.a-book")}, organization,
      venue_instruments());
  REQUIRE_FALSE(duplicate_key);
  CHECK(duplicate_key.error() ==
        model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                     "subscriptions.semantic_key", 1U));
}

TEST_CASE("subscription dependency pairs reject duplicates after canonical sorting",
          "[market_data][subscription]") {
  const auto organization = reference_organization();
  const market_data::VenueInstrumentPair duplicate{id<model::VenueId>("deribit"),
                                                   id<model::InstrumentId>("ETH-USD-PERPETUAL")};

  const auto result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {subscription("subscription.a-book")}, organization,
      {duplicate,
       {id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")},
       duplicate});

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_index(model::DomainErrorCode::DuplicateIdentifier,
                                                       "subscriptions.known_venue_instruments",
                                                       2U));
}

// Reference, relationship, and channel failures remain distinct for actionable startup diagnostics.
TEST_CASE("subscriptions reject every dangling reference", "[market_data][subscription]") {
  const auto organization = reference_organization();
  const auto pairs = venue_instruments();

  auto unknown_bot = subscription("subscription.unknown-bot");
  unknown_bot.bot_id = id<model::BotId>("bot.unknown");
  const auto bot_result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {unknown_bot}, organization, pairs);
  REQUIRE_FALSE(bot_result);
  CHECK(bot_result.error().context.field == "subscriptions.bot_id");

  auto unknown_venue = subscription("subscription.unknown-venue");
  unknown_venue.venue_id = id<model::VenueId>("unknown");
  const auto venue_result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {unknown_venue}, organization, pairs);
  REQUIRE_FALSE(venue_result);
  CHECK(venue_result.error().context.field == "subscriptions.venue_id");

  auto unknown_instrument = subscription("subscription.unknown-instrument");
  unknown_instrument.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto instrument_result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {unknown_instrument}, organization, pairs);
  REQUIRE_FALSE(instrument_result);
  CHECK(instrument_result.error().context.field == "subscriptions.instrument_id");
}

TEST_CASE("subscriptions require an explicitly known venue and instrument pair",
          "[market_data][subscription]") {
  const auto organization = reference_organization();
  auto mismatched = subscription("subscription.mismatched-pair");
  mismatched.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");

  const auto result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {mismatched}, organization,
      {{id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL")},
       {id<model::VenueId>("coinbase"), id<model::InstrumentId>("ETH-USD-PERPETUAL")}});

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                                       "subscriptions.venue_instrument", 0U));
}

TEST_CASE("M1 accepts only the assigned order-book channel", "[market_data][subscription]") {
  const auto organization = reference_organization();
  auto invalid = subscription("subscription.invalid-channel");
  invalid.channel = static_cast<market_data::SubscriptionChannel>(std::uint8_t{99U});

  const auto result = market_data::SubscriptionConfiguration::create(
      model::SubscriptionRevision::initial(), {invalid}, organization, venue_instruments());

  REQUIRE_FALSE(result);
  CHECK(result.error() == model::DomainError::at_index(model::DomainErrorCode::InvalidValue,
                                                       "subscriptions.channel", 0U));
}

} // namespace
