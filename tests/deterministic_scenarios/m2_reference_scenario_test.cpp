// Purpose: prove the complete credential-free M2 market-state script reproduces callbacks and
// canonical runtime evidence across deterministic and dedicated serialized owners.

#include "aegis/market_data/order_book.hpp"
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
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// One copied callback context retains every public attribution and provenance field without
// extending the lifetime of the turn-scoped BotContext.
struct ObservedBotContext {
  model::FirmId firm_id;
  model::DeskId desk_id;
  model::BotId bot_id;
  model::StrategyId strategy_id;
  model::SubscriptionId subscription_id;
  model::CallbackOrdinal callback_ordinal;
  configuration::ConfigurationFingerprint configuration_fingerprint;
  runtime::RuntimePolicyFingerprint runtime_policy_fingerprint;

  // --------------------------------------------------------
  // Structural equality makes the entire public callback attribution replay-comparable.
  friend bool operator==(const ObservedBotContext&, const ObservedBotContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A copied state callback retains the complete sanitized transition and no hidden book alias.
struct ObservedStateCallback {
  ObservedBotContext context;
  market_data::MarketStateEventFields event;

  // --------------------------------------------------------
  // Structural equality compares the full state callback contract.
  friend bool operator==(const ObservedStateCallback&, const ObservedStateCallback&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A copied market callback owns the complete normalized event, commit identity, and coherent
// best-to-worst book levels visible during that synchronous callback.
struct ObservedMarketCallback {
  ObservedBotContext context;
  market_data::NormalizedMarketUpdate update;
  market_data::MarketCommitContext commit;
  std::vector<market_data::BookLevel> bids;
  std::vector<market_data::BookLevel> asks;

  // --------------------------------------------------------
  // Structural equality compares every public market event, context, and book-view value.
  friend bool operator==(const ObservedMarketCallback&, const ObservedMarketCallback&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The closed observation union preserves callback kind without a lossy projection.
using CallbackObservation = std::variant<ObservedStateCallback, ObservedMarketCallback>;

// ########################################################################

// ########################################################################
// A callback gate holds one dedicated turn after its observation is copied so the producer can
// deterministically fill the sole pending slot and exercise attributable capacity loss.
class CallbackGate final {
public:

  // --------------------------------------------------------
  // Arm exactly one later callback and clear the prior handshake state.
  void arm_next_callback() noexcept {
    released_.store(false, std::memory_order_release);
    entered_.store(false, std::memory_order_release);
    armed_.store(true, std::memory_order_release);
  }

  // --------------------------------------------------------
  // Block only the armed callback after publishing that all earlier owner work has completed.
  void wait_if_armed() noexcept {
    if (!armed_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    entered_.store(true, std::memory_order_release);
    while (!released_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  // --------------------------------------------------------
  // Report callback entry to the producer through the release/acquire synchronization edge.
  [[nodiscard]] bool has_callback_entered() const noexcept {
    return entered_.load(std::memory_order_acquire);
  }

  // --------------------------------------------------------
  // Let the held owner callback finish after the producer has recorded its capacity fence.
  void release_waiting_callback() noexcept { released_.store(true, std::memory_order_release); }

  // --------------------------------------------------------
private:
  std::atomic_bool armed_{false};
  std::atomic_bool entered_{false};
  std::atomic_bool released_{false};
};

// ########################################################################

// --------------------------------------------------------
// Copy all public callback-context fields while their borrowed configuration remains valid.
[[nodiscard]] ObservedBotContext copy_callback_context(const runtime::BotContext& context) {
  return ObservedBotContext{context.firm_id(),
                            context.desk_id(),
                            context.bot_id(),
                            context.strategy_id(),
                            context.subscription_id(),
                            context.callback_ordinal(),
                            context.configuration_fingerprint(),
                            context.runtime_policy_fingerprint()};
}

// --------------------------------------------------------

// ########################################################################
// The reference strategy copies complete callbacks and optionally supplies the capacity-test
// handshake without retaining event, view, context, or runtime-owned storage.
class CallbackCapturingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow pre-reserved observation and gate storage whose lifetime encloses the runtime.
  CallbackCapturingStrategy(std::vector<CallbackObservation>& observations,
                            CallbackGate& gate) noexcept
      : observations_{&observations}, gate_{&gate} {}

  // --------------------------------------------------------
  // Copy the normalized event and every coherent book level before the callback returns.
  void on_market_data(const market_data::MarketEvent& event, const market_data::ReadyBookView& book,
                      runtime::BotContext& context) noexcept override {
    ObservedMarketCallback observation{
        copy_callback_context(context), event.update(), event.context(), {}, {}};
    observation.bids.reserve(book.bid_count());
    observation.asks.reserve(book.ask_count());
    for (std::size_t index = 0U; index < book.bid_count(); ++index) {
      const auto level = book.bid_at(index);
      if (level) {
        observation.bids.push_back(*level);
      }
    }
    for (std::size_t index = 0U; index < book.ask_count(); ++index) {
      const auto level = book.ask_at(index);
      if (level) {
        observation.asks.push_back(*level);
      }
    }
    observations_->emplace_back(std::move(observation));
    gate_->wait_if_armed();
  }

  // --------------------------------------------------------
  // Copy the complete sanitized transition before any later owner turn can change source state.
  void on_market_state(const market_data::MarketStateEvent& event,
                       runtime::BotContext& context) noexcept override {
    observations_->emplace_back(
        ObservedStateCallback{copy_callback_context(context), event.fields()});
    gate_->wait_if_armed();
  }

  // --------------------------------------------------------
private:
  std::vector<CallbackObservation>* observations_;
  CallbackGate* gate_;
};

// ########################################################################

// ########################################################################
// An ungranted strategy counts any accidental callback without depending on callback contents.
class UnrelatedBotStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow one atomic counter so dedicated-owner observations remain race-free.
  explicit UnrelatedBotStrategy(std::atomic_uint32_t& callback_count) noexcept
      : callback_count_{&callback_count} {}

  // --------------------------------------------------------
  // Any market callback would prove subscription isolation failed.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {
    callback_count_->fetch_add(1U, std::memory_order_relaxed);
  }

  // --------------------------------------------------------
  // Any state callback would likewise prove dispatch escaped the configured grant.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    callback_count_->fetch_add(1U, std::memory_order_relaxed);
  }

  // --------------------------------------------------------
private:
  std::atomic_uint32_t* callback_count_;
};

// ########################################################################

// ########################################################################
// One immutable script entry binds credential-free bytes to its external session and deterministic
// owner-clock time.
struct ScriptedFrame {
  std::uint64_t clock_nanoseconds;
  std::uint64_t session_epoch;
  std::string_view bytes;
};

// ########################################################################

// The accepted prefix before the capacity exercise covers every state-validity branch in causal
// order. Duplicate entries deliberately reuse byte-identical fixture payloads.
constexpr std::array<ScriptedFrame, 22U> scripted_prefix{{
    {1'100U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:book100|4|"
     "B,50000,2|B,49999.5,4|A,50000.5,3|A,50001,5"},
    {1'200U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|101|100|1100|1|ok:book101|4|"
     "B,50000,0|B,49999.5,6|A,50000.5,0|A,50001.5,8"},
    {1'300U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|101|100|1100|1|ok:book101|4|"
     "B,50000,0|B,49999.5,6|A,50000.5,0|A,50001.5,8"},
    {1'400U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:book100|4|"
     "B,50000,2|B,49999.5,4|A,50000.5,3|A,50001,5"},
    {1'500U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|103|102|1500|1|ok:gap103|1|"
     "B,49999.5,7"},
    {1'600U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|102|101|1600|1|ok:nonready102|1|"
     "B,49999.5,8"},
    {1'700U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|104|none|1700|1|ok:book104|2|"
     "B,50010,7|A,50010.5,8"},
    {1'800U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|104|none|1800|1|ok:conflict104|2|"
     "B,50011,7|A,50011.5,8"},
    {1'900U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|105|none|1900|1|ok:book105|2|"
     "B,50020,9|A,50020.5,10"},
    {2'000U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|106|105|2000|1|bad:book106|1|"
     "B,50020,11"},
    {2'100U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|107|none|2100|1|ok:book107|2|"
     "B,50030,11|A,50030.5,12"},
    {2'200U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|108|107|2200|2|ok:book108|1|"
     "B,50030,13"},
    {2'300U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|109|none|2300|1|ok:book109|2|"
     "B,50040,13|A,50040.5,14"},
    {2'400U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|110|109|2400|1|ok:malformed110|1|"
     "B,50040,broken"},
    {2'500U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|110|none|2500|1|ok:book110|2|"
     "B,50050,15|A,50050.5,16"},
    {3'499U, 1U, "AEGISMD|1|source.deribit-btc-perpetual|staleness-check|3499"},
    {3'500U, 1U, "AEGISMD|1|source.deribit-btc-perpetual|staleness-check|3500"},
    {3'600U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|111|110|3600|1|ok:stale111|1|"
     "B,50050,17"},
    {3'700U, 1U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|112|none|3700|1|ok:book112|2|"
     "B,50060,19|A,50060.5,20"},
    {3'800U, 2U, "AEGISMD|1|source.deribit-btc-perpetual|session-started|3800"},
    {3'900U, 2U,
     "AEGISMD|1|source.deribit-btc-perpetual|delta|1|none|3900|1|ok:session2delta1|1|"
     "B,50060,21"},
    {4'000U, 2U,
     "AEGISMD|1|source.deribit-btc-perpetual|snapshot|2|none|4000|1|ok:session2book2|2|"
     "B,50070,22|A,50070.5,23"},
}};

// The duplicate fills the one-slot queue, the later attributable delta becomes a source-loss
// fence, and the final snapshot proves recovery from that ordered Invalid transition.
constexpr ScriptedFrame capacity_duplicate{4'100U, 2U, scripted_prefix.back().bytes};
constexpr ScriptedFrame capacity_rejected{
    4'100U, 2U, "AEGISMD|1|source.deribit-btc-perpetual|delta|3|2|4100|1|ok:lost3|1|B,50070,24"};
constexpr ScriptedFrame final_recovery{
    4'200U, 2U,
    "AEGISMD|1|source.deribit-btc-perpetual|snapshot|4|none|4200|1|ok:session2book4|2|"
    "B,50080,25|A,50080.5,26"};

// ########################################################################

// ########################################################################
// Replay mode selects only ownership mechanics; all fixture bytes, clocks, and assertions remain
// shared.
enum class M2MarketReplayMode : std::uint8_t {
  Manual = 1,
  Dedicated = 2,
};

// ########################################################################

// --------------------------------------------------------
// Invalid identifier literals are fixture-authoring defects rather than runtime behavior.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M2 reference scenario"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse one exact price without binary floating-point fixture drift.
[[nodiscard]] model::Price parse_price_or_throw(std::string_view text) {
  auto parsed = model::Price::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid price in M2 reference scenario"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse one exact quantity without binary floating-point fixture drift.
[[nodiscard]] model::Quantity parse_quantity_or_throw(std::string_view text) {
  auto parsed = model::Quantity::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in M2 reference scenario"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Extend the single-firm reference with one bot that owns no subscription grant.
[[nodiscard]] configuration::StartupConfiguration create_reference_configuration_or_throw() {
  auto params = test_support::create_reference_configuration_params_or_throw();
  const auto unrelated_bot = parse_identifier_or_throw<model::BotId>("bot.unrelated-reference");
  const auto unrelated_strategy =
      parse_identifier_or_throw<model::StrategyId>("strategy.unrelated-reference");
  params.bots.push_back(organization::BotRegistration{
      unrelated_bot, parse_identifier_or_throw<model::DeskId>("desk.digital-assets"),
      unrelated_strategy});
  params.strategy_settings.push_back(configuration::BotStrategySettings{
      unrelated_bot, unrelated_strategy, configuration::StrategyMode::ObserveOnly});
  auto created =
      configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!created) {
    throw std::logic_error{"invalid startup configuration in M2 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Bind one credential-free source and all scenario bounds to the sealed startup snapshot.
[[nodiscard]] runtime::RuntimePolicy
create_reference_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{1U, 4096U, 64U, 20U, 1'000U, 2U, 64U, 128U, 32U, 100'000U},
          {runtime::RuntimeSourceDefinition{
              parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
              parse_identifier_or_throw<model::VenueId>("deribit"),
              parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
              parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
              model::InstrumentMetadataRevision::create_initial()}},
      });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M2 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Own one bounded ingress attempt while leaving attribution untrusted until policy resolution.
[[nodiscard]] market_data::IngressFrameAttempt
create_ingress_attempt_or_throw(const ScriptedFrame& frame) {
  auto created = market_data::IngressFrameAttempt::create_ingress_frame_attempt(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
      model::SessionEpoch{frame.session_epoch}, std::string{frame.bytes});
  if (!created) {
    throw std::logic_error{"invalid ingress attempt in M2 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Bound every dedicated handshake so a broken owner cannot hang the deterministic test suite.
template <typename Predicate>
void wait_until_condition_or_throw(Predicate predicate, std::string_view failure) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return;
    }
    std::this_thread::yield();
  }
  throw std::logic_error{std::string{failure}};
}

// --------------------------------------------------------

// ########################################################################
// The harness owns clocks, callback storage, strategies, and runtime in an order that keeps every
// borrowed capability alive until after owner shutdown.
class M2MarketReplayHarness final {
public:

  // --------------------------------------------------------
  // Construct the same one-slot runtime for either ownership driver.
  explicit M2MarketReplayHarness(M2MarketReplayMode mode)
      : mode_{mode}, executor_clock_{1'000U}, callback_clock_{100'000U} {
    observations_.reserve(32U);
    auto configuration = create_reference_configuration_or_throw();
    auto policy = create_reference_policy_or_throw(configuration);
    std::vector<runtime::BotStrategyRegistration> registrations;
    registrations.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<CallbackCapturingStrategy>(observations_, gate_)});
    registrations.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.unrelated-reference"),
        std::make_unique<UnrelatedBotStrategy>(unrelated_callback_count_)});
    auto created = runtime::MarketRuntime::create_market_runtime(
        std::move(configuration), std::move(policy), executor_clock_, callback_clock_,
        std::move(registrations));
    if (!created) {
      throw std::logic_error{"invalid composed runtime in M2 reference scenario"};
    }
    runtime_ = std::move(created).value();
  }

  // --------------------------------------------------------
  // Bootstrap through the selected owner and wait for the same first completed turn.
  void start_runtime_or_throw() {
    if (mode_ == M2MarketReplayMode::Manual) {
      if (!runtime_->bind_to_current_thread()) {
        throw std::logic_error{"failed to bind manual M2 owner"};
      }
      execute_one_manual_turn_or_throw();
      return;
    }
    if (!runtime_->start_dedicated()) {
      throw std::logic_error{"failed to start dedicated M2 owner"};
    }
    wait_until_condition_or_throw(
        [this] {
          const auto status = runtime_->status();
          return status.lifecycle == runtime::MarketRuntimeLifecycle::Running &&
                 status.executor.completed_turns == 1U && status.executor.pending_commands == 0U;
        },
        "dedicated M2 owner did not bootstrap");
  }

  // --------------------------------------------------------
  // Replay one ordinary frame only after the previous turn is complete, then require its owner
  // turn.
  void replay_frame_or_throw(const ScriptedFrame& frame, std::uint64_t expected_completed_turns) {
    advance_clock_to_or_throw(frame.clock_nanoseconds);
    require_accepted_frame(frame);
    if (mode_ == M2MarketReplayMode::Manual) {
      execute_one_manual_turn_or_throw();
      return;
    }
    wait_for_completed_turn_count_or_throw(expected_completed_turns);
  }

  // --------------------------------------------------------
  // Hold the dedicated F22 callback after its clock read; manual ownership has no concurrent seam.
  void begin_capacity_turn_or_throw(const ScriptedFrame& frame) {
    advance_clock_to_or_throw(frame.clock_nanoseconds);
    if (mode_ == M2MarketReplayMode::Dedicated) {
      gate_.arm_next_callback();
    }
    auto admitted = runtime_->try_admit(create_ingress_attempt_or_throw(frame));
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted ||
        !admitted.value().receipt.has_value() || admitted.value().discontinuity_recorded) {
      gate_.release_waiting_callback();
      throw std::logic_error{"M2 capacity-anchor frame was not accepted"};
    }
    if (mode_ == M2MarketReplayMode::Manual) {
      execute_one_manual_turn_or_throw();
      return;
    }
    wait_until_condition_or_throw([this] { return gate_.has_callback_entered(); },
                                  "dedicated M2 callback did not enter capacity gate");
  }

  // --------------------------------------------------------
  // Fill the sole slot, reject the next attributable attempt, and drain command then loss fence.
  void exercise_capacity_fence_or_throw() {
    advance_clock_to_or_throw(capacity_duplicate.clock_nanoseconds);
    auto duplicate = runtime_->try_admit(create_ingress_attempt_or_throw(capacity_duplicate));
    auto rejected = runtime_->try_admit(create_ingress_attempt_or_throw(capacity_rejected));
    if (mode_ == M2MarketReplayMode::Dedicated) {
      gate_.release_waiting_callback();
    }
    if (!duplicate || duplicate.value().outcome != runtime::AdmissionOutcome::Accepted ||
        !duplicate.value().receipt.has_value() || duplicate.value().discontinuity_recorded ||
        !rejected || rejected.value().outcome != runtime::AdmissionOutcome::CapacityExceeded ||
        rejected.value().receipt.has_value() || !rejected.value().discontinuity_recorded) {
      throw std::logic_error{"capacity attempt did not produce an attributable M2 fence"};
    }
    if (mode_ == M2MarketReplayMode::Manual) {
      execute_one_manual_turn_or_throw();
      execute_one_manual_turn_or_throw();
      return;
    }
    wait_for_completed_turn_count_or_throw(25U);
  }

  // --------------------------------------------------------
  // Close and release the selected owner before copying immutable replay evidence.
  [[nodiscard]] runtime::MarketRuntimeEvidence finish_replay_or_throw() {
    if (mode_ == M2MarketReplayMode::Manual) {
      runtime_->close();
      if (!runtime_->release_from_current_thread()) {
        throw std::logic_error{"failed to release manual M2 owner"};
      }
    } else {
      runtime_->close_and_wait();
    }
    auto evidence = runtime_->collect_quiescent_evidence();
    if (!evidence) {
      throw std::logic_error{"failed to collect quiescent M2 evidence"};
    }
    return std::move(evidence).value();
  }

  // --------------------------------------------------------
  // Borrow the complete copied callback sequence after callback-local authority has expired.
  [[nodiscard]] const std::vector<CallbackObservation>& callback_observations() const noexcept {
    return observations_;
  }

  // --------------------------------------------------------
  // Return the number of callbacks that escaped the reference bot's configured grant.
  [[nodiscard]] std::uint32_t unrelated_callback_count() const noexcept {
    return unrelated_callback_count_.load(std::memory_order_relaxed);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Advance the shared executor clock monotonically to the next scripted timestamp.
  void advance_clock_to_or_throw(std::uint64_t target) {
    if (target < current_clock_) {
      throw std::logic_error{"M2 reference clock regressed"};
    }
    const auto advanced = executor_clock_.advance_nanoseconds(target - current_clock_);
    if (!advanced) {
      throw std::logic_error{"M2 reference clock exhausted"};
    }
    current_clock_ = target;
  }

  // --------------------------------------------------------
  // Require ordinary acceptance and the one-slot receipt shape used by the replay script.
  void require_accepted_frame(const ScriptedFrame& frame) {
    auto admitted = runtime_->try_admit(create_ingress_attempt_or_throw(frame));
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted ||
        !admitted.value().receipt.has_value() || admitted.value().discontinuity_recorded) {
      throw std::logic_error{"M2 reference frame was not accepted"};
    }
  }

  // --------------------------------------------------------
  // Execute exactly one available manual turn and throw on an unexpected empty or failed result.
  void execute_one_manual_turn_or_throw() {
    auto turn = runtime_->execute_next_turn();
    if (!turn || !turn.value().has_value()) {
      throw std::logic_error{"manual M2 owner did not complete one turn"};
    }
  }

  // --------------------------------------------------------
  // Observe public synchronized status until a dedicated turn and its queue have fully completed.
  void wait_for_completed_turn_count_or_throw(std::uint64_t expected_completed_turns) {
    wait_until_condition_or_throw(
        [this, expected_completed_turns] {
          const auto status = runtime_->status();
          if (status.lifecycle == runtime::MarketRuntimeLifecycle::Faulted) {
            throw std::logic_error{"dedicated M2 owner faulted"};
          }
          return status.executor.completed_turns == expected_completed_turns &&
                 status.executor.pending_commands == 0U && status.executor.pending_fences == 0U &&
                 !status.executor.turn_active;
        },
        "dedicated M2 owner did not complete the scripted turn");
  }

  // --------------------------------------------------------
  M2MarketReplayMode mode_;
  std::vector<CallbackObservation> observations_;
  CallbackGate gate_;
  std::atomic_uint32_t unrelated_callback_count_{0U};
  model::DeterministicClockProvider executor_clock_;
  model::DeterministicClockProvider callback_clock_;
  std::uint64_t current_clock_{1'000U};
  std::unique_ptr<runtime::MarketRuntime> runtime_;
};

// ########################################################################

// ########################################################################
// One replay result owns the complete callback vector and cold runtime evidence after owner
// release.
struct M2MarketReplayResult {
  std::vector<CallbackObservation> callbacks;
  runtime::MarketRuntimeEvidence evidence;
  std::uint32_t unrelated_callback_count;

  // --------------------------------------------------------
  // Structural equality compares both strategy-visible and canonical replay products.
  friend bool operator==(const M2MarketReplayResult&, const M2MarketReplayResult&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Execute the exact accepted/rejected workload through only the public MarketRuntime boundary.
[[nodiscard]] M2MarketReplayResult execute_replay_or_throw(M2MarketReplayMode mode) {
  M2MarketReplayHarness harness{mode};
  harness.start_runtime_or_throw();

  // ++++++++++++++++++++++++++++++++++++++++
  // Run through the pre-capacity table, reserving F22 for the dedicated producer handshake.
  for (std::size_t index = 0U; index + 1U < scripted_prefix.size(); ++index) {
    harness.replay_frame_or_throw(scripted_prefix[index], index + 2U);
  }
  harness.begin_capacity_turn_or_throw(scripted_prefix.back());

  // ++++++++++++++++++++++++++++++++++++++++
  // Turn 23 commits F22 while turns 24 and 25 consume the duplicate and ordered loss fence.
  harness.exercise_capacity_fence_or_throw();
  harness.replay_frame_or_throw(final_recovery, 26U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy callbacks before harness destruction only after closure has made the vector immutable.
  auto evidence = harness.finish_replay_or_throw();
  return M2MarketReplayResult{harness.callback_observations(), std::move(evidence),
                              harness.unrelated_callback_count()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Render a fixed trace digest for a compact golden replay assertion.
[[nodiscard]] std::string digest_to_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex_from_digest(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Verify the complete policy sequence by extracting every canonical input disposition in order.
void check_input_dispositions(const runtime::MarketRuntimeEvidence& evidence) {
  constexpr std::array expected{
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::DeltaApplied,
      trace::RuntimeInputDisposition::ExactDuplicateIgnored,
      trace::RuntimeInputDisposition::OlderInputIgnored,
      trace::RuntimeInputDisposition::GapRejected,
      trace::RuntimeInputDisposition::NonReadyDeltaRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::SequenceConflictRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::ChecksumRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::MetadataRevisionRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::MalformedRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::StalenessChecked,
      trace::RuntimeInputDisposition::StalenessChecked,
      trace::RuntimeInputDisposition::NonReadyDeltaRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::SessionReset,
      trace::RuntimeInputDisposition::NonReadyDeltaRejected,
      trace::RuntimeInputDisposition::SnapshotApplied,
      trace::RuntimeInputDisposition::ExactDuplicateIgnored,
      trace::RuntimeInputDisposition::SourceDiscontinuity,
      trace::RuntimeInputDisposition::SnapshotApplied,
  };
  std::vector<trace::RuntimeInputDisposition> actual;
  actual.reserve(expected.size());
  for (const auto& record : evidence.trace_records) {
    if (record.kind() == trace::RuntimeTraceEventKind::InputDisposition) {
      actual.push_back(record.fields().input_disposition);
    }
  }
  REQUIRE(actual.size() == expected.size());
  CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
}

// --------------------------------------------------------
// Verify every callback's route, ordinal, provenance, kind, and readiness in canonical order.
void check_callback_sequence(const M2MarketReplayResult& replayed) {
  constexpr std::array expected_state{
      market_data::MarketReadiness::Synchronizing, market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Stale,         market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Synchronizing, market_data::MarketReadiness::Ready,
      market_data::MarketReadiness::Invalid,       market_data::MarketReadiness::Ready,
  };
  constexpr std::array<std::uint64_t, expected_state.size()> state_turns{
      1U, 2U, 6U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 18U, 20U, 21U, 23U, 25U, 26U};
  constexpr std::array<std::uint64_t, expected_state.size()> state_generations{
      0U, 1U, 1U, 2U, 2U, 3U, 3U, 4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U, 9U};
  constexpr std::array<std::uint64_t, expected_state.size()> state_revisions{
      0U, 1U, 2U, 3U, 3U, 4U, 4U, 5U, 5U, 6U, 6U, 7U, 7U, 8U, 8U, 9U, 9U, 10U};
  constexpr std::array<bool, 28U> is_state{
      true, true,  false, false, true,  true, false, true,  true, false, true,  true, false, true,
      true, false, true,  true,  false, true, true,  false, true, true,  false, true, true,  false};

  REQUIRE(replayed.callbacks.size() == is_state.size());
  std::size_t state_index = 0U;
  for (std::size_t index = 0U; index < replayed.callbacks.size(); ++index) {
    const auto& callback = replayed.callbacks[index];
    REQUIRE(std::holds_alternative<ObservedStateCallback>(callback) == is_state[index]);
    const auto* context =
        std::visit([](const auto& value) { return std::addressof(value.context); }, callback);
    CHECK(context->firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
    CHECK(context->desk_id == parse_identifier_or_throw<model::DeskId>("desk.digital-assets"));
    CHECK(context->bot_id ==
          parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"));
    CHECK(context->strategy_id ==
          parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference"));
    CHECK(context->subscription_id == parse_identifier_or_throw<model::SubscriptionId>(
                                          "subscription.deribit-btc-perpetual-book"));
    CHECK(context->callback_ordinal.value() == index + 1U);
    CHECK(context->configuration_fingerprint == replayed.evidence.configuration_fingerprint);
    CHECK(context->runtime_policy_fingerprint == replayed.evidence.runtime_policy_fingerprint);
    if (const auto* state = std::get_if<ObservedStateCallback>(&callback)) {
      REQUIRE(state_index < expected_state.size());
      CHECK(state->event.readiness == expected_state[state_index]);
      CHECK(state->event.turn_ordinal.value() == state_turns[state_index]);
      if (state_generations[state_index] == 0U) {
        CHECK_FALSE(state->event.book_generation.has_value());
        CHECK_FALSE(state->event.book_revision.has_value());
      } else {
        REQUIRE(state->event.book_generation.has_value());
        REQUIRE(state->event.book_revision.has_value());
        CHECK(state->event.book_generation->value() == state_generations[state_index]);
        CHECK(state->event.book_revision->value() == state_revisions[state_index]);
      }
      ++state_index;
    }
  }
  CHECK(state_index == expected_state.size());
}

// --------------------------------------------------------
// Build the exact final callback books in the same best-to-worst order exposed by ReadyBookView.
[[nodiscard]] std::vector<
    std::pair<std::vector<market_data::BookLevel>, std::vector<market_data::BookLevel>>>
create_expected_market_books_or_throw() {
  using Level = market_data::BookLevel;
  return {
      {{Level{parse_price_or_throw("50000"), parse_quantity_or_throw("2")},
        Level{parse_price_or_throw("49999.5"), parse_quantity_or_throw("4")}},
       {Level{parse_price_or_throw("50000.5"), parse_quantity_or_throw("3")},
        Level{parse_price_or_throw("50001"), parse_quantity_or_throw("5")}}},
      {{Level{parse_price_or_throw("49999.5"), parse_quantity_or_throw("6")}},
       {Level{parse_price_or_throw("50001"), parse_quantity_or_throw("5")},
        Level{parse_price_or_throw("50001.5"), parse_quantity_or_throw("8")}}},
      {{Level{parse_price_or_throw("50010"), parse_quantity_or_throw("7")}},
       {Level{parse_price_or_throw("50010.5"), parse_quantity_or_throw("8")}}},
      {{Level{parse_price_or_throw("50020"), parse_quantity_or_throw("9")}},
       {Level{parse_price_or_throw("50020.5"), parse_quantity_or_throw("10")}}},
      {{Level{parse_price_or_throw("50030"), parse_quantity_or_throw("11")}},
       {Level{parse_price_or_throw("50030.5"), parse_quantity_or_throw("12")}}},
      {{Level{parse_price_or_throw("50040"), parse_quantity_or_throw("13")}},
       {Level{parse_price_or_throw("50040.5"), parse_quantity_or_throw("14")}}},
      {{Level{parse_price_or_throw("50050"), parse_quantity_or_throw("15")}},
       {Level{parse_price_or_throw("50050.5"), parse_quantity_or_throw("16")}}},
      {{Level{parse_price_or_throw("50060"), parse_quantity_or_throw("19")}},
       {Level{parse_price_or_throw("50060.5"), parse_quantity_or_throw("20")}}},
      {{Level{parse_price_or_throw("50070"), parse_quantity_or_throw("22")}},
       {Level{parse_price_or_throw("50070.5"), parse_quantity_or_throw("23")}}},
      {{Level{parse_price_or_throw("50080"), parse_quantity_or_throw("25")}},
       {Level{parse_price_or_throw("50080.5"), parse_quantity_or_throw("26")}}},
  };
}

// --------------------------------------------------------
// Verify all ten committed callbacks carry exact book identity and complete coherent level copies.
void check_market_callbacks(const M2MarketReplayResult& replayed) {
  constexpr std::array<std::uint64_t, 10U> sequences{100U, 101U, 104U, 105U, 107U,
                                                     109U, 110U, 112U, 2U,   4U};
  constexpr std::array<std::uint64_t, 10U> generations{1U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
  constexpr std::array<std::uint64_t, 10U> revisions{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U};
  const auto expected_books = create_expected_market_books_or_throw();
  std::size_t market_index = 0U;
  for (const auto& callback : replayed.callbacks) {
    const auto* market = std::get_if<ObservedMarketCallback>(&callback);
    if (market == nullptr) {
      continue;
    }
    REQUIRE(market_index < sequences.size());
    CHECK(market->update.source_sequence() == model::SequenceNumber{sequences[market_index]});
    CHECK(market->commit.book_generation.value() == generations[market_index]);
    CHECK(market->commit.book_revision.value() == revisions[market_index]);
    CHECK(market->bids == expected_books[market_index].first);
    CHECK(market->asks == expected_books[market_index].second);
    ++market_index;
  }
  CHECK(market_index == sequences.size());
}

// --------------------------------------------------------
// The full M2 script must reproduce every strategy-visible and canonical byte exactly.
TEST_CASE("the complete M2 reference workload is byte-identical across serialized owners",
          "[m2][deterministic_scenario][runtime][market_data]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Two independent manual runtimes and one dedicated runtime consume the same scripted clocks.
  const auto first = execute_replay_or_throw(M2MarketReplayMode::Manual);
  const auto second = execute_replay_or_throw(M2MarketReplayMode::Manual);
  const auto dedicated = execute_replay_or_throw(M2MarketReplayMode::Dedicated);

  // ++++++++++++++++++++++++++++++++++++++++
  // Complete copied callback vectors and every cold evidence field are driver-independent.
  CHECK(first == second);
  CHECK(first == dedicated);
  CHECK(first.unrelated_callback_count == 0U);
  check_callback_sequence(first);
  check_market_callbacks(first);
  check_input_dispositions(first.evidence);

  // ++++++++++++++++++++++++++++++++++++++++
  // Stable counts cover every disposition, transition, state callback, and market callback.
  CHECK(first.callbacks.size() == 28U);
  CHECK(first.evidence.trace_records.size() == 71U);
  const auto trace_count = [&first](trace::RuntimeTraceEventKind kind) {
    return static_cast<std::size_t>(
        std::count_if(first.evidence.trace_records.begin(), first.evidence.trace_records.end(),
                      [kind](const auto& record) { return record.kind() == kind; }));
  };
  CHECK(trace_count(trace::RuntimeTraceEventKind::InputDisposition) == 25U);
  CHECK(trace_count(trace::RuntimeTraceEventKind::MarketStateTransition) == 18U);
  CHECK(trace_count(trace::RuntimeTraceEventKind::StateCallback) == 18U);
  CHECK(trace_count(trace::RuntimeTraceEventKind::MarketCallback) == 10U);
  CHECK(trace_count(trace::RuntimeTraceEventKind::ReentryDetected) == 0U);
  CHECK(first.evidence.diagnostics.size() == 2U);
  CHECK(first.evidence.diagnostics[0U].kind == runtime::RuntimeDiagnosticKind::MalformedInput);
  CHECK(first.evidence.diagnostics[1U].kind == runtime::RuntimeDiagnosticKind::SourceDiscontinuity);
  CHECK(first.evidence.dropped_diagnostics == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Quiescent source evidence proves the loss fence hid, but did not corrupt, retained state before
  // the strictly newer final snapshot recovered generation nine and global revision ten.
  REQUIRE(first.evidence.sources.size() == 1U);
  const auto& source = first.evidence.sources.front();
  CHECK(source.source_ordinal == model::MarketSourceOrdinal::create_initial());
  CHECK(source.readiness == market_data::MarketReadiness::Ready);
  REQUIRE(source.book_identity.has_value());
  CHECK(source.book_identity->generation().value() == 9U);
  CHECK(source.book_identity->revision().value() == 10U);
  CHECK(source.active_session == model::SessionEpoch{2U});
  CHECK(source.last_source_sequence == model::SequenceNumber{4U});
  CHECK(first.evidence.executor.completed_turns == 26U);
  CHECK(first.evidence.executor.pending_commands == 0U);
  CHECK(first.evidence.executor.pending_fences == 0U);
  CHECK(first.evidence.executor.closed);
  CHECK_FALSE(first.evidence.executor.owner_bound);
  CHECK_FALSE(first.evidence.fault.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // The digest and byte count pin the complete AEGISRTS schema-one stream, not a projected subset.
  CHECK(first.evidence.canonical_trace_bytes.size() == 29'610U);
  CHECK(digest_to_hex(first.evidence.canonical_trace_digest) ==
        "e63b89b8c3a826fd1104f9e9949364606be5efbeedc112cb980260ccb70fda0b");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
