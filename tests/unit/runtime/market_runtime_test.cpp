// Purpose: prove the M2 coordinator composes bounded admission, transactional books, canonical
// subscription callbacks, containment, and quiescent replay evidence through its public API.

#include "aegis/runtime/market_runtime.hpp"
#include "reference_configuration.hpp"

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
// CapturingStrategy records only bounded immutable values from each synchronous callback.
class CapturingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow a construction-time-reserved observation vector that outlives this strategy.
  explicit CapturingStrategy(std::vector<CallbackObservation>& observations) noexcept
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
// OwnerDriveControl publishes the result of one deliberately recursive over-bound drive request.
struct OwnerDriveControl {
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
    close_once();
  }

  // --------------------------------------------------------
  // The bootstrap state callback is the deterministic self-close trigger in the regression test.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    close_once();
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Consume the arm once, close through the stable external handle, and prove the call returned.
  void close_once() noexcept {
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
// OwnerDrivingStrategy proves active-owner reentry takes precedence over an invalid drive bound.
class OwnerDrivingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow control storage whose stable runtime handle is published after factory return.
  explicit OwnerDrivingStrategy(OwnerDriveControl& control) noexcept : control_{&control} {}

  // --------------------------------------------------------
  // A market callback is an equivalent reentry boundary if it is the first armed callback.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {
    drive_once();
  }

  // --------------------------------------------------------
  // Bootstrap supplies the deterministic callback used by the composed reentry test.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    drive_once();
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Request one more turn than policy allows and retain only the stable assigned error code.
  void drive_once() noexcept {
    if (!control_->armed.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    auto* const runtime = control_->runtime.load(std::memory_order_acquire);
    if (runtime != nullptr) {
      const auto driven = runtime->drive(33U);
      if (!driven) {
        control_->error_code.store(static_cast<std::uint32_t>(driven.error().code),
                                   std::memory_order_release);
      }
    }
    control_->returned.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
  OwnerDriveControl* control_;
};

// ########################################################################

// ########################################################################
// ArmedRegressingClock remains stable for bootstrap, then deterministically makes every later
// callback finish before it starts to trigger the production post-commit fault path.
class ArmedRegressingClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Publish the regression script only after bootstrap has completed successfully.
  void arm() noexcept {
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
  void arm() noexcept {
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

// --------------------------------------------------------
// Invalid identifier literals are fixture-authoring defects rather than coordinator behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact prices without binary floating-point fixture drift.
[[nodiscard]] model::Price price(std::string_view text) {
  auto parsed = model::Price::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid price in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact quantities without binary floating-point fixture drift.
[[nodiscard]] model::Quantity quantity(std::string_view text) {
  auto parsed = model::Quantity::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in market-runtime test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal one reference configuration before policy and runtime construction borrow its provenance.
[[nodiscard]] configuration::StartupConfiguration reference_configuration() {
  auto created =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  if (!created) {
    throw std::logic_error{"invalid startup configuration in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Define the sole credential-free public source used by the deterministic reference scenario.
[[nodiscard]] runtime::RuntimeSourceDefinition reference_source() {
  return runtime::RuntimeSourceDefinition{
      id<model::MarketSourceId>("source.deribit-btc-perpetual"),
      id<model::VenueId>("deribit"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::initial(),
  };
}

// --------------------------------------------------------
// Keep unit-test bounds small while leaving enough callback and trace headroom for every turn.
[[nodiscard]] runtime::RuntimePolicy
reference_policy(const configuration::StartupConfiguration& configuration,
                 std::uint32_t ingress_capacity, std::uint32_t trace_capacity = 256U) {
  auto created = runtime::RuntimePolicy::create(
      configuration, runtime::RuntimePolicyParams{
                         runtime::RuntimePolicyLimits{ingress_capacity, 4096U, 64U, 20U, 1'000U, 2U,
                                                      64U, trace_capacity, 32U, 100'000U},
                         {reference_source()},
                     });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Own one caller attempt while leaving its optional attribution untrusted until runtime admission.
[[nodiscard]] market_data::IngressFrameAttempt
attempt(std::string frame,
        std::optional<std::string_view> source_id = "source.deribit-btc-perpetual") {
  std::optional<model::MarketSourceId> typed_source;
  if (source_id.has_value()) {
    typed_source = id<model::MarketSourceId>(source_id.value());
  }
  auto created = market_data::IngressFrameAttempt::create(
      std::move(typed_source), model::SessionEpoch{1U}, std::move(frame));
  if (!created) {
    throw std::logic_error{"invalid ingress attempt in market-runtime test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Return the complete valid snapshot used to establish generation one.
[[nodiscard]] std::string snapshot_frame() {
  return "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:book100|4|"
         "B,30000.5,2|B,30000.0,4|A,30001.0,3|A,30001.5,5";
}

// --------------------------------------------------------
// Return a complete delta whose deletions and replacements expose a partial-apply bug immediately.
[[nodiscard]] std::string delta_frame() {
  return "AEGISMD|1|source.deribit-btc-perpetual|delta|101|100|1100|1|ok:book101|4|"
         "B,30000.5,0|B,30000.0,6|A,30001.0,0|A,30002.0,8";
}

// ########################################################################
// One harness keeps observations and borrowed clocks alive until after the runtime is destroyed.
class RuntimeHarness final {
public:

  // --------------------------------------------------------
  // Create one manual-driver runtime with a pre-reserved, single reference strategy.
  explicit RuntimeHarness(std::uint32_t ingress_capacity)
      : executor_clock_{100U}, callback_measurement_clock_{1'000U} {
    observations_.reserve(32U);
    auto configuration = reference_configuration();
    auto policy = reference_policy(configuration, ingress_capacity);
    std::vector<runtime::BotStrategyRegistration> strategies;
    strategies.push_back(runtime::BotStrategyRegistration{
        id<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<CapturingStrategy>(observations_),
    });
    auto created =
        runtime::MarketRuntime::create(std::move(configuration), std::move(policy), executor_clock_,
                                       callback_measurement_clock_, std::move(strategies));
    if (!created) {
      throw std::logic_error{"invalid market runtime in test fixture"};
    }
    runtime_ = std::move(created).value();
  }

  // --------------------------------------------------------
  [[nodiscard]] runtime::MarketRuntime& runtime() noexcept { return *runtime_; }

  // --------------------------------------------------------
  [[nodiscard]] const std::vector<CallbackObservation>& observations() const noexcept {
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
[[nodiscard]] std::unique_ptr<runtime::MarketRuntime>
runtime_with_strategy(std::uint32_t ingress_capacity, model::ClockProvider& executor_clock,
                      model::ClockProvider& callback_measurement_clock,
                      std::unique_ptr<runtime::Strategy> strategy,
                      std::uint32_t trace_capacity = 256U) {
  auto configuration = reference_configuration();
  auto policy = reference_policy(configuration, ingress_capacity, trace_capacity);
  std::vector<runtime::BotStrategyRegistration> strategies;
  strategies.push_back(runtime::BotStrategyRegistration{
      id<model::BotId>("bot.deribit-btc-perpetual-reference"), std::move(strategy)});
  auto created =
      runtime::MarketRuntime::create(std::move(configuration), std::move(policy), executor_clock,
                                     callback_measurement_clock, std::move(strategies));
  if (!created) {
    throw std::logic_error{"invalid controlled market runtime in test fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Run the genuine queued bootstrap as the first manually owned turn.
void bind_and_bootstrap(RuntimeHarness& harness) {
  REQUIRE(harness.runtime().bind_to_current_thread());
  auto turn = harness.runtime().run_one();
  REQUIRE(turn);
  REQUIRE(turn.value().has_value());
  CHECK(turn.value()->kind == runtime::TurnKind::Command);
  CHECK(turn.value()->turn_ordinal == model::TurnOrdinal::initial());
  CHECK(turn.value()->attempt_ordinal == model::AdmissionOrdinal::initial());
}

// --------------------------------------------------------
// Close, release deterministic ownership, and copy final evidence without Catch control flow.
[[nodiscard]] runtime::MarketRuntimeEvidence close_and_collect(runtime::MarketRuntime& runtime) {
  runtime.close();
  const auto released = runtime.release_from_current_thread();
  if (!released) {
    throw std::logic_error{"failed to release market-runtime owner in test fixture"};
  }
  auto evidence = runtime.quiescent_evidence();
  if (!evidence) {
    throw std::logic_error{"failed to collect quiescent market-runtime evidence"};
  }
  return std::move(evidence).value();
}

// --------------------------------------------------------
// Admit one frame and require ordinary bounded acceptance before its owner turn runs.
void require_accepted(runtime::MarketRuntime& runtime,
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
  RuntimeHarness harness{4U};
  const auto starting = harness.runtime().status();
  CHECK(starting.lifecycle == runtime::MarketRuntimeLifecycle::Starting);
  CHECK(starting.initialized_sources == 0U);
  CHECK(starting.executor.pending_commands == 1U);
  CHECK(starting.executor.pending_fences == 0U);
  CHECK_FALSE(starting.executor.owner_bound);
  CHECK_FALSE(starting.dedicated_driver_started);
  CHECK_FALSE(starting.dedicated_driver_running);

  // ++++++++++++++++++++++++++++++++++++++++
  // The first owner turn publishes Synchronizing to the one canonical reference subscription.
  bind_and_bootstrap(harness);
  const auto running = harness.runtime().status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.initialized_sources == 1U);
  CHECK(running.executor.pending_commands == 0U);
  CHECK(running.executor.owner_bound);
  REQUIRE(running.last_completed_turn.has_value());
  REQUIRE(harness.observations().size() == 1U);
  const auto& synchronizing = harness.observations().front();
  CHECK(synchronizing.kind == ObservedCallbackKind::State);
  CHECK(synchronizing.reference_route);
  CHECK(synchronizing.callback_ordinal == model::CallbackOrdinal::initial());
  CHECK(synchronizing.readiness == market_data::MarketReadiness::Synchronizing);
  CHECK_FALSE(synchronizing.book_generation.has_value());
  CHECK_FALSE(synchronizing.book_revision.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Closed and released evidence owns the final source summary and drained executor state.
  const auto evidence = close_and_collect(harness.runtime());
  CHECK(harness.runtime().status().lifecycle == runtime::MarketRuntimeLifecycle::Closed);
  CHECK_FALSE(evidence.canonical_trace_bytes.empty());
  REQUIRE(evidence.trace_records.size() == 2U);
  CHECK(evidence.trace_records[0U].kind() == trace::RuntimeTraceEventKind::MarketStateTransition);
  CHECK(evidence.trace_records[1U].kind() == trace::RuntimeTraceEventKind::StateCallback);
  CHECK(evidence.diagnostics.empty());
  CHECK(evidence.dropped_diagnostics == 0U);
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().source_ordinal == model::MarketSourceOrdinal::initial());
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
  RuntimeHarness harness{4U};
  bind_and_bootstrap(harness);
  require_accepted(harness.runtime(), attempt(snapshot_frame()));
  const auto snapshot_turn = harness.runtime().run_one();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  REQUIRE(harness.observations().size() == 3U);
  CHECK(harness.observations()[1U].kind == ObservedCallbackKind::State);
  CHECK(harness.observations()[1U].readiness == market_data::MarketReadiness::Ready);
  const auto& snapshot = harness.observations()[2U];
  CHECK(snapshot.kind == ObservedCallbackKind::Market);
  CHECK(snapshot.reference_route);
  CHECK(snapshot.callback_ordinal.value() == 3U);
  CHECK(snapshot.book_generation == model::BookGeneration::initial());
  CHECK(snapshot.book_revision == model::BookRevision::initial());
  CHECK(snapshot.best_bid == price("30000.5"));
  CHECK(snapshot.best_ask == price("30001.0"));
  CHECK(snapshot.bid_count == 2U);
  CHECK(snapshot.ask_count == 2U);
  CHECK((snapshot.bids[0U] == market_data::BookLevel{price("30000.5"), quantity("2")}));
  CHECK((snapshot.bids[1U] == market_data::BookLevel{price("30000.0"), quantity("4")}));
  CHECK((snapshot.asks[0U] == market_data::BookLevel{price("30001.0"), quantity("3")}));
  CHECK((snapshot.asks[1U] == market_data::BookLevel{price("30001.5"), quantity("5")}));

  // ++++++++++++++++++++++++++++++++++++++++
  // One delta callback sees every deletion and replacement together, never an intermediate book.
  require_accepted(harness.runtime(), attempt(delta_frame()));
  const auto delta_turn = harness.runtime().run_one();
  REQUIRE(delta_turn);
  REQUIRE(delta_turn.value().has_value());
  REQUIRE(harness.observations().size() == 4U);
  const auto& delta = harness.observations()[3U];
  CHECK(delta.kind == ObservedCallbackKind::Market);
  CHECK(delta.reference_route);
  CHECK(delta.callback_ordinal.value() == 4U);
  CHECK(delta.book_generation == model::BookGeneration::initial());
  REQUIRE(delta.book_revision.has_value());
  CHECK(delta.book_revision->value() == 2U);
  CHECK(delta.best_bid == price("30000.0"));
  CHECK(delta.best_ask == price("30001.5"));
  CHECK(delta.bid_count == 1U);
  CHECK(delta.ask_count == 2U);
  CHECK((delta.bids[0U] == market_data::BookLevel{price("30000.0"), quantity("6")}));
  CHECK((delta.asks[0U] == market_data::BookLevel{price("30001.5"), quantity("5")}));
  CHECK((delta.asks[1U] == market_data::BookLevel{price("30002.0"), quantity("8")}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Quiescent state retains the last complete commit and exact stream anchor.
  const auto evidence = close_and_collect(harness.runtime());
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
  RuntimeHarness harness{4U};
  bind_and_bootstrap(harness);
  require_accepted(harness.runtime(), attempt(snapshot_frame()));
  const auto snapshot_turn = harness.runtime().run_one();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  const auto resynchronization = harness.runtime().try_resynchronize(
      id<model::MarketSourceId>("source.deribit-btc-perpetual"));
  REQUIRE(resynchronization);
  CHECK(resynchronization.value().outcome == runtime::AdmissionOutcome::Accepted);
  REQUIRE(resynchronization.value().receipt.has_value());
  CHECK_FALSE(resynchronization.value().discontinuity_recorded);

  // ++++++++++++++++++++++++++++++++++++++++
  // The control turn publishes only Synchronizing while retaining identity counters internally.
  const auto control_turn = harness.runtime().run_one();
  REQUIRE(control_turn);
  REQUIRE(control_turn.value().has_value());
  CHECK(control_turn.value()->kind == runtime::TurnKind::Command);
  REQUIRE(harness.observations().size() == 4U);
  const auto& synchronizing = harness.observations().back();
  CHECK(synchronizing.kind == ObservedCallbackKind::State);
  CHECK(synchronizing.readiness == market_data::MarketReadiness::Synchronizing);
  CHECK(synchronizing.book_generation == model::BookGeneration::initial());
  CHECK(synchronizing.book_revision == model::BookRevision::initial());

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence exposes cleared continuity and the retained identity without a market callback.
  const auto evidence = close_and_collect(harness.runtime());
  REQUIRE(evidence.sources.size() == 1U);
  CHECK(evidence.sources.front().readiness == market_data::MarketReadiness::Synchronizing);
  REQUIRE(evidence.sources.front().book_identity.has_value());
  CHECK(evidence.sources.front().book_identity->generation() == model::BookGeneration::initial());
  CHECK(evidence.sources.front().book_identity->revision() == model::BookRevision::initial());
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
  RuntimeHarness harness{4U};
  bind_and_bootstrap(harness);
  require_accepted(
      harness.runtime(),
      attempt("AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:bad|1|"
              "B,30000.5,broken"));
  const auto malformed_turn = harness.runtime().run_one();
  REQUIRE(malformed_turn);
  REQUIRE(malformed_turn.value().has_value());
  REQUIRE(harness.observations().size() == 2U);
  CHECK(harness.observations()[1U].kind == ObservedCallbackKind::State);
  CHECK(harness.observations()[1U].readiness == market_data::MarketReadiness::Invalid);
  CHECK_FALSE(harness.observations()[1U].best_bid.has_value());
  CHECK_FALSE(harness.observations()[1U].best_ask.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // The next valid snapshot starts the first generation from clean temporary parser state.
  require_accepted(harness.runtime(), attempt(snapshot_frame()));
  const auto recovery_turn = harness.runtime().run_one();
  REQUIRE(recovery_turn);
  REQUIRE(recovery_turn.value().has_value());
  REQUIRE(harness.observations().size() == 4U);
  CHECK(harness.observations()[2U].kind == ObservedCallbackKind::State);
  CHECK(harness.observations()[2U].readiness == market_data::MarketReadiness::Ready);
  CHECK(harness.observations()[3U].kind == ObservedCallbackKind::Market);
  CHECK(harness.observations()[3U].best_bid == price("30000.5"));
  CHECK(harness.observations()[3U].best_ask == price("30001.0"));

  // ++++++++++++++++++++++++++++++++++++++++
  // The bounded diagnostic exposes only assigned parse metadata, while state reflects recovery.
  const auto evidence = close_and_collect(harness.runtime());
  REQUIRE(evidence.diagnostics.size() == 1U);
  const auto& diagnostic = evidence.diagnostics.front();
  CHECK(diagnostic.kind == runtime::RuntimeDiagnosticKind::MalformedInput);
  CHECK(diagnostic.fields.source_ordinal == model::MarketSourceOrdinal::initial());
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
// Attributable overload is an ordered source-loss fence, not a silently dropped market update.
TEST_CASE("market runtime orders a capacity fence after older accepted work",
          "[runtime][market_runtime][m2][capacity]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // With one pending slot, the snapshot is accepted and the following attributable delta is not.
  RuntimeHarness harness{1U};
  bind_and_bootstrap(harness);
  require_accepted(harness.runtime(), attempt(snapshot_frame()));
  const auto rejected = harness.runtime().try_admit(attempt(delta_frame()));
  REQUIRE(rejected);
  CHECK(rejected.value().outcome == runtime::AdmissionOutcome::CapacityExceeded);
  CHECK(rejected.value().attempt_ordinal.value() == 3U);
  CHECK_FALSE(rejected.value().receipt.has_value());
  CHECK(rejected.value().discontinuity_recorded);
  const auto queued = harness.runtime().status();
  CHECK(queued.executor.pending_commands == 1U);
  CHECK(queued.executor.pending_fences == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Global attempt ordering commits the older snapshot before consuming its later loss fence.
  const auto snapshot_turn = harness.runtime().run_one();
  REQUIRE(snapshot_turn);
  REQUIRE(snapshot_turn.value().has_value());
  CHECK(snapshot_turn.value()->kind == runtime::TurnKind::Command);
  CHECK(snapshot_turn.value()->attempt_ordinal.value() == 2U);
  REQUIRE(harness.observations().size() == 3U);
  CHECK(harness.observations()[1U].readiness == market_data::MarketReadiness::Ready);
  CHECK(harness.observations()[2U].kind == ObservedCallbackKind::Market);

  const auto fence_turn = harness.runtime().run_one();
  REQUIRE(fence_turn);
  REQUIRE(fence_turn.value().has_value());
  CHECK(fence_turn.value()->kind == runtime::TurnKind::SourceDiscontinuity);
  CHECK(fence_turn.value()->attempt_ordinal.value() == 3U);
  REQUIRE(fence_turn.value()->discontinuity.has_value());
  CHECK(fence_turn.value()->discontinuity->source_ordinal == model::MarketSourceOrdinal::initial());
  CHECK(fence_turn.value()->discontinuity->earliest_failed_attempt.value() == 3U);
  CHECK(fence_turn.value()->discontinuity->lost_attempt_count == 1U);
  REQUIRE(harness.observations().size() == 4U);
  CHECK(harness.observations()[3U].kind == ObservedCallbackKind::State);
  CHECK(harness.observations()[3U].readiness == market_data::MarketReadiness::Invalid);

  // ++++++++++++++++++++++++++++++++++++++++
  // Final evidence preserves both the last valid book identity and the non-ready loss state.
  const auto evidence = close_and_collect(harness.runtime());
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
  RuntimeHarness harness{4U};
  bind_and_bootstrap(harness);
  require_accepted(
      harness.runtime(),
      attempt("AEGISMD|1|source.deribit-btc-perpetual|session-started|1000", std::nullopt));
  require_accepted(harness.runtime(), attempt("AEGISMD|1|source.unconfigured|session-started|1000",
                                              "source.unconfigured"));
  const auto driven = harness.runtime().drive(2U);
  REQUIRE(driven);
  CHECK(driven.value().turns_executed == 2U);
  CHECK(driven.value().pending_commands == 0U);
  CHECK(driven.value().pending_fences == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Neither invalid envelope can publish a callback or mutate the sole configured source.
  REQUIRE(harness.observations().size() == 1U);
  CHECK(harness.observations().front().readiness == market_data::MarketReadiness::Synchronizing);
  const auto evidence = close_and_collect(harness.runtime());
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
  auto runtime = runtime_with_strategy(4U, executor_clock, callback_clock,
                                       std::make_unique<CapturingStrategy>(observations));
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->run_one();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());
  callback_clock.arm();
  require_accepted(*runtime, attempt(snapshot_frame()));
  require_accepted(*runtime, attempt(delta_frame()));

  // ++++++++++++++++++++++++++++++++++++++++
  // The snapshot commits and finishes its complete fan-out before the clock fault closes progress.
  const auto faulting_turn = runtime->run_one();
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
  auto evidence_result = runtime->quiescent_evidence();
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
  auto runtime = runtime_with_strategy(4U, executor_clock, callback_clock,
                                       std::make_unique<CapturingStrategy>(observations), 8U);
  REQUIRE(runtime->bind_to_current_thread());
  REQUIRE(runtime->run_one());
  require_accepted(*runtime, attempt(snapshot_frame()));
  REQUIRE(runtime->run_one());
  REQUIRE(observations.size() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
  // The exact duplicate needs only one input record and zero callback-counter headroom.
  require_accepted(*runtime, attempt(snapshot_frame()));
  require_accepted(*runtime, attempt(delta_frame()));
  require_accepted(*runtime, attempt(delta_frame()));
  const auto duplicate = runtime->run_one();
  REQUIRE(duplicate);
  REQUIRE(duplicate.value().has_value());
  CHECK(observations.size() == 3U);
  const auto after_duplicate = runtime->status();
  REQUIRE(after_duplicate.last_dispatch.has_value());
  CHECK(after_duplicate.last_dispatch->callback_count() == 0U);
  CHECK(after_duplicate.executor.pending_commands == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // One remaining slot cannot cover callback plus reserved reentry; the book stays at sequence 100.
  const auto exhausted = runtime->run_one();
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
  auto evidence = runtime->quiescent_evidence();
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
  auto runtime = runtime_with_strategy(4U, executor_clock, callback_clock,
                                       std::make_unique<CapturingStrategy>(observations));
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->run_one();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());
  callback_clock.arm();
  require_accepted(*runtime, attempt(snapshot_frame()));
  const auto snapshot = runtime->run_one();
  REQUIRE(snapshot);
  REQUIRE(snapshot.value().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Both completed callbacks are reported after return without turning observation into a fault.
  const auto running = runtime->status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.bots.healthy());
  CHECK_FALSE(running.fault.has_value());
  REQUIRE(running.last_dispatch.has_value());
  CHECK(running.last_dispatch->callback_count() == 2U);
  CHECK(running.last_dispatch->callback_budget_exceeded == 2U);
  CHECK(running.last_dispatch->callback_clock_regressions == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold evidence preserves the same metric and both bounded diagnostic observations.
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  auto evidence = runtime->quiescent_evidence();
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
  auto runtime = runtime_with_strategy(4U, executor_clock, callback_clock,
                                       std::make_unique<OwnerClosingStrategy>(control));
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
  auto evidence = runtime->quiescent_evidence();
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
  OwnerDriveControl control;
  auto runtime = runtime_with_strategy(4U, executor_clock, callback_clock,
                                       std::make_unique<OwnerDrivingStrategy>(control));
  control.runtime.store(runtime.get(), std::memory_order_release);
  control.armed.store(true, std::memory_order_release);
  REQUIRE(runtime->bind_to_current_thread());
  const auto bootstrap = runtime->run_one();
  REQUIRE(bootstrap);
  REQUIRE(bootstrap.value().has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Active-turn ownership wins over maximum_drive_turns and publishes one coalesced observation.
  CHECK(control.returned.load(std::memory_order_acquire));
  CHECK(control.error_code.load(std::memory_order_acquire) ==
        static_cast<std::uint32_t>(model::DomainErrorCode::ExecutorReentryDetected));
  const auto running = runtime->status();
  CHECK(running.lifecycle == runtime::MarketRuntimeLifecycle::Running);
  CHECK(running.bots.healthy());
  CHECK(running.executor.completed_turns == 1U);
  CHECK(running.executor.pending_commands == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical and noncanonical evidence each retain the single callback-local recursion attempt.
  runtime->close();
  REQUIRE(runtime->release_from_current_thread());
  auto evidence = runtime->quiescent_evidence();
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
  RuntimeHarness deterministic{4U};
  bind_and_bootstrap(deterministic);
  require_accepted(deterministic.runtime(), attempt(snapshot_frame()));
  require_accepted(deterministic.runtime(), attempt(delta_frame()));
  const auto deterministic_drive = deterministic.runtime().drive(2U);
  REQUIRE(deterministic_drive);
  CHECK(deterministic_drive.value().turns_executed == 2U);
  const auto deterministic_evidence = close_and_collect(deterministic.runtime());

  // ++++++++++++++++++++++++++++++++++++++++
  // Let a dedicated owner consume the genuine bootstrap before accepting the same frame prefix.
  RuntimeHarness dedicated{4U};
  REQUIRE(dedicated.runtime().start_dedicated());
  REQUIRE(wait_until_running(dedicated.runtime()));
  const auto running = dedicated.runtime().status();
  CHECK(running.dedicated_driver_started);
  CHECK(running.dedicated_driver_running);
  CHECK(running.executor.owner_bound);
  require_accepted(dedicated.runtime(), attempt(snapshot_frame()));
  require_accepted(dedicated.runtime(), attempt(delta_frame()));

  // ++++++++++++++++++++++++++++++++++++++++
  // Close drains both accepted frames before releasing ownership and publishing cold evidence.
  dedicated.runtime().close_and_wait();
  const auto closed = dedicated.runtime().status();
  CHECK(closed.lifecycle == runtime::MarketRuntimeLifecycle::Closed);
  CHECK(closed.dedicated_driver_started);
  CHECK_FALSE(closed.dedicated_driver_running);
  CHECK_FALSE(closed.executor.owner_bound);
  CHECK(closed.executor.pending_commands == 0U);
  CHECK(closed.executor.pending_fences == 0U);
  auto dedicated_result = dedicated.runtime().quiescent_evidence();
  REQUIRE(dedicated_result);
  const auto dedicated_evidence = std::move(dedicated_result).value();

  // ++++++++++++++++++++++++++++++++++++++++
  // Shared turn processing makes callbacks and every replay-evidence field driver-independent.
  CHECK(dedicated.observations() == deterministic.observations());
  CHECK(dedicated_evidence == deterministic_evidence);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Identical sealed inputs and scripted clocks reproduce callbacks, status, and evidence exactly.
TEST_CASE("market runtime manual replay is exactly deterministic at quiescence",
          "[runtime][market_runtime][m2][replay]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Feed two independent runtimes the same accepted bootstrap, snapshot, and delta prefix.
  RuntimeHarness first{4U};
  RuntimeHarness second{4U};
  bind_and_bootstrap(first);
  bind_and_bootstrap(second);
  require_accepted(first.runtime(), attempt(snapshot_frame()));
  require_accepted(second.runtime(), attempt(snapshot_frame()));
  require_accepted(first.runtime(), attempt(delta_frame()));
  require_accepted(second.runtime(), attempt(delta_frame()));
  const auto first_drive = first.runtime().drive(2U);
  const auto second_drive = second.runtime().drive(2U);
  REQUIRE(first_drive);
  REQUIRE(second_drive);
  CHECK(first_drive.value() == second_drive.value());
  CHECK(first.runtime().status() == second.runtime().status());
  CHECK(first.observations() == second.observations());

  // ++++++++++++++++++++++++++++++++++++++++
  // Released final copies match across every canonical and bounded external evidence component.
  const auto first_evidence = close_and_collect(first.runtime());
  const auto second_evidence = close_and_collect(second.runtime());
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

} // namespace
