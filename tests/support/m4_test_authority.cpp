// Purpose: compose deterministic fake-only M3/M4 and recovery-bootstrap authorities, then submit
// test orders through the same active BotContext used by production runtime.

#include "m4_test_authority.hpp"

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/time.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "aegis/runtime/runtime_diagnostics.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "aegis/trace/runtime_trace.hpp"
#include "reference_configuration.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::test_support {
namespace {

// --------------------------------------------------------
// Parse one fixture identifier or throw std::logic_error when its literal violates the nominal
// grammar.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 authority fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Create the unchanged M2 runtime policy against the M3-enabled two-firm configuration, or throw
// std::logic_error when the sealed authority is inconsistent.
[[nodiscard]] runtime::RuntimePolicy
create_runtime_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U, 32U,
                                       100'000U},
          {{parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::create_initial()}}});
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M4 authority fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create a deterministic M3 fake coordinator with either the inherited failure overrides or an
// all-success lifecycle script; throw std::logic_error when any dependency or composition fails.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
create_submission_coordinator_or_throw(const configuration::StartupConfiguration& configuration,
                                       const runtime::RuntimePolicy& policy,
                                       bool include_reference_failure_overrides) {
  constexpr std::uint64_t maximum_attempts = 10U;

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the original policy-only failure fixture while owner lifecycle tests receive ordinary
  // successful encoding and fake initiation from their first genuine submission.
  std::vector<execution::FakeEncodingOverride> encoder_overrides;
  std::vector<execution::FakeInitiationOverride> initiator_overrides;
  if (include_reference_failure_overrides) {
    encoder_overrides.push_back({1U, execution::FakeEncodingAction::Fail});
    initiator_overrides.push_back(
        {1U, execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance});
    initiator_overrides.push_back({2U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost});
  }
  auto encoder = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, maximum_attempts, std::move(encoder_overrides));
  auto initiator = execution::FakeInitiatorScript::create_fake_initiator_script(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts,
      std::move(initiator_overrides));

  // ++++++++++++++++++++++++++++++++++++++++
  // Give the coordinator one deterministic identity stream and a finite measurement sequence.
  model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0x42U);
  auto order_ids = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{namespace_bytes});
  if (!encoder || !initiator || !order_ids) {
    throw std::logic_error{"invalid deterministic fake in M4 authority fixture"};
  }

  std::vector<std::optional<std::uint64_t>> clock_readings;
  clock_readings.reserve(static_cast<std::size_t>(maximum_attempts * 2U));
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    clock_readings.emplace_back(10'000U + index);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Compose the fake submission owner only after every authored dependency validates.
  auto created = runtime::SubmissionCoordinator::create_submission_coordinator(
      configuration, policy,
      runtime::FakeSubmissionRuntimeParams{
          create_m3_reference_risk_policy_params_or_throw(configuration),
          execution::SubmissionPolicyCapacities{maximum_attempts, 4U, 4U, 1'024U, 2U, 110U, 8U},
          std::move(encoder).value(), std::move(initiator).value(),
          std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
              std::move(clock_readings)),
          model::DeterministicOrderIdSource{std::move(order_ids).value()}});
  if (!created) {
    throw std::logic_error{"invalid submission coordinator in M4 authority fixture"};
  }
  return std::move(created).value();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// ########################################################################
// One probe strategy invokes exactly one public submission while canonical BotRuntime dispatch
// owns the callback-local capability; the result pointer outlives the temporary strategy.
class M4SubmissionProbeStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Retain one immutable request and a borrowed result slot owned by the synchronous test helper.
  M4SubmissionProbeStrategy(execution::OrderRequest request,
                            std::optional<execution::SubmitResult>& result) noexcept
      : request_{std::move(request)}, result_{&result} {}

  // --------------------------------------------------------
  // Submit from the first Ready market-data callback selected by the test dispatch.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
  // Submit from the first readiness-state callback selected by the test dispatch.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Consume the request at most once even if a future initialization emits multiple callbacks.
  void submit_request_once(runtime::BotContext& context) noexcept {
    if (!has_submitted_) {
      has_submitted_ = true;
      result_->emplace(context.submit_order(request_));
    }
  }

  // --------------------------------------------------------
  // Retain the fixed request, synchronous result destination, and one-shot guard.
  execution::OrderRequest request_;
  std::optional<execution::SubmitResult>* result_;
  bool has_submitted_{false};
};

// ########################################################################
// Peer bots remain registered but deliberately inert so exactly the configured route owner can
// submit during the synthetic initialization dispatch.
class M4IdleStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Observe peer market-data callbacks without causing a submission or other side effect.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
  // Observe peer readiness callbacks without causing a submission or other side effect.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Construct one exact decimal fixture value or throw std::logic_error when its scaled value is
// outside the nominal fixed-point domain.
template <typename Decimal>
[[nodiscard]] Decimal create_decimal_or_throw(std::int64_t coefficient, std::uint8_t scale = 0U) {
  auto created = Decimal::from_scaled(coefficient, scale);
  if (!created) {
    throw std::logic_error{"invalid decimal in M4 owner fixture"};
  }
  return created.value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Construct one deterministic fake-medium lineage from a visible byte seed.
[[nodiscard]] recovery::RecoveryLineageId
create_recovery_lineage_from_seed(std::uint8_t seed) noexcept {
  recovery::RecoveryLineageId::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return recovery::RecoveryLineageId{bytes};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Construct one deterministic restart namespace from a distinct visible byte seed.
[[nodiscard]] model::OrderNamespace create_order_namespace_from_seed(std::uint8_t seed) noexcept {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return model::OrderNamespace{bytes};
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Return coherent generic capacities that satisfy every accepted M4 policy relationship.
runtime::M4PolicyCapacities create_ordinary_m4_policy_capacities() noexcept {
  return runtime::M4PolicyCapacities{
      32U, 32U, 32U, 32U, 32U, 32U, 32U, 4U,  32U, 32U, 32U, 32U, 32U,
      32U, 32U, 32U, 32U, 32U, 32U, 8U,  16U, 16U, 4U,  5U,  32U, 3U,
  };
}

// --------------------------------------------------------
// Build the real sealed M1-M3 chain and derive one matching M4 policy; invalid fixture authority
// throws std::logic_error without returning a partial value.
M4TestAuthority create_m4_test_authority_or_throw(runtime::M4PolicyCapacities capacities) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the unchanged reference configuration and runtime authority first.
  auto configured = configuration::StartupConfiguration::create_startup_configuration(
      create_m3_enabled_two_firm_configuration_params_or_throw());
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 authority fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = create_runtime_policy_or_throw(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive risk/submission authority from the deterministic offline M3 composition.
  auto submission = create_submission_coordinator_or_throw(configuration, runtime, true);
  auto policy = runtime::M4Policy::create_m4_policy(configuration, runtime,
                                                    submission->reservations().policy(),
                                                    submission->policy(), capacities);
  if (!policy) {
    throw std::logic_error{"invalid M4 policy in M4 authority fixture"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Retain only the immutable values needed by public M4 unit-test boundaries.
  return M4TestAuthority{std::move(configuration), std::move(policy).value()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build the same authority with ordinary coherent capacities; invalid fixture authority throws
// std::logic_error without returning a partial value.
M4TestAuthority create_m4_test_authority_or_throw() {
  return create_m4_test_authority_or_throw(create_ordinary_m4_policy_capacities());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Retain the sole M3 owner beside the M4 policy derived from authored sealed authorities; invalid
// fixture authority throws std::logic_error without returning a partial owner.
M4OwnerTestAuthority
create_m4_owner_test_authority_or_throw(configuration::StartupConfigurationParams params) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the authored M3-enabled startup and its matching runtime policy.
  auto configured =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 owner fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = create_runtime_policy_or_throw(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the one coordinator before deriving M4 authority from its retained policies.
  auto submission = create_submission_coordinator_or_throw(configuration, runtime, false);
  auto policy = runtime::M4Policy::create_m4_policy(
      configuration, runtime, submission->reservations().policy(), submission->policy(),
      create_ordinary_m4_policy_capacities());
  if (!policy) {
    throw std::logic_error{"invalid M4 policy in M4 owner fixture"};
  }
  return M4OwnerTestAuthority{std::move(configuration),
                              std::move(runtime),
                              std::move(submission),
                              std::move(policy).value(),
                              std::nullopt,
                              0U,
                              model::TurnOrdinal::create_initial(),
                              1'234'567U};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build the unchanged reference owner through the authored owner fixture; invalid authority throws
// std::logic_error without returning a partial owner.
M4OwnerTestAuthority create_m4_owner_test_authority_or_throw() {
  return create_m4_owner_test_authority_or_throw(
      create_m3_enabled_two_firm_configuration_params_or_throw());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Create one exact fake medium, publish and acknowledge the seeded namespace, and retain both
// lifetime owners; any recovery error becomes one fixture-construction failure.
M4RecoveryBootstrapTestAuthority
create_m4_recovery_bootstrap_test_authority_or_throw(const runtime::M4Policy& policy,
                                                     std::uint8_t recovery_lineage_seed,
                                                     std::uint8_t order_namespace_seed) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate the external medium against the exact policy before acquiring its live lease.
  auto created_medium = recovery::DeterministicFakeRecoveryMedium::
      create_deterministic_fake_recovery_medium_from_policy(
          create_recovery_lineage_from_seed(recovery_lineage_seed), policy);
  if (!created_medium) {
    throw std::logic_error{"invalid deterministic recovery medium in M4 authority fixture"};
  }
  auto medium = std::move(created_medium).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish and fake-acknowledge the namespace before exposing the move-only bootstrap fixture.
  auto created_bootstrap = medium->bootstrap_recovery_from_namespace(
      policy, create_order_namespace_from_seed(order_namespace_seed));
  if (!created_bootstrap) {
    throw std::logic_error{"invalid deterministic recovery bootstrap in M4 authority fixture"};
  }
  return M4RecoveryBootstrapTestAuthority{std::move(medium), std::move(created_bootstrap).value()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Compose and consume one seeded recovery authority at the exact production install boundary.
void install_recovery_bound_private_order_reconciler_or_throw(M4OwnerTestAuthority& authority,
                                                              std::uint8_t recovery_lineage_seed,
                                                              std::uint8_t order_namespace_seed) {
  auto recovery = create_m4_recovery_bootstrap_test_authority_or_throw(
      authority.m4_policy, recovery_lineage_seed, order_namespace_seed);
  auto installed = authority.submission->install_recovery_bound_private_order_reconciler(
      authority.configuration, authority.m4_policy, std::move(recovery.bootstrap));
  if (!installed) {
    throw std::logic_error{"failed recovery-bound M4 reconciler installation"};
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build the fixed limit/GTC request without exposing runtime-derived attribution fields.
execution::OrderRequest create_m4_reference_order_request_or_throw() {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      create_decimal_or_throw<model::Price>(100),
      create_decimal_or_throw<model::Quantity>(2)};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Execute one synchronous submission from the configured route owner's genuine active BotContext;
// invalid composition, exhausted identities, or a missing submission result throws.
execution::SubmitResult submit_m4_order_or_throw(M4OwnerTestAuthority& authority,
                                                 const execution::OrderRequest& request) {
  const auto* const route = authority.configuration.routes().find_route(request.route_id);
  if (route == nullptr) {
    throw std::logic_error{"missing route in M4 submission harness"};
  }
  auto next_turn = authority.next_owner_turn.derive_next_ordinal();
  if (!next_turn || authority.next_processing_timestamp_nanoseconds ==
                        std::numeric_limits<std::uint64_t>::max()) {
    throw std::logic_error{"exhausted longitudinal M4 submission harness"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the submitting strategy only for the configured route owner and cover every peer bot.
  std::optional<execution::SubmitResult> result;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.reserve(authority.configuration.organization().bot_attributions().size());
  for (const auto& attribution : authority.configuration.organization().bot_attributions()) {
    std::unique_ptr<runtime::Strategy> strategy;
    if (attribution.bot_id == route->bot_id) {
      strategy = std::make_unique<M4SubmissionProbeStrategy>(request, result);
    } else {
      strategy = std::make_unique<M4IdleStrategy>();
    }
    registrations.push_back(
        runtime::BotStrategyRegistration{attribution.bot_id, std::move(strategy)});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build one normal source state and dispatcher against the coordinator's exact authorities.
  trace::RuntimeTraceSink trace_sink{authority.runtime_policy};
  runtime::RuntimeDiagnosticSink diagnostics{authority.runtime_policy};
  model::DeterministicClockProvider callback_clock{50U};
  const auto& source = authority.runtime_policy.sources().front();
  const auto& definition = source.definition();
  const auto* const metadata = authority.configuration.find_instrument_metadata(
      definition.venue_id, definition.instrument_id);
  if (metadata == nullptr) {
    throw std::logic_error{"missing metadata in M4 submission harness"};
  }
  auto state = market_data::MarketStateMachine::create_market_state_machine(
      authority.runtime_policy, source, *metadata);
  auto bots = runtime::BotRuntime::create_bot_runtime(
      authority.configuration, authority.runtime_policy, callback_clock, trace_sink, diagnostics,
      std::move(registrations),
      runtime::BotRuntimeCounterSeed{authority.last_callback_ordinal,
                                     authority.completed_dispatch_count},
      authority.submission.get());
  if (!state || !bots) {
    throw std::logic_error{"invalid M4 submission harness"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // One initializing state callback activates the route owner's context for exactly one call.
  auto plan =
      bots.value().preflight_dispatch_callbacks(source.ordinal(), authority.next_owner_turn, 1U);
  auto outcome = state.value().initialize_market_state(
      market_data::OwnerMarketTurnContext{
          authority.next_owner_turn,
          model::ProcessingTimestamp{authority.next_processing_timestamp_nanoseconds}},
      trace_sink);
  if (!plan || !outcome) {
    throw std::logic_error{"failed M4 submission harness preflight"};
  }
  auto dispatched = bots.value().dispatch_callbacks(plan.value(), outcome.value());
  if (!dispatched || !result) {
    throw std::logic_error{"failed M4 submission harness dispatch"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Advance predecessors only after canonical dispatch returned with one synchronous result.
  authority.last_callback_ordinal = bots.value().last_callback_ordinal();
  authority.completed_dispatch_count = bots.value().completed_dispatch_count();
  authority.next_owner_turn = std::move(next_turn).value();
  ++authority.next_processing_timestamp_nanoseconds;
  return std::move(*result);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::test_support
