// Purpose: prove exact bot ownership, persistent bot-bound context semantics, canonical multi-firm
// subscription dispatch, bounded callback measurement, and deterministic re-entry rejection.

#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "reference_configuration.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Strategy context exposes only the normalized submission entry and never raw route, socket, or
// credential capabilities. The observation-only runtime makes that method fail closed.
template <typename Value>
concept HasSubmit = requires(Value& value, const execution::OrderRequest& request) {
  { value.submit(request) } -> std::same_as<execution::SubmitResult>;
};

template <typename Value>
concept HasRoute = requires(Value value) { value.route; };

template <typename Value>
concept HasSocket = requires(Value value) { value.socket; };

template <typename Value>
concept HasCredentials = requires(Value value) { value.credentials; };

static_assert(HasSubmit<runtime::BotContext>);
static_assert(!HasRoute<runtime::BotContext>);
static_assert(!HasSocket<runtime::BotContext>);
static_assert(!HasCredentials<runtime::BotContext>);
static_assert(!std::is_default_constructible_v<runtime::BotContext>);
static_assert(!std::is_copy_constructible_v<runtime::BotContext>);
static_assert(!std::is_move_constructible_v<runtime::BotContext>);
static_assert(!std::is_default_constructible_v<runtime::BotDispatchPlan>);
static_assert(!std::is_aggregate_v<runtime::BotDispatchPlan>);

// ########################################################################

// ########################################################################
// Callback observations copy only immutable strategy-visible values so assertions never retain a
// turn-scoped book view or context reference.
enum class ObservedCallbackKind : std::uint8_t {
  State = 1,
  Market = 2,
};

// ########################################################################

// ########################################################################
// One observation pins callback order, attribution, state, book identity, and coherent top levels.
struct CallbackObservation {
  ObservedCallbackKind kind;
  std::uint32_t strategy_label;
  model::FirmId firm_id;
  model::DeskId desk_id;
  model::BotId bot_id;
  model::StrategyId strategy_id;
  model::SubscriptionId subscription_id;
  model::CallbackOrdinal callback_ordinal;
  market_data::MarketReadiness readiness;
  std::optional<model::BookGeneration> book_generation;
  std::optional<model::BookRevision> book_revision;
  std::optional<model::Price> best_bid;
  std::optional<model::Price> best_ask;
  std::size_t bid_count;
  std::size_t ask_count;
};

// ########################################################################

// ########################################################################
// RecordingStrategy owns no runtime state beyond its test controls and copies each synchronous
// callback into a caller-reserved deterministic vector.
class RecordingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow a pre-reserved observation vector for the strategy lifetime.
  RecordingStrategy(std::uint32_t label, std::vector<CallbackObservation>& observations) noexcept
      : label_{label}, observations_{&observations} {}

  // --------------------------------------------------------
  // Configure one callback to attempt recursive dispatch a fixed number of times.
  void configure_reentry(runtime::BotRuntime& bot_runtime, const runtime::BotDispatchPlan& plan,
                         const market_data::MarketTurnOutcome& outcome, std::uint32_t attempts) {
    bot_runtime_ = &bot_runtime;
    reentry_plan_ = &plan;
    reentry_outcome_ = &outcome;
    reentry_attempts_ = attempts;
    reentry_errors_.reserve(attempts);
  }

  // --------------------------------------------------------
  // Configure callback-local owner-drive recursion attempts that precede nested dispatch attempts.
  void configure_owner_reentry(runtime::BotRuntime& bot_runtime, std::uint32_t attempts) {
    bot_runtime_ = &bot_runtime;
    owner_reentry_attempts_ = attempts;
    owner_reentry_errors_.reserve(attempts);
  }

  // --------------------------------------------------------
  // Advance the deterministic measurement clock during each callback to cross a selected budget.
  void configure_clock_advance(model::DeterministicClockProvider& clock,
                               std::uint64_t nanoseconds) noexcept {
    clock_ = &clock;
    callback_advance_nanoseconds_ = nanoseconds;
  }

  // --------------------------------------------------------
  // Consume one canonical trace slot inside the callback to exercise the fail-closed invariant
  // that ordinarily follows from exclusive serialized-owner access to the sink.
  void configure_trace_slot_consumption(trace::RuntimeTraceSink& trace_sink) noexcept {
    trace_sink_ = &trace_sink;
  }

  // --------------------------------------------------------
  // Exercise the normalized submission API from a callback and retain only the context address for
  // a lifetime-safe post-callback observation while the owning BotRuntime remains alive.
  void configure_submission_probe(execution::OrderRequest request) {
    submission_request_ = std::move(request);
  }

  // --------------------------------------------------------
  // Borrow the copied local result produced by the configured submission probe.
  [[nodiscard]] const std::optional<execution::SubmitResult>& submission_result() const noexcept {
    return submission_result_;
  }

  // --------------------------------------------------------
  // Return the persistent context address observed during the most recent callback.
  [[nodiscard]] runtime::BotContext* retained_context_for_test() const noexcept {
    return retained_context_for_test_;
  }

  // --------------------------------------------------------
  // Report whether the test-only callback-local trace append consumed its selected slot.
  [[nodiscard]] bool trace_slot_consumed() const noexcept { return trace_slot_consumed_; }

  // --------------------------------------------------------
  // Return every nested dispatch failure code observed inside the current callback.
  [[nodiscard]] const std::vector<model::DomainErrorCode>& reentry_errors() const noexcept {
    return reentry_errors_;
  }

  // --------------------------------------------------------
  // Return every owner-drive recursion failure observed inside the current callback.
  [[nodiscard]] const std::vector<model::DomainErrorCode>& owner_reentry_errors() const noexcept {
    return owner_reentry_errors_;
  }

  // --------------------------------------------------------
  // Copy a complete Ready observation without retaining the borrowed view.
  void on_market_data(const market_data::MarketEvent& event, const market_data::ReadyBookView& book,
                      runtime::BotContext& context) noexcept override {
    const auto& commit = event.context();
    observations_->push_back(CallbackObservation{
        ObservedCallbackKind::Market,
        label_,
        context.firm_id(),
        context.desk_id(),
        context.bot_id(),
        context.strategy_id(),
        context.subscription_id(),
        context.callback_ordinal(),
        market_data::MarketReadiness::Ready,
        commit.book_generation,
        commit.book_revision,
        book.best_bid(),
        book.best_ask(),
        book.bid_count(),
        book.ask_count(),
    });
    exercise_callback_controls(context, commit.turn_ordinal);
  }

  // --------------------------------------------------------
  // Copy the sanitized state transition without receiving or retaining any book storage.
  void on_market_state(const market_data::MarketStateEvent& event,
                       runtime::BotContext& context) noexcept override {
    const auto& state = event.fields();
    observations_->push_back(CallbackObservation{
        ObservedCallbackKind::State,
        label_,
        context.firm_id(),
        context.desk_id(),
        context.bot_id(),
        context.strategy_id(),
        context.subscription_id(),
        context.callback_ordinal(),
        state.readiness,
        state.book_generation,
        state.book_revision,
        std::nullopt,
        std::nullopt,
        0U,
        0U,
    });
    exercise_callback_controls(context, state.turn_ordinal);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Execute nested-dispatch and clock controls only after the immutable observation is captured.
  void exercise_callback_controls(runtime::BotContext& context,
                                  model::TurnOrdinal turn_ordinal) noexcept {
    retained_context_for_test_ = &context;
    if (submission_request_) {
      submission_result_ = context.submit(*submission_request_);
    }
    if (trace_sink_ != nullptr) {
      trace::RuntimeTraceFields fields;
      fields.bot_id = context.bot_id();
      fields.subscription_id = context.subscription_id();
      fields.turn_ordinal = turn_ordinal;
      fields.callback_ordinal = context.callback_ordinal();
      fields.failure_reason = trace::RuntimeTraceFailureReason::StrategyDispatchReentry;
      trace_slot_consumed_ = static_cast<bool>(
          trace_sink_->append(trace::RuntimeTraceEventKind::ReentryDetected, std::move(fields)));
      trace_sink_ = nullptr;
    }
    if (bot_runtime_ != nullptr) {
      for (std::uint32_t attempt = 0U; attempt < owner_reentry_attempts_; ++attempt) {
        auto nested = bot_runtime_->record_owner_reentry();
        if (!nested) {
          owner_reentry_errors_.push_back(nested.error().code);
        }
      }
    }
    if (bot_runtime_ != nullptr && reentry_plan_ != nullptr && reentry_outcome_ != nullptr) {
      for (std::uint32_t attempt = 0U; attempt < reentry_attempts_; ++attempt) {
        auto nested = bot_runtime_->dispatch(*reentry_plan_, *reentry_outcome_);
        if (!nested) {
          reentry_errors_.push_back(nested.error().code);
        }
      }
    }
    if (clock_ != nullptr && callback_advance_nanoseconds_ != 0U) {
      const auto advanced = clock_->advance(callback_advance_nanoseconds_);
      if (!advanced) {
        clock_advance_failed_ = true;
      }
    }
  }

  // --------------------------------------------------------
  std::uint32_t label_;
  std::vector<CallbackObservation>* observations_;
  runtime::BotRuntime* bot_runtime_{nullptr};
  const runtime::BotDispatchPlan* reentry_plan_{nullptr};
  const market_data::MarketTurnOutcome* reentry_outcome_{nullptr};
  std::uint32_t reentry_attempts_{0U};
  std::uint32_t owner_reentry_attempts_{0U};
  model::DeterministicClockProvider* clock_{nullptr};
  trace::RuntimeTraceSink* trace_sink_{nullptr};
  std::uint64_t callback_advance_nanoseconds_{0U};
  std::vector<model::DomainErrorCode> reentry_errors_;
  std::vector<model::DomainErrorCode> owner_reentry_errors_;
  std::optional<execution::OrderRequest> submission_request_;
  std::optional<execution::SubmitResult> submission_result_;
  runtime::BotContext* retained_context_for_test_{nullptr};
  bool trace_slot_consumed_{false};
  bool clock_advance_failed_{false};
};

// ########################################################################

// ########################################################################
// A two-reading clock makes callback measurement regression deterministic without changing any
// canonical trace timestamp.
class RegressingMeasurementClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Fix the callback-start and callback-finish observations at construction.
  RegressingMeasurementClock(std::uint64_t started, std::uint64_t finished) noexcept
      : observations_{started, finished} {}

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Return the final observation repeatedly after the two callback reads are consumed.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    const auto selected =
        next_index_ < observations_.size() ? next_index_ : observations_.size() - 1U;
    if (next_index_ < observations_.size()) {
      ++next_index_;
    }
    return observations_[selected];
  }

  // --------------------------------------------------------
  std::array<std::uint64_t, 2U> observations_;
  std::size_t next_index_{0U};
};

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture-authoring failures rather than dispatch behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in bot-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact prices without binary floating-point fixture drift.
[[nodiscard]] model::Price price(std::string_view text) {
  auto parsed = model::Price::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid price in bot-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact quantities without binary floating-point fixture drift.
[[nodiscard]] model::Quantity quantity(std::string_view text) {
  auto parsed = model::Quantity::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in bot-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Construct one checked one-based ordinal for deterministic test identities.
template <typename Ordinal> [[nodiscard]] Ordinal ordinal(std::uint64_t value) {
  auto parsed = Ordinal::from_value(value);
  if (!parsed) {
    throw std::logic_error{"invalid ordinal in bot-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal one startup authoring value before any runtime object borrows its provenance.
[[nodiscard]] configuration::StartupConfiguration
configuration_from(configuration::StartupConfigurationParams params) {
  auto created = configuration::StartupConfiguration::create(std::move(params));
  if (!created) {
    throw std::logic_error{"invalid startup configuration in bot-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Use explicit small bounds while retaining enough callback/trace space for two matching grants.
[[nodiscard]] runtime::RuntimePolicyLimits limits(std::uint32_t maximum_callbacks = 4U,
                                                  std::uint64_t callback_budget_nanoseconds = 100U,
                                                  std::uint32_t diagnostic_capacity = 32U,
                                                  std::uint32_t runtime_trace_capacity = 128U) {
  return runtime::RuntimePolicyLimits{8U,
                                      4096U,
                                      64U,
                                      20U,
                                      1'000U,
                                      maximum_callbacks,
                                      diagnostic_capacity,
                                      runtime_trace_capacity,
                                      32U,
                                      callback_budget_nanoseconds};
}

// --------------------------------------------------------
// Define the sole normalized source used by both M1 reference organizations.
[[nodiscard]] runtime::RuntimeSourceDefinition source_definition() {
  return runtime::RuntimeSourceDefinition{
      id<model::MarketSourceId>("source.deribit-btc-perpetual"), id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"), id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::initial()};
}

// --------------------------------------------------------
// Add one independently identified subscribed instrument so source-plan mismatches are testable
// within a single sealed policy.
[[nodiscard]] configuration::StartupConfigurationParams two_source_configuration_params() {
  auto params = test_support::reference_configuration_params();
  auto metadata = params.instrument_metadata.front();
  metadata.instrument_id = id<model::InstrumentId>("ETH-USD-PERPETUAL");
  metadata.venue_instrument_id = id<model::VenueInstrumentId>("ETH-PERPETUAL");
  metadata.base_currency = "ETH";
  metadata.settlement_currency = "ETH";
  params.instrument_metadata.push_back(std::move(metadata));
  params.subscriptions.push_back(market_data::Subscription{
      id<model::SubscriptionId>("subscription.deribit-eth-perpetual-book"),
      id<model::BotId>("bot.deribit-btc-perpetual-reference"), id<model::VenueId>("deribit"),
      id<model::InstrumentId>("ETH-USD-PERPETUAL"), market_data::SubscriptionChannel::OrderBook});
  return params;
}

// --------------------------------------------------------
// Define the second source in the same canonical policy used by adversarial plan/outcome tests.
[[nodiscard]] runtime::RuntimeSourceDefinition second_source_definition() {
  return runtime::RuntimeSourceDefinition{
      id<model::MarketSourceId>("source.deribit-eth-perpetual"), id<model::VenueId>("deribit"),
      id<model::InstrumentId>("ETH-USD-PERPETUAL"), id<model::VenueInstrumentId>("ETH-PERPETUAL"),
      model::InstrumentMetadataRevision::initial()};
}

// --------------------------------------------------------
// Bind one runtime policy to the exact sealed configuration and selected callback budget.
[[nodiscard]] runtime::RuntimePolicy
policy_from(const configuration::StartupConfiguration& configuration,
            runtime::RuntimePolicyLimits selected_limits = limits()) {
  auto created = runtime::RuntimePolicy::create(
      configuration, runtime::RuntimePolicyParams{selected_limits, {source_definition()}});
  if (!created) {
    throw std::logic_error{"invalid runtime policy in bot-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Bind two canonical sources to one sealed configuration for source-mismatch defenses.
[[nodiscard]] runtime::RuntimePolicy
two_source_policy_from(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create(
      configuration,
      runtime::RuntimePolicyParams{limits(), {source_definition(), second_source_definition()}});
  if (!created) {
    throw std::logic_error{"invalid two-source runtime policy in bot-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create one state owner from the same configured source and immutable metadata as dispatch.
[[nodiscard]] market_data::MarketStateMachine
state_machine_from(const configuration::StartupConfiguration& configuration,
                   const runtime::RuntimePolicy& policy, std::size_t source_index = 0U) {
  const auto& source = policy.sources().at(source_index);
  const auto& definition = source.definition();
  const auto* metadata =
      configuration.find_instrument_metadata(definition.venue_id, definition.instrument_id);
  if (metadata == nullptr) {
    throw std::logic_error{"missing metadata in bot-runtime test fixture"};
  }
  auto created = market_data::MarketStateMachine::create(policy, source, *metadata);
  if (!created) {
    throw std::logic_error{"invalid market state in bot-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build the complete authoritative snapshot used to enter Ready for dispatch.
[[nodiscard]] market_data::NormalizedMarketUpdate
snapshot_from(const runtime::RuntimePolicy& policy) {
  auto token = market_data::IntegrityTokenIdentity::from_token("bot-runtime-snapshot");
  if (!token) {
    throw std::logic_error{"invalid integrity token in bot-runtime test fixture"};
  }
  auto snapshot = market_data::NormalizedMarketUpdate::create(
      market_data::NormalizedMarketUpdateFields{
          market_data::MarketSourceIdentity::from_runtime_source(policy.sources().front()),
          model::SessionEpoch{1U},
          model::SequenceNumber{10U},
          std::nullopt,
          model::SourceTimestamp{1'000U},
          model::ReceiveSequence::initial(),
          model::ReceiveTimestamp{100U},
          model::InstrumentMetadataRevision::initial(),
          market_data::MarketUpdateKind::Snapshot,
          market_data::MarketIntegrity{market_data::IntegrityVerdict::Accepted,
                                       std::move(token).value()},
          {{market_data::BookSide::Ask, price("101"), quantity("2")},
           {market_data::BookSide::Bid, price("99"), quantity("4")},
           {market_data::BookSide::Ask, price("102"), quantity("3")},
           {market_data::BookSide::Bid, price("100"), quantity("1")}},
      },
      policy.limits().maximum_changes_per_update);
  if (!snapshot) {
    throw std::logic_error{"invalid snapshot in bot-runtime test fixture"};
  }
  return std::move(snapshot).value();
}

// --------------------------------------------------------
// Register one recording strategy while returning its stable heap address for test controls.
[[nodiscard]] runtime::BotStrategyRegistration
registration(std::string_view bot_id, std::uint32_t label,
             std::vector<CallbackObservation>& observations, RecordingStrategy*& strategy) {
  auto owned = std::make_unique<RecordingStrategy>(label, observations);
  strategy = owned.get();
  return runtime::BotStrategyRegistration{id<model::BotId>(bot_id), std::move(owned)};
}

// --------------------------------------------------------
// Count one assigned trace kind without depending on unrelated state/input record positions.
[[nodiscard]] std::size_t count_trace_kind(const trace::RuntimeTraceSink& sink,
                                           trace::RuntimeTraceEventKind kind) {
  std::size_t count = 0U;
  for (const auto& record : sink.records()) {
    if (record.kind() == kind) {
      ++count;
    }
  }
  return count;
}

// --------------------------------------------------------
// Build one valid attributable diagnostic used to saturate the noncanonical sink before dispatch.
[[nodiscard]] runtime::RuntimeDiagnosticFields diagnostic_input_fields(std::uint32_t detail_code) {
  runtime::RuntimeDiagnosticFields fields;
  fields.source_ordinal = model::MarketSourceOrdinal::initial();
  fields.admission_ordinal = model::AdmissionOrdinal::initial();
  fields.turn_ordinal = model::TurnOrdinal::initial();
  fields.detail_code = detail_code;
  fields.observed_value = 1U;
  return fields;
}

// --------------------------------------------------------
// A peer-firm bot without a matching observation grant owns a strategy but receives no callbacks.
TEST_CASE("bot runtime dispatches only the subscribed bot with complete attribution",
          "[runtime][bot][multi_firm]") {
  const auto configuration = configuration_from(test_support::two_firm_configuration_params());
  const auto policy = policy_from(configuration);
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{50U};
  auto machine = state_machine_from(configuration, policy);

  std::vector<CallbackObservation> observations;
  observations.reserve(8U);
  RecordingStrategy* primary = nullptr;
  RecordingStrategy* subsidiary = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(registration("bot.subsidiary-reference", 2U, observations, subsidiary));
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, primary));
  const execution::OrderRequest observation_only_request{
      id<model::RouteId>("route.deribit-testnet-btc-perpetual"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      price("60000"),
      quantity("1"),
  };
  primary->configure_submission_probe(observation_only_request);

  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto oversized_initial_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                            model::TurnOrdinal::initial(), 2U);
  REQUIRE(oversized_initial_plan);
  const auto initial_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                  model::TurnOrdinal::initial(), 1U);
  REQUIRE(initial_plan);
  CHECK(initial_plan.value().source_ordinal() == model::MarketSourceOrdinal::initial());
  CHECK(initial_plan.value().matching_subscription_count() == 1U);
  CHECK(initial_plan.value().event_count() == 1U);
  CHECK(initial_plan.value().callback_count() == 1U);
  CHECK(initial_plan.value().callback_trace_record_count() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Initial Synchronizing and snapshot recovery dispatch state before market to the sole grant.
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{60U}},
                         trace_sink);
  REQUIRE(initialized);
  const auto oversized_initial =
      bot_runtime.dispatch(oversized_initial_plan.value(), initialized.value());
  REQUIRE_FALSE(oversized_initial);
  CHECK(oversized_initial.error() ==
        model::DomainError::at_field(model::DomainErrorCode::DispatchCapacityExceeded,
                                     "bot_runtime.outcome_event_count"));
  const auto initial_report = bot_runtime.dispatch(initial_plan.value(), initialized.value());
  REQUIRE(initial_report);
  CHECK(initial_report.value().state_callbacks == 1U);
  REQUIRE(primary->submission_result());
  CHECK(primary->submission_result()->disposition() ==
        execution::SubmitDisposition::LocallyRejected);
  CHECK(primary->submission_result()->stage() == execution::SubmissionStage::Context);
  CHECK(primary->submission_result()->reason() ==
        execution::SubmissionReason::SubmissionCapabilityUnavailable);
  CHECK_FALSE(primary->submission_result()->attempt_id());
  CHECK_FALSE(primary->submission_result()->order_id());
  REQUIRE(primary->retained_context_for_test() != nullptr);
  const auto outside_callback =
      primary->retained_context_for_test()->submit(observation_only_request);
  CHECK(outside_callback.reason() == execution::SubmissionReason::SubmissionCapabilityUnavailable);

  const auto too_small_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                    ordinal<model::TurnOrdinal>(2U), 1U);
  REQUIRE(too_small_plan);
  const auto ready_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                ordinal<model::TurnOrdinal>(2U), 2U);
  REQUIRE(ready_plan);
  CHECK(ready_plan.value().callback_count() == 2U);
  CHECK(ready_plan.value().callback_trace_record_count() == 4U);
  const auto ready =
      machine.process(snapshot_from(policy),
                      market_data::AcceptedMarketTurnContext{model::AdmissionOrdinal::initial(),
                                                             ordinal<model::TurnOrdinal>(2U),
                                                             model::ProcessingTimestamp{200U}},
                      trace_sink);
  REQUIRE(ready);
  const auto too_small = bot_runtime.dispatch(too_small_plan.value(), ready.value());
  REQUIRE_FALSE(too_small);
  CHECK(too_small.error() ==
        model::DomainError::at_field(model::DomainErrorCode::DispatchCapacityExceeded,
                                     "bot_runtime.outcome_event_count"));
  const auto ready_report = bot_runtime.dispatch(ready_plan.value(), ready.value());
  REQUIRE(ready_report);
  CHECK(ready_report.value().state_callbacks == 1U);
  CHECK(ready_report.value().market_callbacks == 1U);

  REQUIRE(observations.size() == 3U);
  CHECK(observations[0U].kind == ObservedCallbackKind::State);
  CHECK(observations[0U].readiness == market_data::MarketReadiness::Synchronizing);
  CHECK(observations[1U].kind == ObservedCallbackKind::State);
  CHECK(observations[1U].readiness == market_data::MarketReadiness::Ready);
  CHECK(observations[2U].kind == ObservedCallbackKind::Market);
  CHECK(observations[2U].best_bid == price("100"));
  CHECK(observations[2U].best_ask == price("101"));
  CHECK(observations[2U].bid_count == 2U);
  CHECK(observations[2U].ask_count == 2U);
  for (std::size_t index = 0U; index < observations.size(); ++index) {
    CHECK(observations[index].strategy_label == 1U);
    CHECK(observations[index].firm_id == id<model::FirmId>("firm.aegis-lab"));
    CHECK(observations[index].callback_ordinal.value() == index + 1U);
  }
  REQUIRE(primary != nullptr);
  REQUIRE(subsidiary != nullptr);
  CHECK(count_trace_kind(trace_sink, trace::RuntimeTraceEventKind::StateCallback) == 2U);
  CHECK(count_trace_kind(trace_sink, trace::RuntimeTraceEventKind::MarketCallback) == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Two peer-firm grants dispatch in subscription-ID order, with all state callbacks before market.
TEST_CASE("bot runtime canonicalizes multi-firm subscription dispatch",
          "[runtime][bot][multi_firm]") {
  auto params = test_support::two_firm_configuration_params();
  params.subscriptions.push_back(market_data::Subscription{
      id<model::SubscriptionId>("subscription.aaa-subsidiary-book"),
      id<model::BotId>("bot.subsidiary-reference"), id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"), market_data::SubscriptionChannel::OrderBook});
  const auto configuration = configuration_from(std::move(params));
  const auto policy = policy_from(configuration);
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto machine = state_machine_from(configuration, policy);

  std::vector<CallbackObservation> observations;
  observations.reserve(12U);
  RecordingStrategy* primary = nullptr;
  RecordingStrategy* subsidiary = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, primary));
  registrations.push_back(registration("bot.subsidiary-reference", 2U, observations, subsidiary));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto initial_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                  model::TurnOrdinal::initial(), 1U);
  REQUIRE(initial_plan);
  REQUIRE(initial_plan.value().matching_subscription_count() == 2U);

  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);
  REQUIRE(bot_runtime.dispatch(initial_plan.value(), initialized.value()));
  observations.clear();

  const auto ready_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                ordinal<model::TurnOrdinal>(2U), 2U);
  REQUIRE(ready_plan);
  const auto ready =
      machine.process(snapshot_from(policy),
                      market_data::AcceptedMarketTurnContext{model::AdmissionOrdinal::initial(),
                                                             ordinal<model::TurnOrdinal>(2U),
                                                             model::ProcessingTimestamp{200U}},
                      trace_sink);
  REQUIRE(ready);
  const auto report = bot_runtime.dispatch(ready_plan.value(), ready.value());
  REQUIRE(report);
  CHECK(report.value().state_callbacks == 2U);
  CHECK(report.value().market_callbacks == 2U);

  REQUIRE(observations.size() == 4U);
  CHECK(observations[0U].kind == ObservedCallbackKind::State);
  CHECK(observations[0U].strategy_label == 2U);
  CHECK(observations[1U].kind == ObservedCallbackKind::State);
  CHECK(observations[1U].strategy_label == 1U);
  CHECK(observations[2U].kind == ObservedCallbackKind::Market);
  CHECK(observations[2U].strategy_label == 2U);
  CHECK(observations[3U].kind == ObservedCallbackKind::Market);
  CHECK(observations[3U].strategy_label == 1U);
  CHECK(observations[0U].subscription_id ==
        id<model::SubscriptionId>("subscription.aaa-subsidiary-book"));
  CHECK(observations[1U].subscription_id ==
        id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book"));
  CHECK(observations[0U].firm_id == id<model::FirmId>("firm.aegis-subsidiary"));
  CHECK(observations[1U].firm_id == id<model::FirmId>("firm.aegis-lab"));
  REQUIRE(primary != nullptr);
  REQUIRE(subsidiary != nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Failed market processing can cancel its no-longer-usable proof without wedging later turns, and
// a successful zero-event outcome still consumes exactly one plan.
TEST_CASE("bot runtime cancels failed turns and consumes zero-event plans", "[runtime][bot]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration);
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto machine = state_machine_from(configuration, policy);
  std::vector<CallbackObservation> observations;
  observations.reserve(2U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Processing before initialization fails without mutation; cancellation has no failure path and
  // makes a subsequent preflight independent of that abandoned attempt.
  const auto status_before = bot_runtime.status();
  const auto callback_before = bot_runtime.last_callback_ordinal();
  const auto dispatches_before = bot_runtime.completed_dispatch_count();
  const auto trace_before = trace_sink.size();
  const auto diagnostics_before = diagnostics.accepted_count();
  const auto dropped_before = diagnostics.dropped_count();
  const auto abandoned_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                    model::TurnOrdinal::initial(), 2U);
  REQUIRE(abandoned_plan);
  const auto rejected =
      machine.process(snapshot_from(policy),
                      market_data::AcceptedMarketTurnContext{model::AdmissionOrdinal::initial(),
                                                             model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{200U}},
                      trace_sink);
  REQUIRE_FALSE(rejected);
  bot_runtime.cancel(abandoned_plan.value());
  bot_runtime.cancel(abandoned_plan.value());
  CHECK(bot_runtime.status() == status_before);
  CHECK(bot_runtime.last_callback_ordinal() == callback_before);
  CHECK(bot_runtime.completed_dispatch_count() == dispatches_before);
  CHECK(trace_sink.size() == trace_before);
  CHECK(diagnostics.accepted_count() == diagnostics_before);
  CHECK(diagnostics.dropped_count() == dropped_before);

  const auto initialization_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                         ordinal<model::TurnOrdinal>(2U), 1U);
  REQUIRE(initialization_plan);
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{ordinal<model::TurnOrdinal>(2U),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);
  REQUIRE(bot_runtime.dispatch(initialization_plan.value(), initialized.value()));

  // ++++++++++++++++++++++++++++++++++++++++
  // Starting the first explicit session while already Synchronizing emits only an input
  // disposition. Dispatch still validates and consumes its zero-callback proof.
  const auto zero_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                               ordinal<model::TurnOrdinal>(3U), 0U);
  REQUIRE(zero_plan);
  CHECK(zero_plan.value().event_count() == 0U);
  CHECK(zero_plan.value().callback_count() == 0U);
  CHECK(zero_plan.value().callback_trace_record_count() == 0U);
  const auto no_change = machine.process(
      market_data::SessionStarted{
          market_data::MarketSourceIdentity::from_runtime_source(policy.sources().front()),
          model::SessionEpoch{1U}, model::SourceTimestamp{1U}, model::ReceiveSequence::initial(),
          model::ReceiveTimestamp{1U}},
      market_data::AcceptedMarketTurnContext{ordinal<model::AdmissionOrdinal>(2U),
                                             ordinal<model::TurnOrdinal>(3U),
                                             model::ProcessingTimestamp{2U}},
      trace_sink);
  REQUIRE(no_change);
  CHECK_FALSE(no_change.value().state_event());
  CHECK_FALSE(no_change.value().market_event());
  CHECK(no_change.value().expected_callback_count() == 0U);
  const auto dispatches_before_zero = bot_runtime.completed_dispatch_count();
  const auto zero_report = bot_runtime.dispatch(zero_plan.value(), no_change.value());
  REQUIRE(zero_report);
  CHECK(zero_report.value().callback_count() == 0U);
  CHECK(bot_runtime.completed_dispatch_count() == dispatches_before_zero + 1U);
  const auto replayed = bot_runtime.dispatch(zero_plan.value(), no_change.value());
  REQUIRE_FALSE(replayed);
  CHECK(replayed.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                     "bot_dispatch_plan.freshness"));
  CHECK(bot_runtime.completed_dispatch_count() == dispatches_before_zero + 1U);
  REQUIRE(strategy != nullptr);
  CHECK(observations.size() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Zero-event outcomes retain source and turn identity so a compatible callback count cannot bypass
// the opaque plan's exact source/turn binding.
TEST_CASE("bot runtime rejects wrong-source and wrong-turn zero-event outcomes", "[runtime][bot]") {
  const auto configuration = configuration_from(two_source_configuration_params());
  const auto policy = two_source_policy_from(configuration);
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto second_machine = state_machine_from(configuration, policy, 1U);
  std::vector<CallbackObservation> observations;
  observations.reserve(1U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();

  const auto initialized =
      second_machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                                    model::ProcessingTimestamp{1U}},
                                trace_sink);
  REQUIRE(initialized);
  const auto& second_source = policy.sources().at(1U);
  const auto zero_outcome = second_machine.process(
      market_data::SessionStarted{
          market_data::MarketSourceIdentity::from_runtime_source(second_source),
          model::SessionEpoch{1U}, model::SourceTimestamp{1U}, model::ReceiveSequence::initial(),
          model::ReceiveTimestamp{1U}},
      market_data::AcceptedMarketTurnContext{model::AdmissionOrdinal::initial(),
                                             ordinal<model::TurnOrdinal>(2U),
                                             model::ProcessingTimestamp{2U}},
      trace_sink);
  REQUIRE(zero_outcome);
  CHECK_FALSE(zero_outcome.value().state_event());
  CHECK_FALSE(zero_outcome.value().market_event());

  // ++++++++++++++++++++++++++++++++++++++++
  // A source-one plan cannot authorize a source-two outcome even though both imply zero callbacks.
  const auto wrong_source_plan = bot_runtime.preflight(policy.sources().front().ordinal(),
                                                       ordinal<model::TurnOrdinal>(2U), 0U);
  REQUIRE(wrong_source_plan);
  const auto wrong_source = bot_runtime.dispatch(wrong_source_plan.value(), zero_outcome.value());
  REQUIRE_FALSE(wrong_source);
  CHECK(wrong_source.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                     "bot_runtime.outcome_source_plan"));
  CHECK(bot_runtime.completed_dispatch_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A later-turn plan for the same source rejects the stale outcome without consuming a dispatch.
  const auto wrong_turn_plan =
      bot_runtime.preflight(second_source.ordinal(), ordinal<model::TurnOrdinal>(3U), 0U);
  REQUIRE(wrong_turn_plan);
  const auto wrong_turn = bot_runtime.dispatch(wrong_turn_plan.value(), zero_outcome.value());
  REQUIRE_FALSE(wrong_turn);
  CHECK(wrong_turn.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                     "bot_runtime.outcome_turn_plan"));
  CHECK(bot_runtime.completed_dispatch_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Retrying with the exact source and turn succeeds and advances the use predecessor once.
  const auto exact_plan =
      bot_runtime.preflight(second_source.ordinal(), ordinal<model::TurnOrdinal>(2U), 0U);
  REQUIRE(exact_plan);
  const auto exact = bot_runtime.dispatch(exact_plan.value(), zero_outcome.value());
  REQUIRE(exact);
  CHECK(exact.value().callback_count() == 0U);
  CHECK(bot_runtime.completed_dispatch_count() == 1U);
  REQUIRE(strategy != nullptr);
  CHECK(observations.empty());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Opaque plans remain bound to their issuing runtime and one completed dispatch invalidates every
// peer proof minted from the same observational predecessor.
TEST_CASE("bot runtime rejects foreign stale and consumed dispatch plans", "[runtime][bot]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration);
  trace::RuntimeTraceSink first_trace{policy};
  trace::RuntimeTraceSink second_trace{policy};
  runtime::RuntimeDiagnosticSink first_diagnostics{policy};
  runtime::RuntimeDiagnosticSink second_diagnostics{policy};
  model::DeterministicClockProvider first_clock{0U};
  model::DeterministicClockProvider second_clock{0U};
  auto machine = state_machine_from(configuration, policy);
  std::vector<CallbackObservation> first_observations;
  std::vector<CallbackObservation> second_observations;
  first_observations.reserve(2U);
  second_observations.reserve(2U);
  RecordingStrategy* first_strategy = nullptr;
  RecordingStrategy* second_strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> first_registrations;
  std::vector<runtime::BotStrategyRegistration> second_registrations;
  first_registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, first_observations, first_strategy));
  second_registrations.push_back(registration("bot.deribit-btc-perpetual-reference", 2U,
                                              second_observations, second_strategy));
  auto first_created =
      runtime::BotRuntime::create(configuration, policy, first_clock, first_trace,
                                  first_diagnostics, std::move(first_registrations));
  auto second_created =
      runtime::BotRuntime::create(configuration, policy, second_clock, second_trace,
                                  second_diagnostics, std::move(second_registrations));
  REQUIRE(first_created);
  REQUIRE(second_created);
  auto first_runtime = std::move(first_created).value();
  auto second_runtime = std::move(second_created).value();

  const auto stale_plan = first_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                  model::TurnOrdinal::initial(), 1U);
  REQUIRE(stale_plan);
  const auto outcome =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         first_trace);
  REQUIRE(outcome);

  // ++++++++++++++++++++++++++++++++++++++++
  // A plan from another BotRuntime cannot authorize an otherwise compatible outcome.
  const auto foreign = second_runtime.dispatch(stale_plan.value(), outcome.value());
  REQUIRE_FALSE(foreign);
  CHECK(foreign.error() == model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                                        "bot_dispatch_plan.owner"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Two observational preflights share one predecessor; the first completed dispatch invalidates
  // the other without any preflight-time mutation.
  const auto current_plan = first_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                    model::TurnOrdinal::initial(), 1U);
  REQUIRE(current_plan);
  REQUIRE(first_runtime.dispatch(current_plan.value(), outcome.value()));
  const auto stale = first_runtime.dispatch(stale_plan.value(), outcome.value());
  REQUIRE_FALSE(stale);
  CHECK(stale.error() == model::DomainError::at_field(model::DomainErrorCode::InvalidRelationship,
                                                      "bot_dispatch_plan.freshness"));

  // ++++++++++++++++++++++++++++++++++++++++
  // One successful use consumes the proof even when all other immutable bindings still match.
  const auto consumed = first_runtime.dispatch(current_plan.value(), outcome.value());
  REQUIRE_FALSE(consumed);
  CHECK(consumed.error() == stale.error());
  REQUIRE(first_strategy != nullptr);
  REQUIRE(second_strategy != nullptr);
  CHECK(first_observations.size() == 1U);
  CHECK(second_observations.empty());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Registration coverage and construction-only seeds fail before any callback state can change.
TEST_CASE("bot runtime validates strategy ownership and callback counter headroom",
          "[runtime][bot]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration);
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  std::vector<CallbackObservation> observations;
  observations.reserve(2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Missing exact bot coverage is rejected without taking partial strategy ownership live.
  auto missing =
      runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics, {});
  REQUIRE_FALSE(missing);
  CHECK(missing.error().code == model::DomainErrorCode::StrategyNotConfigured);

  // ++++++++++++++++++++++++++++++++++++++++
  // A diagnostic sink derived from another sealed policy cannot be attached by capacity alone.
  const auto other_policy = policy_from(configuration, limits(4U, 101U));
  runtime::RuntimeDiagnosticSink other_diagnostics{other_policy};
  RecordingStrategy* mismatched_strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> mismatched_registrations;
  mismatched_registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, mismatched_strategy));
  auto mismatched =
      runtime::BotRuntime::create(configuration, policy, clock, trace_sink, other_diagnostics,
                                  std::move(mismatched_registrations));
  REQUIRE_FALSE(mismatched);
  CHECK(mismatched.error().code == model::DomainErrorCode::InvalidRelationship);

  // ++++++++++++++++++++++++++++++++++++++++
  // The maximum one-based callback seed makes even one matching grant fail before dispatch.
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  const auto maximum = ordinal<model::CallbackOrdinal>(std::numeric_limits<std::uint64_t>::max());
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations),
                                             runtime::BotRuntimeCounterSeed{maximum});
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto zero = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                          model::TurnOrdinal::initial(), 0U);
  REQUIRE(zero);
  CHECK(zero.value().event_count() == 0U);
  CHECK(zero.value().callback_count() == 0U);
  CHECK(zero.value().callback_trace_record_count() == 0U);
  const auto exhausted = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                               model::TurnOrdinal::initial(), 1U);
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::CallbackCounterExhausted);
  const auto oversized = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                               model::TurnOrdinal::initial(), 3U);
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == model::DomainError::at_field(model::DomainErrorCode::InvalidValue,
                                                          "bot_runtime.event_count"));
  REQUIRE(strategy != nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Owner-drive and recursive-dispatch attempts share one canonical record, preserve the first kind,
// and publish one bounded aggregate only after the callback returns.
TEST_CASE("bot runtime rejects reentry and measures callback budget",
          "[runtime][bot][diagnostics]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration, limits(2U, 100U));
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto machine = state_machine_from(configuration, policy);
  std::vector<CallbackObservation> observations;
  observations.reserve(4U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Owner re-entry evidence is legal only while a strategy callback is active.
  const auto trace_size_before = trace_sink.size();
  const auto outside_owner_reentry = bot_runtime.record_owner_reentry();
  REQUIRE_FALSE(outside_owner_reentry);
  CHECK(outside_owner_reentry.error().code == model::DomainErrorCode::ExecutorReentryDetected);
  CHECK(trace_sink.size() == trace_size_before);

  const auto plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                          model::TurnOrdinal::initial(), 1U);
  REQUIRE(plan);
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);
  REQUIRE(strategy != nullptr);
  strategy->configure_owner_reentry(bot_runtime, 2U);
  strategy->configure_reentry(bot_runtime, plan.value(), initialized.value(), 1U);
  strategy->configure_clock_advance(clock, 101U);
  const auto report = bot_runtime.dispatch(plan.value(), initialized.value());
  REQUIRE(report);
  CHECK(report.value().state_callbacks == 1U);
  CHECK(report.value().callback_budget_exceeded == 1U);
  REQUIRE(strategy->owner_reentry_errors().size() == 2U);
  CHECK(strategy->owner_reentry_errors()[0U] == model::DomainErrorCode::ExecutorReentryDetected);
  CHECK(strategy->owner_reentry_errors()[1U] == model::DomainErrorCode::ExecutorReentryDetected);
  REQUIRE(strategy->reentry_errors().size() == 1U);
  CHECK(strategy->reentry_errors()[0U] == model::DomainErrorCode::DispatchReentryDetected);
  CHECK(count_trace_kind(trace_sink, trace::RuntimeTraceEventKind::ReentryDetected) == 1U);

  const trace::RuntimeTraceRecord* reentry_record = nullptr;
  for (const auto& record : trace_sink.records()) {
    if (record.kind() == trace::RuntimeTraceEventKind::ReentryDetected) {
      reentry_record = &record;
    }
  }
  REQUIRE(reentry_record != nullptr);
  CHECK(reentry_record->fields().failure_reason ==
        trace::RuntimeTraceFailureReason::OwnerDriveReentry);
  CHECK(diagnostics.accepted_count() == 2U);
  CHECK(diagnostics.at(0U)->kind == runtime::RuntimeDiagnosticKind::OwnerReentryDetected);
  CHECK(diagnostics.at(0U)->fields.occurrence_count == 3U);
  CHECK(diagnostics.at(1U)->kind == runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Saturated noncanonical telemetry retains its first prefix while every preflighted callback and
// canonical re-entry record still completes.
TEST_CASE("bot runtime completes dispatch after diagnostics saturate",
          "[runtime][bot][diagnostics]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration, limits(2U, 100U, 1U));
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto machine = state_machine_from(configuration, policy);

  REQUIRE(diagnostics.append(runtime::RuntimeDiagnosticKind::MalformedInput,
                             diagnostic_input_fields(77U)));
  REQUIRE(diagnostics.at(0U) != nullptr);
  const auto retained_prefix = *diagnostics.at(0U);

  std::vector<CallbackObservation> observations;
  observations.reserve(2U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                          model::TurnOrdinal::initial(), 1U);
  REQUIRE(plan);
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);
  REQUIRE(strategy != nullptr);
  strategy->configure_reentry(bot_runtime, plan.value(), initialized.value(), 1U);
  strategy->configure_clock_advance(clock, 101U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Re-entry and budget observations are dropped as noncritical detail, never as control flow.
  const auto report = bot_runtime.dispatch(plan.value(), initialized.value());
  REQUIRE(report);
  CHECK(report.value().callback_count() == 1U);
  CHECK(report.value().callback_budget_exceeded == 1U);
  CHECK(report.value().diagnostic_evidence_failures == 0U);
  CHECK(report.value().diagnostic_observations_dropped == 2U);
  CHECK(bot_runtime.status().healthy());
  CHECK(diagnostics.saturated());
  CHECK(diagnostics.accepted_count() == 1U);
  CHECK(diagnostics.dropped_count() == 2U);
  REQUIRE(diagnostics.at(0U) != nullptr);
  CHECK(*diagnostics.at(0U) == retained_prefix);
  CHECK(count_trace_kind(trace_sink, trace::RuntimeTraceEventKind::ReentryDetected) == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Saturation does not close later preflight admission.
  REQUIRE(bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                ordinal<model::TurnOrdinal>(2U), 1U));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// An unexpected canonical append failure during a callback becomes a latched evidence fault after
// the already-entered callback completes.
TEST_CASE("bot runtime latches callback-local canonical trace failure", "[runtime][bot][trace]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration, limits(2U, 100U, 32U, 6U));
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  model::DeterministicClockProvider clock{0U};
  auto machine = state_machine_from(configuration, policy);

  // ++++++++++++++++++++++++++++++++++++++++
  // Leave exactly one state-machine record and the two bot-runtime reservations available. The
  // callback-local test append then consumes the reserved first-reentry slot unexpectedly.
  trace::RuntimeTraceFields filler;
  filler.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  filler.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  filler.turn_ordinal = model::TurnOrdinal::initial();
  filler.callback_ordinal = model::CallbackOrdinal::initial();
  filler.failure_reason = trace::RuntimeTraceFailureReason::StrategyDispatchReentry;
  for (std::uint32_t index = 0U; index < 3U; ++index) {
    REQUIRE(trace_sink.append(trace::RuntimeTraceEventKind::ReentryDetected, filler));
  }

  std::vector<CallbackObservation> observations;
  observations.reserve(1U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                          model::TurnOrdinal::initial(), 1U);
  REQUIRE(plan);
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);
  REQUIRE(strategy != nullptr);
  strategy->configure_trace_slot_consumption(trace_sink);
  strategy->configure_owner_reentry(bot_runtime, 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The callback finishes and its re-entry diagnostic is still structurally valid, but the failed
  // critical append latches a stable fault that closes every later dispatch boundary.
  const auto report = bot_runtime.dispatch(plan.value(), initialized.value());
  REQUIRE(report);
  CHECK(report.value().callback_count() == 1U);
  CHECK(observations.size() == 1U);
  CHECK(strategy->trace_slot_consumed());
  REQUIRE(strategy->owner_reentry_errors().size() == 1U);
  CHECK(strategy->owner_reentry_errors().front() == model::DomainErrorCode::TraceCapacityExceeded);
  CHECK(bot_runtime.status().canonical_trace_failure_latched);
  CHECK_FALSE(bot_runtime.status().healthy());
  REQUIRE(diagnostics.size() == 1U);
  CHECK(diagnostics.at(0U)->kind == runtime::RuntimeDiagnosticKind::OwnerReentryDetected);
  CHECK(diagnostics.at(0U)->fields.occurrence_count == 1U);

  const auto suppressed = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                ordinal<model::TurnOrdinal>(2U), 1U);
  REQUIRE_FALSE(suppressed);
  CHECK(suppressed.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeEvidenceExhausted,
                                     "bot_runtime.canonical_trace_evidence"));
  const auto redispatch = bot_runtime.dispatch(plan.value(), initialized.value());
  REQUIRE_FALSE(redispatch);
  CHECK(redispatch.error() == suppressed.error());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A regressing injected measurement remains noncanonical but cannot masquerade as a huge duration.
TEST_CASE("bot runtime reports callback clock regression", "[runtime][bot][diagnostics]") {
  const auto configuration = configuration_from(test_support::reference_configuration_params());
  const auto policy = policy_from(configuration, limits(2U, 100U));
  trace::RuntimeTraceSink trace_sink{policy};
  runtime::RuntimeDiagnosticSink diagnostics{policy};
  RegressingMeasurementClock clock{100U, 90U};
  auto machine = state_machine_from(configuration, policy);
  std::vector<CallbackObservation> observations;
  observations.reserve(2U);
  RecordingStrategy* strategy = nullptr;
  std::vector<runtime::BotStrategyRegistration> registrations;
  registrations.push_back(
      registration("bot.deribit-btc-perpetual-reference", 1U, observations, strategy));
  auto created = runtime::BotRuntime::create(configuration, policy, clock, trace_sink, diagnostics,
                                             std::move(registrations));
  REQUIRE(created);
  auto bot_runtime = std::move(created).value();
  const auto initialization_plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                         model::TurnOrdinal::initial(), 1U);
  REQUIRE(initialization_plan);
  const auto initialized =
      machine.initialize(market_data::OwnerMarketTurnContext{model::TurnOrdinal::initial(),
                                                             model::ProcessingTimestamp{1U}},
                         trace_sink);
  REQUIRE(initialized);

  // ++++++++++++++++++++++++++++++++++++++++
  // Skip dispatch of the setup transition, then use a two-callback Ready turn to prove the first
  // regressing measurement does not truncate the remaining preflighted market callback.
  const auto plan = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                          ordinal<model::TurnOrdinal>(2U), 2U);
  REQUIRE(plan);
  const auto ready =
      machine.process(snapshot_from(policy),
                      market_data::AcceptedMarketTurnContext{model::AdmissionOrdinal::initial(),
                                                             ordinal<model::TurnOrdinal>(2U),
                                                             model::ProcessingTimestamp{200U}},
                      trace_sink);
  REQUIRE(ready);
  const auto report = bot_runtime.dispatch(plan.value(), ready.value());
  REQUIRE(report);
  CHECK(report.value().callback_count() == 2U);
  CHECK(report.value().callback_budget_exceeded == 0U);
  CHECK(report.value().callback_clock_regressions == 1U);
  REQUIRE(strategy != nullptr);
  CHECK(observations.size() == 2U);
  CHECK_FALSE(bot_runtime.status().healthy());
  CHECK(bot_runtime.status().callback_clock_regression_latched);
  REQUIRE(diagnostics.size() == 1U);
  CHECK(diagnostics.at(0U)->kind == runtime::RuntimeDiagnosticKind::CallbackClockRegression);
  CHECK(diagnostics.at(0U)->fields.observed_value == 90U);
  CHECK(diagnostics.at(0U)->fields.limit_value == 100U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The measurement fault becomes a stable fail-closed boundary only after the current fan-out.
  const auto suppressed = bot_runtime.preflight(model::MarketSourceOrdinal::initial(),
                                                ordinal<model::TurnOrdinal>(3U), 1U);
  REQUIRE_FALSE(suppressed);
  CHECK(suppressed.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidTimestampOrder,
                                     "bot_runtime.callback_clock_regression"));
  const auto redispatch = bot_runtime.dispatch(plan.value(), ready.value());
  REQUIRE_FALSE(redispatch);
  CHECK(redispatch.error() == suppressed.error());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
