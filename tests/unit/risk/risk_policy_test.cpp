// Purpose: prove the immutable M3 risk policy is complete, provenance-bound, canonically encoded,
// order-independent, and fail closed for stale, duplicate, inconsistent, or invalid authority.

#include "aegis/risk/risk_policy.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid fixture identifiers are test-authoring defects, not behavior under test.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid risk-test identifier"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Invalid fixture decimals are test-authoring defects, not policy-validation outcomes.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view value) {
  auto parsed = Decimal::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid risk-test decimal"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal one complete M3 fixture before deriving route and policy authority.
[[nodiscard]] configuration::StartupConfiguration
create_configuration_from_params_or_throw(configuration::StartupConfigurationParams params) {
  auto configured =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid risk-test configuration: " + configured.error().context.field};
  }
  return std::move(configured).value();
}

// --------------------------------------------------------
// Copy only the already validated values accepted by the narrow owner-local route catalog.
[[nodiscard]] execution::OwnerLocalRouteCatalog
create_route_catalog_or_throw(const configuration::StartupConfiguration& configuration) {
  std::vector<execution::SubmissionRouteInput> inputs;
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"incomplete risk-test route"};
    }
    inputs.push_back(execution::SubmissionRouteInput{route, *attribution, *metadata});
  }
  auto catalog = execution::OwnerLocalRouteCatalog::create_owner_local_route_catalog(
      configuration.fingerprint(), configuration.revision(),
      configuration.organization().revision(), configuration.routes().revision(),
      std::move(inputs));
  if (!catalog) {
    throw std::logic_error{"invalid risk-test route catalog"};
  }
  return std::move(catalog).value();
}

// --------------------------------------------------------
// Build a catalog whose labels match startup authority but whose first multiplier was altered; the
// risk-policy boundary must compare the complete sealed metadata rather than trust those labels.
[[nodiscard]] execution::OwnerLocalRouteCatalog
create_forged_metadata_catalog_or_throw(const configuration::StartupConfiguration& configuration) {
  std::vector<execution::SubmissionRouteInput> inputs;
  bool altered = false;
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"incomplete forged risk-test route"};
    }
    auto installed_metadata = *metadata;
    if (!altered) {
      auto changed =
          model::InstrumentMetadata::create_instrument_metadata(model::InstrumentMetadataParams{
              metadata->venue_id(),
              metadata->instrument_id(),
              metadata->venue_instrument_id(),
              metadata->revision(),
              std::string{metadata->base_currency()},
              std::string{metadata->quote_currency()},
              std::string{metadata->settlement_currency()},
              metadata->contract_style(),
              metadata->quantity_unit(),
              metadata->contract_multiplier_unit(),
              metadata->price_scale(),
              metadata->quantity_scale(),
              metadata->tick_size(),
              metadata->quantity_step(),
              metadata->minimum_quantity(),
              parse_decimal_or_throw<model::Notional>("11"),
          });
      if (!changed) {
        throw std::logic_error{"invalid forged risk-test metadata"};
      }
      installed_metadata = std::move(changed).value();
      altered = true;
    }
    inputs.push_back(
        execution::SubmissionRouteInput{route, *attribution, std::move(installed_metadata)});
  }
  auto catalog = execution::OwnerLocalRouteCatalog::create_owner_local_route_catalog(
      configuration.fingerprint(), configuration.revision(),
      configuration.organization().revision(), configuration.routes().revision(),
      std::move(inputs));
  if (!catalog) {
    throw std::logic_error{"invalid forged risk-test route catalog"};
  }
  return std::move(catalog).value();
}

// --------------------------------------------------------
// Resolve the heterogeneous subject identifier from the same sealed authority used in production.
[[nodiscard]] std::string
derive_risk_scope_subject_or_throw(const execution::InstalledSubmissionRoute& route,
                                   risk::RiskScopeKind scope) {
  switch (scope) {
  case risk::RiskScopeKind::Bot:
    return std::string{route.attribution().bot_id.value()};
  case risk::RiskScopeKind::Desk:
    return std::string{route.attribution().desk_id.value()};
  case risk::RiskScopeKind::Firm:
    return std::string{route.attribution().firm_id.value()};
  case risk::RiskScopeKind::Account:
    return std::string{route.route().logical_account_id.value()};
  case risk::RiskScopeKind::Route:
    return std::string{route.route().id.value()};
  case risk::RiskScopeKind::Instrument:
    return std::string{route.metadata().instrument_id().value()};
  case risk::RiskScopeKind::Venue:
    return std::string{route.metadata().venue_id().value()};
  default:
    throw std::logic_error{"invalid risk-test scope"};
  }
}

// --------------------------------------------------------
// Author one generous complete row so each test changes only its intended policy dimension.
[[nodiscard]] risk::RiskLimitSetParams
create_limit_row_or_throw(const execution::InstalledSubmissionRoute& route,
                          risk::RiskScopeKind scope) {
  return risk::RiskLimitSetParams{
      route.attribution().firm_id,
      scope,
      derive_risk_scope_subject_or_throw(route, scope),
      route.metadata().instrument_id(),
      std::string{route.metadata().quote_currency()},
      parse_decimal_or_throw<model::Quantity>("1000"),
      parse_decimal_or_throw<model::Notional>("100000"),
      100U,
      parse_decimal_or_throw<model::Notional>("1000000"),
      parse_decimal_or_throw<model::Quantity>("10000"),
      parse_decimal_or_throw<model::Notional>("1000000"),
  };
}

// --------------------------------------------------------
// Derive exact metadata and seven-scope rows for every enabled installed route.
[[nodiscard]] risk::RiskPolicyParams
create_policy_params_or_throw(const configuration::StartupConfiguration& configuration,
                              const execution::OwnerLocalRouteCatalog& routes) {
  std::vector<configuration::InstrumentMetadataRevisionEntry> metadata;
  std::vector<risk::RiskLimitSetParams> limits;
  for (const auto& route : routes.routes()) {
    if (!route.route().is_enabled()) {
      continue;
    }
    metadata.push_back(configuration::InstrumentMetadataRevisionEntry{
        route.metadata().venue_id(), route.metadata().instrument_id(),
        route.metadata().revision()});
    for (std::uint8_t value = static_cast<std::uint8_t>(risk::RiskScopeKind::Bot);
         value <= static_cast<std::uint8_t>(risk::RiskScopeKind::Venue); ++value) {
      limits.push_back(create_limit_row_or_throw(route, static_cast<risk::RiskScopeKind>(value)));
    }
  }
  std::sort(metadata.begin(), metadata.end(), [](const auto& left, const auto& right) {
    return std::tie(left.venue_id, left.instrument_id) <
           std::tie(right.venue_id, right.instrument_id);
  });
  metadata.erase(std::unique(metadata.begin(), metadata.end()), metadata.end());
  return risk::RiskPolicyParams{
      model::RiskPolicyRevision::create_initial(),
      configuration.fingerprint(),
      configuration.revision(),
      configuration.organization().revision(),
      configuration.routes().revision(),
      2U,
      model::RoundingMode::AwayFromZero,
      std::move(metadata),
      std::move(limits),
  };
}

// --------------------------------------------------------
// Add a second same-firm instrument so RiskScopeCountKey and RiskScopeQuoteNotionalKey consistency
// become observable.
[[nodiscard]] configuration::StartupConfigurationParams create_two_instrument_params_or_throw() {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  const auto venue = parse_identifier_or_throw<model::VenueId>("deribit");
  const auto instrument = parse_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
  params.instrument_metadata.push_back(model::InstrumentMetadataParams{
      venue,
      instrument,
      parse_identifier_or_throw<model::VenueInstrumentId>("ETH-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial(),
      "ETH",
      "USD",
      "ETH",
      model::ContractStyle::Inverse,
      model::QuantityUnit::Contracts,
      model::ContractMultiplierUnit::QuoteCurrencyPerContract,
      2U,
      0U,
      parse_decimal_or_throw<model::Price>("0.05"),
      parse_decimal_or_throw<model::Quantity>("1"),
      parse_decimal_or_throw<model::Quantity>("1"),
      parse_decimal_or_throw<model::Notional>("1"),
  });
  params.routes.push_back(execution::ExecutionRoute{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-eth-perpetual"),
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      venue,
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
      instrument,
      execution::ExecutionRouteState::Enabled,
  });
  return params;
}

// --------------------------------------------------------

// --------------------------------------------------------
// The accepted artifact must expose exact schema bytes, sorted keys, and stable startup provenance.
TEST_CASE("risk policy seals complete seven-scope authority as positional AEGISRSP",
          "[risk][policy][canonical][m3]") {
  const auto configuration = create_configuration_from_params_or_throw(
      test_support::create_m3_enabled_two_firm_configuration_params_or_throw());
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto created = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      create_policy_params_or_throw(configuration, routes), configuration, routes);

  REQUIRE(created);
  const auto& policy = created.value();
  REQUIRE(policy.canonical_bytes().size() > 10U);
  CHECK(std::string{reinterpret_cast<const char*>(policy.canonical_bytes().data()), 8U} ==
        "AEGISRSP");
  CHECK(policy.canonical_bytes()[8U] == std::byte{0U});
  CHECK(policy.canonical_bytes()[9U] == std::byte{1U});
  CHECK(policy.configuration_fingerprint() == configuration.fingerprint());
  CHECK(policy.configuration_revision() == configuration.revision());
  CHECK(policy.organization_revision() == configuration.organization().revision());
  CHECK(policy.route_revision() == configuration.routes().revision());
  CHECK(policy.notional_scale() == 2U);
  CHECK(policy.notional_rounding() == model::RoundingMode::AwayFromZero);
  CHECK(policy.metadata_revisions().size() == 1U);
  CHECK(policy.limit_sets().size() == 14U);
  CHECK(policy.fingerprint().to_hex().size() == model::sha256_hex_size);
  CHECK(policy.fingerprint().to_hex() ==
        "19fe2db66a8c790fdc5236cc53c94f3a46c9c15d944aa9c6c412e85856b3ba25");

  // ++++++++++++++++++++++++++++++++++++++++
  // Every installed firm resolves its own bot and venue rows despite sharing venue/instrument text.
  for (const auto& route : routes.routes()) {
    const auto* const bot = policy.find_limit_set(
        route.attribution().firm_id, risk::RiskScopeKind::Bot, route.attribution().bot_id.value(),
        route.metadata().instrument_id(), route.metadata().quote_currency());
    const auto* const venue =
        policy.find_limit_set(route.attribution().firm_id, risk::RiskScopeKind::Venue,
                              route.metadata().venue_id().value(), route.metadata().instrument_id(),
                              route.metadata().quote_currency());
    REQUIRE(bot != nullptr);
    REQUIRE(venue != nullptr);
    CHECK(bot->firm_id() == route.attribution().firm_id);
    CHECK(venue->firm_id() == route.attribution().firm_id);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Author collection order cannot select limits or change canonical bytes and fingerprint identity.
TEST_CASE("risk policy canonicalizes metadata and complete semantic limit keys",
          "[risk][policy][canonical][m3]") {
  const auto configuration =
      create_configuration_from_params_or_throw(create_two_instrument_params_or_throw());
  const auto routes = create_route_catalog_or_throw(configuration);
  auto ordered = create_policy_params_or_throw(configuration, routes);
  auto reversed = ordered;
  std::reverse(reversed.metadata_revisions.begin(), reversed.metadata_revisions.end());
  std::reverse(reversed.limit_sets.begin(), reversed.limit_sets.end());

  const auto first = risk::RiskPolicySnapshot::create_risk_policy_snapshot(std::move(ordered),
                                                                           configuration, routes);
  const auto second = risk::RiskPolicySnapshot::create_risk_policy_snapshot(std::move(reversed),
                                                                            configuration, routes);

  REQUIRE(first);
  REQUIRE(second);
  CHECK(first.value().canonical_bytes() == second.value().canonical_bytes());
  CHECK(first.value().fingerprint() == second.value().fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Missing, stale, duplicate, zero-valued, and shared-key-inconsistent policy must fail
// construction.
TEST_CASE("risk policy fails closed before mutable risk can exist", "[risk][policy][failure][m3]") {
  const auto configuration =
      create_configuration_from_params_or_throw(create_two_instrument_params_or_throw());
  const auto routes = create_route_catalog_or_throw(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Removing any one of the exact route/scope keys makes policy incomplete.
  auto missing = create_policy_params_or_throw(configuration, routes);
  missing.limit_sets.pop_back();
  const auto missing_result = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(missing), configuration, routes);
  REQUIRE_FALSE(missing_result);
  CHECK(missing_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // A repeated complete key cannot be made authoritative by collection order.
  auto duplicate = create_policy_params_or_throw(configuration, routes);
  duplicate.limit_sets.push_back(duplicate.limit_sets.front());
  const auto duplicate_result = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(duplicate), configuration, routes);
  REQUIRE_FALSE(duplicate_result);
  CHECK(duplicate_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero never means unlimited; every authored limit is required and positive.
  auto zero = create_policy_params_or_throw(configuration, routes);
  zero.limit_sets.front().maximum_open_order_count = 0U;
  const auto zero_result =
      risk::RiskPolicySnapshot::create_risk_policy_snapshot(std::move(zero), configuration, routes);
  REQUIRE_FALSE(zero_result);
  CHECK(zero_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // One same-RiskScopeCountKey row cannot silently choose a different open-order cap for another
  // instrument.
  auto inconsistent = create_policy_params_or_throw(configuration, routes);
  const auto first_subject = inconsistent.limit_sets.front().scope_subject;
  const auto first_firm = inconsistent.limit_sets.front().firm_id;
  const auto first_scope = inconsistent.limit_sets.front().scope;
  const auto peer = std::find_if(
      inconsistent.limit_sets.begin() + 1, inconsistent.limit_sets.end(), [&](const auto& row) {
        return row.firm_id == first_firm && row.scope == first_scope &&
               row.scope_subject == first_subject &&
               row.instrument_id != inconsistent.limit_sets.front().instrument_id;
      });
  REQUIRE(peer != inconsistent.limit_sets.end());
  peer->maximum_open_order_count += 1U;
  const auto inconsistent_result = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(inconsistent), configuration, routes);
  REQUIRE_FALSE(inconsistent_result);
  CHECK(inconsistent_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // A metadata revision different from sealed configuration is stale even when all keys exist.
  auto stale = create_policy_params_or_throw(configuration, routes);
  stale.metadata_revisions.front().revision =
      stale.metadata_revisions.front().revision.derive_next_revision().value();
  const auto stale_result = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(stale), configuration, routes);
  REQUIRE_FALSE(stale_result);
  CHECK(stale_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Matching provenance labels cannot bless metadata economics that differ from sealed startup.
  const auto forged_routes = create_forged_metadata_catalog_or_throw(configuration);
  auto forged = create_policy_params_or_throw(configuration, forged_routes);
  const auto forged_result = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(forged), configuration, forged_routes);
  REQUIRE_FALSE(forged_result);
  CHECK(forged_result.error().code == model::DomainErrorCode::InvalidRiskPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
