// Purpose: prove MarketRuntime composes bounded M2 market dispatch and optional direct M3 fake
// submission with deterministic quiescent evidence through its public API.

#include "aegis/runtime/fake_submission_runtime.hpp"
#include "aegis/runtime/market_runtime.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Callback observations copy every asserted value and never retain a turn-scoped book or context.
enum class ObservedCallbackKind : std::uint8_t {
  State = 1,
  Market = 2,
};

// ########################################################################

// ########################################################################
// Fixed optional level arrays let the capturing strategy remain allocation-free during callbacks.
struct CallbackObservation {
  ObservedCallbackKind kind;
  bool reference_route;
  model::CallbackOrdinal callback_ordinal;
  market_data::MarketReadiness readiness;
  std::optional<model::BookGeneration> book_generation;
  std::optional<model::BookRevision> book_revision;
  std::optional<model::Price> best_bid;
  std::optional<model::Price> best_ask;
  std::size_t bid_count;
  std::size_t ask_count;
  std::array<std::optional<market_data::BookLevel>, runtime::maximum_runtime_retained_book_depth>
      bids;
  std::array<std::optional<market_data::BookLevel>, runtime::maximum_runtime_retained_book_depth>
      asks;

  // --------------------------------------------------------
  // Structural equality makes complete callback sequences directly replay-comparable.
  friend bool operator==(const CallbackObservation&, const CallbackObservation&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// MarketCallbackCapturingStrategy records only bounded immutable values from each synchronous
// callback.
class MarketCallbackCapturingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow a construction-time-reserved observation vector that outlives this strategy.
  explicit MarketCallbackCapturingStrategy(std::vector<CallbackObservation>& observations) noexcept
      : observations_{&observations} {}

  // --------------------------------------------------------
  // Copy the complete committed book visible in one Ready callback.
  void on_market_data(const market_data::MarketEvent& event, const market_data::ReadyBookView& book,
                      runtime::BotContext& context) noexcept override {
    const auto& commit = event.context();
    CallbackObservation observation{
        ObservedCallbackKind::Market,
        is_reference_route(context),
        context.callback_ordinal(),
        market_data::MarketReadiness::Ready,
        commit.book_generation,
        commit.book_revision,
        book.best_bid(),
        book.best_ask(),
        book.bid_count(),
        book.ask_count(),
        {},
        {},
    };
    for (std::size_t index = 0U; index < book.bid_count() && index < observation.bids.size();
         ++index) {
      observation.bids[index] = book.bid_at(index);
    }
    for (std::size_t index = 0U; index < book.ask_count() && index < observation.asks.size();
         ++index) {
      observation.asks[index] = book.ask_at(index);
    }
    observations_->push_back(std::move(observation));
  }

  // --------------------------------------------------------
  // Copy a sanitized state event without receiving any book storage or malformed bytes.
  void on_market_state(const market_data::MarketStateEvent& event,
                       runtime::BotContext& context) noexcept override {
    const auto& state = event.fields();
    observations_->push_back(CallbackObservation{
        ObservedCallbackKind::State,
        is_reference_route(context),
        context.callback_ordinal(),
        state.readiness,
        state.book_generation,
        state.book_revision,
        std::nullopt,
        std::nullopt,
        0U,
        0U,
        {},
        {},
    });
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Match every callback attribution field against the sealed single-firm reference grant.
  [[nodiscard]] static bool is_reference_route(const runtime::BotContext& context) noexcept {
    return context.firm_id().value() == "firm.aegis-lab" &&
           context.desk_id().value() == "desk.digital-assets" &&
           context.bot_id().value() == "bot.deribit-btc-perpetual-reference" &&
           context.strategy_id().value() == "strategy.deterministic-reference" &&
           context.subscription_id().value() == "subscription.deribit-btc-perpetual-book";
  }

  // --------------------------------------------------------
  std::vector<CallbackObservation>* observations_;
};

// ########################################################################

// ########################################################################
// OwnerCloseControl transfers one stable runtime handle into a dedicated callback and publishes
// entry/return observations without exposing any runtime-owned mutable state.
struct OwnerCloseControl {
  std::atomic<runtime::MarketRuntime*> runtime{nullptr};
  std::atomic_bool armed{false};
  std::atomic_bool entered{false};
  std::atomic_bool returned{false};
  std::atomic_bool handle_observed{false};
};

// ########################################################################

// ########################################################################
// OwnerReentrantExecutionControl publishes the result of one deliberately recursive over-bound
// drive request.
struct OwnerReentrantExecutionControl {
  std::atomic<runtime::MarketRuntime*> runtime{nullptr};
  std::atomic_bool armed{false};
  std::atomic_bool returned{false};
  std::atomic_uint32_t error_code{0U};
};

// ########################################################################

// ########################################################################
// OwnerClosingStrategy exercises graceful closure from the dedicated owner callback itself.
class OwnerClosingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow control storage whose lifetime encloses the runtime and dedicated owner.
  explicit OwnerClosingStrategy(OwnerCloseControl& control) noexcept : control_{&control} {}

  // --------------------------------------------------------
  // Exercise the same close path if a Ready update is the first armed callback.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {
    close_runtime_once();
  }

  // --------------------------------------------------------
  // The bootstrap state callback is the deterministic self-close trigger in the regression test.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    close_runtime_once();
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Consume the arm once, close through the stable external handle, and prove the call returned.
  void close_runtime_once() noexcept {
    if (!control_->armed.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    control_->entered.store(true, std::memory_order_release);
    auto* const runtime = control_->runtime.load(std::memory_order_acquire);
    control_->handle_observed.store(runtime != nullptr, std::memory_order_release);
    if (runtime != nullptr) {
      runtime->close_and_wait();
    }
    control_->returned.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
  OwnerCloseControl* control_;
};

// ########################################################################

// ########################################################################
// OwnerReentrantExecutionStrategy proves active-owner reentry takes precedence over an invalid
// drive bound.
class OwnerReentrantExecutionStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow control storage whose stable runtime handle is published after factory return.
  explicit OwnerReentrantExecutionStrategy(OwnerReentrantExecutionControl& control) noexcept
      : control_{&control} {}

  // --------------------------------------------------------
  // A market callback is an equivalent reentry boundary if it is the first armed callback.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {
    request_reentrant_drive_once();
  }

  // --------------------------------------------------------
  // Bootstrap supplies the deterministic callback used by the composed reentry test.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    request_reentrant_drive_once();
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Request one more turn than policy allows and retain only the stable assigned error code.
  void request_reentrant_drive_once() noexcept {
    if (!control_->armed.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    auto* const runtime = control_->runtime.load(std::memory_order_acquire);
    if (runtime != nullptr) {
      const auto driven = runtime->execute_pending_turns(33U);
      if (!driven) {
        control_->error_code.store(static_cast<std::uint32_t>(driven.error().code),
                                   std::memory_order_release);
      }
    }
    control_->returned.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
  OwnerReentrantExecutionControl* control_;
};

// ########################################################################

// ########################################################################
// ArmedRegressingClock remains stable for bootstrap, then deterministically makes every later
// callback finish before it starts to trigger the production post-commit fault path.
class ArmedRegressingClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Publish the regression script only after bootstrap has completed successfully.
  void arm_regression_script() noexcept {
    reading_.store(0U, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Alternate later callback starts and finishes while returning one stable bootstrap value.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    if (!armed_.load(std::memory_order_acquire)) {
      return 1'000U;
    }
    const auto reading = reading_.fetch_add(1U, std::memory_order_relaxed);
    return reading % 2U == 0U ? 2'000U : 1'000U;
  }

  // --------------------------------------------------------
  std::atomic_bool armed_{false};
  std::atomic_uint64_t reading_{0U};
};

// ########################################################################

// ########################################################################
// ArmedBudgetClock leaves bootstrap unmeasured, then makes each callback exceed the sealed budget
// without regressing so the coordinator must expose observation rather than fail closed.
class ArmedBudgetClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Begin a deterministic alternating callback start/finish script.
  void arm_budget_script() noexcept {
    reading_.store(0U, std::memory_order_relaxed);
    armed_.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Return a duration strictly above the reference policy's 100,000-nanosecond budget.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    if (!armed_.load(std::memory_order_acquire)) {
      return 1'000U;
    }
    const auto reading = reading_.fetch_add(1U, std::memory_order_relaxed);
    return reading % 2U == 0U ? 2'000U : 202'001U;
  }

  // --------------------------------------------------------
  std::atomic_bool armed_{false};
  std::atomic_uint64_t reading_{0U};
};

// ########################################################################

// ########################################################################
// Interesting syntax: requires-expression probes prove a caller-authored order has no organization,
// account, venue, or local identity fields with which a strategy could forge runtime-bound
// submission attribution.
template <typename Request>
concept HasCallerFirm = requires(Request request) { request.firm_id; };

template <typename Request>
concept HasCallerBot = requires(Request request) { request.bot_id; };

template <typename Request>
concept HasCallerAccount = requires(Request request) { request.logical_account_id; };

template <typename Request>
concept HasCallerVenue = requires(Request request) { request.venue_id; };

template <typename Request>
concept HasCallerOrderId = requires(Request request) { request.order_id; };

static_assert(!HasCallerFirm<execution::OrderRequest>);
static_assert(!HasCallerBot<execution::OrderRequest>);
static_assert(!HasCallerAccount<execution::OrderRequest>);
static_assert(!HasCallerVenue<execution::OrderRequest>);
static_assert(!HasCallerOrderId<execution::OrderRequest>);

// ########################################################################

// ########################################################################
// SubmissionCapture retains only immutable callback attribution and the synchronous local result
// projection, so manual and dedicated owners can be compared after release.
struct SubmissionCapture {
  std::uint32_t callbacks{0U};
  bool submit_returned{false};
  std::string firm_id;
  std::string desk_id;
  std::string bot_id;
  std::string strategy_id;
  std::optional<model::CallbackOrdinal> callback_ordinal;
  std::optional<execution::SubmitDisposition> disposition;
  std::optional<execution::SubmissionStage> stage;
  std::optional<execution::SubmissionReason> reason;
  std::optional<model::SubmissionAttemptId> attempt_id;
  std::optional<model::OrderId> order_id;

  // --------------------------------------------------------
  // Ignore noncanonical wall duration while comparing every deterministic callback/result field.
  friend bool operator==(const SubmissionCapture&, const SubmissionCapture&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// SubmittingStrategy exercises exactly one synchronous bot-bound submission from the bootstrap
// state callback and records whether the result returned before that callback completed.
class SubmittingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow stable test-owned request and result storage for the strategy's complete lifetime.
  SubmittingStrategy(execution::OrderRequest request, SubmissionCapture& capture) noexcept
      : request_{std::move(request)}, capture_{&capture} {}

  // --------------------------------------------------------
  // A Ready callback is an equivalent submission boundary if it is the first delivered callback.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
  // Bootstrap is the deterministic first callback used by the composition tests.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext& context) noexcept override {
    submit_request_once(context);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Copy authoritative context identity, call submit directly, and retain the result before return.
  void submit_request_once(runtime::BotContext& context) noexcept {
    if (capture_->callbacks != 0U) {
      return;
    }
    ++capture_->callbacks;
    capture_->firm_id = std::string{context.firm_id().value()};
    capture_->desk_id = std::string{context.desk_id().value()};
    capture_->bot_id = std::string{context.bot_id().value()};
    capture_->strategy_id = std::string{context.strategy_id().value()};
    capture_->callback_ordinal = context.callback_ordinal();
    const auto result = context.submit_order(request_);
    capture_->disposition = result.disposition();
    capture_->stage = result.stage();
    capture_->reason = result.reason();
    capture_->attempt_id = result.attempt_id();
    capture_->order_id = result.order_id();
    capture_->submit_returned = true;
  }

  // --------------------------------------------------------
  execution::OrderRequest request_;
  SubmissionCapture* capture_;
};

// ########################################################################

// ########################################################################
// SubmissionGateControl safely publishes a live submission-capable context to the test thread while
// the dedicated owner callback remains active, then retains that same context after deactivation.
struct SubmissionGateControl {
  std::atomic<runtime::BotContext*> context{nullptr};
  std::atomic_bool callback_active{false};
  std::atomic_bool release_callback{false};
};

// ########################################################################

// ########################################################################
// SubmissionGateStrategy holds its first callback open without submitting so wrong-thread and
// retained-context gates can be proved without consuming any attempt or owner-local evidence.
class SubmissionGateStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow control storage whose lifetime encloses the dedicated callback and test thread.
  explicit SubmissionGateStrategy(SubmissionGateControl& control) noexcept : control_{&control} {}

  // --------------------------------------------------------
  // Hold the first market-data callback open without consuming submission authority.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext& context) noexcept override {
    hold_callback_open_once(context);
  }

  // --------------------------------------------------------
  // Hold the first readiness callback open without consuming submission authority.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext& context) noexcept override {
    hold_callback_open_once(context);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish the persistent context while active, then wait until the test has exercised WrongOwner.
  void hold_callback_open_once(runtime::BotContext& context) noexcept {
    if (entered_) {
      return;
    }
    entered_ = true;
    control_->context.store(&context, std::memory_order_release);
    control_->callback_active.store(true, std::memory_order_release);
    while (!control_->release_callback.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  // --------------------------------------------------------
  SubmissionGateControl* control_;
  bool entered_{false};
};

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture-authoring defects rather than coordinator behavior.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact prices without binary floating-point fixture drift.
[[nodiscard]] model::Price parse_price_or_throw(std::string_view text) {
  auto parsed = model::Price::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid price in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact quantities without binary floating-point fixture drift.
[[nodiscard]] model::Quantity parse_quantity_or_throw(std::string_view text) {
  auto parsed = model::Quantity::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact quote notionals without binary floating-point fixture drift.
[[nodiscard]] model::Notional parse_notional_or_throw(std::string_view text) {
  auto parsed = model::Notional::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid notional in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Derive the one-bot M3 authority without changing the accepted observation-only reference fixture.
[[nodiscard]] configuration::StartupConfiguration create_m3_configuration_or_throw() {
  auto params = test_support::create_reference_configuration_params_or_throw();
  params.routes.front().state = execution::ExecutionRouteState::Enabled;
  auto created =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!created) {
    throw std::logic_error{"invalid M3 market-runtime configuration"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Resolve one heterogeneous scope subject from the same sealed route and attribution authority.
[[nodiscard]] std::string derive_m3_scope_subject_or_throw(
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
    throw std::logic_error{"invalid M3 market-runtime risk scope"};
  }
}

// --------------------------------------------------------
// Author one complete immutable seven-scope policy with generous exact limits for one fake order.
[[nodiscard]] risk::RiskPolicyParams
create_m3_risk_policy_params_or_throw(const configuration::StartupConfiguration& configuration) {
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
      throw std::logic_error{"incomplete M3 market-runtime route"};
    }
    metadata_revisions.push_back(configuration::InstrumentMetadataRevisionEntry{
        metadata->venue_id(), metadata->instrument_id(), metadata->revision()});
    for (std::uint8_t value = static_cast<std::uint8_t>(risk::RiskScopeKind::Bot);
         value <= static_cast<std::uint8_t>(risk::RiskScopeKind::Venue); ++value) {
      const auto scope = static_cast<risk::RiskScopeKind>(value);
      limits.push_back(risk::RiskLimitSetParams{
          attribution->firm_id,
          scope,
          derive_m3_scope_subject_or_throw(route, *attribution, *metadata, scope),
          metadata->instrument_id(),
          std::string{metadata->quote_currency()},
          parse_quantity_or_throw("1000"),
          parse_notional_or_throw("100000"),
          8U,
          parse_notional_or_throw("1000000"),
          parse_quantity_or_throw("10000"),
          parse_notional_or_throw("1000000"),
      });
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
// Build one deterministic fake-only stack with enough preallocated state for four submissions.
[[nodiscard]] runtime::FakeSubmissionRuntimeParams create_m3_submission_params_or_throw(
    const configuration::StartupConfiguration& configuration,
    execution::FakeInitiationOutcome outcome =
        execution::FakeInitiationOutcome::AcceptedAndInitiated,
    std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock = nullptr) {
  constexpr std::uint64_t maximum_attempts = 4U;
  auto encoder_script = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, maximum_attempts, {});
  auto initiator_script =
      execution::FakeInitiatorScript::create_fake_initiator_script(outcome, maximum_attempts, {});
  model::OrderNamespace::Bytes namespace_bytes{};
  for (std::size_t index = 0U; index < namespace_bytes.size(); ++index) {
    namespace_bytes[index] = static_cast<std::uint8_t>(0x40U + index);
  }
  auto order_ids = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{namespace_bytes});
  if (!encoder_script || !initiator_script || !order_ids) {
    throw std::logic_error{"invalid M3 market-runtime fake script"};
  }
  if (!measurement_clock) {
    measurement_clock = std::make_unique<execution::SteadySubmissionMeasurementClock>();
  }
  return runtime::FakeSubmissionRuntimeParams{
      create_m3_risk_policy_params_or_throw(configuration),
      execution::SubmissionPolicyCapacities{maximum_attempts, 4U, 4U, 1'024U, 4U, 44U, 16U},
      std::move(encoder_script).value(),
      std::move(initiator_script).value(),
      std::move(measurement_clock),
      std::move(order_ids).value(),
  };
}

// --------------------------------------------------------
// Use the sole enabled route and exact limit/GTC values accepted by reference metadata.
[[nodiscard]] execution::OrderRequest create_m3_order_request_or_throw() {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      parse_price_or_throw("30000.5"),
      parse_quantity_or_throw("2"),
  };
}

// --------------------------------------------------------
// Seal one reference configuration before policy and runtime construction borrow its provenance.
[[nodiscard]] configuration::StartupConfiguration create_reference_configuration_or_throw() {
  auto created = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());
  if (!created) {
    throw std::logic_error{"invalid startup configuration in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Define the sole credential-free public source used by the deterministic reference scenario.
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
// Keep unit-test bounds small while leaving enough callback and trace headroom for every turn.
[[nodiscard]] runtime::RuntimePolicy
create_reference_policy_or_throw(const configuration::StartupConfiguration& configuration,
                                 std::uint32_t ingress_capacity,
                                 std::uint32_t trace_capacity = 256U) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration, runtime::RuntimePolicyParams{
                         runtime::RuntimePolicyLimits{ingress_capacity, 4096U, 64U, 20U, 1'000U, 2U,
                                                      64U, trace_capacity, 32U, 100'000U},
                         {create_reference_source_or_throw()},
                     });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Compose one stable runtime whose sole configured strategy submits through the named M3 factory.
[[nodiscard]] std::unique_ptr<runtime::MarketRuntime>
create_m3_runtime_or_throw(model::ClockProvider& executor_clock,
                           model::ClockProvider& callback_measurement_clock,
                           SubmissionCapture& capture,
                           execution::FakeInitiationOutcome outcome =
                               execution::FakeInitiationOutcome::AcceptedAndInitiated) {
  auto configuration = create_m3_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration, 4U);
  auto submission_params = create_m3_submission_params_or_throw(configuration, outcome);
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      std::make_unique<SubmittingStrategy>(create_m3_order_request_or_throw(), capture),
  });
  auto created = runtime::MarketRuntime::create_with_fake_submission(
      std::move(configuration), std::move(policy), executor_clock, callback_measurement_clock,
      std::move(strategies), std::move(submission_params));
  if (!created) {
    throw std::logic_error{"invalid fake-submission market runtime: " +
                           created.error().context.field};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Own one caller attempt while leaving its optional attribution untrusted until runtime admission.
[[nodiscard]] market_data::IngressFrameAttempt create_ingress_attempt_or_throw(
    std::string frame, std::optional<std::string_view> source_id = "source.deribit-btc-perpetual") {
  std::optional<model::MarketSourceId> typed_source;
  if (source_id.has_value()) {
    typed_source = parse_identifier_or_throw<model::MarketSourceId>(source_id.value());
  }
  auto created = market_data::IngressFrameAttempt::create_ingress_frame_attempt(
      std::move(typed_source), model::SessionEpoch{1U}, std::move(frame));
  if (!created) {
    throw std::logic_error{"invalid ingress attempt in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Return the complete valid snapshot used to establish generation one.
[[nodiscard]] std::string create_snapshot_frame() {
  return "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:book100|4|"
         "B,30000.5,2|B,30000.0,4|A,30001.0,3|A,30001.5,5";
}

// --------------------------------------------------------
// Return a complete delta whose deletions and replacements expose a partial-apply bug immediately.
[[nodiscard]] std::string create_delta_frame() {
  return "AEGISMD|1|source.deribit-btc-perpetual|delta|101|100|1100|1|ok:book101|4|"
         "B,30000.5,0|B,30000.0,6|A,30001.0,0|A,30002.0,8";
}

// ########################################################################
// One harness keeps observations and borrowed clocks alive until after the runtime is destroyed.
class MarketRuntimeHarness final {
public:

  // --------------------------------------------------------
  // Create one manual-driver runtime with a pre-reserved, single reference strategy.
  explicit MarketRuntimeHarness(std::uint32_t ingress_capacity)
      : executor_clock_{100U}, callback_measurement_clock_{1'000U} {
    observations_.reserve(32U);
    auto configuration = create_reference_configuration_or_throw();
    auto policy = create_reference_policy_or_throw(configuration, ingress_capacity);
    std::vector<runtime::BotStrategyRegistration> strategies;
    strategies.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<MarketCallbackCapturingStrategy>(observations_),
    });
    auto created = runtime::MarketRuntime::create_market_runtime(
        std::move(configuration), std::move(policy), executor_clock_, callback_measurement_clock_,
        std::move(strategies));
    if (!created) {
      throw std::logic_error{"invalid market runtime in test fixture"};
    }
    runtime_ = std::move(created).value();
  }

  // --------------------------------------------------------
  // Borrow the manual-driver runtime while the harness retains its clocks and strategy storage.
  [[nodiscard]] runtime::MarketRuntime& market_runtime() noexcept { return *runtime_; }

  // --------------------------------------------------------
  // Borrow the complete copied callback sequence retained by the harness strategy.
  [[nodiscard]] const std::vector<CallbackObservation>& callback_observations() const noexcept {
    return observations_;
  }

  // --------------------------------------------------------
private:
  std::vector<CallbackObservation> observations_;
  model::DeterministicClockProvider executor_clock_;
  model::DeterministicClockProvider callback_measurement_clock_;
  std::unique_ptr<runtime::MarketRuntime> runtime_;
};

// ########################################################################

// --------------------------------------------------------
// Compose one runtime around a caller-selected strategy and clocks for lifecycle fault regressions.
[[nodiscard]] std::unique_ptr<runtime::MarketRuntime> create_runtime_with_strategy_or_throw(
    std::uint32_t ingress_capacity, model::ClockProvider& executor_clock,
    model::ClockProvider& callback_measurement_clock, std::unique_ptr<runtime::Strategy> strategy,
    std::uint32_t trace_capacity = 256U) {
  auto configuration = create_reference_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration, ingress_capacity, trace_capacity);
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      std::move(strategy)});
  auto created = runtime::MarketRuntime::create_market_runtime(
      std::move(configuration), std::move(policy), executor_clock, callback_measurement_clock,
      std::move(strategies));
  if (!created) {
    throw std::logic_error{"invalid controlled market runtime in test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Run the genuine queued bootstrap as the first manually owned turn.
void require_bound_and_bootstrapped_runtime(MarketRuntimeHarness& harness) {
  REQUIRE(harness.market_runtime().bind_to_current_thread());
  auto turn = harness.market_runtime().execute_next_turn();
  REQUIRE(turn);
  REQUIRE(turn.value().has_value());
  CHECK(turn.value()->kind == runtime::TurnKind::Command);
  CHECK(turn.value()->turn_ordinal == model::TurnOrdinal::create_initial());
  CHECK(turn.value()->attempt_ordinal == model::AdmissionOrdinal::create_initial());
}

// --------------------------------------------------------
// Close, release deterministic ownership, and copy final evidence without Catch control flow.
[[nodiscard]] runtime::MarketRuntimeEvidence
close_and_collect_evidence_or_throw(runtime::MarketRuntime& runtime) {
  runtime.close();
  const auto released = runtime.release_from_current_thread();
  if (!released) {
    throw std::logic_error{"failed to release market-runtime owner in test fixture"};
  }
  auto evidence = runtime.collect_quiescent_evidence();
  if (!evidence) {
    throw std::logic_error{"failed to collect quiescent market-runtime evidence"};
  }
  return std::move(evidence).value();
}

// --------------------------------------------------------
// Admit one frame and require ordinary bounded acceptance before its owner turn runs.
void require_accepted_frame(runtime::MarketRuntime& runtime,
                            market_data::IngressFrameAttempt frame_attempt) {
  const auto decision = runtime.try_admit(std::move(frame_attempt));
  REQUIRE(decision);
  CHECK(decision.value().outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(decision.value().receipt.has_value());
  CHECK_FALSE(decision.value().discontinuity_recorded);
}

// --------------------------------------------------------
// Observe the synchronized lifecycle until the dedicated owner completes source bootstrap.
[[nodiscard]] bool wait_until_running(runtime::MarketRuntime& runtime) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto current = runtime.status();
    if (current.lifecycle == runtime::MarketRuntimeLifecycle::Running) {
      return true;
    }
    if (current.lifecycle == runtime::MarketRuntimeLifecycle::Closed ||
        current.lifecycle == runtime::MarketRuntimeLifecycle::Faulted) {
      return false;
    }
    std::this_thread::yield();
  }
  return false;
}

// --------------------------------------------------------
// Bound the dedicated self-close regression while observing only synchronized public state.
[[nodiscard]] bool wait_until_owner_close_stops(runtime::MarketRuntime& runtime,
                                                const OwnerCloseControl& control) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto current = runtime.status();
    if (control.returned.load(std::memory_order_acquire) && !current.dedicated_driver_running) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

// --------------------------------------------------------
// A runtime begins in Starting and reaches Running only through its queued source-bootstrap turn.
TEST_CASE("market runtime bootstraps one source through the manual serialized owner",
          "[runtime][market_runtime][m2]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Construction enqueues exactly one genuine bootstrap command without binding an owner.
  MarketRuntimeHarness harness{4U};
  const auto starting = harness.market_runtime().status();
  CHECK(starting.lifecycle == runtime::MarketRuntimeLifecycle::Starting);
  CHECK(starting.initialized_sources == 0U);
  CHECK(starting.executor.pending_commands == 1U);
  CHECK(starting.executor.pending_fences == 0U);
  CHECK_FALSE(starting.executor.owner_bound);
  CHECK_FALSE(starting.dedicated_driver_started);
  CHECK_FALSE(starting.dedicated_driver_running);

  // ++++++++++++++++++++++++++++++++++++++++
  // The first owner turn publishes Synchronizing to the one canonical reference subscription.
  require_bound_and_bootstrapped_runtime(harness);
  const auto running = harness.market_runtime().status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.initialized_sources == 1U);
  CHECK(running.executor.pending_commands == 0U);
  CHECK(running.executor.owner_bound);
  REQUIRE(running.last_completed_turn.has_value());
  REQUIRE(harness.callback_observations().size() == 1U);
  const auto& synchronizing = harness.callback_observations().front();
  CHECK(synchronizing.kind == ObservedCallbackKind::State);
  CHECK(synchronizing.reference_route);
  CHECK(synchronizing.callback_ordinal == model::CallbackOrdinal::create_initial());
  CHECK(synchronizing.readiness == market_data::MarketReadiness::Synchronizing);
  CHECK_FALSE(synchronizing.book_generation.has_value());
  CHECK_FALSE(synchronizing.book_revision.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Closed and released evidence owns the final source summary and drained executor state.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  CHECK(harness.market_runtime().status().lifecycle == runtime::MarketRuntimeLifecycle::Closed);
  CHECK_FALSE(evidence.canonical_trace_bytes.empty());
  REQUIRE(evidence.trace_records.size() == 2U);
  CHECK(evidence.trace_records[0U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[1U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.diagnostics.empty());
  CHECK(evidence.dropped_diagnostics == 0U);
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().source_ordinal == model::MarketSourceOrdinal::create_initial());
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Synchronizing);
  CHECK_FALSE(evidence.sources.front().book_identity.has_value());
  CHECK_FALSE(evidence.sources.front().active_session.has_value());
  CHECK_FALSE(evidence.sources.front().last_source_sequence.has_value());
  CHECK(evidence.executor.pending_commands == 0U);
  CHECK(evidence.executor.pending_fences == 0U);
  CHECK(evidence.executor.closed);
  CHECK_FALSE(evidence.executor.owner_bound);
  CHECK_FALSE(evidence.executor.turn_active);
  CHECK_FALSE(evidence.executor.faulted);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Snapshot and delta callbacks must observe only complete post-commit books in canonical order.
TEST_CASE("market runtime routes snapshot and delta only after complete book commits",
          "[runtime][market_runtime][m2][market_data]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Bootstrap, then establish Ready with one complete authoritative snapshot.
  MarketRuntimeHarness harness{4U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto snapshot_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 3U);
  CHECK(harness.callback_observations()[1U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[1U].readiness == market_data::MarketReadiness::Ready);
  const auto& snapshot = harness.callback_observations()[2U];
  CHECK(snapshot.kind == ObservedCallbackKind::Market);
  CHECK(snapshot.reference_route);
  CHECK(snapshot.callback_ordinal.value() == 3U);
  CHECK(snapshot.book_generation == model::BookGeneration::create_initial());
  CHECK(snapshot.book_revision == model::BookRevision::create_initial());
  CHECK(snapshot.best_bid == parse_price_or_throw("30000.5"));
  CHECK(snapshot.best_ask == parse_price_or_throw("30001.0"));
  CHECK(snapshot.bid_count == 2U);
  CHECK(snapshot.ask_count == 2U);
  CHECK((snapshot.bids[0U] ==
         market_data::BookLevel{parse_price_or_throw("30000.5"), parse_quantity_or_throw("2")}));
  CHECK((snapshot.bids[1U] ==
         market_data::BookLevel{parse_price_or_throw("30000.0"), parse_quantity_or_throw("4")}));
  CHECK((snapshot.asks[0U] ==
         market_data::BookLevel{parse_price_or_throw("30001.0"), parse_quantity_or_throw("3")}));
  CHECK((snapshot.asks[1U] ==
         market_data::BookLevel{parse_price_or_throw("30001.5"), parse_quantity_or_throw("5")}));

  // ++++++++++++++++++++++++++++++++++++++++
  // One delta callback sees every deletion and replacement together, never an intermediate book.
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_delta_frame()));
  const auto delta_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(delta_turn);
  REQUIRE(delta_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 4U);
  const auto& delta = harness.callback_observations()[3U];
  CHECK(delta.kind == ObservedCallbackKind::Market);
  CHECK(delta.reference_route);
  CHECK(delta.callback_ordinal.value() == 4U);
  CHECK(delta.book_generation == model::BookGeneration::create_initial());
  REQUIRE(delta.book_revision.has_value());
  CHECK(delta.book_revision->value() == 2U);
  CHECK(delta.best_bid == parse_price_or_throw("30000.0"));
  CHECK(delta.best_ask == parse_price_or_throw("30001.5"));
  CHECK(delta.bid_count == 1U);
  CHECK(delta.ask_count == 2U);
  CHECK((delta.bids[0U] ==
         market_data::BookLevel{parse_price_or_throw("30000.0"), parse_quantity_or_throw("6")}));
  CHECK((delta.asks[0U] ==
         market_data::BookLevel{parse_price_or_throw("30001.5"), parse_quantity_or_throw("5")}));
  CHECK((delta.asks[1U] ==
         market_data::BookLevel{parse_price_or_throw("30002.0"), parse_quantity_or_throw("8")}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Quiescent state retains the last complete commit and exact stream anchor.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.sources.size() == 1U);
  const auto& source = evidence.sources.front();
  CHECK(source.readiness == market_data::MarketReadiness::Ready);
  REQUIRE(source.book_identity.has_value());
  CHECK(source.book_identity->generation().value() == 1U);
  CHECK(source.book_identity->revision().value() == 2U);
  CHECK(source.active_session == model::SessionEpoch{1U});
  CHECK(source.last_source_sequence == model::SequenceNumber{101U});
  REQUIRE(evidence.trace_records.size() == 8U);
  CHECK(evidence.trace_records[0U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[1U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.trace_records[2U].kind() == trace::RuntimeTraceEventKind::InputDisposition);
  CHECK(evidence.trace_records[2U].fields().input_disposition ==
        trace::RuntimeInputDisposition::SnapshotApplied);
  CHECK(evidence.trace_records[3U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[4U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.trace_records[5U].kind() == trace::RuntimeTraceEventKind::MarketCallback);
  CHECK(evidence.trace_records[6U].kind() == trace::RuntimeTraceEventKind::InputDisposition);
  CHECK(evidence.trace_records[6U].fields().input_disposition ==
        trace::RuntimeInputDisposition::DeltaApplied);
  CHECK(evidence.trace_records[7U].kind() == trace::RuntimeTraceEventKind::MarketCallback);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Explicit control recovery is serialized and hides a retained book until a fresh snapshot.
TEST_CASE("market runtime resynchronizes one ready source through an owner turn",
          "[runtime][market_runtime][m2][resynchronize]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish one committed book before admitting the source-scoped control command.
  MarketRuntimeHarness harness{4U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto snapshot_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  const auto resynchronization = harness.market_runtime().try_resynchronize(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"));
  REQUIRE(resynchronization);
  CHECK(resynchronization.value().outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(resynchronization.value().receipt.has_value());
  CHECK_FALSE(resynchronization.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // The control turn publishes only Synchronizing while retaining identity counters internally.
  const auto control_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(control_turn);
  REQUIRE(control_turn.value().has_value());
  CHECK(control_turn.value()->kind == runtime::TurnKind::Command);
  REQUIRE(harness.callback_observations().size() == 4U);
  const auto& synchronizing = harness.callback_observations().back();
  CHECK(synchronizing.kind == ObservedCallbackKind::State);
  CHECK(synchronizing.readiness == market_data::MarketReadiness::Synchronizing);
  CHECK(synchronizing.book_generation == model::BookGeneration::create_initial());
  CHECK(synchronizing.book_revision == model::BookRevision::create_initial());

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence exposes cleared continuity and the retained identity without a market callback.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Synchronizing);
  REQUIRE(evidence.sources.front().book_identity.has_value());
  CHECK(evidence.sources.front().book_identity->generation() ==
        model::BookGeneration::create_initial());
  CHECK(evidence.sources.front().book_identity->revision() ==
        model::BookRevision::create_initial());
  CHECK_FALSE(evidence.sources.front().active_session.has_value());
  CHECK_FALSE(evidence.sources.front().last_source_sequence.has_value());
  REQUIRE(evidence.trace_records.size() == 8U);
  CHECK(evidence.trace_records[6U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[7U].kind() == trace::RuntimeTraceEventKind::StateCallback);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Attributable malformed bytes may invalidate state but cannot publish data or poison recovery.
TEST_CASE("market runtime contains malformed input and recovers on a later valid snapshot",
          "[runtime][market_runtime][m2][malformed]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A malformed quantity produces only one sanitized Invalid state callback.
  MarketRuntimeHarness harness{4U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(
      harness.market_runtime(),
      create_ingress_attempt_or_throw(
          "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:bad|1|"
          "B,30000.5,broken"));
  const auto malformed_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(malformed_turn);
  REQUIRE(malformed_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 2U);
  CHECK(harness.callback_observations()[1U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[1U].readiness == market_data::MarketReadiness::Invalid);
  CHECK_FALSE(harness.callback_observations()[1U].best_bid.has_value());
  CHECK_FALSE(harness.callback_observations()[1U].best_ask.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // The next valid snapshot starts the first generation from clean temporary parser state.
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto recovery_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(recovery_turn);
  REQUIRE(recovery_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 4U);
  CHECK(harness.callback_observations()[2U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[2U].readiness == market_data::MarketReadiness::Ready);
  CHECK(harness.callback_observations()[3U].kind == ObservedCallbackKind::Market);
  CHECK(harness.callback_observations()[3U].best_bid == parse_price_or_throw("30000.5"));
  CHECK(harness.callback_observations()[3U].best_ask == parse_price_or_throw("30001.0"));

  // ++++++++++++++++++++++++++++++++++++++++
  // The bounded diagnostic exposes only assigned parse metadata, while state reflects recovery.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.diagnostics.size() == 1U);
  const auto& diagnostic = evidence.diagnostics.front();
  CHECK(diagnostic.kind == runtime::RuntimeDiagnosticKind::MalformedInput);
  CHECK(diagnostic.fields.source_ordinal == model::MarketSourceOrdinal::create_initial());
  CHECK(diagnostic.fields.detail_code ==
        (0x00010000U |
         static_cast<std::uint32_t>(market_data::RecordedFixtureParseCode::InvalidQuantity)));
  CHECK(diagnostic.fields.occurrence_count == 1U);
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Ready);
  REQUIRE(evidence.sources.front().book_identity.has_value());
  CHECK(evidence.sources.front().book_identity->generation().value() == 1U);
  CHECK(evidence.sources.front().book_identity->revision().value() == 1U);
  REQUIRE(evidence.trace_records.size() == 9U);
  CHECK(evidence.trace_records[2U].kind() == trace::RuntimeTraceEventKind::InputDisposition);
  CHECK(evidence.trace_records[2U].fields().input_disposition ==
        trace::RuntimeInputDisposition::MalformedRejected);
  CHECK(evidence.trace_records[3U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[4U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.trace_records[5U].fields().input_disposition ==
        trace::RuntimeInputDisposition::SnapshotApplied);
  CHECK(evidence.trace_records[8U].kind() == trace::RuntimeTraceEventKind::MarketCallback);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A zero source sequence is contained as absent identity and cannot commit or poison recovery.
TEST_CASE("market runtime rejects zero source sequence before book publication",
          "[runtime][market_runtime][m2][sequence]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // The syntactically valid zero sequence yields only a sanitized Invalid state callback.
  MarketRuntimeHarness harness{4U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(
      harness.market_runtime(),
      create_ingress_attempt_or_throw(
          "AEGISMD|1|source.deribit-btc-perpetual|snapshot|0|none|1000|1|ok:zero|2|"
          "B,30000.5,2|A,30001.0,3"));
  const auto rejected_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(rejected_turn);
  REQUIRE(rejected_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 2U);
  CHECK(harness.callback_observations()[1U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[1U].readiness == market_data::MarketReadiness::Invalid);
  CHECK_FALSE(harness.callback_observations()[1U].book_generation.has_value());
  CHECK_FALSE(harness.callback_observations()[1U].book_revision.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A later valid snapshot creates generation one from untouched book storage.
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto recovery_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(recovery_turn);
  REQUIRE(recovery_turn.value().has_value());
  REQUIRE(harness.callback_observations().size() == 4U);
  CHECK(harness.callback_observations()[2U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[2U].readiness == market_data::MarketReadiness::Ready);
  CHECK(harness.callback_observations()[3U].kind == ObservedCallbackKind::Market);
  CHECK(harness.callback_observations()[3U].book_generation ==
        model::BookGeneration::create_initial());
  CHECK(harness.callback_observations()[3U].book_revision == model::BookRevision::create_initial());
  CHECK(harness.callback_observations()[3U].best_bid == parse_price_or_throw("30000.5"));
  CHECK(harness.callback_observations()[3U].best_ask == parse_price_or_throw("30001.0"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence preserves the semantic rejection and only the later valid book identity.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.diagnostics.size() == 1U);
  CHECK(evidence.diagnostics.front().kind == runtime::RuntimeDiagnosticKind::MalformedInput);
  CHECK(evidence.diagnostics.front().fields.detail_code ==
        (0x00020000U | static_cast<std::uint32_t>(model::DomainErrorCode::InvalidMarketEvent)));
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Ready);
  REQUIRE(evidence.sources.front().book_identity.has_value());
  CHECK(evidence.sources.front().book_identity->generation() ==
        model::BookGeneration::create_initial());
  CHECK(evidence.sources.front().book_identity->revision() ==
        model::BookRevision::create_initial());
  CHECK(evidence.sources.front().last_source_sequence == model::SequenceNumber{100U});
  REQUIRE(evidence.trace_records.size() == 9U);
  CHECK(evidence.trace_records[2U].fields().input_disposition ==
        trace::RuntimeInputDisposition::MalformedRejected);
  CHECK(evidence.trace_records[5U].fields().input_disposition ==
        trace::RuntimeInputDisposition::SnapshotApplied);
  CHECK(evidence.trace_records[8U].kind() == trace::RuntimeTraceEventKind::MarketCallback);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Attributable overload is an ordered source-loss fence, not a silently dropped market update.
TEST_CASE("market runtime orders a capacity fence after older accepted work",
          "[runtime][market_runtime][m2][capacity]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // With one pending slot, the snapshot is accepted and the following attributable delta is not.
  MarketRuntimeHarness harness{1U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(harness.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto rejected =
      harness.market_runtime().try_admit(create_ingress_attempt_or_throw(create_delta_frame()));
  REQUIRE(rejected);
  CHECK(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(rejected.value().attempt_ordinal.value() == 3U);
  CHECK_FALSE(rejected.value().receipt.has_value());
  CHECK(rejected.value().discontinuity_recorded);
  const auto queued = harness.market_runtime().status();
  CHECK(queued.executor.pending_commands == 1U);
  CHECK(queued.executor.pending_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Global attempt ordering commits the older snapshot before consuming its later loss fence.
  const auto snapshot_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  CHECK(snapshot_turn.value()->kind == runtime::TurnKind::Command);
  CHECK(snapshot_turn.value()->attempt_ordinal.value() == 2U);
  REQUIRE(harness.callback_observations().size() == 3U);
  CHECK(harness.callback_observations()[1U].readiness == market_data::MarketReadiness::Ready);
  CHECK(harness.callback_observations()[2U].kind == ObservedCallbackKind::Market);

  const auto fence_turn = harness.market_runtime().execute_next_turn();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::SourceDiscontinuity);
  CHECK(fence_turn.value()->attempt_ordinal.value() == 3U);
  REQUIRE(fence_turn.value()->discontinuity.has_value());
  CHECK(fence_turn.value()->discontinuity->source_ordinal ==
        model::MarketSourceOrdinal::create_initial());
  CHECK(fence_turn.value()->discontinuity->earliest_failed_attempt.value() == 3U);
  CHECK(fence_turn.value()->discontinuity->lost_attempt_count == 1U);
  REQUIRE(harness.callback_observations().size() == 4U);
  CHECK(harness.callback_observations()[3U].kind == ObservedCallbackKind::State);
  CHECK(harness.callback_observations()[3U].readiness == market_data::MarketReadiness::Invalid);

  // ++++++++++++++++++++++++++++++++++++++++
  // Final evidence preserves both the last valid book identity and the non-ready loss state.
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Invalid);
  REQUIRE(evidence.sources.front().book_identity.has_value());
  CHECK(evidence.sources.front().book_identity->generation().value() == 1U);
  CHECK(evidence.sources.front().book_identity->revision().value() == 1U);
  REQUIRE(evidence.diagnostics.size() == 1U);
  CHECK(evidence.diagnostics.front().kind == runtime::RuntimeDiagnosticKind::SourceDiscontinuity);
  CHECK(evidence.diagnostics.front().fields.occurrence_count == 1U);
  REQUIRE(evidence.trace_records.size() == 9U);
  CHECK(evidence.trace_records[6U].kind() == trace::RuntimeTraceEventKind::InputDisposition);
  CHECK(evidence.trace_records[6U].fields().input_disposition ==
        trace::RuntimeInputDisposition::SourceDiscontinuity);
  CHECK(evidence.trace_records[7U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[8U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.executor.pending_commands == 0U);
  CHECK(evidence.executor.pending_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Missing or unconfigured attribution remains contained and cannot select configured source state.
TEST_CASE("market runtime contains source-less and unconfigured accepted attempts",
          "[runtime][market_runtime][m2][containment]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Both bounded attempts enter the FIFO without receiving authority over the configured source.
  MarketRuntimeHarness harness{4U};
  require_bound_and_bootstrapped_runtime(harness);
  require_accepted_frame(
      harness.market_runtime(),
      create_ingress_attempt_or_throw("AEGISMD|1|source.deribit-btc-perpetual|session-started|1000",
                                      std::nullopt));
  require_accepted_frame(
      harness.market_runtime(),
      create_ingress_attempt_or_throw("AEGISMD|1|source.unconfigured|session-started|1000",
                                      "source.unconfigured"));
  const auto driven = harness.market_runtime().execute_pending_turns(2U);
  REQUIRE(driven);
  CHECK(driven.value().turns_executed == 2U);
  CHECK(driven.value().pending_commands == 0U);
  CHECK(driven.value().pending_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Neither invalid envelope can publish a callback or mutate the sole configured source.
  REQUIRE(harness.callback_observations().size() == 1U);
  CHECK(harness.callback_observations().front().readiness ==
        market_data::MarketReadiness::Synchronizing);
  const auto evidence = close_and_collect_evidence_or_throw(harness.market_runtime());
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Synchronizing);
  CHECK_FALSE(evidence.sources.front().book_identity.has_value());
  CHECK_FALSE(evidence.sources.front().active_session.has_value());
  CHECK_FALSE(evidence.sources.front().last_source_sequence.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Diagnostics identify unsupported envelopes with fixed codes but do not invent attribution.
  REQUIRE(evidence.diagnostics.size() == 2U);
  for (std::size_t index = 0U; index < evidence.diagnostics.size(); ++index) {
    const auto& diagnostic = evidence.diagnostics[index];
    CHECK(diagnostic.kind == runtime::RuntimeDiagnosticKind::UnsupportedInput);
    CHECK_FALSE(diagnostic.fields.source_ordinal.has_value());
    CHECK(diagnostic.fields.detail_code ==
          (0x00020000U |
           static_cast<std::uint32_t>(model::DomainErrorCode::RuntimeSourceNotConfigured)));
    REQUIRE(diagnostic.fields.admission_ordinal.has_value());
    CHECK(diagnostic.fields.admission_ordinal->value() == index + 2U);
    CHECK(diagnostic.fields.occurrence_count == 1U);
  }
  REQUIRE(evidence.trace_records.size() == 2U);
  CHECK(evidence.trace_records[0U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[1U].kind() == trace::RuntimeTraceEventKind::StateCallback);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A post-commit terminal fault suppresses rather than drains its already accepted command suffix.
TEST_CASE("market runtime exposes quiescent fault evidence with suppressed backlog",
          "[runtime][market_runtime][m2][fault][quiescence]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Bootstrap with a healthy clock, then queue two updates before arming callback regression.
  model::DeterministicClockProvider executor_clock{100U};
  ArmedRegressingClock callback_clock;
  std::vector<CallbackObservation> observations;
  observations.reserve(8U);
  auto runtime = create_runtime_with_strategy_or_throw(
      4U, executor_clock, callback_clock,
      std::make_unique<MarketCallbackCapturingStrategy>(observations));
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->execute_next_turn();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());
  callback_clock.arm_regression_script();
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_delta_frame()));

  // ++++++++++++++++++++++++++++++++++++++++
  // The snapshot commits and finishes its complete fan-out before the clock fault closes progress.
  const auto faulting_turn = runtime->execute_next_turn();
  REQUIRE(faulting_turn);
  REQUIRE(faulting_turn.value().has_value());
  REQUIRE(observations.size() == 3U);
  CHECK(observations[1U].kind == ObservedCallbackKind::State);
  CHECK(observations[2U].kind == ObservedCallbackKind::Market);
  const auto faulted = runtime->status();
  CHECK(faulted.lifecycle == runtime::MarketRuntimeLifecycle::Faulted);
  CHECK(faulted.executor.faulted);
  CHECK(faulted.executor.closed);
  CHECK(faulted.executor.pending_commands == 1U);
  REQUIRE(faulted.fault.has_value());
  CHECK(faulted.fault->code == model::DomainErrorCode::InvalidTimestampOrder);

  // ++++++++++++++++++++++++++++++++++++++++
  // Releasing the failed owner makes the immutable applied prefix and suppressed count collectible.
  REQUIRE(runtime->release_from_current_thread());
  auto evidence_result = runtime->collect_quiescent_evidence();
  REQUIRE(evidence_result);
  const auto evidence = std::move(evidence_result).value();
  CHECK(evidence.executor.faulted);
  CHECK(evidence.executor.closed);
  CHECK_FALSE(evidence.executor.owner_bound);
  CHECK(evidence.executor.pending_commands == 1U);
  REQUIRE(evidence.fault.has_value());
  CHECK(evidence.fault->code == model::DomainErrorCode::InvalidTimestampOrder);
  REQUIRE(evidence.last_dispatch.has_value());
  CHECK(evidence.last_dispatch->callback_count() == 2U);
  CHECK(evidence.last_dispatch->callback_clock_regressions == 2U);
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Ready);
  CHECK(evidence.sources.front().last_source_sequence == model::SequenceNumber{100U});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Exact callback preflight lets a zero-event duplicate consume its sole disposition slot, then
// reports the next callback-bearing turn as runtime evidence exhaustion before mutation.
TEST_CASE("market runtime uses exact fanout at the canonical trace boundary",
          "[runtime][market_runtime][m2][trace][preflight]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Eight records cover bootstrap plus one recovery snapshot and leave two canonical slots.
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  std::vector<CallbackObservation> observations;
  observations.reserve(8U);
  auto runtime = create_runtime_with_strategy_or_throw(
      4U, executor_clock, callback_clock,
      std::make_unique<MarketCallbackCapturingStrategy>(observations), 8U);
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_snapshot_frame()));
  REQUIRE(runtime->execute_next_turn());
  REQUIRE(observations.size() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The exact duplicate needs only one input record and zero callback-counter headroom.
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_delta_frame()));
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_delta_frame()));
  const auto duplicate = runtime->execute_next_turn();
  REQUIRE(duplicate);
  REQUIRE(duplicate.value().has_value());
  CHECK(observations.size() == 3U);
  const auto after_duplicate = runtime->status();
  REQUIRE(after_duplicate.last_dispatch.has_value());
  CHECK(after_duplicate.last_dispatch->callback_count() == 0U);
  CHECK(after_duplicate.executor.pending_commands == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // One remaining slot cannot cover callback plus reserved reentry; the book stays at sequence 100.
  const auto exhausted = runtime->execute_next_turn();
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::RuntimeEvidenceExhausted);
  const auto faulted = runtime->status();
  CHECK(faulted.lifecycle == runtime::MarketRuntimeLifecycle::Faulted);
  CHECK(faulted.executor.pending_commands == 1U);
  REQUIRE(faulted.fault.has_value());
  CHECK(faulted.fault->code == model::DomainErrorCode::RuntimeEvidenceExhausted);
  CHECK(observations.size() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence preserves the exact accepted trace prefix and suppressed work count.
  REQUIRE(runtime->release_from_current_thread());
  auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().fault.has_value());
  CHECK(evidence.value().fault->code == model::DomainErrorCode::RuntimeEvidenceExhausted);
  CHECK(evidence.value().trace_records.size() == 7U);
  CHECK(evidence.value().trace_records.back().fields().input_disposition ==
        trace::RuntimeInputDisposition::ExactDuplicateIgnored);
  CHECK(evidence.value().executor.pending_commands == 1U);
  REQUIRE(evidence.value().sources.size() == 1U);
  CHECK(evidence.value().sources.front().readiness == market_data::MarketReadiness::Ready);
  CHECK(evidence.value().sources.front().last_source_sequence == model::SequenceNumber{100U});
  REQUIRE(evidence.value().sources.front().book_identity.has_value());
  CHECK(evidence.value().sources.front().book_identity->revision().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Callback budget observation remains non-preemptive and visible through live and cold reports.
TEST_CASE("market runtime publishes completed callback budget overruns",
          "[runtime][market_runtime][m2][callback][budget]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Bootstrap under a stable clock, then arm deterministic over-budget callback measurements.
  model::DeterministicClockProvider executor_clock{100U};
  ArmedBudgetClock callback_clock;
  std::vector<CallbackObservation> observations;
  observations.reserve(8U);
  auto runtime = create_runtime_with_strategy_or_throw(
      4U, executor_clock, callback_clock,
      std::make_unique<MarketCallbackCapturingStrategy>(observations));
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->execute_next_turn();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());
  callback_clock.arm_budget_script();
  require_accepted_frame(*runtime, create_ingress_attempt_or_throw(create_snapshot_frame()));
  const auto snapshot = runtime->execute_next_turn();
  REQUIRE(snapshot);
  REQUIRE(snapshot.value().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Both completed callbacks are reported after return without turning observation into a fault.
  const auto running = runtime->status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.bots.is_healthy());
  CHECK_FALSE(running.fault.has_value());
  REQUIRE(running.last_dispatch.has_value());
  CHECK(running.last_dispatch->callback_count() == 2U);
  CHECK(running.last_dispatch->callback_budget_exceeded == 2U);
  CHECK(running.last_dispatch->callback_clock_regressions == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence preserves the same metric and both bounded diagnostic observations.
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().last_dispatch.has_value());
  CHECK(evidence.value().last_dispatch == running.last_dispatch);
  CHECK_FALSE(evidence.value().fault.has_value());
  REQUIRE(evidence.value().diagnostics.size() == 2U);
  CHECK(evidence.value().diagnostics[0U].kind ==
        runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded);
  CHECK(evidence.value().diagnostics[1U].kind ==
        runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A dedicated callback may request graceful closure without waiting for its own owner-thread exit.
TEST_CASE("market runtime dedicated callback close returns before owner shutdown",
          "[runtime][market_runtime][m2][dedicated][close]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the stable runtime handle and arm the genuine bootstrap callback before owner startup.
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  OwnerCloseControl control;
  auto runtime = create_runtime_with_strategy_or_throw(
      4U, executor_clock, callback_clock, std::make_unique<OwnerClosingStrategy>(control));
  control.runtime.store(runtime.get(), std::memory_order_release);
  control.armed.store(true, std::memory_order_release);
  REQUIRE(runtime->start_dedicated());

  // ++++++++++++++++++++++++++++++++++++++++
  // The callback-local call returns, then the shared driver exits after completing that owner turn.
  const auto stopped = wait_until_owner_close_stops(*runtime, control);
  if (!stopped) {
    // A deliberately leaked stuck owner bounds this regression failure instead of deadlocking the
    // combined unit-test executable during stack unwinding.
    static_cast<void>(runtime.release());
  }
  REQUIRE(stopped);
  CHECK(control.entered.load(std::memory_order_acquire));
  CHECK(control.handle_observed.load(std::memory_order_acquire));
  CHECK(control.returned.load(std::memory_order_acquire));
  const auto closed = runtime->status();
  CHECK(closed.lifecycle == runtime::MarketRuntimeLifecycle::Closed);
  CHECK(closed.dedicated_driver_started);
  CHECK_FALSE(closed.dedicated_driver_running);
  CHECK_FALSE(closed.executor.owner_bound);

  // ++++++++++++++++++++++++++++++++++++++++
  // With no accepted suffix, the completed bootstrap is immediately available as cold evidence.
  auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  CHECK(evidence.value().executor.pending_commands == 0U);
  CHECK(evidence.value().executor.pending_fences == 0U);
  REQUIRE(evidence.value().sources.size() == 1U);
  CHECK(evidence.value().sources.front().readiness == market_data::MarketReadiness::Synchronizing);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Recursive owner progression is rejected before drive-bound validation or nested mutation.
TEST_CASE("market runtime records over-bound callback drive as owner reentry",
          "[runtime][market_runtime][m2][reentry]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the final stable handle before manually executing the genuine bootstrap callback.
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  OwnerReentrantExecutionControl control;
  auto runtime = create_runtime_with_strategy_or_throw(
      4U, executor_clock, callback_clock,
      std::make_unique<OwnerReentrantExecutionStrategy>(control));
  control.runtime.store(runtime.get(), std::memory_order_release);
  control.armed.store(true, std::memory_order_release);
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->execute_next_turn();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Active-turn ownership wins over maximum_drive_turns and publishes one coalesced observation.
  CHECK(control.returned.load(std::memory_order_acquire));
  CHECK(control.error_code.load(std::memory_order_acquire) ==
        static_cast<std::uint32_t>(model::DomainErrorCode::ExecutorReentryDetected));
  const auto running = runtime->status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.bots.is_healthy());
  CHECK(running.executor.completed_turns == 1U);
  CHECK(running.executor.pending_commands == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical and noncanonical evidence each retain the single callback-local recursion attempt.
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().trace_records.size() == 3U);
  CHECK(evidence.value().trace_records.back().kind() ==
        trace::RuntimeTraceEventKind::ReentryDetected);
  REQUIRE(evidence.value().diagnostics.size() == 1U);
  CHECK(evidence.value().diagnostics.front().kind ==
        runtime::RuntimeDiagnosticKind::OwnerReentryDetected);
  CHECK(evidence.value().diagnostics.front().fields.occurrence_count == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Dedicated ownership must drain the same accepted prefix to byte-identical quiescent evidence.
TEST_CASE("market runtime dedicated driver matches deterministic replay at quiescence",
          "[runtime][market_runtime][m2][dedicated][replay]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Produce the reference callback and evidence sequence through explicit deterministic turns.
  MarketRuntimeHarness deterministic{4U};
  require_bound_and_bootstrapped_runtime(deterministic);
  require_accepted_frame(deterministic.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(deterministic.market_runtime(),
                         create_ingress_attempt_or_throw(create_delta_frame()));
  const auto deterministic_drive = deterministic.market_runtime().execute_pending_turns(2U);
  REQUIRE(deterministic_drive);
  CHECK(deterministic_drive.value().turns_executed == 2U);
  const auto deterministic_evidence =
      close_and_collect_evidence_or_throw(deterministic.market_runtime());

  // ++++++++++++++++++++++++++++++++++++++++
  // Let a dedicated owner consume the genuine bootstrap before accepting the same frame prefix.
  MarketRuntimeHarness dedicated{4U};
  REQUIRE(dedicated.market_runtime().start_dedicated());
  REQUIRE(wait_until_running(dedicated.market_runtime()));
  const auto running = dedicated.market_runtime().status();
  CHECK(running.dedicated_driver_started);
  CHECK(running.dedicated_driver_running);
  CHECK(running.executor.owner_bound);
  require_accepted_frame(dedicated.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(dedicated.market_runtime(),
                         create_ingress_attempt_or_throw(create_delta_frame()));

  // ++++++++++++++++++++++++++++++++++++++++
  // Close drains both accepted frames before releasing ownership and publishing cold evidence.
  dedicated.market_runtime().close_and_wait();
  const auto closed = dedicated.market_runtime().status();
  CHECK(closed.lifecycle == runtime::MarketRuntimeLifecycle::Closed);
  CHECK(closed.dedicated_driver_started);
  CHECK_FALSE(closed.dedicated_driver_running);
  CHECK_FALSE(closed.executor.owner_bound);
  CHECK(closed.executor.pending_commands == 0U);
  CHECK(closed.executor.pending_fences == 0U);
  auto dedicated_result = dedicated.market_runtime().collect_quiescent_evidence();
  REQUIRE(dedicated_result);
  const auto dedicated_evidence = std::move(dedicated_result).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Shared turn processing makes callbacks and every replay-evidence field driver-independent.
  CHECK(dedicated.callback_observations() == deterministic.callback_observations());
  CHECK(dedicated_evidence == deterministic_evidence);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Identical sealed inputs and scripted clocks reproduce callbacks, status, and evidence exactly.
TEST_CASE("market runtime manual replay is exactly deterministic at quiescence",
          "[runtime][market_runtime][m2][replay]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Feed two independent runtimes the same accepted bootstrap, snapshot, and delta prefix.
  MarketRuntimeHarness first{4U};
  MarketRuntimeHarness second{4U};
  require_bound_and_bootstrapped_runtime(first);
  require_bound_and_bootstrapped_runtime(second);
  require_accepted_frame(first.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(second.market_runtime(),
                         create_ingress_attempt_or_throw(create_snapshot_frame()));
  require_accepted_frame(first.market_runtime(),
                         create_ingress_attempt_or_throw(create_delta_frame()));
  require_accepted_frame(second.market_runtime(),
                         create_ingress_attempt_or_throw(create_delta_frame()));
  const auto first_drive = first.market_runtime().execute_pending_turns(2U);
  const auto second_drive = second.market_runtime().execute_pending_turns(2U);
  REQUIRE(first_drive);
  REQUIRE(second_drive);
  CHECK(first_drive.value() == second_drive.value());
  CHECK(first.market_runtime().status() == second.market_runtime().status());
  CHECK(first.callback_observations() == second.callback_observations());

  // ++++++++++++++++++++++++++++++++++++++++
  // Released final copies match across every canonical and bounded external evidence component.
  const auto first_evidence = close_and_collect_evidence_or_throw(first.market_runtime());
  const auto second_evidence = close_and_collect_evidence_or_throw(second.market_runtime());
  CHECK(first_evidence.configuration_fingerprint == second_evidence.configuration_fingerprint);
  CHECK(first_evidence.runtime_policy_fingerprint == second_evidence.runtime_policy_fingerprint);
  CHECK(first_evidence.trace_records == second_evidence.trace_records);
  CHECK(first_evidence.canonical_trace_bytes == second_evidence.canonical_trace_bytes);
  CHECK(first_evidence.canonical_trace_digest == second_evidence.canonical_trace_digest);
  CHECK(first_evidence.diagnostics == second_evidence.diagnostics);
  CHECK(first_evidence.dropped_diagnostics == second_evidence.dropped_diagnostics);
  CHECK(first_evidence.sources == second_evidence.sources);
  CHECK(first_evidence.executor == second_evidence.executor);
  CHECK(first_evidence.last_completed_turn == second_evidence.last_completed_turn);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The named fake-only composition must return synchronously in the bootstrap owner turn and expose
// complete provenance-rich cold evidence without any extra executor command.
TEST_CASE("market runtime composes direct fake submission and complete quiescent evidence",
          "[runtime][market_runtime][submission][m3]") {
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  SubmissionCapture capture;
  auto runtime = create_m3_runtime_or_throw(executor_clock, callback_clock, capture);

  // ++++++++++++++++++++++++++++++++++++++++
  // One genuine bootstrap turn enters the callback, completes submission, and returns the result
  // without publishing another executor command or handoff.
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->execute_next_turn();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());
  CHECK(bootstrap.value()->turn_ordinal == model::TurnOrdinal::create_initial());
  CHECK(capture.callbacks == 1U);
  CHECK(capture.submit_returned);
  CHECK(capture.disposition == execution::SubmitDisposition::WriteInitiated);
  CHECK(capture.stage == execution::SubmissionStage::Initiation);
  CHECK(capture.reason == execution::SubmissionReason::None);
  REQUIRE(capture.attempt_id.has_value());
  REQUIRE(capture.order_id.has_value());
  const auto after_submit = runtime->status();
  CHECK(after_submit.executor.completed_turns == 1U);
  CHECK(after_submit.executor.pending_commands == 0U);
  CHECK(after_submit.executor.pending_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Closure copies both canonical streams and every retained fake-submission subsystem state.
  const auto evidence = close_and_collect_evidence_or_throw(*runtime);
  REQUIRE(evidence.submission.has_value());
  const auto& submission = *evidence.submission;
  CHECK_FALSE(submission.runtime_faulted);
  CHECK_FALSE(submission.terminal_error.has_value());
  CHECK(submission.encoder_invocations_consumed == 1U);
  CHECK(submission.initiator_invocations_consumed == 1U);
  CHECK(submission.oms_order_count == 1U);
  REQUIRE(submission.oms_orders.size() == 1U);
  CHECK(submission.oms_orders.front().state == oms::OutboundOrderState::WriteInitiated);
  CHECK(submission.oms_orders.front().admission.attempt_id == *capture.attempt_id);
  CHECK(submission.oms_orders.front().admission.order_id == *capture.order_id);
  CHECK(submission.held_reservation_count == 1U);
  REQUIRE(submission.held_reservations.size() == 1U);
  CHECK(submission.held_reservations.front().state == risk::ReservationState::Held);
  CHECK(submission.held_reservations.front().reservation_id.value() == capture.attempt_id->value());
  REQUIRE(submission.scope_exposures.size() == 7U);
  for (const auto& scope : submission.scope_exposures) {
    CHECK(scope.exposure.open_order_count == 1U);
    CHECK(scope.exposure.gross_reserved_quote_notional == parse_notional_or_throw("20"));
  }
  REQUIRE(submission.accepted_writes.size() == 1U);
  CHECK(submission.accepted_writes.front().attempt_id == *capture.attempt_id);
  CHECK_FALSE(submission.accepted_writes.front().bytes.empty());

  // ++++++++++++++++++++++++++++++++++++++++
  // The OMS projection proves all attribution came from BotContext, not the caller-authored order.
  const auto& provenance = submission.oms_orders.front().admission.provenance;
  CHECK(provenance.firm_id.value() == capture.firm_id);
  CHECK(provenance.desk_id.value() == capture.desk_id);
  CHECK(provenance.bot_id.value() == capture.bot_id);
  CHECK(provenance.strategy_id.value() == capture.strategy_id);
  CHECK(provenance.risk_policy_fingerprint == submission.risk_policy_fingerprint.bytes());
  CHECK(provenance.risk_policy_revision == submission.risk_policy_revision);
  CHECK(provenance.submission_policy_fingerprint ==
        submission.submission_policy_fingerprint.bytes());

  // ++++++++++++++++++++++++++++++++++++++++
  // The exact success sequence names local WriteInitiated and never invents acknowledgement state.
  constexpr std::array expected_events{
      trace::SubmissionTraceEventKind::Attempt,
      trace::SubmissionTraceEventKind::RouteAuthorized,
      trace::SubmissionTraceEventKind::CanonicalValidated,
      trace::SubmissionTraceEventKind::IdentityGenerated,
      trace::SubmissionTraceEventKind::RiskReserved,
      trace::SubmissionTraceEventKind::OmsAdmitted,
      trace::SubmissionTraceEventKind::Encoded,
      trace::SubmissionTraceEventKind::WriteInitiated,
      trace::SubmissionTraceEventKind::SubmissionCompleted,
  };
  REQUIRE(submission.trace_records.size() == expected_events.size());
  for (std::size_t index = 0U; index < expected_events.size(); ++index) {
    CHECK(submission.trace_records[index].kind() == expected_events[index]);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Observation-only construction remains valid and rejects submit before consuming any M3 identity
// or creating an optional submission evidence bundle.
TEST_CASE("market runtime observation-only factory installs no submission authority",
          "[runtime][market_runtime][submission][observation][m3]") {
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  SubmissionCapture capture;
  auto configuration = create_reference_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration, 4U);
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      std::make_unique<SubmittingStrategy>(create_m3_order_request_or_throw(), capture),
  });
  auto created = runtime::MarketRuntime::create_market_runtime(
      std::move(configuration), std::move(policy), executor_clock, callback_clock,
      std::move(strategies));
  REQUIRE(created);
  auto runtime = std::move(created).value();

  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->execute_next_turn());
  CHECK(capture.submit_returned);
  CHECK(capture.disposition == execution::SubmitDisposition::LocallyRejected);
  CHECK(capture.stage == execution::SubmissionStage::Context);
  CHECK(capture.reason == execution::SubmissionReason::SubmissionCapabilityUnavailable);
  CHECK_FALSE(capture.attempt_id.has_value());
  CHECK_FALSE(capture.order_id.has_value());
  const auto evidence = close_and_collect_evidence_or_throw(*runtime);
  CHECK_FALSE(evidence.submission.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A submission-capable context reads only the dedicated thread-safe clock before rejecting inactive
// or non-owner callers; neither gate can consume an attempt or touch owner-local evidence.
TEST_CASE("submission-capable BotContext gates retained and wrong-thread callers before mutation",
          "[runtime][market_runtime][bot_context][submission][owner][m3]") {
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  SubmissionGateControl control;
  auto configuration = create_m3_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration, 4U);
  auto measurement = std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
      std::vector<std::optional<std::uint64_t>>{10U, 20U});
  auto* const measurement_access = measurement.get();
  auto submission_params = create_m3_submission_params_or_throw(
      configuration, execution::FakeInitiationOutcome::AcceptedAndInitiated,
      std::move(measurement));
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      std::make_unique<SubmissionGateStrategy>(control),
  });
  auto created = runtime::MarketRuntime::create_with_fake_submission(
      std::move(configuration), std::move(policy), executor_clock, callback_clock,
      std::move(strategies), std::move(submission_params));
  REQUIRE(created);
  auto runtime = std::move(created).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Dedicated bootstrap publishes an active context while retaining all owner-local submission
  // counters and evidence at their initial values.
  REQUIRE(runtime->start_dedicated());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!control.callback_active.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto callback_active = control.callback_active.load(std::memory_order_acquire);
  if (!callback_active) {
    control.release_callback.store(true, std::memory_order_release);
  }
  REQUIRE(callback_active);
  auto* const context = control.context.load(std::memory_order_acquire);
  if (context == nullptr) {
    control.release_callback.store(true, std::memory_order_release);
  }
  REQUIRE(context != nullptr);

  const auto wrong_owner = context->submit_order(create_m3_order_request_or_throw());
  control.release_callback.store(true, std::memory_order_release);
  runtime->close_and_wait();
  const auto inactive = context->submit_order(create_m3_order_request_or_throw());

  CHECK(wrong_owner.disposition() == execution::SubmitDisposition::LocallyRejected);
  CHECK(wrong_owner.stage() == execution::SubmissionStage::Context);
  CHECK(wrong_owner.reason() == execution::SubmissionReason::WrongOwner);
  CHECK_FALSE(wrong_owner.attempt_id());
  CHECK_FALSE(wrong_owner.order_id());
  CHECK_FALSE(wrong_owner.local_path_nanoseconds());

  // ++++++++++++++++++++++++++++++++++++++++
  // After callback release and dedicated-owner shutdown, the same persistent object has no active
  // callback authority and must reject before all submission state exactly as before.
  CHECK(inactive.disposition() == execution::SubmitDisposition::LocallyRejected);
  CHECK(inactive.stage() == execution::SubmissionStage::Context);
  CHECK(inactive.reason() == execution::SubmissionReason::ContextInactive);
  CHECK_FALSE(inactive.attempt_id());
  CHECK_FALSE(inactive.order_id());
  CHECK_FALSE(inactive.local_path_nanoseconds());
  CHECK(measurement_access->readings_consumed() == 2U);

  auto evidence = runtime->collect_quiescent_evidence();
  REQUIRE(evidence);
  REQUIRE(evidence.value().submission);
  const auto& submission = *evidence.value().submission;
  CHECK(submission.trace_records.empty());
  CHECK(submission.diagnostics.empty());
  CHECK(submission.oms_order_count == 0U);
  CHECK(submission.held_reservation_count == 0U);
  CHECK(submission.accepted_writes.empty());
  CHECK(submission.encoder_invocations_consumed == 0U);
  CHECK(submission.initiator_invocations_consumed == 0U);
  CHECK_FALSE(submission.runtime_faulted);
  CHECK_FALSE(submission.terminal_error);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Manual and dedicated owners must produce identical callback results and complete M2/M3 evidence
// from the same configuration, order namespace, fake script, clocks, and bootstrap input.
TEST_CASE("market runtime fake submission is identical across manual and dedicated owners",
          "[runtime][market_runtime][submission][dedicated][replay][m3]") {
  model::DeterministicClockProvider manual_executor_clock{100U};
  model::DeterministicClockProvider manual_callback_clock{1'000U};
  SubmissionCapture manual_capture;
  auto manual =
      create_m3_runtime_or_throw(manual_executor_clock, manual_callback_clock, manual_capture);
  REQUIRE(manual->bind_to_current_thread());
  REQUIRE(manual->execute_next_turn());
  const auto manual_evidence = close_and_collect_evidence_or_throw(*manual);

  // ++++++++++++++++++++++++++++++++++++++++
  // Replay the same fixture under a dedicated owner and collect its cold quiescent evidence.
  model::DeterministicClockProvider dedicated_executor_clock{100U};
  model::DeterministicClockProvider dedicated_callback_clock{1'000U};
  SubmissionCapture dedicated_capture;
  auto dedicated = create_m3_runtime_or_throw(dedicated_executor_clock, dedicated_callback_clock,
                                              dedicated_capture);
  REQUIRE(dedicated->start_dedicated());
  REQUIRE(wait_until_running(*dedicated));
  dedicated->close_and_wait();
  auto dedicated_result = dedicated->collect_quiescent_evidence();
  REQUIRE(dedicated_result);
  const auto dedicated_evidence = std::move(dedicated_result).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Compare both strategy-visible results and the complete canonical evidence bundle.
  CHECK(dedicated_capture == manual_capture);
  CHECK(dedicated_evidence == manual_evidence);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A named submission composition with an incomplete immutable risk snapshot must fail before any
// runtime, callback capability, executor, reservation, or fake slot can become reachable.
TEST_CASE("market runtime fake submission fails closed for incomplete policy",
          "[runtime][market_runtime][submission][policy][m3]") {
  model::DeterministicClockProvider executor_clock{100U};
  model::DeterministicClockProvider callback_clock{1'000U};
  SubmissionCapture capture;
  auto configuration = create_m3_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration, 4U);
  auto submission_params = create_m3_submission_params_or_throw(configuration);
  submission_params.risk_policy.limit_sets.pop_back();
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
      std::make_unique<SubmittingStrategy>(create_m3_order_request_or_throw(), capture),
  });

  const auto created = runtime::MarketRuntime::create_with_fake_submission(
      std::move(configuration), std::move(policy), executor_clock, callback_clock,
      std::move(strategies), std::move(submission_params));
  REQUIRE_FALSE(created);
  CHECK(created.error().code == model::DomainErrorCode::InvalidRiskPolicy);
  CHECK_FALSE(capture.submit_returned);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
