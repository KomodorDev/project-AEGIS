// Purpose: prove owner-local route installation, authorization precedence, canonical ordering, and
// multi-firm isolation without consulting market-data subscriptions.

#include "aegis/execution/submission_route.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid identifier text indicates a broken test fixture.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier test fixture"};
  }
  return parsed.value();
}

// --------------------------------------------------------
// Invalid decimal text indicates a broken test fixture.
template <typename Decimal> [[nodiscard]] Decimal decimal(std::string_view text) {
  auto parsed = Decimal::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid decimal test fixture"};
  }
  return parsed.value();
}

// --------------------------------------------------------
// Enable only the explicit reference route while preserving all other sealed configuration input.
[[nodiscard]] configuration::StartupConfiguration enabled_configuration() {
  auto params = test_support::two_firm_configuration_params();
  params.routes.front().state = execution::ExecutionRouteState::Enabled;
  auto created = configuration::StartupConfiguration::create(std::move(params));
  if (!created) {
    throw std::logic_error{"invalid enabled configuration fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Project the sealed catalog through only narrow route, attribution, metadata, and provenance data.
[[nodiscard]] execution::OwnerLocalRouteCatalog
catalog(const configuration::StartupConfiguration& configuration) {
  std::vector<execution::SubmissionRouteInput> inputs;
  inputs.reserve(configuration.routes().routes().size());
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const instrument =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || instrument == nullptr) {
      throw std::logic_error{"incomplete route projection fixture"};
    }
    inputs.push_back(execution::SubmissionRouteInput{route, *attribution, *instrument});
  }
  auto created = execution::OwnerLocalRouteCatalog::create(
      configuration.fingerprint(), configuration.revision(),
      configuration.organization().revision(), configuration.routes().revision(),
      std::move(inputs));
  if (!created) {
    throw std::logic_error{"invalid route catalog fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build a valid request for the route under examination.
[[nodiscard]] execution::OrderRequest request(const model::RouteId& route_id) {
  return execution::OrderRequest{route_id,
                                 id<model::InstrumentId>("BTC-USD-PERPETUAL"),
                                 execution::OrderSide::Buy,
                                 execution::OrderType::Limit,
                                 execution::TimeInForce::GoodTilCancelled,
                                 decimal<model::Price>("60000"),
                                 decimal<model::Quantity>("10")};
}

// --------------------------------------------------------
// Stable authorization precedence distinguishes missing, foreign, disabled, and instrument error.
TEST_CASE("owner-local route authorization applies the canonical rejection order",
          "[execution][submission][route]") {
  const auto configuration = enabled_configuration();
  const auto routes = catalog(configuration);
  const auto* const owner = configuration.organization().find_bot(
      id<model::BotId>("bot.deribit-btc-perpetual-reference"));
  const auto* const peer =
      configuration.organization().find_bot(id<model::BotId>("bot.subsidiary-reference"));
  REQUIRE(owner != nullptr);
  REQUIRE(peer != nullptr);
  const auto route_id = configuration.routes().routes().front().id;

  CHECK(routes.authorize(*owner, request(id<model::RouteId>("route.missing"))).reason ==
        execution::SubmissionReason::RouteNotFound);
  CHECK(routes.authorize(*peer, request(route_id)).reason ==
        execution::SubmissionReason::RouteNotOwned);

  auto wrong_instrument = request(route_id);
  wrong_instrument.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  CHECK(routes.authorize(*owner, wrong_instrument).reason ==
        execution::SubmissionReason::RouteInstrumentMismatch);

  const auto authorized = routes.authorize(*owner, request(route_id));
  REQUIRE(authorized.authorized());
  REQUIRE(authorized.installed_route != nullptr);
  CHECK(authorized.installed_route->route().logical_account_id ==
        id<model::LogicalAccountId>("account.deribit-testnet-aegis"));
  CHECK(authorized.installed_route->configuration_fingerprint() == configuration.fingerprint());
}

// --------------------------------------------------------
// Projection construction rejects mismatched metadata before any direct-path lookup is possible.
TEST_CASE("route projection construction fails closed on metadata mismatch",
          "[execution][submission][route]") {
  const auto configuration = enabled_configuration();
  const auto route = configuration.routes().routes().front();
  const auto* const attribution = configuration.organization().find_bot(route.bot_id);
  REQUIRE(attribution != nullptr);
  auto params = test_support::reference_configuration_params().instrument_metadata.front();
  params.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  const auto mismatched = model::InstrumentMetadata::create(std::move(params));
  REQUIRE(mismatched);

  const auto created = execution::OwnerLocalRouteCatalog::create(
      configuration.fingerprint(), configuration.revision(),
      configuration.organization().revision(), configuration.routes().revision(),
      {execution::SubmissionRouteInput{route, *attribution, mismatched.value()}});
  REQUIRE_FALSE(created);
  CHECK(created.error() == model::DomainError::at_index(model::DomainErrorCode::InvalidRelationship,
                                                        "submission_routes", 0U));
}

// --------------------------------------------------------

} // namespace
