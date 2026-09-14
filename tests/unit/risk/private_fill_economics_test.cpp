// Purpose: independently qualify bounded known-order fill economics, atomic reservation/inventory
// replacement, retained ownership, and all seven risk scopes without consuming private events.

#include "aegis/risk/inventory_ledger.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

constexpr std::string_view baseline_route = "route.deribit-testnet-btc-perpetual";
constexpr std::string_view peer_route = "route.deribit-testnet-subsidiary-btc-perpetual";
constexpr std::string_view second_instrument_route = "route.deribit-testnet-eth-perpetual";
constexpr std::array all_scopes{
    risk::RiskScopeKind::Bot,     risk::RiskScopeKind::Desk,  risk::RiskScopeKind::Firm,
    risk::RiskScopeKind::Account, risk::RiskScopeKind::Route, risk::RiskScopeKind::Instrument,
    risk::RiskScopeKind::Venue,
};

// Opaque plans cannot be default-authored or copied, and only the stable component owns their
// constructor and commit validation. Inventory is never independently copyable or movable.
static_assert(!std::is_default_constructible_v<risk::ReservationInventoryPlan>);
static_assert(!std::is_copy_constructible_v<risk::ReservationInventoryPlan>);
static_assert(!std::is_copy_assignable_v<risk::ReservationInventoryPlan>);
static_assert(std::is_nothrow_move_constructible_v<risk::ReservationInventoryPlan>);
static_assert(!std::is_copy_constructible_v<risk::InventoryLedger>);
static_assert(!std::is_move_constructible_v<risk::InventoryLedger>);

// --------------------------------------------------------
// Stop fixture construction at its first invalid authored value rather than testing partial setup.
template <typename Value>
[[nodiscard]] Value extract_economics_result_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid private fill economics fixture: " +
                           std::to_string(static_cast<std::uint32_t>(result.error().code)) + " " +
                           result.error().context.field};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Extend sealed authority with a different multiplier or distinct quote currencies only when an
// aggregation scenario needs those independent keys.
[[nodiscard]] configuration::StartupConfigurationParams
create_economics_configuration_params_or_throw(bool two_instruments, bool three_currencies) {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Different instrument quantities never offset, even when their quote-currency totals combine.
  if (two_instruments) {
    const auto venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("deribit");
    const auto instrument =
        test_support::parse_m4_identifier_or_throw<model::InstrumentId>("ETH-USD-PERPETUAL");
    params.instrument_metadata.push_back(model::InstrumentMetadataParams{
        venue,
        instrument,
        test_support::parse_m4_identifier_or_throw<model::VenueInstrumentId>("ETH-PERPETUAL"),
        model::InstrumentMetadataRevision::create_initial(),
        "ETH",
        "USD",
        "ETH",
        model::ContractStyle::Inverse,
        model::QuantityUnit::Contracts,
        model::ContractMultiplierUnit::QuoteCurrencyPerContract,
        2U,
        0U,
        test_support::create_m4_decimal_or_throw<model::Price>(5, 2U),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Notional>(7),
    });
    params.routes.push_back(execution::ExecutionRoute{
        test_support::parse_m4_identifier_or_throw<model::RouteId>(second_instrument_route),
        test_support::parse_m4_identifier_or_throw<model::BotId>(
            "bot.deribit-btc-perpetual-reference"),
        venue,
        test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
            "account.deribit-testnet-aegis"),
        instrument,
        execution::ExecutionRouteState::Enabled,
    });
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The normalized instrument may share quantity scope across separately owned venue currencies.
  if (three_currencies) {
    for (const auto& [suffix, currency] :
         {std::pair<std::string_view, std::string_view>{"aaa", "AAA"},
          {"bbb", "BBB"},
          {"zzz", "ZZZ"}}) {
      const auto venue = test_support::parse_m4_identifier_or_throw<model::VenueId>(
          "economics-" + std::string{suffix});
      const auto account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
          "account.economics-" + std::string{suffix});
      const auto instrument =
          test_support::parse_m4_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL");
      params.venues.push_back(
          configuration::VenueDefinition{venue, configuration::VenueEnvironment::Testnet});
      params.logical_accounts.push_back(configuration::LogicalAccountVenueBinding{
          account, test_support::parse_m4_identifier_or_throw<model::FirmId>("firm.aegis-lab"),
          venue});
      params.instrument_metadata.push_back(model::InstrumentMetadataParams{
          venue, instrument,
          test_support::parse_m4_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
          model::InstrumentMetadataRevision::create_initial(), "BTC", std::string{currency}, "BTC",
          model::ContractStyle::Inverse, model::QuantityUnit::Contracts,
          model::ContractMultiplierUnit::QuoteCurrencyPerContract, 0U, 0U,
          test_support::create_m4_decimal_or_throw<model::Price>(1),
          test_support::create_m4_decimal_or_throw<model::Quantity>(1),
          test_support::create_m4_decimal_or_throw<model::Quantity>(1),
          test_support::create_m4_decimal_or_throw<model::Notional>(1)});
      params.routes.push_back(execution::ExecutionRoute{
          test_support::parse_m4_identifier_or_throw<model::RouteId>("route.economics-" +
                                                                     std::string{suffix}),
          test_support::parse_m4_identifier_or_throw<model::BotId>(
              "bot.deribit-btc-perpetual-reference"),
          venue, account, instrument, execution::ExecutionRouteState::Enabled});
    }
  }
  return params;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preserve sealed reference authority while giving component scenarios sufficient genuine submit
// headroom; a finite quantity limit can be selected to prove risk reads confirmed inventory.
[[nodiscard]] test_support::M4OwnerTestAuthority create_economics_authority_or_throw(
    std::uint32_t reservation_capacity = 8U, std::uint32_t inventory_capacity = 32U,
    bool two_instruments = false, std::int64_t maximum_worst_quantity = 1000,
    std::uint32_t aggregate_capacity = 32U, bool three_currencies = false) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Rebind the ordinary source definitions to the complete authored configuration before any
  // replacement submission policy or inventory capacity is derived.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  authority.configuration = extract_economics_result_or_throw(
      configuration::StartupConfiguration::create_startup_configuration(
          create_economics_configuration_params_or_throw(two_instruments, three_currencies)));
  std::vector<runtime::RuntimeSourceDefinition> sources;
  for (const auto& source : authority.runtime_policy.sources()) {
    sources.push_back(source.definition());
  }
  authority.runtime_policy =
      extract_economics_result_or_throw(runtime::RuntimePolicy::create_runtime_policy(
          authority.configuration,
          runtime::RuntimePolicyParams{authority.runtime_policy.limits(), std::move(sources)}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Keep every shared policy key coherent while selecting ordinary or representational headroom.
  auto risk_params =
      test_support::create_m3_reference_risk_policy_params_or_throw(authority.configuration);
  for (auto& row : risk_params.limit_sets) {
    const bool maximum_values = maximum_worst_quantity == std::numeric_limits<std::int64_t>::max();
    row.maximum_single_order_quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(
        maximum_values ? maximum_worst_quantity : 1000);
    row.maximum_single_order_quote_notional =
        test_support::create_m4_decimal_or_throw<model::Notional>(
            maximum_values ? maximum_worst_quantity : 100000);
    row.maximum_open_order_count = 32U;
    row.maximum_gross_reserved_quote_notional =
        test_support::create_m4_decimal_or_throw<model::Notional>(
            maximum_values ? maximum_worst_quantity : 1000000);
    row.maximum_worst_case_position_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(maximum_worst_quantity);
    row.maximum_worst_case_position_quote_notional =
        test_support::create_m4_decimal_or_throw<model::Notional>(
            maximum_values ? maximum_worst_quantity : 1000000);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Use finite deterministic fake scripts and timestamps solely to produce matching M3 authority.
  constexpr std::uint64_t maximum_attempts = 32U;
  auto encoder =
      extract_economics_result_or_throw(execution::FakeEncoderScript::create_fake_encoder_script(
          execution::FakeEncodingAction::Encode, maximum_attempts, {}));
  auto initiator = extract_economics_result_or_throw(
      execution::FakeInitiatorScript::create_fake_initiator_script(
          execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts, {}));
  model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0x73U);
  auto identities = extract_economics_result_or_throw(
      model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
          model::OrderNamespace{namespace_bytes}));
  std::vector<std::optional<std::uint64_t>> measurements;
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    measurements.emplace_back(10000U + index);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the metadata-only coordinator before binding the independently exercised components to
  // the exact resulting policy fingerprints and explicit capacities.
  authority.submission = extract_economics_result_or_throw(
      runtime::SubmissionCoordinator::create_submission_coordinator(
          authority.configuration, authority.runtime_policy,
          runtime::FakeSubmissionRuntimeParams{
              std::move(risk_params),
              execution::SubmissionPolicyCapacities{maximum_attempts, reservation_capacity,
                                                    reservation_capacity, 1024U,
                                                    reservation_capacity, 400U, 32U},
              std::move(encoder), std::move(initiator),
              std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
                  std::move(measurements)),
              model::DeterministicOrderIdSource{std::move(identities)}}));
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_inventory_source_rows = inventory_capacity;
  capacities.max_inventory_aggregate_cells = aggregate_capacity;
  authority.m4_policy = extract_economics_result_or_throw(runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities));
  return authority;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve only installed route authority; test spellings cannot author provenance or economics.
[[nodiscard]] const execution::InstalledSubmissionRoute&
find_economics_route_or_throw(const test_support::M4OwnerTestAuthority& authority,
                              std::string_view route_text) {
  const auto* route = authority.submission->routes().find_route(
      test_support::parse_m4_identifier_or_throw<model::RouteId>(route_text));
  if (route == nullptr) {
    throw std::logic_error{"missing private fill economics route"};
  }
  return *route;
}

// --------------------------------------------------------
// Derive the exact seven-scope subject from immutable route authority for independent observations.
[[nodiscard]] std::string_view
economics_scope_subject(const execution::InstalledSubmissionRoute& route,
                        risk::RiskScopeKind scope) {
  switch (scope) {
  case risk::RiskScopeKind::Bot:
    return route.attribution().bot_id.value();
  case risk::RiskScopeKind::Desk:
    return route.attribution().desk_id.value();
  case risk::RiskScopeKind::Firm:
    return route.attribution().firm_id.value();
  case risk::RiskScopeKind::Account:
    return route.route().logical_account_id.value();
  case risk::RiskScopeKind::Route:
    return route.route().id.value();
  case risk::RiskScopeKind::Instrument:
    return route.metadata().instrument_id().value();
  case risk::RiskScopeKind::Venue:
    return route.metadata().venue_id().value();
  default:
    throw std::logic_error{"invalid private fill economics scope"};
  }
}

// --------------------------------------------------------
// Copy one complete coherent reservation projection under the precise installed scope key.
[[nodiscard]] risk::RiskScopeExposure
calculate_economics_scope_or_throw(const test_support::M4OwnerTestAuthority& authority,
                                   const risk::ReservationLedger& reservations,
                                   std::string_view route_text, risk::RiskScopeKind scope) {
  const auto& route = find_economics_route_or_throw(authority, route_text);
  const auto result = reservations.calculate_scope_exposure(
      route.attribution().firm_id, scope, economics_scope_subject(route, scope),
      route.metadata().instrument_id(), route.metadata().quote_currency());
  if (!result) {
    throw std::logic_error{"missing private fill economics scope"};
  }
  return *result;
}

// --------------------------------------------------------
// Create a normal caller request; the fixture separately reserves risk and retains OMS admission
// under the exact installed authority before that order can enter component planning.
[[nodiscard]] execution::OrderRequest
create_economics_request_or_throw(const test_support::M4OwnerTestAuthority& authority,
                                  execution::OrderSide side, std::int64_t quantity,
                                  std::string_view route_text = baseline_route) {
  const auto& route = find_economics_route_or_throw(authority, route_text);
  return execution::OrderRequest{
      route.route().id,
      route.metadata().instrument_id(),
      side,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      test_support::create_m4_decimal_or_throw<model::Price>(100),
      test_support::create_m4_decimal_or_throw<model::Quantity>(quantity)};
}

// --------------------------------------------------------

// ########################################################################
// Owns one sealed policy/catalog and one stable pair of actual reservation and OMS components.
// Fill commits exercise the closed economics seam only; the OMS projection deliberately remains
// unchanged until the future journal-backed private reducer owns the complete business commit.
class PrivateFillEconomicsFixture final {
public:

  // --------------------------------------------------------
  // Install the policy-sized inventory component before any reservation or retained OMS admission.
  explicit PrivateFillEconomicsFixture(std::uint32_t reservation_capacity = 8U,
                                       std::uint32_t inventory_capacity = 32U,
                                       bool two_instruments = false,
                                       std::int64_t maximum_worst_quantity = 1000,
                                       std::uint32_t aggregate_capacity = 32U,
                                       bool three_currencies = false)
      : authority{create_economics_authority_or_throw(reservation_capacity, inventory_capacity,
                                                      two_instruments, maximum_worst_quantity,
                                                      aggregate_capacity, three_currencies)},
        outbound{extract_economics_result_or_throw(oms::OutboundOms::create_outbound_oms(32U))},
        reservations{
            extract_economics_result_or_throw(risk::ReservationLedger::create_reservation_ledger(
                authority.submission->reservations().policy(), reservation_capacity))},
        factory{extract_economics_result_or_throw(
            runtime::M4ProvenanceResolver::create_m4_provenance_resolver(authority.configuration,
                                                                         authority.m4_policy))} {
    auto installed = risk::InventoryLedger::install_on_pristine_reservations(
        reservations, outbound, authority.submission->routes(), authority.m4_policy);
    if (!installed) {
      throw std::logic_error{"failed private fill inventory installation"};
    }
  }

  // --------------------------------------------------------
  // Prevent moving the OMS façade or ledger while installed ownership and row pointers bind them.
  PrivateFillEconomicsFixture(const PrivateFillEconomicsFixture&) = delete;
  PrivateFillEconomicsFixture& operator=(const PrivateFillEconomicsFixture&) = delete;
  PrivateFillEconomicsFixture(PrivateFillEconomicsFixture&&) = delete;
  PrivateFillEconomicsFixture& operator=(PrivateFillEconomicsFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the concrete installed component without using a const cast or test mutation bypass.
  [[nodiscard]] risk::InventoryLedger& inventory() noexcept {
    return *risk::InventoryLedger::installed_inventory(reservations);
  }

  // --------------------------------------------------------
  // Run actual risk admission and retain its immutable approved economics in the bound OMS.
  [[nodiscard]] const oms::OutboundOrderRecord&
  reserve_and_admit_order_or_throw(execution::OrderSide side, std::int64_t quantity,
                                   std::string_view route_text = baseline_route) {
    const auto& route = find_economics_route_or_throw(authority, route_text);
    const auto request = create_economics_request_or_throw(authority, side, quantity, route_text);
    const execution::CanonicalOrderEconomics economics{
        request.side, request.type, request.time_in_force, request.price, request.quantity};
    const auto attempt =
        test_support::create_m4_ordinal_or_throw<model::SubmissionAttemptId>(next_attempt_++);
    const auto reserved = reservations.check_and_reserve(attempt, route, economics);
    if (!reserved.is_reserved()) {
      throw std::logic_error{"failed private fill reservation"};
    }
    const auto& root = authority.m4_policy.root_provenance();
    const auto identity = test_support::create_m4_order_id_or_throw(attempt.value());
    auto admitted =
        extract_economics_result_or_throw(outbound.admit_outbound_order(oms::OutboundOrderAdmission{
            attempt, identity, *reserved.reservation_id(), economics, *reserved.exposure(),
            oms::OutboundOrderProvenance{
                route.route().id, route.route().venue_id, route.route().logical_account_id,
                route.metadata().instrument_id(), route.metadata().venue_instrument_id(),
                route.attribution().firm_id, route.attribution().desk_id,
                route.attribution().bot_id, route.attribution().strategy_id,
                root.configuration_fingerprint(), authority.configuration.revision(),
                root.organization_revision(), authority.configuration.routes().revision(),
                route.metadata().revision(), root.runtime_policy_fingerprint(),
                root.risk_policy_fingerprint(), root.risk_policy_revision(),
                root.submission_policy_fingerprint()}}));
    if (!admitted.is_admitted() || admitted.record() == nullptr ||
        !outbound.mark_encoding_succeeded(identity) || !outbound.mark_write_initiated(identity)) {
      throw std::logic_error{"failed private fill OMS admission"};
    }
    return *admitted.record();
  }

  // --------------------------------------------------------
  // Normalize a known-order cumulative execution from source evidence; the normalization result
  // itself claims neither executor admission, trade deduplication, nor economic consumption.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  create_execution_or_throw(const oms::OutboundOrderRecord& order, std::int64_t incremental,
                            std::int64_t cumulative, std::uint8_t identity = 1U,
                            bool reconciliation = false,
                            std::optional<execution::OrderSide> source_side = std::nullopt,
                            std::int64_t execution_price = 100) const {
    const auto& provenance = order.provenance();
    auto locator = extract_economics_result_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(order.order_id(), std::nullopt));
    const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(identity);
    const auto increment = test_support::create_m4_decimal_or_throw<model::Quantity>(incremental);
    const auto cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(cumulative);
    const auto price = test_support::create_m4_decimal_or_throw<model::Price>(execution_price);
    if (reconciliation) {
      return extract_economics_result_or_throw(factory.normalize_reconciliation_execution(
          oms::ReconciliationPrivateEventOrigin{
              test_support::create_m4_reconciliation_epoch_or_throw(),
              test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
              test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(
                  identity),
              model::SourceTimestamp{100U}, model::ReceiveTimestamp{200U}},
          provenance.logical_account_id, provenance.venue_id, std::move(locator), trade,
          provenance.instrument_id, provenance.metadata_revision, increment, cumulative_quantity,
          price, source_side.value_or(order.economics().side)));
    }
    return extract_economics_result_or_throw(factory.normalize_venue_execution(
        oms::VenuePrivateEventOrigin{
            oms::VenuePrivateEventKey{
                provenance.venue_id, provenance.logical_account_id,
                test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U),
                test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(identity)},
            model::SourceTimestamp{100U}, model::ReceiveTimestamp{200U}},
        std::move(locator), trade, provenance.instrument_id, provenance.metadata_revision,
        increment, cumulative_quantity, price, source_side));
  }

  // --------------------------------------------------------
  // Apply one fully planned component replacement and fail setup if that expected commit rejects.
  void apply_fill_or_throw(const oms::OutboundOrderRecord& order, std::int64_t incremental,
                           std::int64_t cumulative, std::uint8_t identity = 1U,
                           bool reconciliation = false) {
    auto plan = extract_economics_result_or_throw(inventory().plan_cumulative_fill(
        order, create_execution_or_throw(order, incremental, cumulative, identity, reconciliation),
        test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(identity)));
    if (!inventory().commit_reservation_inventory_plan(std::move(plan))) {
      throw std::logic_error{"failed private fill component commit"};
    }
  }

  // --------------------------------------------------------
  // Copy one route's exact scope projection after the serialized component operation returns.
  [[nodiscard]] risk::RiskScopeExposure
  scope_exposure_or_throw(risk::RiskScopeKind scope,
                          std::string_view route_text = baseline_route) const {
    return calculate_economics_scope_or_throw(authority, reservations, route_text, scope);
  }

  // --------------------------------------------------------
  // Keep borrowed route and OMS owners alive until the reservation-owned inventory is destroyed.
  test_support::M4OwnerTestAuthority authority;
  oms::OutboundOms outbound;
  risk::ReservationLedger reservations;
  runtime::PrivateOrderEventFactory factory;

private:
  std::uint64_t next_attempt_{1U};
};

// ########################################################################

// --------------------------------------------------------
// Compare every risk field against integer arithmetic independent from the production planner.
void check_integer_scope_projection(const risk::RiskScopeExposure& observed,
                                    std::uint64_t open_orders, std::int64_t confirmed,
                                    std::int64_t reserved_buy, std::int64_t reserved_sell,
                                    std::int64_t multiplier = 10,
                                    std::optional<std::int64_t> currency_worst = std::nullopt,
                                    std::optional<std::int64_t> currency_gross = std::nullopt) {
  const auto calculate_absolute_magnitude = [](std::int64_t value) {
    return value < 0 ? -value : value;
  };
  const auto worst = std::max(calculate_absolute_magnitude(confirmed + reserved_buy),
                              calculate_absolute_magnitude(confirmed - reserved_sell));
  CHECK(observed.open_order_count == open_orders);
  CHECK(observed.confirmed_quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(confirmed));
  CHECK(observed.confirmed_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(confirmed * multiplier));
  CHECK(observed.reserved_buy_quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(reserved_buy));
  CHECK(observed.reserved_sell_quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(reserved_sell));
  CHECK(observed.reserved_buy_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(reserved_buy * multiplier));
  CHECK(observed.reserved_sell_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(reserved_sell * multiplier));
  CHECK(observed.gross_reserved_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(
            currency_gross.value_or((reserved_buy + reserved_sell) * multiplier)));
  CHECK(observed.worst_case_position_quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(worst));
  CHECK(observed.instrument_worst_case_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(worst * multiplier));
  CHECK(observed.worst_case_position_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(
            currency_worst.value_or(worst * multiplier)));
}

// --------------------------------------------------------
// Partial fills transfer residual exposure into signed inventory across every scope; a release
// removes only the unfilled remainder and a later full fill removes its Held count exactly once.
TEST_CASE("private fill economics transfers both sides and releases residuals in all seven scopes",
          "[risk][m4][private-fill-economics][atomic][seven-scopes]") {
  PrivateFillEconomicsFixture fixture;
  const auto& buy = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  const auto& sell = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 4);
  const auto unchanged_oms = buy.private_projection();
  fixture.apply_fill_or_throw(buy, 2, 2, 1U);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 2U, 2, 3, 4);
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope, peer_route), 0U, 0, 0, 0);
  }
  REQUIRE(fixture.reservations.find_reservation(buy.reservation_id()) != nullptr);
  const auto& partial = *fixture.reservations.find_reservation(buy.reservation_id());
  CHECK(partial.state == risk::ReservationState::Held);
  CHECK(partial.closure_cause == risk::ReservationClosureCause::Unassigned);
  CHECK(partial.exposure == buy.exposure());
  CHECK(partial.remaining_exposure.quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(3));
  CHECK(partial.cumulative_confirmed_exposure.quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(2));
  CHECK(buy.private_projection() == unchanged_oms);

  fixture.apply_fill_or_throw(sell, 3, 3, 2U, true);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 2U, -1, 3, 1);
  }
  auto released = extract_economics_result_or_throw(fixture.inventory().plan_terminal_release(
      buy, risk::ReservationClosureCause::DefinitiveCancellation));
  CHECK_FALSE(released.source_after());
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(released)));
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 1U, -1, 0, 1);
  }
  CHECK(fixture.reservations.find_reservation(buy.reservation_id())->closure_cause ==
        risk::ReservationClosureCause::DefinitiveCancellation);
  fixture.apply_fill_or_throw(sell, 1, 4, 3U);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 0U, -2, 0, 0);
  }
  const auto* consumed = fixture.reservations.find_reservation(sell.reservation_id());
  REQUIRE(consumed != nullptr);
  CHECK(consumed->state == risk::ReservationState::ConsumedByFill);
  CHECK(consumed->closure_cause == risk::ReservationClosureCause::FullFill);
  CHECK(fixture.reservations.held_reservation_count() == 0U);
  CHECK(fixture.inventory().source_row_count() == 2U);
  CHECK_FALSE(fixture.inventory().plan_terminal_release(
      sell, risk::ReservationClosureCause::DefinitiveCancellation));
  CHECK_FALSE(fixture.reservations.release_reservation(sell.reservation_id()));
  CHECK(fixture.reservations.held_reservation_count() == 0U);
}

// --------------------------------------------------------
// Cumulative replacement has one final economic answer regardless of ordinary/reconciliation
// message partition, while every retained source keeps the final actual execution and audit ID.
TEST_CASE("private fill economics is partition independent and retains final source provenance",
          "[risk][m4][private-fill-economics][deterministic][provenance]") {
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    PrivateFillEconomicsFixture partitioned;
    PrivateFillEconomicsFixture whole;
    const auto& split_order = partitioned.reserve_and_admit_order_or_throw(side, 5);
    const auto& whole_order = whole.reserve_and_admit_order_or_throw(side, 5);
    partitioned.apply_fill_or_throw(split_order, 1, 1, 1U);
    partitioned.apply_fill_or_throw(split_order, 2, 3, 2U, true);
    partitioned.apply_fill_or_throw(split_order, 2, 5, 3U);
    whole.apply_fill_or_throw(whole_order, 5, 5, 4U, true);
    for (const auto scope : all_scopes) {
      CHECK(partitioned.scope_exposure_or_throw(scope) == whole.scope_exposure_or_throw(scope));
    }
    const auto* split_source = partitioned.inventory().find_source(split_order.order_id());
    const auto* whole_source = whole.inventory().find_source(whole_order.order_id());
    REQUIRE(split_source != nullptr);
    REQUIRE(whole_source != nullptr);
    CHECK(split_source->admission == split_order.admission());
    CHECK(split_source->confirmed_quantity == whole_source->confirmed_quantity);
    CHECK(split_source->confirmed_quote_notional == whole_source->confirmed_quote_notional);
    CHECK(split_source->latest_execution ==
          partitioned.create_execution_or_throw(split_order, 2, 5, 3U));
    CHECK(split_source->latest_audit_ordinal ==
          test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(3U));
    CHECK(partitioned.inventory().source_at(0U) == split_source);
    CHECK(partitioned.inventory().source_at(1U) == nullptr);
  }
}

// --------------------------------------------------------
// A detached plan is authority for one unchanged component incarnation only; neither another owner,
// a competing commit, nor a moved-from plan may apply its replacement a second time.
TEST_CASE("private fill economics rejects wrong owner stale and consumed plans atomically",
          "[risk][m4][private-fill-economics][authority][atomic]") {
  PrivateFillEconomicsFixture fixture;
  PrivateFillEconomicsFixture foreign;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  const auto& other = foreign.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  auto first = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1, 1U),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  auto competing = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 2, 2, 2U),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(2U)));
  const auto before = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  CHECK(first.reservation_before() ==
        *fixture.reservations.find_reservation(order.reservation_id()));
  CHECK(fixture.inventory().source_row_count() == 0U);
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
  CHECK_FALSE(foreign.inventory().commit_reservation_inventory_plan(std::move(first)));
  CHECK(foreign.inventory().source_row_count() == 0U);
  CHECK(foreign.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
  CHECK_FALSE(fixture.inventory().plan_cumulative_fill(
      other, foreign.create_execution_or_throw(other, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(first)));
  const auto after = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const auto source = *fixture.inventory().find_source(order.order_id());
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(first)));
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(competing)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == after);
  CHECK(*fixture.inventory().find_source(order.order_id()) == source);
  CHECK(fixture.inventory().source_row_count() == 1U);
}

// --------------------------------------------------------
// Inventory remains authoritative after its reservation slot is reused; exhausting source history
// rejects the complete fill without losing the new order's conservative remaining exposure.
TEST_CASE("private fill economics preserves source history through reservation reuse and capacity",
          "[risk][m4][private-fill-economics][capacity][atomic]") {
  for (const std::uint32_t source_capacity : {1U, 2U}) {
    PrivateFillEconomicsFixture fixture{1U, source_capacity};
    const auto& first = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2);
    fixture.apply_fill_or_throw(first, 2, 2);
    const auto source = *fixture.inventory().find_source(first.order_id());
    const auto& replacement =
        fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 3);
    CHECK(fixture.reservations.find_reservation(first.reservation_id()) == nullptr);
    CHECK(*fixture.inventory().find_source(first.order_id()) == source);
    const auto before = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
    auto planned = fixture.inventory().plan_cumulative_fill(
        replacement, fixture.create_execution_or_throw(replacement, 1, 1, 2U),
        test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(2U));
    CHECK(fixture.inventory().source_row_capacity() == source_capacity);
    if (source_capacity == 1U) {
      REQUIRE_FALSE(planned);
      CHECK(planned.error().code == model::DomainErrorCode::InventoryCapacityExceeded);
      CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
      CHECK(fixture.inventory().source_row_count() == 1U);
      CHECK(fixture.inventory().find_source(replacement.order_id()) == nullptr);
      CHECK(fixture.reservations.find_reservation(replacement.reservation_id())->state ==
            risk::ReservationState::Held);
    } else {
      REQUIRE(planned);
      REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
      CHECK(fixture.inventory().source_row_count() == 2U);
      for (const auto scope : all_scopes) {
        check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 1U, 1, 0, 2);
      }
    }
    CHECK(*fixture.inventory().find_source(first.order_id()) == source);
  }
}

// --------------------------------------------------------
// Confirmed positions must participate in subsequent risk checks even after a fully consumed
// reservation frees its slot; an opposing order cannot offset beyond the directional worst case.
TEST_CASE("private fill economics participates in later genuine reservation risk decisions",
          "[risk][m4][private-fill-economics][risk-gate]") {
  PrivateFillEconomicsFixture fixture{2U, 32U, false, 5};
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  fixture.apply_fill_or_throw(order, 5, 5);
  const auto& route = find_economics_route_or_throw(fixture.authority, baseline_route);
  const auto before = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const execution::CanonicalOrderEconomics buy{
      execution::OrderSide::Buy, execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      test_support::create_m4_decimal_or_throw<model::Price>(100),
      test_support::create_m4_decimal_or_throw<model::Quantity>(1)};
  const auto rejected = fixture.reservations.check_and_reserve(
      test_support::create_m4_ordinal_or_throw<model::SubmissionAttemptId>(2U), route, buy);
  CHECK_FALSE(rejected.is_reserved());
  CHECK(rejected.reason() == execution::SubmissionReason::WorstCasePositionQuantityExceeded);
  REQUIRE(rejected.risk_evidence());
  CHECK(rejected.risk_evidence()->scope() == risk::RiskScopeKind::Bot);
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
  auto sell = buy;
  sell.side = execution::OrderSide::Sell;
  sell.quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(10);
  const auto accepted = fixture.reservations.check_and_reserve(
      test_support::create_m4_ordinal_or_throw<model::SubmissionAttemptId>(3U), route, sell);
  REQUIRE(accepted.is_reserved());
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 1U, 5, 0, 10);
  }
  REQUIRE(fixture.reservations.release_reservation(*accepted.reservation_id()));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
}

// --------------------------------------------------------
// Shared quote-currency totals sum each instrument's absolute directional maximum; peer firms and
// route/instrument subjects cannot borrow a different instrument's confirmed economics.
TEST_CASE("private fill economics sums instrument maxima and preserves firm ownership",
          "[risk][m4][private-fill-economics][aggregate][seven-scopes]") {
  PrivateFillEconomicsFixture fixture{8U, 32U, true};
  const auto& btc = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  const auto& eth = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 6,
                                                             second_instrument_route);
  fixture.apply_fill_or_throw(btc, 2, 2, 1U);
  fixture.apply_fill_or_throw(eth, 2, 2, 2U, true);
  for (const auto scope : all_scopes) {
    const bool shared =
        scope != risk::RiskScopeKind::Route && scope != risk::RiskScopeKind::Instrument;
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), shared ? 2U : 1U, 2, 3,
                                   0, 10, shared ? 92 : 50, shared ? 58 : 30);
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope, second_instrument_route),
                                   shared ? 2U : 1U, -2, 0, 4, 7, shared ? 92 : 42,
                                   shared ? 58 : 28);
  }
  const auto original_firm = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const auto& peer =
      fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2, peer_route);
  fixture.apply_fill_or_throw(peer, 2, 2, 3U);
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == original_firm);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope, peer_route), 0U, 2, 0, 0);
  }
  auto release = extract_economics_result_or_throw(fixture.inventory().plan_terminal_release(
      btc, risk::ReservationClosureCause::DefinitiveCancellation));
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(release)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm)
            .worst_case_position_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(62));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm).gross_reserved_quote_notional ==
        test_support::create_m4_decimal_or_throw<model::Notional>(28));
  CHECK(fixture.inventory().source_row_count() == 3U);
  CHECK(fixture.inventory().aggregate_cell_count() == fixture.reservations.scope_evidence_count());
  CHECK(fixture.inventory().aggregate_cell_capacity() ==
        fixture.authority.m4_policy.capacities().max_inventory_aggregate_cells);
  for (std::size_t index = 0U; index < fixture.inventory().aggregate_cell_count(); ++index) {
    const auto* cell = fixture.inventory().aggregate_at(index);
    REQUIRE(cell != nullptr);
    const auto coherent = fixture.reservations.scope_evidence_at(index);
    REQUIRE(coherent);
    CHECK(cell->firm_id == coherent->firm_id);
    CHECK(cell->scope == coherent->scope);
    CHECK(cell->scope_subject == coherent->scope_subject);
    CHECK(cell->instrument_id == coherent->instrument_id);
    CHECK(cell->quote_currency == coherent->quote_currency);
    CHECK(cell->confirmed_quantity == coherent->exposure.confirmed_quantity);
    CHECK(cell->confirmed_quote_notional == coherent->exposure.confirmed_quote_notional);
  }
  CHECK(fixture.inventory().aggregate_at(fixture.inventory().aggregate_cell_count()) == nullptr);
}

// --------------------------------------------------------
// A shape-valid source fact is insufficient fill authority when its side, interval, limit price,
// or exact retained order disagrees; every rejection preserves reservations and source history.
TEST_CASE("private fill economics rejects contradictory and noncontiguous source facts",
          "[risk][m4][private-fill-economics][validation][atomic]") {
  PrivateFillEconomicsFixture fixture;
  PrivateFillEconomicsFixture mismatched_policy{8U, 32U, false, 500};
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  const auto& mismatched_order =
      mismatched_policy.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  const auto& peer =
      fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2, peer_route);
  const auto before = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const auto reservation = *fixture.reservations.find_reservation(order.reservation_id());
  const auto ordinary = fixture.create_execution_or_throw(order, 1, 1);
  const auto& payload = std::get<oms::ExecutionPayload>(ordinary.payload());
  const auto& origin = std::get<oms::VenuePrivateEventOrigin>(ordinary.origin_value());
  const auto wrong_metadata =
      extract_economics_result_or_throw(fixture.factory.normalize_venue_execution(
          origin, payload.locator, payload.trade_id, payload.instrument_id,
          test_support::create_m4_ordinal_or_throw<model::InstrumentMetadataRevision>(2U),
          payload.incremental_quantity, payload.cumulative_quantity, payload.execution_price,
          payload.source_side));
  const auto acknowledgement =
      extract_economics_result_or_throw(fixture.factory.normalize_venue_acknowledgement(
          origin, test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U),
          order.order_id()));
  const std::array contradictory{
      fixture.create_execution_or_throw(order, 1, 1, 1U, false, execution::OrderSide::Sell),
      fixture.create_execution_or_throw(order, 1, 2, 2U),
      fixture.create_execution_or_throw(order, 6, 6, 3U),
      fixture.create_execution_or_throw(order, 1, 1, 4U, false, std::nullopt, 101),
      fixture.create_execution_or_throw(peer, 1, 1, 5U),
      mismatched_policy.create_execution_or_throw(mismatched_order, 1, 1, 6U),
      wrong_metadata,
      acknowledgement,
  };
  for (const auto& execution : contradictory) {
    const auto rejected = fixture.inventory().plan_cumulative_fill(
        order, execution, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U));
    REQUIRE_FALSE(rejected);
    CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
    CHECK(*fixture.reservations.find_reservation(order.reservation_id()) == reservation);
    CHECK(fixture.inventory().source_row_count() == 0U);
  }
  fixture.apply_fill_or_throw(order, 2, 2);
  const auto after = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const auto source = *fixture.inventory().find_source(order.order_id());
  for (const auto& repeated : {fixture.create_execution_or_throw(order, 2, 2, 1U),
                               fixture.create_execution_or_throw(order, 1, 1, 6U)}) {
    CHECK_FALSE(fixture.inventory().plan_cumulative_fill(
        order, repeated, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(2U)));
    CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == after);
    CHECK(*fixture.inventory().find_source(order.order_id()) == source);
  }
}

// --------------------------------------------------------
// Only assigned non-fill causes can close residual exposure. Invalid causes, installation retry,
// and intervening ordinary reservation writes cannot preserve a detached plan's mutation authority.
TEST_CASE(
    "private fill economics validates closure causes and invalidates plans on reservation writes",
    "[risk][m4][private-fill-economics][closure][authority]") {
  constexpr std::array valid_causes{
      risk::ReservationClosureCause::DefiniteLocalFailure,
      risk::ReservationClosureCause::ExchangeRejected,
      risk::ReservationClosureCause::DefinitiveCancellation,
      risk::ReservationClosureCause::CompleteAuthoritativeNegative,
  };
  for (const auto cause : valid_causes) {
    PrivateFillEconomicsFixture fixture;
    const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
    const bool has_fill = cause == risk::ReservationClosureCause::DefinitiveCancellation ||
                          cause == risk::ReservationClosureCause::CompleteAuthoritativeNegative;
    if (has_fill) {
      fixture.apply_fill_or_throw(order, 2, 2);
    }
    auto planned =
        extract_economics_result_or_throw(fixture.inventory().plan_terminal_release(order, cause));
    REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned)));
    const auto* closed = fixture.reservations.find_reservation(order.reservation_id());
    REQUIRE(closed != nullptr);
    CHECK(closed->state == risk::ReservationState::Released);
    CHECK(closed->closure_cause == cause);
    for (const auto scope : all_scopes) {
      check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 0U, has_fill ? 2 : 0,
                                     0, 0);
    }
  }
  PrivateFillEconomicsFixture fixture;
  const auto* installed = &fixture.inventory();
  CHECK_FALSE(risk::InventoryLedger::install_on_pristine_reservations(
      fixture.reservations, fixture.outbound, fixture.authority.submission->routes(),
      fixture.authority.m4_policy));
  CHECK(&fixture.inventory() == installed);
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  for (const auto cause :
       {risk::ReservationClosureCause::Unassigned, risk::ReservationClosureCause::FullFill,
        static_cast<risk::ReservationClosureCause>(255U)}) {
    CHECK_FALSE(fixture.inventory().plan_terminal_release(order, cause));
  }
  auto pending = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  const auto& second = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 2);
  const auto after_second = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(pending)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == after_second);
  auto before_release = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  REQUIRE(fixture.reservations.release_reservation(second.reservation_id()));
  const auto after_release = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(before_release)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == after_release);
  CHECK(fixture.inventory().source_row_count() == 0U);
  auto before_oms_change =
      extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
          order, fixture.create_execution_or_throw(order, 1, 1),
          test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  REQUIRE(fixture.outbound.mark_submission_unknown_after_internal_fault(order.order_id()));
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(before_oms_change)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == after_release);
  CHECK(fixture.inventory().source_row_count() == 0U);
}

// --------------------------------------------------------
// Relocating the reservation owner invalidates earlier detached authority while preserving its
// single inventory source. A destroyed incarnation cannot grant its old plan to a replacement.
TEST_CASE("private fill economics rejects detached plans after owner relocation and destruction",
          "[risk][m4][private-fill-economics][authority][lifetime]") {
  PrivateFillEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  auto planned = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  auto relocated = std::move(fixture.reservations);
  auto* inventory = risk::InventoryLedger::installed_inventory(relocated);
  REQUIRE(inventory != nullptr);
  CHECK_FALSE(inventory->commit_reservation_inventory_plan(std::move(planned)));
  CHECK(inventory->source_row_count() == 0U);
  auto fresh = extract_economics_result_or_throw(inventory->plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  REQUIRE(inventory->commit_reservation_inventory_plan(std::move(fresh)));
  fixture.reservations = std::move(relocated);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(fixture.scope_exposure_or_throw(scope), 1U, 1, 4, 0);
  }

  std::optional<risk::ReservationInventoryPlan> expired;
  {
    PrivateFillEconomicsFixture old_incarnation;
    const auto& old_order =
        old_incarnation.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
    expired.emplace(
        extract_economics_result_or_throw(old_incarnation.inventory().plan_cumulative_fill(
            old_order, old_incarnation.create_execution_or_throw(old_order, 1, 1),
            test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U))));
  }
  PrivateFillEconomicsFixture replacement;
  static_cast<void>(replacement.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5));
  CHECK_FALSE(replacement.inventory().commit_reservation_inventory_plan(std::move(*expired)));
  CHECK(replacement.inventory().source_row_count() == 0U);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(replacement.scope_exposure_or_throw(scope), 1U, 0, 5, 0);
  }
}

// --------------------------------------------------------
// Cold installation accepts the exact complete risk-key boundary and leaves existing reservations
// untouched when policy provenance differs or ordinary reservation activity already occurred.
TEST_CASE(
    "private fill economics installs exact aggregate capacity and rejects nonpristine authority",
    "[risk][m4][private-fill-economics][capacity][installation]") {
  PrivateFillEconomicsFixture boundary{1U, 1U, false, 1000, 14U};
  CHECK(boundary.inventory().aggregate_cell_capacity() == 14U);
  CHECK(boundary.inventory().aggregate_cell_count() == 14U);
  CHECK(boundary.inventory().source_row_capacity() == 1U);
  const auto& order = boundary.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2);
  boundary.apply_fill_or_throw(order, 1, 1);
  for (const auto scope : all_scopes) {
    check_integer_scope_projection(boundary.scope_exposure_or_throw(scope), 1U, 1, 1, 0);
  }

  const auto authority = create_economics_authority_or_throw();
  const auto mismatched = create_economics_authority_or_throw(8U, 32U, false, 500);
  auto orders = extract_economics_result_or_throw(oms::OutboundOms::create_outbound_oms(32U));
  auto reservations =
      extract_economics_result_or_throw(risk::ReservationLedger::create_reservation_ledger(
          authority.submission->reservations().policy(), 8U));
  CHECK_FALSE(risk::InventoryLedger::install_on_pristine_reservations(
      reservations, orders, authority.submission->routes(), mismatched.m4_policy));
  CHECK(risk::InventoryLedger::installed_inventory(reservations) == nullptr);
  CHECK(reservations.held_reservation_count() == 0U);

  const auto& route = find_economics_route_or_throw(authority, baseline_route);
  const execution::CanonicalOrderEconomics economics{
      execution::OrderSide::Buy, execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      test_support::create_m4_decimal_or_throw<model::Price>(100),
      test_support::create_m4_decimal_or_throw<model::Quantity>(2)};
  const auto reserved = reservations.check_and_reserve(model::SubmissionAttemptId::create_initial(),
                                                       route, economics);
  REQUIRE(reserved.is_reserved());
  const auto before = *reservations.find_reservation(*reserved.reservation_id());
  CHECK_FALSE(risk::InventoryLedger::install_on_pristine_reservations(
      reservations, orders, authority.submission->routes(), authority.m4_policy));
  CHECK(risk::InventoryLedger::installed_inventory(reservations) == nullptr);
  CHECK(*reservations.find_reservation(*reserved.reservation_id()) == before);
  CHECK(reservations.held_reservation_count() == 1U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// The OMS façade address is not retained-row lifetime authority. Replacing its table must reject
// an outstanding plan before dereferencing any pointer into the destroyed admission storage.
TEST_CASE("private fill economics rejects plans after bound OMS storage replacement",
          "[risk][m4][private-fill-economics][authority][lifetime]") {
  PrivateFillEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 5);
  auto planned = extract_economics_result_or_throw(fixture.inventory().plan_cumulative_fill(
      order, fixture.create_execution_or_throw(order, 1, 1),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U)));
  const auto before = fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm);
  const auto reservation_id = order.reservation_id();
  const auto retained_reservation = *fixture.reservations.find_reservation(reservation_id);
  fixture.outbound = extract_economics_result_or_throw(oms::OutboundOms::create_outbound_oms(32U));
  CHECK(fixture.outbound.order_count() == 0U);
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned)));
  CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm) == before);
  CHECK(*fixture.reservations.find_reservation(reservation_id) == retained_reservation);
  CHECK(fixture.inventory().source_row_count() == 0U);
}

// --------------------------------------------------------
// A representable final signed sum may contain an unrepresentable canonical prefix. Fill planning
// must reject that exact prefix before publishing either ledger's otherwise individually valid
// cells.
TEST_CASE("private fill economics preflights canonical cross-currency quantity fold overflow",
          "[risk][m4][private-fill-economics][overflow][atomic]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  PrivateFillEconomicsFixture fixture{3U, 3U, false, maximum, 64U, true};
  const auto& first = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, maximum,
                                                               "route.economics-aaa");
  fixture.apply_fill_or_throw(first, maximum, maximum, 1U);
  const auto& last = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, maximum,
                                                              "route.economics-zzz");
  fixture.apply_fill_or_throw(last, maximum, maximum, 2U);
  const auto& middle = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, maximum,
                                                                "route.economics-bbb");
  std::vector<risk::RiskScopeExposureEvidence> before;
  for (std::size_t index = 0U; index < fixture.reservations.scope_evidence_count(); ++index) {
    const auto scope = fixture.reservations.scope_evidence_at(index);
    REQUIRE(scope);
    before.push_back(*scope);
  }
  const auto first_source = *fixture.inventory().find_source(first.order_id());
  const auto last_source = *fixture.inventory().find_source(last.order_id());
  const auto middle_reservation = *fixture.reservations.find_reservation(middle.reservation_id());
  const auto rejected = fixture.inventory().plan_cumulative_fill(
      middle, fixture.create_execution_or_throw(middle, maximum, maximum, 3U),
      test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(3U));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::ArithmeticOverflow);
  CHECK(fixture.inventory().source_row_count() == 2U);
  CHECK(fixture.inventory().find_source(middle.order_id()) == nullptr);
  CHECK(*fixture.inventory().find_source(first.order_id()) == first_source);
  CHECK(*fixture.inventory().find_source(last.order_id()) == last_source);
  CHECK(*fixture.reservations.find_reservation(middle.reservation_id()) == middle_reservation);
  for (std::size_t index = 0U; index < before.size(); ++index) {
    const auto after = fixture.reservations.scope_evidence_at(index);
    REQUIRE(after);
    CHECK(*after == before[index]);
  }
}

// --------------------------------------------------------

} // namespace
