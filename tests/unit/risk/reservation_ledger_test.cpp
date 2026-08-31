// Purpose: prove M3 risk check-and-reserve is scope-major, exact, atomic, capacity-bounded,
// multi-firm isolated, and releases reusable reservation slots exactly once.

#include "aegis/risk/reservation_ledger.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid fixture identifiers are test-authoring defects rather than ledger behavior.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid ledger-test identifier"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Invalid fixture decimals are test-authoring defects rather than ledger behavior.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view value) {
  auto parsed = Decimal::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid ledger-test decimal"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal the enabled two-firm reference authority once per isolated ledger test.
[[nodiscard]] configuration::StartupConfiguration create_enabled_configuration_or_throw() {
  auto configured = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_m3_enabled_two_firm_configuration_params_or_throw());
  if (!configured) {
    throw std::logic_error{"invalid ledger-test configuration"};
  }
  return std::move(configured).value();
}

// --------------------------------------------------------
// Add a second inverse instrument for the baseline firm so quote-currency aggregate replacement is
// independently observable without crossing either firm or currency boundaries.
[[nodiscard]] configuration::StartupConfiguration create_two_instrument_configuration_or_throw() {
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
      parse_decimal_or_throw<model::Notional>("7"),
  });
  params.routes.push_back(execution::ExecutionRoute{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-eth-perpetual"),
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      venue,
      parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"),
      instrument,
      execution::ExecutionRouteState::Enabled,
  });
  auto configured =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid two-instrument ledger-test configuration"};
  }
  return std::move(configured).value();
}

// --------------------------------------------------------
// Install exact route, attribution, metadata, and provenance copies for owner-local risk use.
[[nodiscard]] execution::OwnerLocalRouteCatalog
create_route_catalog_or_throw(const configuration::StartupConfiguration& configuration) {
  std::vector<execution::SubmissionRouteInput> inputs;
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"incomplete ledger-test route"};
    }
    inputs.push_back(execution::SubmissionRouteInput{route, *attribution, *metadata});
  }
  auto catalog = execution::OwnerLocalRouteCatalog::create_owner_local_route_catalog(
      configuration.fingerprint(), configuration.revision(),
      configuration.organization().revision(), configuration.routes().revision(),
      std::move(inputs));
  if (!catalog) {
    throw std::logic_error{"invalid ledger-test route catalog"};
  }
  return std::move(catalog).value();
}

// --------------------------------------------------------
// Derive the exact heterogeneous scope subject from sealed route authority.
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
    throw std::logic_error{"invalid ledger-test scope"};
  }
}

// --------------------------------------------------------
// Author generous limits so a test can tighten exactly one scope/kind without another winner.
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
// Produce one complete policy authoring value for every enabled route and all seven scopes.
[[nodiscard]] risk::RiskPolicyParams
create_policy_params_or_throw(const configuration::StartupConfiguration& configuration,
                              const execution::OwnerLocalRouteCatalog& routes) {
  std::vector<configuration::InstrumentMetadataRevisionEntry> metadata;
  std::vector<risk::RiskLimitSetParams> limits;
  for (const auto& route : routes.routes()) {
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
// Construct one ledger and fail immediately when a test-authored policy is invalid.
[[nodiscard]] risk::ReservationLedger
create_ledger_or_throw(const configuration::StartupConfiguration& configuration,
                       const execution::OwnerLocalRouteCatalog& routes,
                       risk::RiskPolicyParams params, std::uint32_t capacity = 8U) {
  auto policy = risk::RiskPolicySnapshot::create_risk_policy_snapshot(std::move(params),
                                                                      configuration, routes);
  if (!policy) {
    throw std::logic_error{"invalid ledger-test policy"};
  }
  auto ledger =
      risk::ReservationLedger::create_reservation_ledger(std::move(policy).value(), capacity);
  if (!ledger) {
    throw std::logic_error{"invalid ledger-test capacity"};
  }
  return std::move(ledger).value();
}

// --------------------------------------------------------
// Use canonical economics whose inverse face notional is exactly quantity times ten dollars.
[[nodiscard]] execution::CanonicalOrderEconomics
create_order_economics_or_throw(execution::OrderSide side, std::string_view quantity) {
  return execution::CanonicalOrderEconomics{side, execution::OrderType::Limit,
                                            execution::TimeInForce::GoodTilCancelled,
                                            parse_decimal_or_throw<model::Price>("100.0"),
                                            parse_decimal_or_throw<model::Quantity>(quantity)};
}

// --------------------------------------------------------
// Resolve the authored row for one exact route/scope semantic key.
[[nodiscard]] risk::RiskLimitSetParams&
find_limit_row_or_throw(risk::RiskPolicyParams& params,
                        const execution::InstalledSubmissionRoute& route,
                        risk::RiskScopeKind scope) {
  const auto found =
      std::find_if(params.limit_sets.begin(), params.limit_sets.end(), [&](const auto& row) {
        return row.firm_id == route.attribution().firm_id && row.scope == scope &&
               row.scope_subject == derive_risk_scope_subject_or_throw(route, scope) &&
               row.instrument_id == route.metadata().instrument_id();
      });
  if (found == params.limit_sets.end()) {
    throw std::logic_error{"missing ledger-test limit row"};
  }
  return *found;
}

// --------------------------------------------------------
// Return a coherent evidence snapshot for the exact route-derived scope projection.
[[nodiscard]] risk::RiskScopeExposure
calculate_scope_exposure_or_throw(const risk::ReservationLedger& ledger,
                                  const execution::InstalledSubmissionRoute& route,
                                  risk::RiskScopeKind scope) {
  const auto snapshot = ledger.scope_exposure(
      route.attribution().firm_id, scope, derive_risk_scope_subject_or_throw(route, scope),
      route.metadata().instrument_id(), route.metadata().quote_currency());
  if (!snapshot) {
    throw std::logic_error{"missing ledger-test exposure cell"};
  }
  return *snapshot;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Every scope and limit kind must win at its exact canonical position and leave state unchanged.
TEST_CASE("risk ledger enforces all scope-major and limit-kind-minor rejection positions",
          "[risk][ledger][precedence][atomic][m3]") {
  const auto configuration = create_enabled_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto& route = routes.routes().front();
  constexpr std::array scopes{
      risk::RiskScopeKind::Bot,     risk::RiskScopeKind::Desk,  risk::RiskScopeKind::Firm,
      risk::RiskScopeKind::Account, risk::RiskScopeKind::Route, risk::RiskScopeKind::Instrument,
      risk::RiskScopeKind::Venue,
  };
  constexpr std::array reasons{
      execution::SubmissionReason::SingleOrderQuantityExceeded,
      execution::SubmissionReason::SingleOrderNotionalExceeded,
      execution::SubmissionReason::OpenOrderCountExceeded,
      execution::SubmissionReason::GrossReservedNotionalExceeded,
      execution::SubmissionReason::WorstCasePositionQuantityExceeded,
      execution::SubmissionReason::WorstCasePositionNotionalExceeded,
  };

  for (const auto scope : scopes) {
    for (std::size_t kind = 0U; kind < reasons.size(); ++kind) {
      auto params = create_policy_params_or_throw(configuration, routes);
      auto& target = find_limit_row_or_throw(params, route, scope);
      bool requires_baseline = false;
      switch (kind) {
      case 0U:
        target.maximum_single_order_quantity = parse_decimal_or_throw<model::Quantity>("1");
        break;
      case 1U:
        target.maximum_single_order_quote_notional = parse_decimal_or_throw<model::Notional>("19");
        break;
      case 2U:
        target.maximum_open_order_count = 1U;
        requires_baseline = true;
        break;
      case 3U:
        target.maximum_gross_reserved_quote_notional =
            parse_decimal_or_throw<model::Notional>("30");
        requires_baseline = true;
        break;
      case 4U:
        target.maximum_worst_case_position_quantity = parse_decimal_or_throw<model::Quantity>("3");
        requires_baseline = true;
        break;
      case 5U:
        target.maximum_worst_case_position_quote_notional =
            parse_decimal_or_throw<model::Notional>("30");
        requires_baseline = true;
        break;
      default:
        FAIL("unassigned risk-test limit kind");
      }

      auto ledger = create_ledger_or_throw(configuration, routes, std::move(params));
      auto before = calculate_scope_exposure_or_throw(ledger, route, scope);
      auto attempt = model::SubmissionAttemptId::create_initial();
      if (requires_baseline) {
        const auto first = ledger.check_and_reserve(
            attempt, route, create_order_economics_or_throw(execution::OrderSide::Buy, "2"));
        REQUIRE(first.is_reserved());
        attempt = attempt.derive_next_ordinal().value();
        before = calculate_scope_exposure_or_throw(ledger, route, scope);
      }

      const auto rejected = ledger.check_and_reserve(
          attempt, route, create_order_economics_or_throw(execution::OrderSide::Buy, "2"));
      CHECK_FALSE(rejected.is_reserved());
      CHECK(rejected.reason() == reasons[kind]);
      REQUIRE(rejected.risk_evidence().has_value());
      CHECK(rejected.risk_evidence()->scope() == scope);
      CHECK(calculate_scope_exposure_or_throw(ledger, route, scope) == before);
      CHECK(ledger.held_reservation_count() == (requires_baseline ? 1U : 0U));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Arithmetic failure in any one scratch cell must reject before committing any of the 35 values.
TEST_CASE("risk ledger rejects accumulator overflow atomically",
          "[risk][ledger][overflow][atomic][m3]") {
  const auto configuration = create_enabled_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto& route = routes.routes().front();
  auto params = create_policy_params_or_throw(configuration, routes);
  const auto maximum_quantity =
      model::Quantity::from_scaled(std::numeric_limits<std::int64_t>::max(), 0U).value();
  const auto maximum_notional =
      model::Notional::from_scaled(std::numeric_limits<std::int64_t>::max(), 0U).value();
  for (auto& row : params.limit_sets) {
    row.maximum_single_order_quantity = maximum_quantity;
    row.maximum_single_order_quote_notional = maximum_notional;
    row.maximum_gross_reserved_quote_notional = maximum_notional;
    row.maximum_worst_case_position_quantity = maximum_quantity;
    row.maximum_worst_case_position_quote_notional = maximum_notional;
  }
  auto ledger = create_ledger_or_throw(configuration, routes, std::move(params));
  const auto nearly_full = model::Quantity::from_scaled(922'337'203'685'477'580LL, 0U).value();
  const auto first_economics =
      execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                         execution::TimeInForce::GoodTilCancelled,
                                         parse_decimal_or_throw<model::Price>("100"), nearly_full};
  const auto first = ledger.check_and_reserve(model::SubmissionAttemptId::create_initial(), route,
                                              first_economics);
  REQUIRE(first.is_reserved());
  const auto before = calculate_scope_exposure_or_throw(ledger, route, risk::RiskScopeKind::Bot);

  const auto overflow =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(2U).value(), route,
                               create_order_economics_or_throw(execution::OrderSide::Buy, "1"));
  CHECK_FALSE(overflow.is_reserved());
  CHECK(overflow.reason() == execution::SubmissionReason::RiskArithmeticFailure);
  CHECK_FALSE(overflow.risk_evidence().has_value());
  CHECK(calculate_scope_exposure_or_throw(ledger, route, risk::RiskScopeKind::Bot) == before);
  CHECK(ledger.held_reservation_count() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// An independent integer sequence proves directional maxima never net opposite-side reservations.
TEST_CASE("risk ledger worst-case exposure is never understated across an order grid",
          "[risk][ledger][property][m3]") {
  const auto configuration = create_enabled_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto& route = routes.routes().front();
  auto ledger = create_ledger_or_throw(configuration, routes,
                                       create_policy_params_or_throw(configuration, routes), 32U);
  std::int64_t independent_buy = 0;
  std::int64_t independent_sell = 0;

  for (std::uint64_t value = 1U; value <= 20U; ++value) {
    const auto side = value % 2U == 0U ? execution::OrderSide::Sell : execution::OrderSide::Buy;
    const auto quantity = static_cast<std::int64_t>(value);
    if (side == execution::OrderSide::Buy) {
      independent_buy += quantity;
    } else {
      independent_sell += quantity;
    }
    const auto reserved =
        ledger.check_and_reserve(model::SubmissionAttemptId::from_value(value).value(), route,
                                 create_order_economics_or_throw(side, std::to_string(value)));
    REQUIRE(reserved.is_reserved());

    // ++++++++++++++++++++++++++++++++++++++++
    // The production accumulator must equal an independently recomputed gross and directional max.
    const auto observed =
        calculate_scope_exposure_or_throw(ledger, route, risk::RiskScopeKind::Bot);
    const auto independent_worst = std::max(independent_buy, independent_sell);
    CHECK(observed.open_order_count == value);
    CHECK(observed.gross_reserved_quote_notional ==
          model::Notional::from_scaled((independent_buy + independent_sell) * 10, 0U).value());
    CHECK(observed.worst_case_position_quantity ==
          model::Quantity::from_scaled(independent_worst, 0U).value());
    CHECK(observed.worst_case_position_quote_notional ==
          model::Notional::from_scaled(independent_worst * 10, 0U).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Quote-currency aggregate worst must equal the independent sum of each instrument's directional
// maximum, including after a release replaces only one instrument contribution.
TEST_CASE("risk ledger aggregates and releases same-currency instrument maxima exactly",
          "[risk][ledger][property][aggregate][release][m3]") {
  const auto configuration = create_two_instrument_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto* const btc_route = routes.find_route(
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"));
  const auto* const eth_route = routes.find_route(
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-eth-perpetual"));
  REQUIRE(btc_route != nullptr);
  REQUIRE(eth_route != nullptr);
  auto ledger = create_ledger_or_throw(configuration, routes,
                                       create_policy_params_or_throw(configuration, routes), 8U);

  const auto btc_buy =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(1U).value(), *btc_route,
                               create_order_economics_or_throw(execution::OrderSide::Buy, "5"));
  const auto btc_sell =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(2U).value(), *btc_route,
                               create_order_economics_or_throw(execution::OrderSide::Sell, "3"));
  const auto eth_buy =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(3U).value(), *eth_route,
                               create_order_economics_or_throw(execution::OrderSide::Buy, "4"));
  const auto eth_sell =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(4U).value(), *eth_route,
                               create_order_economics_or_throw(execution::OrderSide::Sell, "6"));
  REQUIRE(btc_buy.is_reserved());
  REQUIRE(btc_sell.is_reserved());
  REQUIRE(eth_buy.is_reserved());
  REQUIRE(eth_sell.is_reserved());

  // ++++++++++++++++++++++++++++++++++++++++
  // Independent arithmetic is max(50,30) + max(28,42) = 92, never a cross-instrument net.
  const auto before_release =
      calculate_scope_exposure_or_throw(ledger, *btc_route, risk::RiskScopeKind::Firm);
  CHECK(before_release.gross_reserved_quote_notional ==
        parse_decimal_or_throw<model::Notional>("150"));
  CHECK(before_release.worst_case_position_quote_notional ==
        parse_decimal_or_throw<model::Notional>("92"));
  CHECK(calculate_scope_exposure_or_throw(ledger, *eth_route, risk::RiskScopeKind::Firm)
            .worst_case_position_quote_notional == parse_decimal_or_throw<model::Notional>("92"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Removing BTC buy changes only BTC's contribution: max(0,30) + max(28,42) = 72.
  REQUIRE(ledger.release_reservation(btc_buy.reservation_id().value()));
  const auto after_btc_release =
      calculate_scope_exposure_or_throw(ledger, *btc_route, risk::RiskScopeKind::Firm);
  CHECK(after_btc_release.gross_reserved_quote_notional ==
        parse_decimal_or_throw<model::Notional>("100"));
  CHECK(after_btc_release.worst_case_position_quote_notional ==
        parse_decimal_or_throw<model::Notional>("72"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Removing ETH sell changes only ETH's contribution: max(0,30) + max(28,0) = 58.
  REQUIRE(ledger.release_reservation(eth_sell.reservation_id().value()));
  const auto after_eth_release =
      calculate_scope_exposure_or_throw(ledger, *eth_route, risk::RiskScopeKind::Firm);
  CHECK(after_eth_release.gross_reserved_quote_notional ==
        parse_decimal_or_throw<model::Notional>("58"));
  CHECK(after_eth_release.worst_case_position_quote_notional ==
        parse_decimal_or_throw<model::Notional>("58"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Capacity rejects after limits; release restores every cell once and permits monotonically newer
// reuse.
TEST_CASE("risk reservations release exactly once and reuse only for a newer attempt",
          "[risk][ledger][release][capacity][m3]") {
  const auto configuration = create_enabled_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  const auto& route = routes.routes().front();

  // ++++++++++++++++++++++++++++++++++++++++
  // Reservation capacity is authored by AEGISSUP, so zero maps to InvalidSubmissionPolicy.
  auto zero_capacity_policy = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      create_policy_params_or_throw(configuration, routes), configuration, routes);
  REQUIRE(zero_capacity_policy);
  const auto zero_capacity = risk::ReservationLedger::create_reservation_ledger(
      std::move(zero_capacity_policy).value(), 0U);
  REQUIRE_FALSE(zero_capacity);
  CHECK(zero_capacity.error().code == model::DomainErrorCode::InvalidSubmissionPolicy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove one retained reservation blocks the next attempt, then releases exactly once for reuse.
  auto ledger = create_ledger_or_throw(configuration, routes,
                                       create_policy_params_or_throw(configuration, routes), 1U);
  const auto first_attempt = model::SubmissionAttemptId::create_initial();
  const auto second_attempt = first_attempt.derive_next_ordinal().value();
  const auto third_attempt = second_attempt.derive_next_ordinal().value();

  const auto first = ledger.check_and_reserve(
      first_attempt, route, create_order_economics_or_throw(execution::OrderSide::Buy, "5"));
  REQUIRE(first.is_reserved());
  const auto at_capacity = ledger.check_and_reserve(
      second_attempt, route, create_order_economics_or_throw(execution::OrderSide::Sell, "3"));
  CHECK_FALSE(at_capacity.is_reserved());
  CHECK(at_capacity.reason() == execution::SubmissionReason::ReservationCapacityExceeded);
  CHECK(ledger.held_reservation_count() == 1U);
  REQUIRE(ledger.reservation_at(0U) != nullptr);
  CHECK(ledger.reservation_at(0U)->reservation_id == first.reservation_id().value());
  CHECK(ledger.reservation_at(1U) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
  // Releasing the held buy restores zero and records Released without decrementing twice.
  const auto first_id = first.reservation_id().value();
  REQUIRE(ledger.release_reservation(first_id));
  CHECK(ledger.held_reservation_count() == 0U);
  REQUIRE(ledger.find_reservation(first_id) != nullptr);
  CHECK(ledger.find_reservation(first_id)->state == risk::ReservationState::Released);
  const auto repeated = ledger.release_reservation(first_id);
  REQUIRE_FALSE(repeated);
  CHECK(repeated.error().code == model::DomainErrorCode::InvalidRiskReservationState);
  CHECK(ledger.held_reservation_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The same slot accepts a newer identity; the replaced old identity can never release it.
  const auto replacement = ledger.check_and_reserve(
      third_attempt, route, create_order_economics_or_throw(execution::OrderSide::Sell, "3"));
  REQUIRE(replacement.is_reserved());
  CHECK(replacement.reservation_id()->value() == third_attempt.value());
  CHECK(ledger.find_reservation(first_id) == nullptr);
  REQUIRE(ledger.reservation_at(0U) != nullptr);
  CHECK(ledger.reservation_at(0U)->reservation_id == replacement.reservation_id().value());
  const auto stale_release = ledger.release_reservation(first_id);
  REQUIRE_FALSE(stale_release);
  CHECK(ledger.held_reservation_count() == 1U);
  REQUIRE(ledger.release_reservation(replacement.reservation_id().value()));
  CHECK(ledger.held_reservation_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Directional maximum, gross sum, release recomputation, and peer-firm keys remain independent.
TEST_CASE("risk ledger preserves directional worst case and multi-firm isolation",
          "[risk][ledger][directional][multi-firm][m3]") {
  const auto configuration = create_enabled_configuration_or_throw();
  const auto routes = create_route_catalog_or_throw(configuration);
  REQUIRE(routes.routes().size() == 2U);
  const auto& first_route = routes.routes()[0U];
  const auto& second_route = routes.routes()[1U];
  auto ledger = create_ledger_or_throw(configuration, routes,
                                       create_policy_params_or_throw(configuration, routes), 4U);

  const auto buy =
      ledger.check_and_reserve(model::SubmissionAttemptId::create_initial(), first_route,
                               create_order_economics_or_throw(execution::OrderSide::Buy, "5"));
  REQUIRE(buy.is_reserved());
  const auto sell =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(2U).value(), first_route,
                               create_order_economics_or_throw(execution::OrderSide::Sell, "3"));
  REQUIRE(sell.is_reserved());

  // ++++++++++++++++++++++++++++++++++++++++
  // Gross adds both sides, while directional worst remains max(buy,sell) rather than their sum/net.
  const auto first_firm =
      calculate_scope_exposure_or_throw(ledger, first_route, risk::RiskScopeKind::Firm);
  CHECK(first_firm.open_order_count == 2U);
  CHECK(first_firm.gross_reserved_quote_notional == parse_decimal_or_throw<model::Notional>("80"));
  CHECK(first_firm.reserved_buy_quantity == parse_decimal_or_throw<model::Quantity>("5"));
  CHECK(first_firm.reserved_sell_quantity == parse_decimal_or_throw<model::Quantity>("3"));
  CHECK(first_firm.worst_case_position_quantity == parse_decimal_or_throw<model::Quantity>("5"));
  CHECK(first_firm.instrument_worst_case_quote_notional ==
        parse_decimal_or_throw<model::Notional>("50"));
  CHECK(first_firm.worst_case_position_quote_notional ==
        parse_decimal_or_throw<model::Notional>("50"));

  // ++++++++++++++++++++++++++++++++++++++++
  // The peer firm shares venue/instrument spelling but none of the first firm's mutable buckets.
  const auto peer_before =
      calculate_scope_exposure_or_throw(ledger, second_route, risk::RiskScopeKind::Firm);
  CHECK(peer_before.open_order_count == 0U);
  CHECK(peer_before.gross_reserved_quote_notional == parse_decimal_or_throw<model::Notional>("0"));
  const auto peer =
      ledger.check_and_reserve(model::SubmissionAttemptId::from_value(3U).value(), second_route,
                               create_order_economics_or_throw(execution::OrderSide::Buy, "2"));
  REQUIRE(peer.is_reserved());
  CHECK(calculate_scope_exposure_or_throw(ledger, first_route, risk::RiskScopeKind::Firm) ==
        first_firm);
  CHECK(calculate_scope_exposure_or_throw(ledger, second_route, risk::RiskScopeKind::Firm)
            .open_order_count == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Complete scope evidence follows the immutable policy's canonical semantic-key ordering.
  REQUIRE(ledger.scope_evidence_count() == ledger.policy().limit_sets().size());
  for (std::size_t index = 0U; index < ledger.scope_evidence_count(); ++index) {
    const auto& expected = ledger.policy().limit_sets()[index];
    const auto observed = ledger.scope_evidence_at(index);
    REQUIRE(observed.has_value());
    CHECK(observed->firm_id == expected.firm_id());
    CHECK(observed->scope == expected.scope());
    CHECK(observed->scope_subject == expected.scope_subject());
    CHECK(observed->instrument_id == expected.instrument_id());
    CHECK(observed->quote_currency == expected.quote_currency());
    const auto direct =
        ledger.scope_exposure(expected.firm_id(), expected.scope(), expected.scope_subject(),
                              expected.instrument_id(), expected.quote_currency());
    REQUIRE(direct.has_value());
    CHECK(observed->exposure == *direct);
  }
  CHECK_FALSE(ledger.scope_evidence_at(ledger.scope_evidence_count()).has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Releasing only the buy recomputes the first firm's directional maximum from the retained sell.
  REQUIRE(ledger.release_reservation(buy.reservation_id().value()));
  const auto after_buy_release =
      calculate_scope_exposure_or_throw(ledger, first_route, risk::RiskScopeKind::Firm);
  CHECK(after_buy_release.gross_reserved_quote_notional ==
        parse_decimal_or_throw<model::Notional>("30"));
  CHECK(after_buy_release.worst_case_position_quantity ==
        parse_decimal_or_throw<model::Quantity>("3"));
  CHECK(after_buy_release.worst_case_position_quote_notional ==
        parse_decimal_or_throw<model::Notional>("30"));
  REQUIRE(ledger.release_reservation(sell.reservation_id().value()));
  CHECK(calculate_scope_exposure_or_throw(ledger, first_route, risk::RiskScopeKind::Firm)
            .open_order_count == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
