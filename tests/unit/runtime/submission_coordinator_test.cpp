// Purpose: prove the complete M3 coordinator exit matrix, rollback/retention rules, canonical
// evidence order, bot-derived attribution, bounded exhaustion, and deterministic replay.

#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/model/domain_error.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: requires-expression probes prevent a local result or caller-authored request
// from growing exchange acknowledgement or organizational-attribution escape hatches unnoticed.
template <typename Value>
concept HasExchangeAcknowledgement =
    requires(const Value& value) { value.exchange_acknowledged(); };

template <typename Value>
concept HasCallerFirm = requires(Value& value) { value.firm_id; };

template <typename Value>
concept HasPublicCoordinatorSubmitOrder = requires { &Value::submit_order; };

template <typename Value>
concept HasPublicMeasurementClock = requires { &Value::take_measurement_nanosecond_reading; };

template <typename Value>
concept HasPublicCallbackBinding = requires { typename Value::CallbackBinding; };

static_assert(!HasExchangeAcknowledgement<execution::SubmitResult>);
static_assert(!HasCallerFirm<execution::OrderRequest>);
static_assert(!HasPublicCoordinatorSubmitOrder<runtime::SubmissionCoordinator>);
static_assert(!HasPublicMeasurementClock<runtime::SubmissionCoordinator>);
static_assert(!HasPublicCallbackBinding<runtime::SubmissionCoordinator>);
static_assert(std::same_as<decltype(runtime::FakeSubmissionRuntimeParams::order_ids),
                           model::DeterministicOrderIdSource>);
static_assert(!std::is_default_constructible_v<runtime::FakeSubmissionRuntimeParams>);

// ########################################################################

// --------------------------------------------------------
// Parse one nominal identifier or stop on a test-authoring error before exercising the coordinator.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in submission-coordinator fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact decimal fixture input without introducing binary floating-point arithmetic.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view value) {
  auto parsed = Decimal::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid decimal in submission-coordinator fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Construct one checked one-based callback identity or fail as a fixture-authoring defect.
template <typename Ordinal> [[nodiscard]] Ordinal create_ordinal_or_throw(std::uint64_t value) {
  auto created = Ordinal::from_value(value);
  if (!created) {
    throw std::logic_error{"invalid ordinal in submission-coordinator fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Resolve the exact heterogeneous scope subject from immutable startup route authority.
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
    throw std::logic_error{"invalid scope in submission-coordinator fixture"};
  }
}

// --------------------------------------------------------
// Author one complete generous row so individual tests can tighten only the intended risk limit.
[[nodiscard]] risk::RiskLimitSetParams
create_limit_row_or_throw(const execution::ExecutionRoute& route,
                          const organization::BotAttribution& attribution,
                          const model::InstrumentMetadata& metadata, risk::RiskScopeKind scope) {
  return risk::RiskLimitSetParams{
      attribution.firm_id,
      scope,
      derive_risk_scope_subject_or_throw(route, attribution, metadata, scope),
      metadata.instrument_id(),
      std::string{metadata.quote_currency()},
      parse_decimal_or_throw<model::Quantity>("1000"),
      parse_decimal_or_throw<model::Notional>("100000"),
      100U,
      parse_decimal_or_throw<model::Notional>("1000000"),
      parse_decimal_or_throw<model::Quantity>("10000"),
      parse_decimal_or_throw<model::Notional>("1000000"),
  };
}

// --------------------------------------------------------
// Derive the exact revision list and seven complete scopes for every enabled configured route.
[[nodiscard]] risk::RiskPolicyParams
create_risk_policy_params_or_throw(const configuration::StartupConfiguration& configuration) {
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
      throw std::logic_error{"incomplete route in submission-coordinator fixture"};
    }
    metadata_revisions.push_back(configuration::InstrumentMetadataRevisionEntry{
        metadata->venue_id(), metadata->instrument_id(), metadata->revision()});
    for (std::uint8_t value = static_cast<std::uint8_t>(risk::RiskScopeKind::Bot);
         value <= static_cast<std::uint8_t>(risk::RiskScopeKind::Venue); ++value) {
      limits.push_back(create_limit_row_or_throw(route, *attribution, *metadata,
                                                 static_cast<risk::RiskScopeKind>(value)));
    }
  }
  std::sort(metadata_revisions.begin(), metadata_revisions.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.venue_id, left.instrument_id) <
                     std::tie(right.venue_id, right.instrument_id);
            });
  metadata_revisions.erase(std::unique(metadata_revisions.begin(), metadata_revisions.end()),
                           metadata_revisions.end());
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
}

// --------------------------------------------------------

// ########################################################################
// One fixture value keeps the M1 startup authority and its exact M2 policy alive together.
struct SubmissionCoordinatorTestAuthority {
  configuration::StartupConfiguration configuration;
  runtime::RuntimePolicy runtime_policy;
};

// ########################################################################

// --------------------------------------------------------
// Seal the enabled multi-firm startup fixture and a deterministic policy for its public source.
[[nodiscard]] SubmissionCoordinatorTestAuthority create_test_authority_or_throw() {
  auto params = test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  params.subscriptions.push_back(
      market_data::Subscription{parse_identifier_or_throw<model::SubscriptionId>(
                                    "subscription.deribit-subsidiary-btc-perpetual-book"),
                                parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"),
                                parse_identifier_or_throw<model::VenueId>("deribit"),
                                parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
                                market_data::SubscriptionChannel::OrderBook});
  auto configured =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid startup authority in submission-coordinator fixture"};
  }
  auto configuration = std::move(configured).value();
  auto policy = runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U, 32U,
                                       100'000U},
          {{parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::create_initial()}},
      });
  if (!policy) {
    throw std::logic_error{"invalid runtime policy in submission-coordinator fixture"};
  }
  return SubmissionCoordinatorTestAuthority{std::move(configuration), std::move(policy).value()};
}

// --------------------------------------------------------
// Supply balanced positive defaults with enough evidence for every longest ordinary attempt.
[[nodiscard]] execution::SubmissionPolicyCapacities
create_submission_capacities(std::uint64_t maximum_attempts = 8U, std::uint32_t reservations = 8U,
                             std::uint32_t oms_orders = 8U, std::uint32_t accepted_writes = 8U,
                             std::uint32_t trace_records = 88U, std::uint32_t diagnostics = 32U) {
  return execution::SubmissionPolicyCapacities{maximum_attempts, reservations,  oms_orders, 512U,
                                               accepted_writes,  trace_records, diagnostics};
}

// --------------------------------------------------------
// Create a deterministic namespace/counter stream used by ordinary and replay scenarios.
[[nodiscard]] model::DeterministicOrderIdSource
create_order_id_source_or_throw(std::uint8_t namespace_byte = 0x42U,
                                std::uint64_t initial_counter = 1U) {
  model::OrderNamespace::Bytes bytes{};
  bytes.fill(namespace_byte);
  auto created = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{bytes}, initial_counter);
  if (!created) {
    throw std::logic_error{"invalid order provider in submission-coordinator fixture"};
  }
  return model::DeterministicOrderIdSource{std::move(created).value()};
}

// --------------------------------------------------------
// Assemble one complete fake-only runtime parameter value with scripts bounded by outer attempts.
[[nodiscard]] runtime::FakeSubmissionRuntimeParams create_submission_params_or_throw(
    const configuration::StartupConfiguration& configuration,
    execution::SubmissionPolicyCapacities selected_capacities = create_submission_capacities(),
    execution::FakeEncodingAction encoder_default = execution::FakeEncodingAction::Encode,
    execution::FakeInitiationOutcome initiator_default =
        execution::FakeInitiationOutcome::AcceptedAndInitiated,
    std::vector<execution::FakeEncodingOverride> encoder_overrides = {},
    std::vector<execution::FakeInitiationOverride> initiator_overrides = {},
    std::optional<model::DeterministicOrderIdSource> identities = std::nullopt,
    std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock = nullptr) {
  auto encoder = execution::FakeEncoderScript::create_fake_encoder_script(
      encoder_default, selected_capacities.maximum_submission_attempts,
      std::move(encoder_overrides));
  auto initiator = execution::FakeInitiatorScript::create_fake_initiator_script(
      initiator_default, selected_capacities.maximum_submission_attempts,
      std::move(initiator_overrides));
  if (!encoder || !initiator) {
    throw std::logic_error{"invalid fake script in submission-coordinator fixture"};
  }
  if (!identities) {
    identities.emplace(create_order_id_source_or_throw());
  }
  if (!measurement_clock) {
    measurement_clock = std::make_unique<execution::SteadySubmissionMeasurementClock>();
  }
  return runtime::FakeSubmissionRuntimeParams{create_risk_policy_params_or_throw(configuration),
                                              selected_capacities,
                                              std::move(encoder).value(),
                                              std::move(initiator).value(),
                                              std::move(measurement_clock),
                                              std::move(*identities)};
}

// --------------------------------------------------------
// Create a complete coordinator or fail immediately when a supposedly valid fixture is rejected.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
create_coordinator_from_authority_or_throw(const SubmissionCoordinatorTestAuthority& accepted,
                                           runtime::FakeSubmissionRuntimeParams params) {
  auto created = runtime::SubmissionCoordinator::create_submission_coordinator(
      accepted.configuration, accepted.runtime_policy, std::move(params));
  if (!created) {
    throw std::logic_error{"invalid coordinator fixture: " + created.error().context.field};
  }
  return std::move(created).value();
}

// ########################################################################
// A target names only the configured bot whose genuine BotContext must execute a test submission.
struct SubmissionCallbackTarget {
  const configuration::StartupConfiguration* configuration;
  const runtime::RuntimePolicy* runtime_policy;
  const organization::BotAttribution* attribution;
  model::BotId bot_id;
};

// ########################################################################
// The probe strategy invokes exactly one public BotContext submission from a real canonical
// BotRuntime callback and copies the synchronous local result into caller-owned test storage.
class SubmissionProbeStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Copy one immutable request and borrow the result slot that outlives canonical dispatch.
  SubmissionProbeStrategy(execution::OrderRequest request,
                          std::optional<execution::SubmitResult>& result) noexcept
      : request_{std::move(request)}, result_{&result} {}

  // --------------------------------------------------------
  // Submit from the first Ready market-data callback selected by the fixture.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
  // Submit from the first readiness callback selected by the fixture.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Invoke the sole outer submission while canonical dispatch owns the active BotContext.
  void submit_request_once(runtime::BotContext& context) noexcept {
    if (invoked_) {
      return;
    }
    invoked_ = true;
    result_->emplace(context.submit_order(request_));
  }

  // --------------------------------------------------------
  execution::OrderRequest request_;
  std::optional<execution::SubmitResult>* result_;
  bool invoked_{false};
};

// ########################################################################
// Complete strategy coverage requires peer bots even when only one selected context submits.
class IdleStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Accept an unreachable peer market-data callback without creating submission effects.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
  // Accept an unreachable peer state callback without creating submission effects.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Resolve one immutable configured bot identity without manufacturing private callback authority.
[[nodiscard]] SubmissionCallbackTarget
find_callback_binding_or_throw(const SubmissionCoordinatorTestAuthority& authority,
                               std::string_view bot) {
  const auto* const attribution =
      authority.configuration.organization().find_bot(parse_identifier_or_throw<model::BotId>(bot));
  if (attribution == nullptr) {
    throw std::logic_error{"missing bot in submission-coordinator fixture"};
  }
  return SubmissionCallbackTarget{&authority.configuration, &authority.runtime_policy, attribution,
                                  attribution->bot_id};
}

// --------------------------------------------------------
// Build the sole supported exact limit/GTC request for the baseline enabled route.
[[nodiscard]] execution::OrderRequest create_valid_order_request_or_throw() {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      parse_decimal_or_throw<model::Price>("100"),
      parse_decimal_or_throw<model::Quantity>("2")};
}

// --------------------------------------------------------
// Execute one synchronous submission from a genuine active BotContext issued by canonical dispatch;
// the private coordinator remains available only for immutable post-call inspection.
[[nodiscard]] execution::SubmitResult
submit_from_active_context_or_throw(runtime::SubmissionCoordinator& coordinator,
                                    const SubmissionCallbackTarget& callback,
                                    const execution::OrderRequest& request) {
  const auto& configuration = *callback.configuration;
  const auto& policy = *callback.runtime_policy;
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider callback_clock{50U};
  std::optional<execution::SubmitResult> result;

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the one probe strategy at its configured bot and inert strategies for exact peer cover.
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.reserve(configuration.organization().bot_attributions().size());
  for (const auto& attribution : configuration.organization().bot_attributions()) {
    std::unique_ptr<runtime::Strategy> strategy;
    if (attribution.bot_id == callback.bot_id) {
      strategy = std::make_unique<SubmissionProbeStrategy>(request, result);
    } else {
      strategy = std::make_unique<IdleStrategy>();
    }
    registrations.push_back(
        runtime::BotStrategyRegistration{attribution.bot_id, std::move(strategy)});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the normal source owner and BotRuntime against the same sealed provenance as coordinator.
  const auto& source = policy.sources().front();
  const auto& definition = source.definition();
  const auto* const metadata =
      configuration.find_instrument_metadata(definition.venue_id, definition.instrument_id);
  if (metadata == nullptr) {
    throw std::logic_error{"missing metadata in submission BotContext harness"};
  }
  auto state =
      market_data::MarketStateMachine::create_market_state_machine(policy, source, *metadata);
  auto bots = runtime::BotRuntime::create_bot_runtime(configuration, policy, callback_clock,
                                                      trace_sink, diagnostics,
                                                      std::move(registrations), {}, &coordinator);
  if (!state || !bots) {
    throw std::logic_error{"invalid submission BotContext harness"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // One genuine initializing state callback activates the target context for the synchronous call.
  auto plan = bots.value().preflight_dispatch_callbacks(source.ordinal(),
                                                        model::TurnOrdinal::create_initial(), 1U);
  auto outcome = state.value().initialize_market_state(
      market_data::OwnerMarketTurnContext{model::TurnOrdinal::create_initial(),
                                          model::ProcessingTimestamp{1'234'567U}},
      trace_sink);
  if (!plan || !outcome) {
    throw std::logic_error{"failed submission BotContext harness preflight"};
  }
  auto dispatched = bots.value().dispatch_callbacks(plan.value(), outcome.value());
  if (!dispatched || !result) {
    throw std::logic_error{"failed submission BotContext harness dispatch"};
  }
  return std::move(*result);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Assert an ordinary local result without conflating it with exchange or uncertainty semantics.
void check_rejection(const execution::SubmitResult& result, execution::SubmissionStage stage,
                     execution::SubmissionReason reason) {
  CHECK(result.disposition() == execution::SubmitDisposition::LocallyRejected);
  CHECK(result.stage() == stage);
  CHECK(result.reason() == reason);
}

// --------------------------------------------------------
// Compare one attempt's exact accepted trace-event prefix in causal order.
void check_trace(const runtime::SubmissionCoordinator& coordinator,
                 std::initializer_list<trace::SubmissionTraceEventKind> expected,
                 std::optional<std::uint64_t> attempt = std::nullopt) {
  std::vector<trace::SubmissionTraceEventKind> observed;
  for (const auto& record : coordinator.trace_sink().records()) {
    if (!attempt || record.fields().context.attempt_id.value() == *attempt) {
      observed.push_back(record.kind());
    }
  }
  REQUIRE(observed.size() == expected.size());
  auto expected_kind = expected.begin();
  for (const auto observed_kind : observed) {
    CHECK(observed_kind == *expected_kind);
    ++expected_kind;
  }
}

// --------------------------------------------------------
// Count one bounded noncanonical diagnostic kind without assuming unrelated invariant-observation
// multiplicity in defensive paths.
[[nodiscard]] std::uint64_t count_diagnostics(const runtime::SubmissionCoordinator& coordinator,
                                              runtime::SubmissionDiagnosticKind expected) {
  std::uint64_t count = 0U;
  for (std::size_t index = 0U; index < coordinator.diagnostics().diagnostic_count(); ++index) {
    const auto* const record = coordinator.diagnostics().diagnostic_at(index);
    if (record != nullptr && record->kind == expected) {
      ++count;
    }
  }
  return count;
}

// --------------------------------------------------------
// Derive the reservation identity that must equal a result's creating attempt identity.
[[nodiscard]] model::ReservationId
derive_reservation_id_or_throw(const execution::SubmitResult& result) {
  if (!result.attempt_id()) {
    throw std::logic_error{"missing attempt on reservation-bearing result"};
  }
  auto created = model::ReservationId::from_value(result.attempt_id()->value());
  if (!created) {
    throw std::logic_error{"invalid reservation identity in coordinator result"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Mint one valid identity once, then close two copies into the finite scripted source alternative.
[[nodiscard]] model::DeterministicOrderIdSource create_repeated_order_id_source_or_throw() {
  model::OrderNamespace::Bytes bytes{};
  bytes.fill(0x51U);
  auto created = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{bytes});
  if (!created) {
    throw std::logic_error{"failed to create repeated order identity source"};
  }
  auto provider = std::move(created).value();
  auto identity = provider.generate_next_order_id();
  if (!identity) {
    throw std::logic_error{"failed to mint repeated order identity"};
  }
  return model::ScriptedOrderIdProvider{
      std::vector<model::OrderId>{identity.value(), identity.value()}};
}

// --------------------------------------------------------
// Route authority, canonical economics, and fixed risk each stop at their assigned earliest stage.
TEST_CASE("submission coordinator rejects route, canonical, and risk failures in order",
          "[runtime][submission][coordinator][rejection][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove callback attribution cannot borrow an otherwise valid route owned by a peer firm.
  SECTION("route ownership comes only from callback attribution") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration));
    const auto peer = find_callback_binding_or_throw(accepted, "bot.subsidiary-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, peer,
                                                            create_valid_order_request_or_throw());

    check_rejection(result, execution::SubmissionStage::Route,
                    execution::SubmissionReason::RouteNotOwned);
    REQUIRE(result.attempt_id());
    CHECK_FALSE(result.order_id());
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove canonical validation rejects zero quantity before consuming identity or risk capacity.
  SECTION("invalid canonical quantity is rejected before identity and risk") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    auto request = create_valid_order_request_or_throw();
    request.quantity = parse_decimal_or_throw<model::Quantity>("0");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback, request);

    check_rejection(result, execution::SubmissionStage::CanonicalValidation,
                    execution::SubmissionReason::QuantityNotPositive);
    REQUIRE(result.attempt_id());
    CHECK_FALSE(result.order_id());
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove the first fixed limit wins with exact attempt identity and immutable risk evidence.
  SECTION("first fixed risk limit rejection carries identity and exact evidence") {
    const auto accepted = create_test_authority_or_throw();
    auto params = create_submission_params_or_throw(accepted.configuration);
    for (auto& row : params.risk_policy.limit_sets) {
      row.maximum_single_order_quantity = parse_decimal_or_throw<model::Quantity>("1");
    }
    auto coordinator = create_coordinator_from_authority_or_throw(accepted, std::move(params));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    check_rejection(result, execution::SubmissionStage::Risk,
                    execution::SubmissionReason::SingleOrderQuantityExceeded);
    REQUIRE(result.attempt_id());
    REQUIRE(result.order_id());
    REQUIRE(result.risk_evidence());
    CHECK(result.risk_evidence()->scope() == risk::RiskScopeKind::Bot);
    CHECK(result.risk_evidence()->observed_quantity() ==
          parse_decimal_or_throw<model::Quantity>("2"));
    CHECK(result.risk_evidence()->quantity_limit() == parse_decimal_or_throw<model::Quantity>("1"));
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskRejected,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// OMS duplicate and fixed-capacity non-admission release only the current reservation exactly once.
TEST_CASE("submission coordinator rolls back every OMS non-admission",
          "[runtime][submission][coordinator][oms][release][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove duplicate identity precedence wins before an independently full OMS table.
  SECTION("complete duplicate identity wins before OMS capacity") {
    const auto accepted = create_test_authority_or_throw();
    const auto selected = create_submission_capacities(2U, 2U, 2U, 2U, 22U, 8U);
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(
                      accepted.configuration, selected, execution::FakeEncodingAction::Encode,
                      execution::FakeInitiationOutcome::AcceptedAndInitiated, {}, {},
                      create_repeated_order_id_source_or_throw()));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");

    const auto first = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    REQUIRE(first.disposition() == execution::SubmitDisposition::WriteInitiated);
    const auto duplicate = submit_from_active_context_or_throw(
        *coordinator, callback, create_valid_order_request_or_throw());
    check_rejection(duplicate, execution::SubmissionStage::Oms,
                    execution::SubmissionReason::DuplicateOrderIdentity);
    REQUIRE(first.order_id());
    REQUIRE(duplicate.order_id());
    CHECK(*first.order_id() == *duplicate.order_id());
    CHECK(coordinator->outbound_oms().order_count() == 1U);
    REQUIRE(coordinator->outbound_oms().find_order(*first.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*first.order_id())->state() ==
          oms::OutboundOrderState::WriteInitiated);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(first)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(first))
              ->state == risk::ReservationState::Held);
    REQUIRE(coordinator->reservations().find_reservation(
                derive_reservation_id_or_throw(duplicate)) != nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(duplicate))
              ->state == risk::ReservationState::Released);
    CHECK(coordinator->encoder().invocations_consumed() == 1U);
    CHECK(coordinator->initiator().invocations_consumed() == 1U);
    check_trace(*coordinator,
                {trace::SubmissionTraceEventKind::Attempt,
                 trace::SubmissionTraceEventKind::RouteAuthorized,
                 trace::SubmissionTraceEventKind::CanonicalValidated,
                 trace::SubmissionTraceEventKind::IdentityGenerated,
                 trace::SubmissionTraceEventKind::RiskReserved,
                 trace::SubmissionTraceEventKind::OmsNonAdmission,
                 trace::SubmissionTraceEventKind::ReservationReleased,
                 trace::SubmissionTraceEventKind::SubmissionCompleted},
                2U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove a later unique identity reaches the retained OMS capacity boundary.
  SECTION("retained OMS history makes the next unique order hit table capacity") {
    const auto accepted = create_test_authority_or_throw();
    const auto selected = create_submission_capacities(2U, 2U, 1U, 1U, 22U, 8U);
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(
                      accepted.configuration, selected, execution::FakeEncodingAction::Encode,
                      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");

    const auto first = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    check_rejection(first, execution::SubmissionStage::Initiation,
                    execution::SubmissionReason::InitiationDefinitelyFailed);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    const auto full = submit_from_active_context_or_throw(*coordinator, callback,
                                                          create_valid_order_request_or_throw());
    check_rejection(full, execution::SubmissionStage::Oms,
                    execution::SubmissionReason::OmsCapacityExceeded);
    CHECK(coordinator->outbound_oms().order_count() == 1U);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(full)) !=
            nullptr);
    CHECK(
        coordinator->reservations().find_reservation(derive_reservation_id_or_throw(full))->state ==
        risk::ReservationState::Released);
    CHECK(coordinator->encoder().invocations_consumed() == 1U);
    CHECK(coordinator->initiator().invocations_consumed() == 1U);
    check_trace(*coordinator,
                {trace::SubmissionTraceEventKind::Attempt,
                 trace::SubmissionTraceEventKind::RouteAuthorized,
                 trace::SubmissionTraceEventKind::CanonicalValidated,
                 trace::SubmissionTraceEventKind::IdentityGenerated,
                 trace::SubmissionTraceEventKind::RiskReserved,
                 trace::SubmissionTraceEventKind::OmsNonAdmission,
                 trace::SubmissionTraceEventKind::ReservationReleased,
                 trace::SubmissionTraceEventKind::SubmissionCompleted},
                2U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Each fake boundary outcome owns one distinct result, OMS terminal, exposure transition, and
// trace.
TEST_CASE("submission coordinator distinguishes every fake outcome boundary",
          "[runtime][submission][coordinator][fake][outcome][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove a pre-acceptance encoding failure releases its reservation once and remains definite.
  SECTION("scripted encoding failure is definite and releases exactly once") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Fail));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    check_rejection(result, execution::SubmissionStage::Encoding,
                    execution::SubmissionReason::EncodingFailed);
    REQUIRE(result.order_id());
    REQUIRE(coordinator->outbound_oms().find_order(*result.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*result.order_id())->state() ==
          oms::OutboundOrderState::LocallyFailed);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(result))
              ->state == risk::ReservationState::Released);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->initiator().invocations_consumed() == 0U);
    CHECK(coordinator->diagnostics().accepted_count() == 1U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::EncodingFailed,
                               trace::SubmissionTraceEventKind::ReservationReleased,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove definite initiation failure before acceptance releases once without uncertain exposure.
  SECTION("definite pre-acceptance initiation failure releases exactly once") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(
                      accepted.configuration, create_submission_capacities(),
                      execution::FakeEncodingAction::Encode,
                      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    check_rejection(result, execution::SubmissionStage::Initiation,
                    execution::SubmissionReason::InitiationDefinitelyFailed);
    REQUIRE(result.order_id());
    REQUIRE(coordinator->outbound_oms().find_order(*result.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*result.order_id())->state() ==
          oms::OutboundOrderState::LocallyFailed);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(result))
              ->state == risk::ReservationState::Released);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->initiator().accepted_writes().empty());
    CHECK(coordinator->diagnostics().accepted_count() == 1U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::Encoded,
                               trace::SubmissionTraceEventKind::InitiationDefinitelyFailed,
                               trace::SubmissionTraceEventKind::ReservationReleased,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove lost post-acceptance outcome retains conservative exposure and is never retried locally.
  SECTION("accepted then outcome lost is uncertain, retained, and never retried") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(
                      accepted.configuration, create_submission_capacities(),
                      execution::FakeEncodingAction::Encode,
                      execution::FakeInitiationOutcome::AcceptedThenOutcomeLost));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    CHECK(result.disposition() == execution::SubmitDisposition::SubmissionUnknown);
    CHECK(result.stage() == execution::SubmissionStage::Initiation);
    CHECK(result.reason() == execution::SubmissionReason::InitiationOutcomeUnknown);
    REQUIRE(result.order_id());
    REQUIRE(coordinator->outbound_oms().find_order(*result.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*result.order_id())->state() ==
          oms::OutboundOrderState::SubmissionUnknown);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(result))
              ->state == risk::ReservationState::Held);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
    CHECK(coordinator->encoder().invocations_consumed() == 1U);
    CHECK(coordinator->initiator().invocations_consumed() == 1U);
    REQUIRE(coordinator->initiator().accepted_writes().size() == 1U);
    CHECK(coordinator->initiator().accepted_writes().front().attempt_id() == *result.attempt_id());
    CHECK(coordinator->diagnostics().accepted_count() == 1U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::Encoded,
                               trace::SubmissionTraceEventKind::SubmissionUnknown,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove successful fake initiation retains exact attribution without implying acknowledgement.
  SECTION("successful local write initiation retains exact runtime attribution but is never ack") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    CHECK(result.disposition() == execution::SubmitDisposition::WriteInitiated);
    CHECK(result.stage() == execution::SubmissionStage::Initiation);
    CHECK(result.reason() == execution::SubmissionReason::None);
    REQUIRE(result.order_id());
    const auto* const order = coordinator->outbound_oms().find_order(*result.order_id());
    REQUIRE(order != nullptr);
    CHECK(order->state() == oms::OutboundOrderState::WriteInitiated);
    CHECK(order->provenance().firm_id == callback.attribution->firm_id);
    CHECK(order->provenance().desk_id == callback.attribution->desk_id);
    CHECK(order->provenance().bot_id == callback.attribution->bot_id);
    CHECK(order->provenance().strategy_id == callback.attribution->strategy_id);
    CHECK(order->economics().price == create_valid_order_request_or_throw().price);
    CHECK(order->economics().quantity == create_valid_order_request_or_throw().quantity);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(result))
              ->state == risk::ReservationState::Held);
    REQUIRE(coordinator->initiator().accepted_writes().size() == 1U);
    const auto& completed = coordinator->trace_sink().records().back();
    CHECK(completed.fields().context.attribution.firm_id == callback.attribution->firm_id);
    CHECK(completed.fields().release_transition == trace::SubmissionReleaseTransition::Retained);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::Encoded,
                               trace::SubmissionTraceEventKind::WriteInitiated,
                               trace::SubmissionTraceEventKind::SubmissionCompleted});
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Missing and invalid fixed risk policy must prevent publication of any submission-capable stack.
TEST_CASE("submission coordinator construction fails closed without valid complete risk policy",
          "[runtime][submission][coordinator][policy][m3]") {
  const auto accepted = create_test_authority_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove an incomplete seven-scope policy prevents construction before submission is reachable.
  SECTION("missing one required scope is incomplete policy") {
    auto params = create_submission_params_or_throw(accepted.configuration);
    params.risk_policy.limit_sets.pop_back();
    const auto created = runtime::SubmissionCoordinator::create_submission_coordinator(
        accepted.configuration, accepted.runtime_policy, std::move(params));
    REQUIRE_FALSE(created);
    CHECK(created.error().code == model::DomainErrorCode::InvalidRiskPolicy);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove zero capacity remains invalid rather than acquiring an implicit unlimited meaning.
  SECTION("zero never means unlimited") {
    auto params = create_submission_params_or_throw(accepted.configuration);
    params.risk_policy.limit_sets.front().maximum_open_order_count = 0U;
    const auto created = runtime::SubmissionCoordinator::create_submission_coordinator(
        accepted.configuration, accepted.runtime_policy, std::move(params));
    REQUIRE_FALSE(created);
    CHECK(created.error().code == model::DomainErrorCode::InvalidRiskPolicy);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove construction requires an explicit submission measurement clock dependency.
  SECTION("missing submission measurement clock also prevents construction") {
    auto params = create_submission_params_or_throw(accepted.configuration);
    params.measurement_clock.reset();
    const auto created = runtime::SubmissionCoordinator::create_submission_coordinator(
        accepted.configuration, accepted.runtime_policy, std::move(params));
    REQUIRE_FALSE(created);
    CHECK(created.error().code == model::DomainErrorCode::InvalidRelationship);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Attempt, order-identity, and trace-evidence ceilings each fail at their exact non-mutating gate.
TEST_CASE("submission coordinator exhausts every bounded identity and evidence authority",
          "[runtime][submission][coordinator][capacity][exhaustion][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove exhausted attempt identity rejects without advancing trace or owner-local state.
  SECTION("submission attempt exhaustion consumes no new attempt or trace") {
    const auto accepted = create_test_authority_or_throw();
    const auto selected = create_submission_capacities(1U, 1U, 1U, 1U, 11U, 4U);
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration, selected));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    auto request = create_valid_order_request_or_throw();
    request.route_id = parse_identifier_or_throw<model::RouteId>("route.missing");
    const auto first = submit_from_active_context_or_throw(*coordinator, callback, request);
    check_rejection(first, execution::SubmissionStage::Route,
                    execution::SubmissionReason::RouteNotFound);
    const auto before = coordinator->trace_sink().record_count();
    const auto exhausted = submit_from_active_context_or_throw(*coordinator, callback, request);
    check_rejection(exhausted, execution::SubmissionStage::Evidence,
                    execution::SubmissionReason::SubmissionAttemptExhausted);
    CHECK_FALSE(exhausted.attempt_id());
    CHECK_FALSE(exhausted.order_id());
    CHECK_FALSE(exhausted.local_path_nanoseconds());
    CHECK(coordinator->trace_sink().record_count() == before);
    CHECK(coordinator->diagnostics().accepted_count() == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove order counter exhaustion occurs after validation but before any risk reservation.
  SECTION("order counter exhaustion occurs after validation and before risk") {
    const auto accepted = create_test_authority_or_throw();
    const auto selected = create_submission_capacities(2U, 2U, 2U, 2U, 22U, 8U);
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(
            accepted.configuration, selected, execution::FakeEncodingAction::Encode,
            execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, {}, {},
            create_order_id_source_or_throw(0x63U, std::numeric_limits<std::uint64_t>::max())));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto first = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    check_rejection(first, execution::SubmissionStage::Initiation,
                    execution::SubmissionReason::InitiationDefinitelyFailed);
    const auto exhausted = submit_from_active_context_or_throw(
        *coordinator, callback, create_valid_order_request_or_throw());
    check_rejection(exhausted, execution::SubmissionStage::Identity,
                    execution::SubmissionReason::OrderIdentityExhausted);
    REQUIRE(exhausted.attempt_id());
    CHECK_FALSE(exhausted.order_id());
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->outbound_oms().order_count() == 1U);
    check_trace(*coordinator,
                {trace::SubmissionTraceEventKind::Attempt,
                 trace::SubmissionTraceEventKind::RouteAuthorized,
                 trace::SubmissionTraceEventKind::CanonicalValidated,
                 trace::SubmissionTraceEventKind::SubmissionCompleted},
                2U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove evidence preflight failure leaves route, risk, OMS, and trace state untouched.
  SECTION("evidence preflight rejects before route, risk, and trace mutation") {
    const auto accepted = create_test_authority_or_throw();
    const auto selected = create_submission_capacities(2U, 2U, 2U, 2U, 11U, 8U);
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration, selected));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    auto request = create_valid_order_request_or_throw();
    request.route_id = parse_identifier_or_throw<model::RouteId>("route.missing");
    const auto first = submit_from_active_context_or_throw(*coordinator, callback, request);
    check_rejection(first, execution::SubmissionStage::Route,
                    execution::SubmissionReason::RouteNotFound);
    CHECK(coordinator->trace_sink().record_count() == 2U);
    const auto exhausted = submit_from_active_context_or_throw(
        *coordinator, callback, create_valid_order_request_or_throw());
    check_rejection(exhausted, execution::SubmissionStage::Evidence,
                    execution::SubmissionReason::EvidenceCapacityExceeded);
    REQUIRE(exhausted.attempt_id());
    CHECK(exhausted.attempt_id()->value() == 2U);
    CHECK_FALSE(exhausted.order_id());
    CHECK(coordinator->trace_sink().record_count() == 2U);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    CHECK(coordinator->diagnostics().accepted_count() == 1U);
    CHECK_FALSE(exhausted.local_path_nanoseconds());
    REQUIRE(coordinator->diagnostics().diagnostic_at(0U) != nullptr);
    CHECK(coordinator->diagnostics().diagnostic_at(0U)->kind ==
          runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The dedicated clock starts at the public submit entry and uses the exact ADR-assigned endpoint;
// missing or regressing readings never become unsigned durations.
TEST_CASE("submission coordinator measures only valid owner-authorized local paths",
          "[runtime][submission][coordinator][measurement][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove a definitive route rejection records its assigned completion endpoint and no later stage.
  SECTION("definitive route rejection ends after its canonical completion") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{100U, 145U});
    auto* const measurement_access = measurement.get();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    auto request = create_valid_order_request_or_throw();
    request.route_id = parse_identifier_or_throw<model::RouteId>("route.missing");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback, request);

    check_rejection(result, execution::SubmissionStage::Route,
                    execution::SubmissionReason::RouteNotFound);
    CHECK(result.local_path_nanoseconds() == 45U);
    CHECK(measurement_access->readings_consumed() == 2U);
    CHECK(coordinator->diagnostics().accepted_count() == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove definite fake failure records no post-acceptance timing endpoint.
  SECTION("definite fake failure consumes no premature acceptance endpoint") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{200U, 260U});
    auto* const measurement_access = measurement.get();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(
                      accepted.configuration, create_submission_capacities(),
                      execution::FakeEncodingAction::Encode,
                      execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, {}, {},
                      std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    check_rejection(result, execution::SubmissionStage::Initiation,
                    execution::SubmissionReason::InitiationDefinitelyFailed);
    CHECK(result.local_path_nanoseconds() == 60U);
    CHECK(measurement_access->readings_consumed() == 2U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove successful acceptance ends timing at the captured fake initiation outcome.
  SECTION("accepted success ends at the captured fake outcome") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{300U, 325U});
    auto* const measurement_access = measurement.get();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    CHECK(result.disposition() == execution::SubmitDisposition::WriteInitiated);
    CHECK(result.local_path_nanoseconds() == 25U);
    CHECK(measurement_access->readings_consumed() == 2U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove a regressing endpoint omits duration and emits one bounded diagnostic after preflight.
  SECTION("regression yields absence and one post-preflight diagnostic") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{500U, 499U});
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    CHECK(result.disposition() == execution::SubmitDisposition::WriteInitiated);
    CHECK_FALSE(result.local_path_nanoseconds());
    REQUIRE(coordinator->diagnostics().accepted_count() == 1U);
    const auto* const diagnostic = coordinator->diagnostics().diagnostic_at(0U);
    REQUIRE(diagnostic != nullptr);
    CHECK(diagnostic->kind == runtime::SubmissionDiagnosticKind::MeasurementUnavailable);
    CHECK(diagnostic->fields.stage == execution::SubmissionStage::Initiation);
    CHECK(diagnostic->fields.reason == execution::SubmissionReason::None);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove an unavailable entry omits duration and emits one bounded diagnostic after preflight.
  SECTION("unavailable entry yields absence and one post-preflight diagnostic") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{std::nullopt, 700U});
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());

    CHECK(result.disposition() == execution::SubmitDisposition::WriteInitiated);
    CHECK_FALSE(result.local_path_nanoseconds());
    REQUIRE(coordinator->diagnostics().accepted_count() == 1U);
    REQUIRE(coordinator->diagnostics().diagnostic_at(0U) != nullptr);
    CHECK(coordinator->diagnostics().diagnostic_at(0U)->kind ==
          runtime::SubmissionDiagnosticKind::MeasurementUnavailable);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Recursive entry shares the active outer attempt, records one bounded exception, and never starts
// a second order path.
TEST_CASE("submission coordinator rejects recursive submission without a second attempt",
          "[runtime][submission][coordinator][reentry][m3]") {
  const auto accepted = create_test_authority_or_throw();
  const auto selected = create_submission_capacities(2U, 2U, 2U, 2U, 22U, 8U);
  auto coordinator = create_coordinator_from_authority_or_throw(
      accepted, create_submission_params_or_throw(
                    accepted.configuration, selected, execution::FakeEncodingAction::Encode,
                    execution::FakeInitiationOutcome::AcceptedAndInitiated, {}, {},
                    create_order_id_source_or_throw(0x74U)));
  const auto callback =
      find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
  auto nested_request = create_valid_order_request_or_throw();
  nested_request.quantity = parse_decimal_or_throw<model::Quantity>("3");
  CHECK_FALSE(coordinator->arm_reentry_probe_for_test(nested_request, 0U));
  CHECK_FALSE(coordinator->arm_reentry_probe_for_test(nested_request, 4U));
  REQUIRE(coordinator->arm_reentry_probe_for_test(nested_request, 3U));
  CHECK_FALSE(coordinator->arm_reentry_probe_for_test(nested_request, 1U));

  const auto outer = submit_from_active_context_or_throw(*coordinator, callback,
                                                         create_valid_order_request_or_throw());
  REQUIRE(outer.disposition() == execution::SubmitDisposition::WriteInitiated);
  CHECK(coordinator->outbound_oms().order_count() == 1U);
  CHECK(coordinator->reservations().held_reservation_count() == 1U);
  CHECK(coordinator->encoder().invocations_consumed() == 1U);
  CHECK(coordinator->initiator().invocations_consumed() == 1U);
  CHECK(coordinator->diagnostics().accepted_count() == 1U);
  REQUIRE(coordinator->diagnostics().diagnostic_at(0U) != nullptr);
  CHECK(coordinator->diagnostics().diagnostic_at(0U)->kind ==
        runtime::SubmissionDiagnosticKind::ReentryDetected);
  CHECK(coordinator->diagnostics().diagnostic_at(0U)->fields.occurrence_count == 3U);
  const auto reentry =
      std::find_if(coordinator->trace_sink().records().begin(),
                   coordinator->trace_sink().records().end(), [](const auto& record) {
                     return record.kind() == trace::SubmissionTraceEventKind::ReentryRejected;
                   });
  REQUIRE(reentry != coordinator->trace_sink().records().end());
  CHECK(reentry->fields().context.request == nested_request);
  REQUIRE(reentry->fields().final_result.has_value());
  CHECK(reentry->fields().final_result->disposition ==
        execution::SubmitDisposition::LocallyRejected);
  CHECK(reentry->fields().final_result->stage == execution::SubmissionStage::Context);
  CHECK(reentry->fields().final_result->reason == execution::SubmissionReason::SubmissionReentry);
  CHECK(reentry->fields().context.attempt_id == *outer.attempt_id());
  check_trace(
      *coordinator,
      {trace::SubmissionTraceEventKind::Attempt, trace::SubmissionTraceEventKind::RouteAuthorized,
       trace::SubmissionTraceEventKind::CanonicalValidated,
       trace::SubmissionTraceEventKind::ReentryRejected,
       trace::SubmissionTraceEventKind::IdentityGenerated,
       trace::SubmissionTraceEventKind::RiskReserved, trace::SubmissionTraceEventKind::OmsAdmitted,
       trace::SubmissionTraceEventKind::Encoded, trace::SubmissionTraceEventKind::WriteInitiated,
       trace::SubmissionTraceEventKind::SubmissionCompleted});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Canonical append faults must freeze the accepted prefix, stop all later direct-path stages, and
// preserve exact rollback or post-acceptance uncertainty according to the reached boundary.
TEST_CASE("submission coordinator contains every injected canonical append fault",
          "[runtime][submission][coordinator][evidence-fault][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove first re-entry evidence exhaustion rejects before local identities are assigned.
  SECTION("first re-entry evidence failure stops before local identity") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    auto nested_request = create_valid_order_request_or_throw();
    nested_request.quantity = parse_decimal_or_throw<model::Quantity>("3");
    REQUIRE(coordinator->arm_reentry_probe_for_test(nested_request, 1U));
    CHECK_FALSE(coordinator->arm_trace_append_fault_for_test(
        static_cast<runtime::TraceAppendFaultPointForTest>(0U)));
    REQUIRE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::FirstReentryRejected));
    CHECK_FALSE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::RiskReservedBeforeOms));

    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());
    check_rejection(result, execution::SubmissionStage::Internal,
                    execution::SubmissionReason::SubmissionRuntimeFaulted);
    REQUIRE(result.attempt_id());
    CHECK(result.attempt_id()->value() == 1U);
    CHECK_FALSE(result.order_id());
    CHECK(coordinator->is_runtime_faulted());
    CHECK_FALSE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::RiskReservedBeforeOms));
    REQUIRE(coordinator->terminal_error());
    CHECK(coordinator->terminal_error()->context.field ==
          "submission_trace.injected_append_failure");
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated});
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    CHECK(coordinator->initiator().invocations_consumed() == 0U);
    CHECK(coordinator->initiator().accepted_writes().empty());
    CHECK(count_diagnostics(*coordinator,
                            runtime::SubmissionDiagnosticKind::InternalInvariantFailure) >= 1U);
    CHECK(count_diagnostics(*coordinator, runtime::SubmissionDiagnosticKind::ReentryDetected) ==
          1U);
    CHECK(count_diagnostics(*coordinator, runtime::SubmissionDiagnosticKind::ReservationReleased) ==
          0U);
    CHECK(count_diagnostics(*coordinator,
                            runtime::SubmissionDiagnosticKind::UnknownExposureRetained) == 0U);

    const auto trace_size = coordinator->trace_sink().record_count();
    const auto diagnostic_size = coordinator->diagnostics().diagnostic_count();
    const auto later = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    check_rejection(later, execution::SubmissionStage::Internal,
                    execution::SubmissionReason::SubmissionRuntimeFaulted);
    CHECK_FALSE(later.attempt_id());
    CHECK_FALSE(later.order_id());
    CHECK(coordinator->trace_sink().record_count() == trace_size);
    CHECK(coordinator->diagnostics().diagnostic_count() == diagnostic_size);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    CHECK(coordinator->initiator().invocations_consumed() == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove post-reservation evidence exhaustion releases the reservation once before OMS admission.
  SECTION("RiskReserved evidence failure releases exactly once before OMS") {
    const auto accepted = create_test_authority_or_throw();
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted, create_submission_params_or_throw(accepted.configuration));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    REQUIRE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::RiskReservedBeforeOms));

    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());
    check_rejection(result, execution::SubmissionStage::Internal,
                    execution::SubmissionReason::SubmissionRuntimeFaulted);
    REQUIRE(result.attempt_id());
    REQUIRE(result.order_id());
    CHECK(coordinator->is_runtime_faulted());
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated});
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
    const auto* const released =
        coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result));
    REQUIRE(released != nullptr);
    CHECK(released->state == risk::ReservationState::Released);
    CHECK(coordinator->outbound_oms().order_count() == 0U);
    CHECK(coordinator->encoder().invocations_consumed() == 0U);
    CHECK(coordinator->initiator().invocations_consumed() == 0U);
    CHECK(count_diagnostics(*coordinator, runtime::SubmissionDiagnosticKind::ReservationReleased) ==
          1U);
    CHECK(count_diagnostics(*coordinator,
                            runtime::SubmissionDiagnosticKind::UnknownExposureRetained) == 0U);

    const auto trace_size = coordinator->trace_sink().record_count();
    const auto diagnostic_size = coordinator->diagnostics().diagnostic_count();
    const auto later = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    check_rejection(later, execution::SubmissionStage::Internal,
                    execution::SubmissionReason::SubmissionRuntimeFaulted);
    CHECK_FALSE(later.attempt_id());
    CHECK(coordinator->trace_sink().record_count() == trace_size);
    CHECK(coordinator->diagnostics().diagnostic_count() == diagnostic_size);
    CHECK(coordinator->reservations().held_reservation_count() == 0U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove post-initiation evidence exhaustion conservatively downgrades the authoritative OMS row.
  SECTION("WriteInitiated evidence failure downgrades authoritative OMS uncertainty") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{100U, 125U});
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    REQUIRE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::WriteInitiatedAfterAcceptance));

    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());
    CHECK(result.disposition() == execution::SubmitDisposition::SubmissionUnknown);
    CHECK(result.stage() == execution::SubmissionStage::Initiation);
    CHECK(result.reason() == execution::SubmissionReason::InitiationOutcomeUnknown);
    CHECK(result.local_path_nanoseconds() == 25U);
    REQUIRE(result.order_id());
    REQUIRE(coordinator->outbound_oms().find_order(*result.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*result.order_id())->state() ==
          oms::OutboundOrderState::SubmissionUnknown);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
    REQUIRE(coordinator->reservations().find_reservation(derive_reservation_id_or_throw(result)) !=
            nullptr);
    CHECK(coordinator->reservations()
              .find_reservation(derive_reservation_id_or_throw(result))
              ->state == risk::ReservationState::Held);
    REQUIRE(coordinator->initiator().accepted_writes().size() == 1U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::Encoded});
    CHECK(count_diagnostics(*coordinator, runtime::SubmissionDiagnosticKind::ReservationReleased) ==
          0U);
    CHECK(coordinator->is_runtime_faulted());

    const auto trace_size = coordinator->trace_sink().record_count();
    const auto accepted_writes = coordinator->initiator().accepted_writes().size();
    const auto later = submit_from_active_context_or_throw(*coordinator, callback,
                                                           create_valid_order_request_or_throw());
    check_rejection(later, execution::SubmissionStage::Internal,
                    execution::SubmissionReason::SubmissionRuntimeFaulted);
    CHECK_FALSE(later.attempt_id());
    CHECK(coordinator->trace_sink().record_count() == trace_size);
    CHECK(coordinator->initiator().accepted_writes().size() == accepted_writes);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove completion-evidence exhaustion preserves the initiated prefix while downgrading this row.
  SECTION("completion evidence failure preserves initiated prefix but downgrades current OMS") {
    const auto accepted = create_test_authority_or_throw();
    auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
        std::vector<std::optional<std::uint64_t>>{200U, 225U});
    auto coordinator = create_coordinator_from_authority_or_throw(
        accepted,
        create_submission_params_or_throw(accepted.configuration, create_submission_capacities(),
                                          execution::FakeEncodingAction::Encode,
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          {}, {}, std::nullopt, std::move(measurement)));
    const auto callback =
        find_callback_binding_or_throw(accepted, "bot.deribit-btc-perpetual-reference");
    REQUIRE(coordinator->arm_trace_append_fault_for_test(
        runtime::TraceAppendFaultPointForTest::SubmissionCompletedAfterInitiation));

    const auto result = submit_from_active_context_or_throw(*coordinator, callback,
                                                            create_valid_order_request_or_throw());
    CHECK(result.disposition() == execution::SubmitDisposition::SubmissionUnknown);
    CHECK(result.stage() == execution::SubmissionStage::Initiation);
    CHECK(result.reason() == execution::SubmissionReason::InitiationOutcomeUnknown);
    CHECK(result.local_path_nanoseconds() == 25U);
    REQUIRE(result.order_id());
    REQUIRE(coordinator->outbound_oms().find_order(*result.order_id()) != nullptr);
    CHECK(coordinator->outbound_oms().find_order(*result.order_id())->state() ==
          oms::OutboundOrderState::SubmissionUnknown);
    CHECK(coordinator->reservations().held_reservation_count() == 1U);
    REQUIRE(coordinator->initiator().accepted_writes().size() == 1U);
    check_trace(*coordinator, {trace::SubmissionTraceEventKind::Attempt,
                               trace::SubmissionTraceEventKind::RouteAuthorized,
                               trace::SubmissionTraceEventKind::CanonicalValidated,
                               trace::SubmissionTraceEventKind::IdentityGenerated,
                               trace::SubmissionTraceEventKind::RiskReserved,
                               trace::SubmissionTraceEventKind::OmsAdmitted,
                               trace::SubmissionTraceEventKind::Encoded,
                               trace::SubmissionTraceEventKind::WriteInitiated});
    const auto& initiated = coordinator->trace_sink().records().back();
    REQUIRE(initiated.fields().oms_state);
    CHECK(*initiated.fields().oms_state == oms::OutboundOrderState::WriteInitiated);
    REQUIRE(initiated.fields().initiation);
    CHECK(initiated.fields().initiation->outcome ==
          execution::FakeInitiationOutcome::AcceptedAndInitiated);
    CHECK(count_diagnostics(*coordinator, runtime::SubmissionDiagnosticKind::ReservationReleased) ==
          0U);
    CHECK(coordinator->is_runtime_faulted());
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Identical authority, scripts, identities, bindings, and ordered inputs reproduce canonical bytes.
TEST_CASE("submission coordinator reproduces result and trace sequences exactly",
          "[runtime][submission][coordinator][deterministic][m3]") {
  const auto first_authority = create_test_authority_or_throw();
  const auto second_authority = create_test_authority_or_throw();
  const auto selected = create_submission_capacities(2U, 2U, 2U, 2U, 22U, 8U);
  auto first = create_coordinator_from_authority_or_throw(
      first_authority,
      create_submission_params_or_throw(first_authority.configuration, selected,
                                        execution::FakeEncodingAction::Encode,
                                        execution::FakeInitiationOutcome::AcceptedAndInitiated, {},
                                        {}, create_order_id_source_or_throw(0x85U)));
  auto second = create_coordinator_from_authority_or_throw(
      second_authority,
      create_submission_params_or_throw(second_authority.configuration, selected,
                                        execution::FakeEncodingAction::Encode,
                                        execution::FakeInitiationOutcome::AcceptedAndInitiated, {},
                                        {}, create_order_id_source_or_throw(0x85U)));
  const auto first_binding =
      find_callback_binding_or_throw(first_authority, "bot.deribit-btc-perpetual-reference");
  const auto second_binding =
      find_callback_binding_or_throw(second_authority, "bot.deribit-btc-perpetual-reference");
  auto missing = create_valid_order_request_or_throw();
  missing.route_id = parse_identifier_or_throw<model::RouteId>("route.missing");

  const auto first_success = submit_from_active_context_or_throw(
      *first, first_binding, create_valid_order_request_or_throw());
  const auto second_success = submit_from_active_context_or_throw(
      *second, second_binding, create_valid_order_request_or_throw());
  const auto first_rejection = submit_from_active_context_or_throw(*first, first_binding, missing);
  const auto second_rejection =
      submit_from_active_context_or_throw(*second, second_binding, missing);

  // ++++++++++++++++++++++++++++++++++++++++
  // Noncanonical measured durations may differ; every canonical result field and identity matches.
  CHECK(first_success.disposition() == second_success.disposition());
  CHECK(first_success.stage() == second_success.stage());
  CHECK(first_success.reason() == second_success.reason());
  CHECK(first_success.attempt_id() == second_success.attempt_id());
  CHECK(first_success.order_id() == second_success.order_id());
  CHECK(first_rejection.disposition() == second_rejection.disposition());
  CHECK(first_rejection.stage() == second_rejection.stage());
  CHECK(first_rejection.reason() == second_rejection.reason());
  CHECK(first_rejection.attempt_id() == second_rejection.attempt_id());
  CHECK(first_rejection.order_id() == second_rejection.order_id());

  // ++++++++++++++++++++++++++++++++++++++++
  // Trace bytes and accepted fake payloads are exact replay identities, independent of wall timing.
  const auto first_trace = first->trace_sink().encode_canonical_bytes();
  const auto second_trace = second->trace_sink().encode_canonical_bytes();
  REQUIRE(first_trace);
  REQUIRE(second_trace);
  CHECK(first_trace.value() == second_trace.value());
  REQUIRE(first->initiator().accepted_writes().size() == 1U);
  REQUIRE(second->initiator().accepted_writes().size() == 1U);
  const auto first_bytes = first->initiator().accepted_writes().front().bytes();
  const auto second_bytes = second->initiator().accepted_writes().front().bytes();
  REQUIRE(first_bytes.size() == second_bytes.size());
  CHECK(std::equal(first_bytes.begin(), first_bytes.end(), second_bytes.begin()));
  CHECK(first->reservations().held_reservation_count() ==
        second->reservations().held_reservation_count());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
