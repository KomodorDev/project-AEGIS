// Purpose: qualify bounded execution-batch economics using independent fractional-allocation
// expectations, genuine owners, and failure/lifetime checks without consuming private events.

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
#include <span>
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
[[nodiscard]] Value extract_batch_economics_result_or_throw(model::Result<Value> result) {
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
create_batch_economics_configuration_params_or_throw(bool two_instruments, bool three_currencies) {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();

  // Fractional quote face value forces cumulative rounding at the policy's two-decimal scale.
  params.instrument_metadata.front().contract_multiplier =
      test_support::create_m4_decimal_or_throw<model::Notional>(333, 3U);

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
[[nodiscard]] test_support::M4OwnerTestAuthority create_batch_economics_authority_or_throw(
    std::uint32_t reservation_capacity = 8U, std::uint32_t inventory_capacity = 32U,
    bool two_instruments = false, std::int64_t maximum_worst_quantity = 1000,
    std::uint32_t aggregate_capacity = 32U, bool three_currencies = false,
    std::uint32_t pending_capacity = 4U) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Rebind the ordinary source definitions to the complete authored configuration before any
  // replacement submission policy or inventory capacity is derived.
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  authority.configuration = extract_batch_economics_result_or_throw(
      configuration::StartupConfiguration::create_startup_configuration(
          create_batch_economics_configuration_params_or_throw(two_instruments, three_currencies)));
  std::vector<runtime::RuntimeSourceDefinition> sources;
  for (const auto& source : authority.runtime_policy.sources()) {
    sources.push_back(source.definition());
  }
  authority.runtime_policy =
      extract_batch_economics_result_or_throw(runtime::RuntimePolicy::create_runtime_policy(
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
  auto encoder = extract_batch_economics_result_or_throw(
      execution::FakeEncoderScript::create_fake_encoder_script(
          execution::FakeEncodingAction::Encode, maximum_attempts, {}));
  auto initiator = extract_batch_economics_result_or_throw(
      execution::FakeInitiatorScript::create_fake_initiator_script(
          execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts, {}));
  model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0x73U);
  auto identities = extract_batch_economics_result_or_throw(
      model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
          model::OrderNamespace{namespace_bytes}));
  std::vector<std::optional<std::uint64_t>> measurements;
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    measurements.emplace_back(10000U + index);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the metadata-only coordinator before binding the independently exercised components to
  // the exact resulting policy fingerprints and explicit capacities.
  authority.submission = extract_batch_economics_result_or_throw(
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
  capacities.max_pending_fill_intervals_per_order = pending_capacity;
  capacities.max_inventory_source_rows = inventory_capacity;
  capacities.max_inventory_aggregate_cells = aggregate_capacity;
  authority.m4_policy = extract_batch_economics_result_or_throw(runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities));
  return authority;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve only installed route authority; test spellings cannot author provenance or economics.
[[nodiscard]] const execution::InstalledSubmissionRoute&
find_batch_economics_route_or_throw(const test_support::M4OwnerTestAuthority& authority,
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
derive_batch_economics_scope_subject_or_throw(const execution::InstalledSubmissionRoute& route,
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
calculate_batch_economics_scope_or_throw(const test_support::M4OwnerTestAuthority& authority,
                                         const risk::ReservationLedger& reservations,
                                         std::string_view route_text, risk::RiskScopeKind scope) {
  const auto& route = find_batch_economics_route_or_throw(authority, route_text);
  const auto result = reservations.calculate_scope_exposure(
      route.attribution().firm_id, scope,
      derive_batch_economics_scope_subject_or_throw(route, scope), route.metadata().instrument_id(),
      route.metadata().quote_currency());
  if (!result) {
    throw std::logic_error{"missing private fill economics scope"};
  }
  return *result;
}

// --------------------------------------------------------
// Create a normal caller request; the fixture separately reserves risk and retains OMS admission
// under the exact installed authority before that order can enter component planning.
[[nodiscard]] execution::OrderRequest
create_batch_economics_request_or_throw(const test_support::M4OwnerTestAuthority& authority,
                                        execution::OrderSide side, std::int64_t quantity,
                                        std::string_view route_text = baseline_route) {
  const auto& route = find_batch_economics_route_or_throw(authority, route_text);
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
class PrivateFillBatchEconomicsFixture final {
public:

  // --------------------------------------------------------
  // Install the policy-sized inventory component before any reservation or retained OMS admission.
  explicit PrivateFillBatchEconomicsFixture(std::uint32_t reservation_capacity = 8U,
                                            std::uint32_t inventory_capacity = 32U,
                                            bool two_instruments = false,
                                            std::int64_t maximum_worst_quantity = 1000,
                                            std::uint32_t aggregate_capacity = 32U,
                                            bool three_currencies = false,
                                            std::uint32_t pending_capacity = 4U)
      : authority{create_batch_economics_authority_or_throw(
            reservation_capacity, inventory_capacity, two_instruments, maximum_worst_quantity,
            aggregate_capacity, three_currencies, pending_capacity)},
        outbound{
            extract_batch_economics_result_or_throw(oms::OutboundOms::create_outbound_oms(32U))},
        reservations{extract_batch_economics_result_or_throw(
            risk::ReservationLedger::create_reservation_ledger(
                authority.submission->reservations().policy(), reservation_capacity))},
        factory{extract_batch_economics_result_or_throw(
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
  PrivateFillBatchEconomicsFixture(const PrivateFillBatchEconomicsFixture&) = delete;
  PrivateFillBatchEconomicsFixture& operator=(const PrivateFillBatchEconomicsFixture&) = delete;
  PrivateFillBatchEconomicsFixture(PrivateFillBatchEconomicsFixture&&) = delete;
  PrivateFillBatchEconomicsFixture& operator=(PrivateFillBatchEconomicsFixture&&) = delete;

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
    const auto& route = find_batch_economics_route_or_throw(authority, route_text);
    const auto request =
        create_batch_economics_request_or_throw(authority, side, quantity, route_text);
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
    auto admitted = extract_batch_economics_result_or_throw(
        outbound.admit_outbound_order(oms::OutboundOrderAdmission{
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
    auto locator = extract_batch_economics_result_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(order.order_id(), std::nullopt));
    const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(identity);
    const auto increment = test_support::create_m4_decimal_or_throw<model::Quantity>(incremental);
    const auto cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(cumulative);
    const auto price = test_support::create_m4_decimal_or_throw<model::Price>(execution_price);
    if (reconciliation) {
      return extract_batch_economics_result_or_throw(factory.normalize_reconciliation_execution(
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
    return extract_batch_economics_result_or_throw(factory.normalize_venue_execution(
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
    auto plan = extract_batch_economics_result_or_throw(inventory().plan_cumulative_fill(
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
    return calculate_batch_economics_scope_or_throw(authority, reservations, route_text, scope);
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
// Build distinct complete source facts before borrowing them for a contiguous unit-fill batch.
[[nodiscard]] std::vector<oms::NormalizedPrivateOrderInput>
create_batch_executions_or_throw(const PrivateFillBatchEconomicsFixture& fixture,
                                 const oms::OutboundOrderRecord& order, std::size_t count,
                                 bool reconciliation = false, std::int64_t already_applied = 0) {
  std::vector<oms::NormalizedPrivateOrderInput> result;
  result.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    result.push_back(fixture.create_execution_or_throw(
        order, 1, already_applied + static_cast<std::int64_t>(index) + 1,
        static_cast<std::uint8_t>(index + 1U), reconciliation));
  }
  return result;
}

// --------------------------------------------------------
// Borrow stable authored facts and assign prospective increasing audit identities, without
// reserving or publishing any audit record.
[[nodiscard]] std::vector<risk::ReservationInventoryExecutionInput>
create_batch_inputs_or_throw(std::span<const oms::NormalizedPrivateOrderInput> executions,
                             std::uint64_t first_audit = 1U) {
  std::vector<risk::ReservationInventoryExecutionInput> result;
  result.reserve(executions.size());
  for (std::size_t index = 0U; index < executions.size(); ++index) {
    if (index > std::numeric_limits<std::uint64_t>::max() - first_audit) {
      throw std::logic_error{"private fill batch fixture audit overflow"};
    }
    result.push_back(risk::ReservationInventoryExecutionInput{
        executions[index],
        test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(first_audit + index)});
  }
  return result;
}

// --------------------------------------------------------

// ########################################################################
// A copied observation covers every economic cell and source, including peer subjects; equality
// proves failed batch planning or authority checks did not leave an economic prefix behind.
struct BatchEconomicSnapshot {
  std::vector<std::optional<risk::ReservationEvidence>> reservations;
  std::vector<risk::RiskScopeExposureEvidence> scopes;
  std::vector<risk::InventorySourceRecord> sources;
  std::uint64_t held_count;

  // --------------------------------------------------------
  // Compare all business values rather than relying on one summary balance.
  friend bool operator==(const BatchEconomicSnapshot&, const BatchEconomicSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Copy quiescent economics through normal observation APIs, keeping test snapshots detached.
[[nodiscard]] BatchEconomicSnapshot
capture_batch_economics_or_throw(PrivateFillBatchEconomicsFixture& fixture) {
  BatchEconomicSnapshot result{{}, {}, {}, fixture.reservations.held_reservation_count()};
  for (std::size_t index = 0U; index < fixture.reservations.capacity(); ++index) {
    const auto* row = fixture.reservations.reservation_at(index);
    result.reservations.push_back(row == nullptr ? std::nullopt
                                                 : std::optional<risk::ReservationEvidence>{*row});
  }
  for (std::size_t index = 0U; index < fixture.reservations.scope_evidence_count(); ++index) {
    auto row = fixture.reservations.scope_evidence_at(index);
    if (!row) {
      throw std::logic_error{"missing private fill batch scope"};
    }
    result.scopes.push_back(*row);
  }
  for (std::size_t index = 0U; index < fixture.inventory().source_row_count(); ++index) {
    result.sources.push_back(*fixture.inventory().source_at(index));
  }
  return result;
}

// --------------------------------------------------------
// Use integer cents to calculate the independently authored 0.333 multiplier's conservative
// remainder. Quantities in this oracle are small positive fixture integers, never production math.
[[nodiscard]] std::int64_t calculate_fractional_face_cents(std::int64_t quantity) noexcept {
  return (333 * quantity + 9) / 10;
}

// --------------------------------------------------------
// Check every risk field for a single attributed order using independent signed integer values.
void check_fractional_batch_scope(const risk::RiskScopeExposure& observed,
                                  execution::OrderSide side, std::int64_t original,
                                  std::int64_t cumulative, bool released = false) {
  const auto sign = side == execution::OrderSide::Buy ? 1 : -1;
  const auto allocated = calculate_fractional_face_cents(original) -
                         calculate_fractional_face_cents(original - cumulative);
  const auto remaining = released ? 0 : original - cumulative;
  const auto remaining_cents = released ? 0 : calculate_fractional_face_cents(remaining);
  const bool held = remaining != 0;
  // Convert the independent whole-contract expectation into its exact checked decimal.
  const auto create_quantity_or_throw = [](std::int64_t value) {
    return test_support::create_m4_decimal_or_throw<model::Quantity>(value);
  };
  // Convert independent integer cents into the policy's two-place quote notional.
  const auto create_notional_or_throw = [](std::int64_t cents) {
    return test_support::create_m4_decimal_or_throw<model::Notional>(cents, 2U);
  };
  CHECK(observed.open_order_count == (held ? 1U : 0U));
  CHECK(observed.confirmed_quantity == create_quantity_or_throw(sign * cumulative));
  CHECK(observed.confirmed_quote_notional == create_notional_or_throw(sign * allocated));
  CHECK(observed.reserved_buy_quantity ==
        create_quantity_or_throw(side == execution::OrderSide::Buy ? remaining : 0));
  CHECK(observed.reserved_sell_quantity ==
        create_quantity_or_throw(side == execution::OrderSide::Sell ? remaining : 0));
  CHECK(observed.reserved_buy_quote_notional ==
        create_notional_or_throw(side == execution::OrderSide::Buy ? remaining_cents : 0));
  CHECK(observed.reserved_sell_quote_notional ==
        create_notional_or_throw(side == execution::OrderSide::Sell ? remaining_cents : 0));
  CHECK(observed.gross_reserved_quote_notional == create_notional_or_throw(remaining_cents));
  CHECK(observed.worst_case_position_quantity == create_quantity_or_throw(cumulative + remaining));
  CHECK(observed.instrument_worst_case_quote_notional ==
        create_notional_or_throw(allocated + remaining_cents));
  CHECK(observed.worst_case_position_quote_notional ==
        create_notional_or_throw(allocated + remaining_cents));
}

// --------------------------------------------------------
// Each member keeps its real event/trade and independent prefix rounding; the complete five-row
// policy width publishes one coherent seven-scope replacement for either economic side and origin.
TEST_CASE("private fill batch preserves every prefix and final seven-scope economics",
          "[risk][m4][private-fill-batch][rounding][seven-scopes]") {
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    for (const bool reconciliation : {false, true}) {
      for (const std::int64_t original : {5, 7}) {
        CAPTURE(side, reconciliation, original);
        PrivateFillBatchEconomicsFixture fixture;
        const auto& order = fixture.reserve_and_admit_order_or_throw(side, original);
        const auto before = capture_batch_economics_or_throw(fixture);
        const auto peer_before =
            fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm, peer_route);
        const auto oms_before = order.private_projection();
        const auto executions =
            create_batch_executions_or_throw(fixture, order, 5U, reconciliation);
        const auto inputs = create_batch_inputs_or_throw(executions, 11U);
        auto planned = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
        REQUIRE(planned);
        CHECK(capture_batch_economics_or_throw(fixture) == before);
        CHECK(planned.value().execution_effect_count() == 5U);
        CHECK(planned.value().execution_effect_at(5U) == nullptr);
        for (std::size_t index = 0U; index < 5U; ++index) {
          const auto* effect = planned.value().execution_effect_at(index);
          REQUIRE(effect != nullptr);
          const auto cumulative = static_cast<std::int64_t>(index + 1U);
          const auto before_cents = calculate_fractional_face_cents(original) -
                                    calculate_fractional_face_cents(original - cumulative + 1);
          const auto after_cents = calculate_fractional_face_cents(original) -
                                   calculate_fractional_face_cents(original - cumulative);
          const auto sign = side == execution::OrderSide::Buy ? 1 : -1;
          CHECK(effect->execution == executions[index]);
          CHECK(effect->audit_ordinal == inputs[index].audit_ordinal);
          CHECK(effect->reservation_before.cumulative_confirmed_exposure.quantity ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(cumulative - 1));
          CHECK(effect->reservation_after.cumulative_confirmed_exposure.quantity ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(cumulative));
          CHECK(effect->reservation_after.cumulative_confirmed_exposure.quote_notional ==
                test_support::create_m4_decimal_or_throw<model::Notional>(after_cents, 2U));
          CHECK(effect->signed_quantity_delta ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(sign));
          CHECK(effect->signed_notional_delta ==
                test_support::create_m4_decimal_or_throw<model::Notional>(
                    sign * (after_cents - before_cents), 2U));
          for (const auto& scope : effect->scopes) {
            check_fractional_batch_scope(scope.exposure, side, original, cumulative);
          }
        }
        REQUIRE(planned.value().source_after());
        CHECK(planned.value().source_after()->latest_execution == executions.back());
        CHECK(planned.value().source_after()->latest_audit_ordinal == inputs.back().audit_ordinal);
        CHECK(planned.value().reservation_after().state ==
              (original == 5 ? risk::ReservationState::ConsumedByFill
                             : risk::ReservationState::Held));
        for (const auto& scope : planned.value().scope_replacements()) {
          check_fractional_batch_scope(scope.exposure, side, original, 5);
        }
        REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
        CHECK(order.private_projection() == oms_before);
        CHECK(fixture.scope_exposure_or_throw(risk::RiskScopeKind::Firm, peer_route) ==
              peer_before);
        REQUIRE(fixture.inventory().source_row_count() == 1U);
        CHECK(fixture.inventory().find_source(order.order_id())->latest_execution ==
              executions.back());
        for (const auto scope : all_scopes) {
          check_fractional_batch_scope(fixture.scope_exposure_or_throw(scope), side, original, 5);
        }
      }
    }
  }
}

// --------------------------------------------------------
// A cancellation target reached by several fills releases only the final unfilled remainder.
// Execution effects preserve their pre-release economics, and the source still names the last
// trade.
TEST_CASE("private fill batch combines executions with one residual cancellation release",
          "[risk][m4][private-fill-batch][closure]") {
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    PrivateFillBatchEconomicsFixture fixture;
    const auto& order = fixture.reserve_and_admit_order_or_throw(side, 7);
    const auto executions = create_batch_executions_or_throw(fixture, order, 3U);
    const auto inputs = create_batch_inputs_or_throw(executions);
    auto planned = fixture.inventory().plan_cumulative_fill_batch(
        order, inputs, risk::ReservationClosureCause::DefinitiveCancellation);
    REQUIRE(planned);
    CHECK(planned.value().reservation_after().state == risk::ReservationState::Released);
    CHECK(planned.value().reservation_after().closure_cause ==
          risk::ReservationClosureCause::DefinitiveCancellation);
    CHECK(planned.value().reservation_after().remaining_exposure.quantity.coefficient() == 0);
    CHECK(planned.value().reservation_after().remaining_exposure.quote_notional.coefficient() == 0);
    for (const auto& scope : planned.value().scope_replacements()) {
      check_fractional_batch_scope(scope.exposure, side, 7, 3, true);
    }
    const auto* final_effect = planned.value().execution_effect_at(2U);
    REQUIRE(final_effect != nullptr);
    CHECK(final_effect->reservation_after.state == risk::ReservationState::Held);
    for (const auto& scope : final_effect->scopes) {
      check_fractional_batch_scope(scope.exposure, side, 7, 3);
    }
    REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
    for (const auto scope : all_scopes) {
      check_fractional_batch_scope(fixture.scope_exposure_or_throw(scope), side, 7, 3, true);
    }
    CHECK(fixture.inventory().find_source(order.order_id())->latest_execution == executions.back());
    CHECK_FALSE(fixture.inventory().plan_terminal_release(
        order, risk::ReservationClosureCause::DefinitiveCancellation));
  }
}

// --------------------------------------------------------
// Any contradictory member rejects the entire batch even after earlier valid prefixes. A failed
// derivation relinquishes temporary backing so a corrected batch can immediately be planned.
TEST_CASE("private fill batch rejects every failing member without committing a prefix",
          "[risk][m4][private-fill-batch][validation][atomic]") {
  PrivateFillBatchEconomicsFixture fixture;
  PrivateFillBatchEconomicsFixture foreign{8U, 32U, false, 500};
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto& foreign_order =
      foreign.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto before = capture_batch_economics_or_throw(fixture);
  const auto oms_before = order.private_projection();
  for (const bool reconciliation : {false, true}) {
    for (std::size_t failing = 0U; failing < 5U; ++failing) {
      for (std::uint8_t defect = 0U; defect < 6U; ++defect) {
        CAPTURE(reconciliation, failing, defect);
        auto executions = create_batch_executions_or_throw(fixture, order, 5U, reconciliation);
        const auto cumulative = static_cast<std::int64_t>(failing + 1U);
        if (defect == 0U) {
          executions[failing] = fixture.create_execution_or_throw(
              order, 1, cumulative, 20U, reconciliation, execution::OrderSide::Sell);
        } else if (defect == 1U) {
          executions[failing] =
              fixture.create_execution_or_throw(order, 1, cumulative + 1, 20U, reconciliation);
        } else if (defect == 2U) {
          executions[failing] = fixture.create_execution_or_throw(
              order, 1, cumulative, 20U, reconciliation, std::nullopt, 101);
        } else if (defect == 3U) {
          executions[failing] =
              foreign.create_execution_or_throw(foreign_order, 1, cumulative, 20U, reconciliation);
        } else if (defect == 4U) {
          executions[failing] = fixture.create_execution_or_throw(
              order, 8 - static_cast<std::int64_t>(failing), 8, 20U, reconciliation);
        } else {
          executions[failing] = fixture.create_execution_or_throw(
              order, 1, failing == 0U ? 2 : cumulative - 1, 20U, reconciliation);
        }
        const auto inputs = create_batch_inputs_or_throw(executions);
        CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, inputs));
        CHECK(capture_batch_economics_or_throw(fixture) == before);
        CHECK(order.private_projection() == oms_before);
        const auto valid = create_batch_executions_or_throw(fixture, order, 5U, reconciliation);
        const auto valid_inputs = create_batch_inputs_or_throw(valid);
        CHECK(fixture.inventory().plan_cumulative_fill_batch(order, valid_inputs));
      }
    }
  }
}

// --------------------------------------------------------
// Re-normalize one deliberately changed execution payload while preserving its chosen ordinary
// or reconciliation origin. This uses the production shape boundary without mutating sealed input.
[[nodiscard]] oms::NormalizedPrivateOrderInput
normalize_batch_payload_or_throw(const PrivateFillBatchEconomicsFixture& fixture,
                                 const oms::NormalizedPrivateOrderInput& original,
                                 oms::ExecutionPayload payload) {
  if (const auto* origin = std::get_if<oms::VenuePrivateEventOrigin>(&original.origin_value())) {
    return extract_batch_economics_result_or_throw(fixture.factory.normalize_venue_execution(
        *origin, payload.locator, payload.trade_id, payload.instrument_id,
        payload.metadata_revision, payload.incremental_quantity, payload.cumulative_quantity,
        payload.execution_price, payload.source_side));
  }
  return extract_batch_economics_result_or_throw(fixture.factory.normalize_reconciliation_execution(
      std::get<oms::ReconciliationPrivateEventOrigin>(original.origin_value()),
      original.logical_account_id(), original.venue_id(), payload.locator, payload.trade_id,
      payload.instrument_id, payload.metadata_revision, payload.incremental_quantity,
      payload.cumulative_quantity, payload.execution_price, *payload.source_side));
}

// --------------------------------------------------------
// Scale, tick, quantity-step, metadata, and identity defects remain distinguishable from mere
// positive shape validity. A defective late member cannot publish its valid economic predecessor.
TEST_CASE("private fill batch rejects metadata precision and repeated identity defects",
          "[risk][m4][private-fill-batch][validation][atomic]") {
  PrivateFillBatchEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto before = capture_batch_economics_or_throw(fixture);
  for (const bool reconciliation : {false, true}) {
    for (std::uint8_t defect = 0U; defect < 6U; ++defect) {
      CAPTURE(reconciliation, defect);
      auto executions = create_batch_executions_or_throw(fixture, order, 3U, reconciliation);
      auto payload = std::get<oms::ExecutionPayload>(executions.back().payload());
      if (defect == 0U) {
        payload.metadata_revision =
            test_support::create_m4_ordinal_or_throw<model::InstrumentMetadataRevision>(2U);
      } else if (defect == 1U) {
        payload.execution_price = test_support::create_m4_decimal_or_throw<model::Price>(9999, 2U);
      } else if (defect == 2U) {
        payload.execution_price = test_support::create_m4_decimal_or_throw<model::Price>(999, 1U);
      } else if (defect == 3U) {
        payload.incremental_quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(5, 1U);
        payload.cumulative_quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(25, 1U);
      } else if (defect == 4U) {
        payload.trade_id = std::get<oms::ExecutionPayload>(executions.front().payload()).trade_id;
      } else {
        // Reuse only the event/row key; retain the original last member's distinct trade identity
        // so the event check must independently reject this contradiction.
        executions.back() = fixture.create_execution_or_throw(order, 1, 3, 1U, reconciliation);
        REQUIRE(payload.trade_id !=
                std::get<oms::ExecutionPayload>(executions.front().payload()).trade_id);
      }
      executions.back() = normalize_batch_payload_or_throw(fixture, executions.back(), payload);
      const auto inputs = create_batch_inputs_or_throw(executions);
      CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, inputs));
      CHECK(capture_batch_economics_or_throw(fixture) == before);
    }
  }
}

// --------------------------------------------------------
// Distinct exchange keys cannot hide behind one matching local locator, while an earlier candidate
// cannot become correlation authority for a later exchange-only execution in the same batch.
TEST_CASE(
    "private fill batch preserves exchange-key consistency without inventing mapping authority",
    "[risk][m4][private-fill-batch][mapping][atomic]") {
  for (const bool reconciliation : {false, true}) {
    for (std::uint8_t scenario = 0U; scenario < 3U; ++scenario) {
      CAPTURE(reconciliation, scenario);
      PrivateFillBatchEconomicsFixture fixture;
      const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
      const auto exchange_a =
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x31U);
      const auto exchange_b =
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x32U);
      auto executions = create_batch_executions_or_throw(fixture, order, 2U, reconciliation);
      auto first_payload = std::get<oms::ExecutionPayload>(executions.front().payload());
      first_payload.locator = extract_batch_economics_result_or_throw(
          oms::PrivateOrderLocator::create_private_order_locator(order.order_id(), exchange_a));
      executions.front() =
          normalize_batch_payload_or_throw(fixture, executions.front(), first_payload);
      if (scenario == 1U) {
        auto prior = fixture.inventory().plan_cumulative_fill(
            order, executions.front(),
            test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(1U));
        REQUIRE(prior);
        REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(prior).value()));
        executions = create_batch_executions_or_throw(fixture, order, 2U, reconciliation, 1);
      }
      auto last_payload = std::get<oms::ExecutionPayload>(executions.back().payload());
      last_payload.locator = extract_batch_economics_result_or_throw(
          oms::PrivateOrderLocator::create_private_order_locator(
              scenario == 2U ? std::nullopt : std::optional{order.order_id()},
              scenario == 2U ? exchange_a : exchange_b));
      executions.back() =
          normalize_batch_payload_or_throw(fixture, executions.back(), last_payload);
      const auto inputs = create_batch_inputs_or_throw(executions, scenario == 1U ? 2U : 1U);
      const auto before = capture_batch_economics_or_throw(fixture);
      const auto rejected = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().code == model::DomainErrorCode::InvalidReservationConversion);
      CHECK(capture_batch_economics_or_throw(fixture) == before);
      CHECK_FALSE(order.private_projection().exchange_order_id.has_value());
    }
  }
}

// --------------------------------------------------------
// Cold policy width limits retained batch effects, and only a strict partial-fill cancellation
// may append residual release. Empty or over-bound input never acquires lasting scratch authority.
TEST_CASE("private fill batch enforces exact policy width and terminal release shape",
          "[risk][m4][private-fill-batch][capacity][closure]") {
  PrivateFillBatchEconomicsFixture fixture{4U, 4U, false, 1000, 14U, false, 1U};
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 3);
  const auto executions = create_batch_executions_or_throw(fixture, order, 3U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  const auto before = capture_batch_economics_or_throw(fixture);
  CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, {}));
  const auto excessive = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
  REQUIRE_FALSE(excessive);
  CHECK(excessive.error().code == model::DomainErrorCode::InventoryCapacityExceeded);
  for (const auto cause : {risk::ReservationClosureCause::Unassigned,
                           risk::ReservationClosureCause::DefiniteLocalFailure,
                           risk::ReservationClosureCause::ExchangeRejected,
                           risk::ReservationClosureCause::CompleteAuthoritativeNegative,
                           risk::ReservationClosureCause::FullFill,
                           static_cast<risk::ReservationClosureCause>(255U)}) {
    CHECK_FALSE(
        fixture.inventory().plan_cumulative_fill_batch(order, std::span{inputs}.first(2U), cause));
  }
  CHECK(capture_batch_economics_or_throw(fixture) == before);
  auto exact = fixture.inventory().plan_cumulative_fill_batch(order, std::span{inputs}.first(2U));
  REQUIRE(exact);
  CHECK(exact.value().execution_effect_count() == 2U);
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(exact).value()));
  const auto last = fixture.create_execution_or_throw(order, 1, 3, 3U);
  const std::array last_input{risk::ReservationInventoryExecutionInput{
      last, test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(3U)}};
  const auto partial = capture_batch_economics_or_throw(fixture);
  CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(
      order, last_input, risk::ReservationClosureCause::DefinitiveCancellation));
  CHECK(capture_batch_economics_or_throw(fixture) == partial);
  CHECK(fixture.inventory().source_row_capacity() == 4U);
  CHECK(fixture.inventory().aggregate_cell_capacity() == 14U);
}

// --------------------------------------------------------
// Prospective audit ordinals preserve source chronology across a whole batch and prior fills;
// duplicate, decreasing, or numerically wrapped candidates cannot become source provenance.
TEST_CASE("private fill batch requires strictly increasing audit linkage",
          "[risk][m4][private-fill-batch][audit]") {
  PrivateFillBatchEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  fixture.apply_fill_or_throw(order, 1, 1, 10U);
  const auto executions = create_batch_executions_or_throw(fixture, order, 3U, false, 1);
  const auto before = capture_batch_economics_or_throw(fixture);
  constexpr std::array audit_sequences{
      std::array<std::uint64_t, 3U>{10U, 11U, 12U}, std::array<std::uint64_t, 3U>{11U, 11U, 12U},
      std::array<std::uint64_t, 3U>{11U, 12U, 11U}, std::array<std::uint64_t, 3U>{11U, 13U, 14U},
      std::array<std::uint64_t, 3U>{11U, std::numeric_limits<std::uint64_t>::max(), 1U}};
  for (const auto& ordinals : audit_sequences) {
    const std::array inputs{
        risk::ReservationInventoryExecutionInput{
            executions[0],
            test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(ordinals[0])},
        risk::ReservationInventoryExecutionInput{
            executions[1],
            test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(ordinals[1])},
        risk::ReservationInventoryExecutionInput{
            executions[2],
            test_support::create_m4_ordinal_or_throw<recovery::AuditOrdinal>(ordinals[2])}};
    CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, inputs));
    CHECK(capture_batch_economics_or_throw(fixture) == before);
  }
  const auto valid = create_batch_inputs_or_throw(executions, 11U);
  auto planned = fixture.inventory().plan_cumulative_fill_batch(order, valid);
  REQUIRE(planned);
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
  CHECK(fixture.inventory().find_source(order.order_id())->latest_audit_ordinal.value() == 13U);
  for (const auto scope : all_scopes) {
    check_fractional_batch_scope(fixture.scope_exposure_or_throw(scope), execution::OrderSide::Buy,
                                 7, 4);
  }
}

// --------------------------------------------------------
// One cold scratch lease belongs to one live batch result. Moves transfer it, destruction returns
// it, and legacy scalar plans neither seize that lease nor gain authority from its contents.
TEST_CASE("private fill batch owns exclusive movable scratch without altering legacy plans",
          "[risk][m4][private-fill-batch][lifetime][authority]") {
  PrivateFillBatchEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto executions = create_batch_executions_or_throw(fixture, order, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  const auto before = capture_batch_economics_or_throw(fixture);
  auto legacy = fixture.inventory().plan_cumulative_fill(order, executions.front(),
                                                         inputs.front().audit_ordinal);
  REQUIRE(legacy);
  CHECK(legacy.value().execution_effect_count() == 0U);
  {
    auto first = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
    REQUIRE(first);
    CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, inputs));
    auto second_legacy = fixture.inventory().plan_cumulative_fill(order, executions.front(),
                                                                  inputs.front().audit_ordinal);
    REQUIRE(second_legacy);
    CHECK(second_legacy.value().execution_effect_count() == 0U);
    auto moved = std::move(first).value();
    CHECK(first.value().execution_effect_count() == 0U);
    CHECK(first.value().execution_effect_at(0U) == nullptr);
    CHECK(moved.execution_effect_count() == 2U);
    CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(first).value()));
    CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(order, inputs));
    CHECK(capture_batch_economics_or_throw(fixture) == before);
  }
  auto replacement = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
  REQUIRE(replacement);
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(replacement).value()));
  CHECK(replacement.value().execution_effect_count() == 0U);
  CHECK(replacement.value().execution_effect_at(0U) == nullptr);
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(legacy).value()));
  const auto after = capture_batch_economics_or_throw(fixture);
  CHECK_FALSE(
      fixture.inventory().commit_reservation_inventory_plan(std::move(replacement).value()));
  CHECK(capture_batch_economics_or_throw(fixture) == after);
  const auto later = create_batch_executions_or_throw(fixture, order, 1U, false, 2);
  const auto later_inputs = create_batch_inputs_or_throw(later, 3U);
  CHECK(fixture.inventory().plan_cumulative_fill_batch(order, later_inputs));
  CHECK(fixture.inventory().execution_batch_capacity() == 5U);
}

// --------------------------------------------------------
// Plans retain complete execution evidence after caller-owned inputs disappear and after their
// owner dies. Retained bytes remain inspectable, but neither foreign nor replacement owners can
// acquire the dead incarnation's mutation authority.
TEST_CASE("private fill batch retains detached evidence without reviving destroyed ownership",
          "[risk][m4][private-fill-batch][lifetime][authority]") {
  std::optional<risk::ReservationInventoryPlan> expired;
  std::optional<oms::NormalizedPrivateOrderInput> expected_last;
  {
    PrivateFillBatchEconomicsFixture fixture;
    const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 7);
    {
      const auto executions = create_batch_executions_or_throw(fixture, order, 3U, true);
      const auto inputs = create_batch_inputs_or_throw(executions, 7U);
      expected_last = executions.back();
      expired.emplace(extract_batch_economics_result_or_throw(
          fixture.inventory().plan_cumulative_fill_batch(order, inputs)));
    }
    REQUIRE(expired->execution_effect_at(2U) != nullptr);
    CHECK(expired->execution_effect_at(2U)->execution == *expected_last);
    CHECK(fixture.inventory().source_row_count() == 0U);
  }
  REQUIRE(expired->execution_effect_at(2U) != nullptr);
  CHECK(expired->execution_effect_at(2U)->execution == *expected_last);
  CHECK(expired->execution_effect_at(2U)->audit_ordinal.value() == 9U);
  PrivateFillBatchEconomicsFixture replacement;
  const auto& order = replacement.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 7);
  const auto before = capture_batch_economics_or_throw(replacement);
  CHECK_FALSE(replacement.inventory().commit_reservation_inventory_plan(std::move(*expired)));
  CHECK(capture_batch_economics_or_throw(replacement) == before);
  const auto executions = create_batch_executions_or_throw(replacement, order, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  CHECK(replacement.inventory().plan_cumulative_fill_batch(order, inputs));
}

// --------------------------------------------------------
// A genuine record belongs to exactly one bound table; an equal copy or a peer owner's row cannot
// authorize planning, and a foreign commit cannot consume the original owner's scratch lease.
TEST_CASE("private fill batch rejects detached rows and foreign owners before mutation",
          "[risk][m4][private-fill-batch][authority]") {
  PrivateFillBatchEconomicsFixture fixture;
  PrivateFillBatchEconomicsFixture foreign;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto& foreign_order =
      foreign.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto detached = order;
  const auto executions = create_batch_executions_or_throw(fixture, order, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  const auto before = capture_batch_economics_or_throw(fixture);
  const auto foreign_before = capture_batch_economics_or_throw(foreign);
  CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(detached, inputs));
  CHECK_FALSE(fixture.inventory().plan_cumulative_fill_batch(foreign_order, inputs));
  auto plan = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
  REQUIRE(plan);
  CHECK_FALSE(foreign.inventory().commit_reservation_inventory_plan(std::move(plan).value()));
  CHECK(plan.value().execution_effect_count() == 2U);
  CHECK(capture_batch_economics_or_throw(fixture) == before);
  CHECK(capture_batch_economics_or_throw(foreign) == foreign_before);
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(plan).value()));
}

// --------------------------------------------------------
// Every component mutation invalidates old batch authority. Keeping a stale plan alive retains its
// evidence lease but never lets a rejected commit replace newer reservations, sources, or OMS
// state.
TEST_CASE("private fill batch rejects stale plans after intervening owner mutations",
          "[risk][m4][private-fill-batch][authority][atomic]") {
  PrivateFillBatchEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto& other = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell, 2);
  const auto executions = create_batch_executions_or_throw(fixture, order, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  auto planned = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
  REQUIRE(planned);
  SECTION("new reservation") {
    static_cast<void>(fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 1));
  }
  SECTION("other reservation release") {
    REQUIRE(fixture.reservations.release_reservation(other.reservation_id()));
  }
  SECTION("legacy fill commit") { fixture.apply_fill_or_throw(order, 1, 1, 3U); }
  SECTION("same OMS projection changes") {
    REQUIRE(fixture.outbound.mark_submission_unknown_after_internal_fault(order.order_id()));
  }
  const auto before_rejection = capture_batch_economics_or_throw(fixture);
  const auto oms_before_rejection = order.private_projection();
  CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
  CHECK(planned.value().execution_effect_count() == 2U);
  CHECK(capture_batch_economics_or_throw(fixture) == before_rejection);
  CHECK(order.private_projection() == oms_before_rejection);
}

// --------------------------------------------------------
// Relocation changes reservation authority, and replacing the OMS storage invalidates row pointers
// even when its façade address is reused. Neither rejection may inspect a destroyed row.
TEST_CASE("private fill batch rejects plans after ledger relocation and OMS replacement",
          "[risk][m4][private-fill-batch][authority][lifetime]") {
  PrivateFillBatchEconomicsFixture fixture;
  const auto& order = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 7);
  const auto executions = create_batch_executions_or_throw(fixture, order, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions);
  auto planned = fixture.inventory().plan_cumulative_fill_batch(order, inputs);
  REQUIRE(planned);
  const auto before = capture_batch_economics_or_throw(fixture);
  SECTION("reservation owner relocates") {
    auto relocated = std::move(fixture.reservations);
    auto* inventory = risk::InventoryLedger::installed_inventory(relocated);
    REQUIRE(inventory != nullptr);
    CHECK_FALSE(inventory->commit_reservation_inventory_plan(std::move(planned).value()));
    fixture.reservations = std::move(relocated);
  }
  SECTION("OMS storage is replaced at the same address") {
    fixture.outbound =
        extract_batch_economics_result_or_throw(oms::OutboundOms::create_outbound_oms(32U));
    CHECK_FALSE(fixture.inventory().commit_reservation_inventory_plan(std::move(planned).value()));
    CHECK(fixture.outbound.order_count() == 0U);
  }
  CHECK(capture_batch_economics_or_throw(fixture) == before);
  REQUIRE(planned.value().execution_effect_at(1U) != nullptr);
  CHECK(planned.value().execution_effect_at(1U)->execution == executions[1U]);
}

// --------------------------------------------------------
// Append-only confirmed sources outlive reusable reservation slots. A full source table rejects
// an entire new-order batch while leaving every earlier source and newly held reservation intact.
TEST_CASE("private fill batch preflights retained source capacity before committing any member",
          "[risk][m4][private-fill-batch][capacity][atomic]") {
  PrivateFillBatchEconomicsFixture fixture{1U, 1U, false, 1000, 14U};
  const auto& first = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2);
  const auto first_executions = create_batch_executions_or_throw(fixture, first, 2U);
  const auto first_inputs = create_batch_inputs_or_throw(first_executions);
  auto first_plan = fixture.inventory().plan_cumulative_fill_batch(first, first_inputs);
  REQUIRE(first_plan);
  REQUIRE(fixture.inventory().commit_reservation_inventory_plan(std::move(first_plan).value()));
  const auto source = *fixture.inventory().find_source(first.order_id());
  const auto& next = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 3);
  const auto executions = create_batch_executions_or_throw(fixture, next, 3U);
  const auto inputs = create_batch_inputs_or_throw(executions, 3U);
  const auto before = capture_batch_economics_or_throw(fixture);
  const auto rejected = fixture.inventory().plan_cumulative_fill_batch(next, inputs);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::InventoryCapacityExceeded);
  CHECK(capture_batch_economics_or_throw(fixture) == before);
  CHECK(*fixture.inventory().find_source(first.order_id()) == source);
  CHECK(fixture.inventory().find_source(next.order_id()) == nullptr);
  CHECK(fixture.reservations.find_reservation(next.reservation_id())->state ==
        risk::ReservationState::Held);
}

// --------------------------------------------------------
// The second valid fill can overflow a canonical aggregate prefix even when its final signed sum
// fits. The planner must discard the earlier successful virtual prefix and preserve both ledgers.
TEST_CASE("private fill batch rejects a late canonical aggregate overflow atomically",
          "[risk][m4][private-fill-batch][overflow][atomic]") {
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  PrivateFillBatchEconomicsFixture fixture{3U, 3U, false, maximum, 64U, true};
  const auto& first = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy,
                                                               maximum - 1, "route.economics-aaa");
  fixture.apply_fill_or_throw(first, maximum - 1, maximum - 1, 1U);
  const auto& last = fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Sell,
                                                              maximum - 1, "route.economics-zzz");
  fixture.apply_fill_or_throw(last, maximum - 1, maximum - 1, 2U);
  const auto& middle =
      fixture.reserve_and_admit_order_or_throw(execution::OrderSide::Buy, 2, "route.economics-bbb");
  const auto executions = create_batch_executions_or_throw(fixture, middle, 2U);
  const auto inputs = create_batch_inputs_or_throw(executions, 3U);
  const auto before = capture_batch_economics_or_throw(fixture);
  const auto rejected = fixture.inventory().plan_cumulative_fill_batch(middle, inputs);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::ArithmeticOverflow);
  CHECK(capture_batch_economics_or_throw(fixture) == before);
  CHECK(fixture.inventory().source_row_count() == 2U);
  CHECK(fixture.inventory().find_source(middle.order_id()) == nullptr);
  auto prefix = fixture.inventory().plan_cumulative_fill_batch(middle, std::span{inputs}.first(1U));
  REQUIRE(prefix);
  CHECK(prefix.value().reservation_after().cumulative_confirmed_exposure.quantity ==
        test_support::create_m4_decimal_or_throw<model::Quantity>(1));
  CHECK(capture_batch_economics_or_throw(fixture) == before);
}

// --------------------------------------------------------

} // namespace
