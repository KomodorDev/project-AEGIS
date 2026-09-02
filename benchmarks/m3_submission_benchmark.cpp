// Purpose: measure the two named M3 bot-bound submission workloads using only the duration captured
// inside the synchronous route-to-fake path and allocations scoped exactly around submit_order().

#include "aegis/runtime/fake_submission_runtime.hpp"
#include "aegis/runtime/market_runtime.hpp"
#include "reference_configuration.hpp"
#include "support/allocation_tracking.hpp"
#include "support/distribution_metrics.hpp"

#include <algorithm>
#include <array>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// Both workloads use the same fixed sample count and capacity shape so only the rejecting risk
// fingerprint, its bound submission fingerprint, and workload identity differ between labels.
constexpr std::int64_t distribution_iterations = 10'000;
constexpr std::uint32_t submission_attempt_capacity = 10'000U;
constexpr std::uint32_t submission_trace_capacity = 110'000U;
constexpr std::uint32_t runtime_trace_capacity = 20'016U;

// ########################################################################
// The workload kind selects only the generous or first-limit-rejecting risk snapshot and the
// corresponding expected local result; request, route, runtime, scripts, and capacities are shared.
enum class SubmissionBenchmarkKind : std::uint8_t {
  AuthorizedFakeInitiation = 1,
  InlineRiskRejection = 2,
};

// ########################################################################

// --------------------------------------------------------
// Fail immediately when a benchmark fixture contains an invalid nominal identifier literal.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M3 benchmark fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Fail immediately when a benchmark fixture contains an invalid exact decimal literal.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view text) {
  auto parsed = Decimal::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid decimal in M3 benchmark fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal the enabled two-firm startup authority independently for each workload fixture.
[[nodiscard]] configuration::StartupConfiguration create_reference_configuration_or_throw() {
  auto created = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_m3_enabled_two_firm_configuration_params_or_throw());
  if (!created) {
    throw std::logic_error{"invalid startup configuration in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Define the same sole public BTC perpetual source already used by the accepted M2 fixture.
[[nodiscard]] runtime::RuntimeSourceDefinition create_reference_source_or_throw() {
  return runtime::RuntimeSourceDefinition{
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial(),
  };
}

// --------------------------------------------------------
// Seal one value-identical runtime policy with enough trace space for bootstrap plus 10,001 frames.
[[nodiscard]] runtime::RuntimePolicy
create_reference_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration, runtime::RuntimePolicyParams{
                         runtime::RuntimePolicyLimits{1U, 4'096U, 64U, 20U, 1'000'000'000U, 2U, 32U,
                                                      runtime_trace_capacity, 32U, 1'000'000'000U},
                         {create_reference_source_or_throw()},
                     });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Install the same route projection the coordinator will seal so benchmark labels derive from the
// exact risk and submission policy bytes rather than from hard-coded fingerprint text.
[[nodiscard]] execution::OwnerLocalRouteCatalog
create_route_catalog_or_throw(const configuration::StartupConfiguration& configuration) {
  std::vector<execution::SubmissionRouteInput> inputs;
  inputs.reserve(configuration.routes().routes().size());
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"incomplete route projection in M3 benchmark fixture"};
    }
    inputs.push_back(execution::SubmissionRouteInput{route, *attribution, *metadata});
  }
  const auto& provenance = configuration.provenance();
  auto created = execution::OwnerLocalRouteCatalog::create_owner_local_route_catalog(
      configuration.fingerprint(), provenance.configuration_revision(),
      provenance.organization_revision(), provenance.route_revision(), std::move(inputs));
  if (!created) {
    throw std::logic_error{"invalid route catalog in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Reproduce the assigned AEGISFOE positional-size proof over every installed route. This value is
// a validation input only; it is not part of either measured submission interval.
[[nodiscard]] std::uint64_t
calculate_required_encoded_order_bytes_or_throw(const execution::OwnerLocalRouteCatalog& routes) {
  constexpr std::uint64_t fixed_bytes = 8U + 2U + model::OrderId::byte_size + (9U * 2U) + 3U +
                                        (2U * 9U) + (4U * model::sha256_digest_size) +
                                        (5U * sizeof(std::uint64_t));
  std::uint64_t maximum = fixed_bytes;
  for (const auto& installed : routes.routes()) {
    const auto& route = installed.route();
    const auto& attribution = installed.attribution();
    const std::string_view identifiers[]{
        route.id.value(),
        route.venue_id.value(),
        route.logical_account_id.value(),
        route.instrument_id.value(),
        installed.metadata().venue_instrument_id().value(),
        attribution.firm_id.value(),
        attribution.desk_id.value(),
        attribution.bot_id.value(),
        attribution.strategy_id.value(),
    };
    std::uint64_t candidate = fixed_bytes;
    for (const auto identifier : identifiers) {
      if (identifier.size() > std::numeric_limits<std::uint64_t>::max() - candidate) {
        throw std::logic_error{"AEGISFOE size overflow in M3 benchmark fixture"};
      }
      candidate += static_cast<std::uint64_t>(identifier.size());
    }
    maximum = std::max(maximum, candidate);
  }
  return maximum;
}

// --------------------------------------------------------
// Give both workloads exactly the same bounded owner-local storage and fake-script authority.
[[nodiscard]] constexpr execution::SubmissionPolicyCapacities
create_submission_capacities() noexcept {
  return execution::SubmissionPolicyCapacities{
      distribution_iterations,
      submission_attempt_capacity,
      submission_attempt_capacity,
      static_cast<std::uint16_t>(execution::maximum_encoded_fake_order_bytes),
      submission_attempt_capacity,
      submission_trace_capacity,
      32U,
  };
}

// --------------------------------------------------------
// Create the permanent all-encode script with exactly the outer-attempt invocation domain.
[[nodiscard]] execution::FakeEncoderScript create_encoder_script_or_throw() {
  auto created = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, distribution_iterations, {});
  if (!created) {
    throw std::logic_error{"invalid encoder script in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create the permanent accepted-and-initiated fake script with no failure override.
[[nodiscard]] execution::FakeInitiatorScript create_initiator_script_or_throw() {
  auto created = execution::FakeInitiatorScript::create_fake_initiator_script(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, distribution_iterations, {});
  if (!created) {
    throw std::logic_error{"invalid initiator script in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Fix one 128-bit namespace whose 10,000 one-based counters cannot collide or exhaust.
[[nodiscard]] constexpr model::OrderNamespace benchmark_order_namespace() noexcept {
  return model::OrderNamespace{model::OrderNamespace::Bytes{
      0x00U,
      0x01U,
      0x02U,
      0x03U,
      0x04U,
      0x05U,
      0x06U,
      0x07U,
      0x08U,
      0x09U,
      0x0aU,
      0x0bU,
      0x0cU,
      0x0dU,
      0x0eU,
      0x0fU,
  }};
}

// --------------------------------------------------------
// Transfer one fresh deterministic identity stream into the concrete fake-only composition.
[[nodiscard]] model::DeterministicOrderIdSource create_order_id_source_or_throw() {
  auto created = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      benchmark_order_namespace());
  if (!created) {
    throw std::logic_error{"invalid order identity stream in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Give both workloads the exact same authorized, aligned, limit-only quantity-two request.
[[nodiscard]] execution::OrderRequest create_benchmark_request_or_throw() {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      parse_decimal_or_throw<model::Price>("100.0"),
      parse_decimal_or_throw<model::Quantity>("2"),
  };
}

// --------------------------------------------------------
// Encode the exact immutable fixture provenance grammar consumed by the M3 evidence collector.
[[nodiscard]] std::string create_m3_evidence_label_or_throw(
    std::string_view workload_id, const configuration::StartupConfiguration& configuration,
    const runtime::RuntimePolicy& runtime_policy, const risk::RiskPolicySnapshot& risk_policy,
    const execution::SubmissionPolicy& submission_policy) {
  const auto route_id =
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual");
  const auto* const route = configuration.routes().find_route(route_id);
  const auto* const metadata = configuration.find_instrument_metadata(
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"));
  if (route == nullptr || metadata == nullptr) {
    throw std::logic_error{"missing label authority in M3 benchmark fixture"};
  }
  const auto& provenance = configuration.provenance();
  return "workload_id=" + std::string{workload_id} +
         ";configuration_fingerprint_sha256=" + configuration.fingerprint().to_hex() +
         ";configuration_revision=" + std::to_string(provenance.configuration_revision().value()) +
         ";organization_revision=" + std::to_string(provenance.organization_revision().value()) +
         ";runtime_policy_fingerprint_sha256=" + runtime_policy.fingerprint().to_hex() +
         ";risk_policy_fingerprint_sha256=" + risk_policy.fingerprint().to_hex() +
         ";risk_policy_revision=" + std::to_string(risk_policy.revision().value()) +
         ";submission_policy_fingerprint_sha256=" + submission_policy.fingerprint().to_hex() +
         ";route_id=" + std::string{route->id.value()} +
         ";route_revision=" + std::to_string(provenance.route_revision().value()) +
         ";account_id=" + std::string{route->logical_account_id.value()} +
         ";venue_id=" + std::string{route->venue_id.value()} +
         ";instrument_id=" + std::string{route->instrument_id.value()} +
         ";metadata_revision=" + std::to_string(metadata->revision().value()) +
         ";order_namespace_hex=" + benchmark_order_namespace().to_hex();
}

// --------------------------------------------------------
// Build one complete canonical snapshot that makes the market owner Ready before measurements.
[[nodiscard]] std::string create_snapshot_frame() {
  return "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|"
         "ok:m3-benchmark-snapshot|2|B,100.0,1|A,100.5,1";
}

// --------------------------------------------------------
// Build one contiguous absolute delta that changes both levels and therefore dispatches one event.
[[nodiscard]] std::string create_delta_frame(std::uint64_t sequence) {
  const auto predecessor = sequence - 1U;
  const auto bid_quantity = sequence % 2U == 0U ? 1U : 2U;
  const auto ask_quantity = sequence % 2U == 0U ? 2U : 1U;
  return "AEGISMD|1|source.deribit-btc-perpetual|delta|" + std::to_string(sequence) + "|" +
         std::to_string(predecessor) + "|" + std::to_string(1'000U + sequence) +
         "|1|ok:m3-benchmark-delta|2|B,100.0," + std::to_string(bid_quantity) + "|A,100.5," +
         std::to_string(ask_quantity);
}

// --------------------------------------------------------
// Transfer one bounded recorded frame into a caller-owned credential-free ingress attempt.
[[nodiscard]] market_data::IngressFrameAttempt create_ingress_attempt_or_throw(std::string frame) {
  auto created = market_data::IngressFrameAttempt::create_ingress_frame_attempt(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
      model::SessionEpoch{1U}, std::move(frame));
  if (!created) {
    throw std::logic_error{"invalid recorded frame in M3 benchmark fixture"};
  }
  return std::move(created).value();
}

// ########################################################################
// The scalar probe retains only one completed local result observation after the callback returns;
// it never retains BotContext, MarketEvent, book-view, or any other owner-turn capability.
struct SubmissionMeasurementProbe {
  bool armed{false};
  bool completed{false};
  std::uint64_t callback_count{0U};
  std::optional<std::uint64_t> local_duration_nanoseconds;
  std::uint64_t allocation_count{0U};
  execution::SubmitDisposition disposition{execution::SubmitDisposition::LocallyRejected};
  execution::SubmissionStage stage{execution::SubmissionStage::Context};
  execution::SubmissionReason reason{execution::SubmissionReason::ContextInactive};
  bool attempt_id_present{false};
  bool order_id_present{false};
  std::optional<risk::RiskScopeKind> risk_scope;
  execution::RiskMeasureKind risk_measure{execution::RiskMeasureKind::None};

  // --------------------------------------------------------
  // Request exactly one submit on the next Ready market-data callback.
  void arm_next_submission() noexcept {
    armed = true;
    completed = false;
    local_duration_nanoseconds.reset();
    allocation_count = 0U;
    risk_scope.reset();
    risk_measure = execution::RiskMeasureKind::None;
  }

  // --------------------------------------------------------
  // Copy only stable result scalars after allocation tracking has already been disabled.
  void capture_submission_result(const execution::SubmitResult& result,
                                 std::uint64_t allocations) noexcept {
    local_duration_nanoseconds = result.local_path_nanoseconds();
    allocation_count = allocations;
    disposition = result.disposition();
    stage = result.stage();
    reason = result.reason();
    attempt_id_present = result.attempt_id().has_value();
    order_id_present = result.order_id().has_value();
    if (result.risk_evidence()) {
      risk_scope = result.risk_evidence()->scope();
      risk_measure = result.risk_evidence()->measure_kind();
    }
    completed = true;
    armed = false;
  }

  // --------------------------------------------------------
};

// ########################################################################
// The baseline strategy invokes the sole public bot-bound submission capability and brackets no
// operation except that exact synchronous call with benchmark-only allocation tracking.
class MeasuredSubmissionStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Copy one immutable request and borrow the scalar probe whose lifetime encloses this strategy.
  MeasuredSubmissionStrategy(execution::OrderRequest request, SubmissionMeasurementProbe& probe)
      : request_{std::move(request)}, probe_{&probe} {}

  // --------------------------------------------------------
  // Submit only when armed; setup callbacks establish Ready state without consuming an attempt.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext& context) noexcept override {
    ++probe_->callback_count;
    if (!probe_->armed) {
      return;
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // These adjacent calls define the exact allocation interval required by BENCH-M3-SUBMIT.
    aegis_benchmark_support::allocation_tracking::begin_allocation_interval();
    const auto result = context.submit_order(request_);
    const auto allocations =
        aegis_benchmark_support::allocation_tracking::finish_allocation_interval();

    // ++++++++++++++++++++++++++++++++++++++++
    // Copy the returned noncanonical duration and stable result fields only after the interval.
    probe_->capture_submission_result(result, allocations);

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Non-ready setup transitions never submit and cannot receive a coherent book view.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
private:
  execution::OrderRequest request_;
  SubmissionMeasurementProbe* probe_;
};

// ########################################################################
// The peer-firm strategy satisfies complete configured-bot ownership without receiving a market
// subscription or participating in either measured baseline-firm submission.
class NoopStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Ignore an unreachable market-data callback without acquiring submission authority.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
  // Ignore an unreachable state callback without acquiring submission authority.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {}

  // --------------------------------------------------------
};

// ########################################################################
// One fixture seals the exact policies, preallocates all 10,000-attempt state, establishes Ready
// market state, and admits a fresh delta outside every local submission duration observation.
class SubmissionBenchmarkHarness final {
public:

  // --------------------------------------------------------
  // Build one success or first-risk-limit rejection fixture with otherwise identical inputs.
  explicit SubmissionBenchmarkHarness(SubmissionBenchmarkKind kind) : kind_{kind} {

    // ++++++++++++++++++++++++++++++++++++++++
    // Seal immutable startup/runtime authority and the selected complete risk authoring snapshot.
    auto configuration = create_reference_configuration_or_throw();
    auto runtime_policy = create_reference_policy_or_throw(configuration);
    auto risk_params =
        kind == SubmissionBenchmarkKind::AuthorizedFakeInitiation
            ? test_support::create_m3_reference_risk_policy_params_or_throw(configuration)
            : test_support::create_m3_rejecting_risk_policy_params_or_throw(configuration);
    auto routes = create_route_catalog_or_throw(configuration);
    auto risk_policy =
        risk::RiskPolicySnapshot::create_risk_policy_snapshot(risk_params, configuration, routes);
    if (!risk_policy) {
      throw std::logic_error{"invalid risk policy in M3 benchmark fixture"};
    }

    // ++++++++++++++++++++++++++++++++++++++++
    // Seal identical scripts/capacities and derive the exact bound submission fingerprint.
    auto encoder = create_encoder_script_or_throw();
    auto initiator = create_initiator_script_or_throw();
    auto submission_policy =
        execution::SubmissionPolicy::create_submission_policy(execution::SubmissionPolicyParams{
            execution::SubmissionCapability::DeterministicFakeOnly,
            configuration.fingerprint().bytes(),
            runtime_policy.fingerprint().bytes(),
            risk_policy.value().fingerprint().bytes(),
            risk_policy.value().revision(),
            create_submission_capacities(),
            calculate_required_encoded_order_bytes_or_throw(routes),
            encoder,
            initiator,
        });
    if (!submission_policy) {
      throw std::logic_error{"invalid submission policy in M3 benchmark fixture"};
    }
    const auto workload_id = kind == SubmissionBenchmarkKind::AuthorizedFakeInitiation
                                 ? "BENCH-M3-SUBMIT-001"
                                 : "BENCH-M3-SUBMIT-002";
    evidence_label_ = create_m3_evidence_label_or_throw(
        workload_id, configuration, runtime_policy, risk_policy.value(), submission_policy.value());
    expected_risk_fingerprint_ = risk_policy.value().fingerprint().to_hex();
    expected_submission_fingerprint_ = submission_policy.value().fingerprint().to_hex();

    // ++++++++++++++++++++++++++++++++++++++++
    // Transfer the concrete fake-only stack and both configured strategy owners into one runtime.
    std::vector<runtime::BotStrategyRegistration> strategies;
    strategies.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<MeasuredSubmissionStrategy>(create_benchmark_request_or_throw(), probe_),
    });
    strategies.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"),
        std::make_unique<NoopStrategy>(),
    });
    auto created = runtime::MarketRuntime::create_with_fake_submission(
        std::move(configuration), std::move(runtime_policy), executor_clock_,
        callback_measurement_clock_, std::move(strategies),
        runtime::FakeSubmissionRuntimeParams{
            std::move(risk_params), create_submission_capacities(), std::move(encoder),
            std::move(initiator), std::make_unique<execution::SteadySubmissionMeasurementClock>(),
            create_order_id_source_or_throw()});
    if (!created) {
      throw std::logic_error{"invalid market runtime in M3 benchmark fixture"};
    }
    runtime_ = std::move(created).value();

    // ++++++++++++++++++++++++++++++++++++++++
    // Bind, bootstrap, and establish Ready state while the strategy remains deliberately unarmed.
    if (!runtime_->bind_to_current_thread()) {
      throw std::logic_error{"failed to bind M3 benchmark runtime"};
    }
    owner_bound_ = true;
    const auto bootstrap = runtime_->execute_next_turn();
    if (!bootstrap || !bootstrap.value()) {
      throw std::logic_error{"failed to bootstrap M3 benchmark runtime"};
    }
    require_accepted_frame(create_snapshot_frame());
    const auto snapshot = runtime_->execute_next_turn();
    if (!snapshot || !snapshot.value()) {
      throw std::logic_error{"failed to establish Ready M3 benchmark state"};
    }
    probe_.callback_count = 0U;

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Close and release deterministic ownership if a failed benchmark stopped before final evidence.
  ~SubmissionBenchmarkHarness() {
    if (runtime_ && owner_bound_) {
      runtime_->close();
      static_cast<void>(runtime_->release_from_current_thread());
    }
  }

  // --------------------------------------------------------
  // Admit and run one market turn whose armed callback performs exactly one local submission.
  [[nodiscard]] bool execute_submission_sample_or_throw() {
    const auto callbacks_before = probe_.callback_count;
    probe_.arm_next_submission();
    require_accepted_frame(create_delta_frame(next_sequence_));
    const auto completed = runtime_->execute_next_turn();
    ++next_sequence_;
    return completed && completed.value().has_value() && probe_.completed &&
           probe_.callback_count == callbacks_before + 1U &&
           probe_.local_duration_nanoseconds.has_value();
  }

  // --------------------------------------------------------
  // Check the exact expected local disposition, evidence shape, and allocation-free direct path.
  [[nodiscard]] bool is_sample_consistent_with_workload() const noexcept {
    if (!probe_.attempt_id_present || !probe_.order_id_present || probe_.allocation_count != 0U) {
      return false;
    }
    if (kind_ == SubmissionBenchmarkKind::AuthorizedFakeInitiation) {
      return probe_.disposition == execution::SubmitDisposition::WriteInitiated &&
             probe_.stage == execution::SubmissionStage::Initiation &&
             probe_.reason == execution::SubmissionReason::None && !probe_.risk_scope &&
             probe_.risk_measure == execution::RiskMeasureKind::None;
    }
    return probe_.disposition == execution::SubmitDisposition::LocallyRejected &&
           probe_.stage == execution::SubmissionStage::Risk &&
           probe_.reason == execution::SubmissionReason::SingleOrderQuantityExceeded &&
           probe_.risk_scope == risk::RiskScopeKind::Bot &&
           probe_.risk_measure == execution::RiskMeasureKind::Quantity;
  }

  // --------------------------------------------------------
  // Return the one duration captured inside the completed BotContext::submit_order call.
  [[nodiscard]] std::uint64_t local_duration_nanoseconds() const noexcept {
    return probe_.local_duration_nanoseconds.value_or(0U);
  }

  // --------------------------------------------------------
  // Return the exact successful C++ heap allocation count from the same submit interval.
  [[nodiscard]] std::uint64_t allocation_count() const noexcept { return probe_.allocation_count; }

  // --------------------------------------------------------
  // Borrow the exact raw provenance label derived before runtime ownership transfer.
  [[nodiscard]] const std::string& evidence_label() const noexcept { return evidence_label_; }

  // --------------------------------------------------------
  // Prove the success workload retained every M4-pending object and the rejection workload reached
  // none of them, then cross-check the fingerprints used in the raw evidence label.
  [[nodiscard]] bool finalize_and_verify_submission_evidence() {
    runtime_->close();
    const auto released = runtime_->release_from_current_thread();
    if (!released) {
      return false;
    }
    owner_bound_ = false;
    auto evidence = runtime_->collect_quiescent_evidence();
    if (!evidence || !evidence.value().submission || evidence.value().fault) {
      return false;
    }
    const auto& submission = *evidence.value().submission;
    if (submission.runtime_faulted || submission.terminal_error ||
        submission.risk_policy_fingerprint.to_hex() != expected_risk_fingerprint_ ||
        submission.submission_policy_fingerprint.to_hex() != expected_submission_fingerprint_ ||
        submission.dropped_diagnostics != 0U) {
      return false;
    }
    if (kind_ == SubmissionBenchmarkKind::AuthorizedFakeInitiation) {
      return submission.held_reservation_count == submission_attempt_capacity &&
             submission.held_reservations.size() == submission_attempt_capacity &&
             submission.oms_order_count == submission_attempt_capacity &&
             submission.oms_orders.size() == submission_attempt_capacity &&
             submission.accepted_writes.size() == submission_attempt_capacity &&
             submission.encoder_invocations_consumed == distribution_iterations &&
             submission.initiator_invocations_consumed == distribution_iterations &&
             std::all_of(submission.oms_orders.begin(), submission.oms_orders.end(),
                         [](const auto& order) {
                           return order.state == oms::OutboundOrderState::WriteInitiated;
                         });
    }
    return submission.held_reservation_count == 0U && submission.held_reservations.empty() &&
           submission.oms_order_count == 0U && submission.oms_orders.empty() &&
           submission.accepted_writes.empty() && submission.encoder_invocations_consumed == 0U &&
           submission.initiator_invocations_consumed == 0U;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Require one nonblocking accepted frame decision while all ingress work remains unmeasured.
  void require_accepted_frame(std::string frame) {
    auto admitted = runtime_->try_admit(create_ingress_attempt_or_throw(std::move(frame)));
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted) {
      throw std::logic_error{"failed to admit M3 benchmark frame"};
    }
  }

  // --------------------------------------------------------
  // Retain fixture state in declaration order so both borrowed clocks outlive the runtime.
  SubmissionBenchmarkKind kind_;
  SubmissionMeasurementProbe probe_;
  model::DeterministicClockProvider executor_clock_{100U};
  model::DeterministicClockProvider callback_measurement_clock_{200U};
  std::string evidence_label_;
  std::string expected_risk_fingerprint_;
  std::string expected_submission_fingerprint_;
  std::unique_ptr<runtime::MarketRuntime> runtime_;
  std::uint64_t next_sequence_{101U};
  bool owner_bound_{false};
};

// ########################################################################

// --------------------------------------------------------
// Run one fixed M3 distribution while selecting only its expected outcome and counter names.
void execute_submission_benchmark_or_throw(benchmark::State& state, SubmissionBenchmarkKind kind,
                                           std::string_view throughput_name,
                                           std::string_view allocation_name) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct and preallocate the complete fake-only runtime before collecting local durations.
  SubmissionBenchmarkHarness harness{kind};
  state.SetLabel(harness.evidence_label());
  std::vector<std::uint64_t> samples;
  samples.reserve(static_cast<std::size_t>(distribution_iterations));
  std::uint64_t allocation_count = 0U;

  // ++++++++++++++++++++++++++++++++++++++++
  // The owner turn is intentionally not wall-timed: each sample uses the duration captured inside
  // submit from bot-bound entry through rejection or accepted fake initiation.
  for ([[maybe_unused]] const auto iteration : state) {
    if (!harness.execute_submission_sample_or_throw()) {
      state.SkipWithError("M3 benchmark owner turn did not produce one measured submit result");
      break;
    }
    if (!harness.is_sample_consistent_with_workload()) {
      state.SkipWithError("M3 benchmark submit result did not match its named workload");
      break;
    }
    const auto duration = harness.local_duration_nanoseconds();
    samples.push_back(duration);
    allocation_count += harness.allocation_count();
    state.SetIterationTime(aegis_benchmark_support::nanoseconds_to_seconds(duration));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only a complete 10,000-sample run may claim its exact retained or pre-OMS terminal state.
  if (samples.size() == static_cast<std::size_t>(distribution_iterations) &&
      !harness.finalize_and_verify_submission_evidence()) {
    state.SkipWithError("M3 benchmark final bounded submission evidence did not match workload");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the exact workload-specific rate and allocation vocabulary plus common percentiles.
  aegis_benchmark_support::publish_latency_distribution(state, samples, allocation_count,
                                                        throughput_name, allocation_name);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// BENCH-M3-SUBMIT-001 reaches successful local fake write initiation without acknowledgement.
void benchmark_authorized_fake_initiation_or_throw(benchmark::State& state) {
  execute_submission_benchmark_or_throw(state, SubmissionBenchmarkKind::AuthorizedFakeInitiation,
                                        "orders_per_second", "allocations_per_order");
}

// --------------------------------------------------------
// BENCH-M3-SUBMIT-002 rejects the value-identical request at the first bot quantity risk limit.
void benchmark_inline_risk_rejection_or_throw(benchmark::State& state) {
  execute_submission_benchmark_or_throw(state, SubmissionBenchmarkKind::InlineRiskRejection,
                                        "rejections_per_second", "allocations_per_request");
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Register the full authorized route-to-fake path with the exact evidence-tooling identity.
BENCHMARK(benchmark_authorized_fake_initiation_or_throw)
    ->Name("BENCH-M3-SUBMIT-001/submission.authorized-limit-fake-initiation")
    ->Iterations(distribution_iterations)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// --------------------------------------------------------
// Register the paired inline risk rejection with the exact evidence-tooling identity.
BENCHMARK(benchmark_inline_risk_rejection_or_throw)
    ->Name("BENCH-M3-SUBMIT-002/submission.inline-risk-rejection")
    ->Iterations(distribution_iterations)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// --------------------------------------------------------
