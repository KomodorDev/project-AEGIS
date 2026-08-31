// Purpose: prove the complete credential-free M3 submission matrix reproduces synchronous local
// results and canonical cold evidence across independent manual and dedicated serialized owners.

#include "aegis/market_data/order_book.hpp"
#include "aegis/model/domain_error.hpp"
#include "aegis/model/sha256.hpp"
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
#include <span>
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
// Interesting syntax: this requires-expression makes an accidental acknowledgement API a
// compile-time scenario failure without invoking the operation.
template <typename Value>
concept HasExchangeAcknowledgement =
    requires(const Value& value) { value.exchange_acknowledged(); };

static_assert(!HasExchangeAcknowledgement<execution::SubmitResult>);

// ########################################################################

// ########################################################################
// Replay mode changes only owner mechanics; every input, clock, script, identity, and capacity is
// otherwise identical.
enum class M3SubmissionReplayMode : std::uint8_t {
  Manual = 1,
  Dedicated = 2,
};

// ########################################################################

// ########################################################################
// One copied callback context retains all public attribution and policy identities after the
// callback-local BotContext capability becomes inactive.
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
  // Compare every copied callback-context field for deterministic replay equality.
  friend bool operator==(const ObservedBotContext&, const ObservedBotContext&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A copied state callback preserves the complete sanitized transition and exact bound context.
struct ObservedStateCallback {
  ObservedBotContext context;
  market_data::MarketStateEventFields event;

  // --------------------------------------------------------
  // Compare the complete copied state callback and its bound context.
  friend bool operator==(const ObservedStateCallback&, const ObservedStateCallback&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A copied market callback owns the normalized input, commit identity, and coherent visible book.
struct ObservedMarketCallback {
  ObservedBotContext context;
  market_data::NormalizedMarketUpdate update;
  market_data::MarketCommitContext commit;
  std::vector<market_data::BookLevel> bids;
  std::vector<market_data::BookLevel> asks;

  // --------------------------------------------------------
  // Compare the complete copied market callback, commit identity, and visible book.
  friend bool operator==(const ObservedMarketCallback&, const ObservedMarketCallback&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The closed union keeps callback kind explicit while permitting one ordered replay vector.
using CallbackObservation = std::variant<ObservedStateCallback, ObservedMarketCallback>;

// ########################################################################

// ########################################################################
// The result projection retains every SubmitResult field, including the dedicated scripted
// measurement-clock duration, so owner-mode replay equality is complete.
struct ObservedSubmitResult {
  execution::SubmitDisposition disposition;
  execution::SubmissionStage stage;
  execution::SubmissionReason reason;
  std::optional<model::SubmissionAttemptId> attempt_id;
  std::optional<model::OrderId> order_id;
  std::optional<execution::RiskLimitEvidence> risk_evidence;
  std::optional<std::uint64_t> local_path_nanoseconds;

  // --------------------------------------------------------
  // Compare every deterministic submission result field while retaining optional-value identity.
  friend bool operator==(const ObservedSubmitResult&, const ObservedSubmitResult&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One assigned expectation row makes each stable local outcome and identity-presence rule explicit.
struct ExpectedSubmitResultShape {
  execution::SubmitDisposition disposition;
  execution::SubmissionStage stage;
  execution::SubmissionReason reason;
  bool has_order_id;
};

// ########################################################################

// --------------------------------------------------------
// Parse one nominal identifier or stop before a fixture-authoring mistake reaches production code.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view value) {
  auto parsed = Identifier::parse_identifier(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M3 reference scenario"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact decimal input without introducing any binary floating-point representation.
template <typename Decimal> [[nodiscard]] Decimal parse_decimal_or_throw(std::string_view value) {
  auto parsed = Decimal::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid decimal in M3 reference scenario"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Construct a checked one-based ordinal or reject an invalid test-authored value immediately.
template <typename Ordinal> [[nodiscard]] Ordinal create_ordinal_or_throw(std::uint64_t value) {
  auto created = Ordinal::from_value(value);
  if (!created) {
    throw std::logic_error{"invalid ordinal in M3 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Copy all public BotContext fields while the synchronous callback capability remains active.
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
// Copy all SubmitResult fields, including exact deterministic local-path measurement.
[[nodiscard]] ObservedSubmitResult copy_submit_result(const execution::SubmitResult& result) {
  return ObservedSubmitResult{result.disposition(),
                              result.stage(),
                              result.reason(),
                              result.attempt_id(),
                              result.order_id(),
                              result.risk_evidence(),
                              result.local_path_nanoseconds()};
}

// --------------------------------------------------------
// Render a fixed-width digest for compact canonical golden assertions.
[[nodiscard]] std::string digest_to_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex_from_digest(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------

// ########################################################################
// The baseline strategy records all callbacks and submits the complete ordered matrix exactly once
// from the Ready market callback on the serialized owner.
class MatrixSubmittingStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Own immutable requests while borrowing pre-reserved observations and result projections.
  MatrixSubmittingStrategy(std::vector<execution::OrderRequest> requests,
                           std::vector<CallbackObservation>& callbacks,
                           std::vector<ObservedSubmitResult>& results) noexcept
      : requests_{std::move(requests)}, callbacks_{&callbacks}, results_{&results} {}

  // --------------------------------------------------------
  // Copy the complete Ready callback before executing every synchronous direct-path attempt.
  void on_market_data(const market_data::MarketEvent& event, const market_data::ReadyBookView& book,
                      runtime::BotContext& context) noexcept override {
    ObservedMarketCallback observation{
        copy_callback_context(context), event.update(), event.context(), {}, {}};
    observation.bids.reserve(book.bid_count());
    observation.asks.reserve(book.ask_count());
    for (std::size_t index = 0U; index < book.bid_count(); ++index) {
      if (const auto level = book.bid_at(index); level) {
        observation.bids.push_back(*level);
      }
    }
    for (std::size_t index = 0U; index < book.ask_count(); ++index) {
      if (const auto level = book.ask_at(index); level) {
        observation.asks.push_back(*level);
      }
    }
    callbacks_->emplace_back(std::move(observation));
    if (submitted_) {
      return;
    }
    submitted_ = true;
    for (const auto& request : requests_) {
      results_->push_back(copy_submit_result(context.submit_order(request)));
    }
  }

  // --------------------------------------------------------
  // Copy startup and Ready transitions without submitting before canonical market readiness.
  void on_market_state(const market_data::MarketStateEvent& event,
                       runtime::BotContext& context) noexcept override {
    callbacks_->emplace_back(ObservedStateCallback{copy_callback_context(context), event.fields()});
  }

  // --------------------------------------------------------
private:
  std::vector<execution::OrderRequest> requests_;
  std::vector<CallbackObservation>* callbacks_;
  std::vector<ObservedSubmitResult>* results_;
  bool submitted_{false};
};

// ########################################################################

// ########################################################################
// The peer-firm strategy exposes any accidental observation grant through a race-free counter.
class UnrelatedBotStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow the peer callback counter whose lifetime encloses this strategy.
  explicit UnrelatedBotStrategy(std::atomic_uint32_t& callback_count) noexcept
      : callback_count_{&callback_count} {}

  // --------------------------------------------------------
  // Record any unexpected peer market-data callback without retaining turn-scoped authority.
  void on_market_data(const market_data::MarketEvent&, const market_data::ReadyBookView&,
                      runtime::BotContext&) noexcept override {
    callback_count_->fetch_add(1U, std::memory_order_relaxed);
  }

  // --------------------------------------------------------
  // Record any unexpected peer state callback without retaining turn-scoped authority.
  void on_market_state(const market_data::MarketStateEvent&,
                       runtime::BotContext&) noexcept override {
    callback_count_->fetch_add(1U, std::memory_order_relaxed);
  }

  // --------------------------------------------------------
private:
  std::atomic_uint32_t* callback_count_;
};

// ########################################################################

// --------------------------------------------------------
// Mint the six canonical identities consumed after the three pre-identity rejection attempts.
[[nodiscard]] std::vector<model::OrderId> create_unique_order_ids_or_throw() {
  model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0xa3U);
  auto created = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{namespace_bytes}, 100U);
  if (!created) {
    throw std::logic_error{"invalid order identity namespace in M3 reference scenario"};
  }
  auto provider = std::move(created).value();
  std::vector<model::OrderId> identities;
  identities.reserve(6U);
  for (std::size_t index = 0U; index < 6U; ++index) {
    auto next = provider.generate_next_order_id();
    if (!next) {
      throw std::logic_error{"failed to mint order identity in M3 reference scenario"};
    }
    identities.push_back(std::move(next).value());
  }
  return identities;
}

// --------------------------------------------------------
// Repeat identity five in the sixth closed-source position so duplicate precedence is
// deterministic.
[[nodiscard]] model::DeterministicOrderIdSource create_scripted_order_id_source_or_throw() {
  const auto unique = create_unique_order_ids_or_throw();
  std::vector<model::OrderId> scripted;
  scripted.reserve(7U);
  scripted.push_back(unique[0U]);
  scripted.push_back(unique[1U]);
  scripted.push_back(unique[2U]);
  scripted.push_back(unique[3U]);
  scripted.push_back(unique[4U]);
  scripted.push_back(unique[4U]);
  scripted.push_back(unique[5U]);
  return model::ScriptedOrderIdProvider{std::move(scripted)};
}

// --------------------------------------------------------
// Seal the separately enabled two-firm M3 authority without altering the accepted M1/M2 fixture.
[[nodiscard]] configuration::StartupConfiguration create_m3_configuration_or_throw() {
  auto created = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_m3_enabled_two_firm_configuration_params_or_throw());
  if (!created) {
    throw std::logic_error{"invalid startup configuration in M3 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Bind the one public recorded source and bounded callback/trace storage to sealed configuration.
[[nodiscard]] runtime::RuntimePolicy
create_m3_runtime_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U, 32U,
                                       100'000U},
          {{parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::create_initial()}},
      });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M3 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build one canonical baseline request while allowing exact route, price, and quantity variants.
[[nodiscard]] execution::OrderRequest create_order_request_or_throw(std::string_view route,
                                                                    std::string_view price,
                                                                    std::string_view quantity) {
  return execution::OrderRequest{
      parse_identifier_or_throw<model::RouteId>(route),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      execution::OrderSide::Buy,
      execution::OrderType::Limit,
      execution::TimeInForce::GoodTilCancelled,
      parse_decimal_or_throw<model::Price>(price),
      parse_decimal_or_throw<model::Quantity>(quantity)};
}

// --------------------------------------------------------
// Order every exit so route/canonical failures consume no identity and later identities are fixed.
[[nodiscard]] std::vector<execution::OrderRequest> create_request_matrix_or_throw() {
  constexpr std::string_view baseline_route = "route.deribit-testnet-btc-perpetual";
  std::vector<execution::OrderRequest> requests;
  requests.reserve(10U);
  requests.push_back(
      create_order_request_or_throw("route.deribit-testnet-subsidiary-btc-perpetual", "100", "1"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100.1", "1"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "2.5"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "1000001"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "1"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "2"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "3"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "4"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "5"));
  requests.push_back(create_order_request_or_throw(baseline_route, "100", "6"));
  return requests;
}

// --------------------------------------------------------
// Script one encoding failure, one definite pre-copy failure, one accepted uncertainty, then
// ordinary success; all storage is fixed before callbacks begin.
[[nodiscard]] runtime::FakeSubmissionRuntimeParams
create_submission_params_or_throw(const configuration::StartupConfiguration& configuration) {
  constexpr std::uint64_t maximum_attempts = 10U;
  auto encoder = execution::FakeEncoderScript::create_fake_encoder_script(
      execution::FakeEncodingAction::Encode, maximum_attempts,
      {{1U, execution::FakeEncodingAction::Fail}});
  auto initiator = execution::FakeInitiatorScript::create_fake_initiator_script(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts,
      {{1U, execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance},
       {2U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}});
  if (!encoder || !initiator) {
    throw std::logic_error{"invalid fake script in M3 reference scenario"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Each valid owner-local attempt receives one entry and one endpoint exactly 25 nanoseconds
  // apart.
  std::vector<std::optional<std::uint64_t>> measurement_readings;
  measurement_readings.reserve(static_cast<std::size_t>(maximum_attempts * 2U));
  for (std::uint64_t attempt_index = 0U; attempt_index < maximum_attempts; ++attempt_index) {
    const auto entry = 10'000U + (attempt_index * 100U);
    measurement_readings.emplace_back(entry);
    measurement_readings.emplace_back(entry + 25U);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Transfer the complete fixed policy, scripts, clock, and identity source as one runtime input.
  return runtime::FakeSubmissionRuntimeParams{
      test_support::create_m3_reference_risk_policy_params_or_throw(configuration),
      execution::SubmissionPolicyCapacities{maximum_attempts, 4U, 4U, 1'024U, 2U, 110U, 8U},
      std::move(encoder).value(),
      std::move(initiator).value(),
      std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
          std::move(measurement_readings)),
      create_scripted_order_id_source_or_throw()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Own the sole exact recorded snapshot admitted after bootstrap and before the submission callback.
[[nodiscard]] market_data::IngressFrameAttempt create_snapshot_attempt_or_throw() {
  auto created = market_data::IngressFrameAttempt::create_ingress_frame_attempt(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
      model::SessionEpoch{1U},
      "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:m3-submit|2|"
      "B,50000,2|A,50000.5,3");
  if (!created) {
    throw std::logic_error{"invalid recorded snapshot in M3 reference scenario"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Bound dedicated-owner observation so a regression cannot hang the deterministic scenario target.
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
// The harness keeps borrowed clocks and capture storage alive through owner shutdown and copies
// evidence only after the selected owner has fully released the runtime.
class M3SubmissionReplayHarness final {
public:

  // --------------------------------------------------------
  // Compose the exact same runtime, strategies, scripts, and capacity ceilings in either mode.
  explicit M3SubmissionReplayHarness(M3SubmissionReplayMode mode)
      : mode_{mode}, executor_clock_{1'000U}, callback_clock_{100'000U} {
    callbacks_.reserve(4U);
    results_.reserve(10U);
    auto configuration = create_m3_configuration_or_throw();
    auto policy = create_m3_runtime_policy_or_throw(configuration);
    auto fake_submission = create_submission_params_or_throw(configuration);
    std::vector<runtime::BotStrategyRegistration> registrations;
    registrations.reserve(2U);
    registrations.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<MatrixSubmittingStrategy>(create_request_matrix_or_throw(), callbacks_,
                                                   results_)});
    registrations.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.subsidiary-reference"),
        std::make_unique<UnrelatedBotStrategy>(unrelated_callback_count_)});
    auto created = runtime::MarketRuntime::create_with_fake_submission(
        std::move(configuration), std::move(policy), executor_clock_, callback_clock_,
        std::move(registrations), std::move(fake_submission));
    if (!created) {
      throw std::logic_error{"invalid composed runtime in M3 reference scenario: " +
                             created.error().context.field};
    }
    runtime_ = std::move(created).value();
  }

  // --------------------------------------------------------
  // Complete the one bootstrap turn under the chosen owner and require an idle Running state.
  void start_runtime_or_throw() {
    if (mode_ == M3SubmissionReplayMode::Manual) {
      if (!runtime_->bind_to_current_thread()) {
        throw std::logic_error{"failed to bind manual M3 owner"};
      }
      execute_one_manual_turn_or_throw();
      return;
    }
    if (!runtime_->start_dedicated()) {
      throw std::logic_error{"failed to start dedicated M3 owner"};
    }
    wait_for_completed_turn_count_or_throw(1U);
  }

  // --------------------------------------------------------
  // Apply the sole admitted snapshot at the same deterministic owner timestamp in both modes.
  void apply_snapshot_or_throw() {
    if (!executor_clock_.advance_nanoseconds(100U)) {
      throw std::logic_error{"failed to advance M3 reference clock"};
    }
    auto admitted = runtime_->try_admit(create_snapshot_attempt_or_throw());
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted ||
        !admitted.value().receipt.has_value() || admitted.value().discontinuity_recorded) {
      throw std::logic_error{"M3 reference snapshot was not accepted"};
    }
    if (mode_ == M3SubmissionReplayMode::Manual) {
      execute_one_manual_turn_or_throw();
      return;
    }
    wait_for_completed_turn_count_or_throw(2U);
  }

  // --------------------------------------------------------
  // Release ownership, then copy complete immutable callback, result, and runtime evidence.
  [[nodiscard]] runtime::MarketRuntimeEvidence finish_replay_or_throw() {
    if (mode_ == M3SubmissionReplayMode::Manual) {
      runtime_->close();
      if (!runtime_->release_from_current_thread()) {
        throw std::logic_error{"failed to release manual M3 owner"};
      }
    } else {
      runtime_->close_and_wait();
    }
    auto evidence = runtime_->collect_quiescent_evidence();
    if (!evidence) {
      throw std::logic_error{"failed to collect quiescent M3 evidence"};
    }
    return std::move(evidence).value();
  }

  // --------------------------------------------------------
  // Borrow the complete callback sequence after all callback-local authority has expired.
  [[nodiscard]] const std::vector<CallbackObservation>& callback_observations() const noexcept {
    return callbacks_;
  }

  // --------------------------------------------------------
  // Borrow the copied synchronous result sequence produced by the request matrix.
  [[nodiscard]] const std::vector<ObservedSubmitResult>& submission_results() const noexcept {
    return results_;
  }

  // --------------------------------------------------------
  // Return the number of callbacks that escaped the baseline bot's configured grant.
  [[nodiscard]] std::uint32_t unrelated_callback_count() const noexcept {
    return unrelated_callback_count_.load(std::memory_order_relaxed);
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Execute exactly one nonempty deterministic turn and throw on owner progression failure.
  void execute_one_manual_turn_or_throw() {
    auto completed = runtime_->execute_next_turn();
    if (!completed || !completed.value().has_value()) {
      throw std::logic_error{"manual M3 owner did not complete one turn"};
    }
  }

  // --------------------------------------------------------
  // Observe synchronized status until the dedicated owner is fully idle at the expected prefix.
  void wait_for_completed_turn_count_or_throw(std::uint64_t expected_turns) {
    wait_until_condition_or_throw(
        [this, expected_turns] {
          const auto status = runtime_->status();
          if (status.lifecycle == runtime::MarketRuntimeLifecycle::Faulted) {
            throw std::logic_error{"dedicated M3 owner faulted"};
          }
          return status.lifecycle == runtime::MarketRuntimeLifecycle::Running &&
                 status.executor.completed_turns == expected_turns &&
                 status.executor.pending_commands == 0U && status.executor.pending_fences == 0U &&
                 !status.executor.turn_active;
        },
        "dedicated M3 owner did not complete the scripted turn");
  }

  // --------------------------------------------------------
  M3SubmissionReplayMode mode_;
  std::vector<CallbackObservation> callbacks_;
  std::vector<ObservedSubmitResult> results_;
  std::atomic_uint32_t unrelated_callback_count_{0U};
  model::DeterministicClockProvider executor_clock_;
  model::DeterministicClockProvider callback_clock_;
  std::unique_ptr<runtime::MarketRuntime> runtime_;
};

// ########################################################################

// ########################################################################
// One replay result owns all strategy-visible and cold canonical products after owner release.
struct M3SubmissionReplayResult {
  std::vector<CallbackObservation> callbacks;
  std::vector<ObservedSubmitResult> results;
  runtime::MarketRuntimeEvidence evidence;
  std::uint32_t unrelated_callback_count;

  // --------------------------------------------------------
  // Compare every strategy-visible and cold canonical product after both replays are quiescent.
  friend bool operator==(const M3SubmissionReplayResult&,
                         const M3SubmissionReplayResult&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Execute the exact matrix through the public MarketRuntime boundary and return only cold copies.
[[nodiscard]] M3SubmissionReplayResult execute_replay_or_throw(M3SubmissionReplayMode mode) {
  M3SubmissionReplayHarness harness{mode};
  harness.start_runtime_or_throw();
  harness.apply_snapshot_or_throw();
  auto evidence = harness.finish_replay_or_throw();
  return M3SubmissionReplayResult{harness.callback_observations(), harness.submission_results(),
                                  std::move(evidence), harness.unrelated_callback_count()};
}

// --------------------------------------------------------
// Verify all three callbacks and their source, owner-turn, readiness, and book identities.
void check_callback_sequence(const M3SubmissionReplayResult& replayed) {
  REQUIRE(replayed.callbacks.size() == 3U);
  REQUIRE(std::holds_alternative<ObservedStateCallback>(replayed.callbacks[0U]));
  REQUIRE(std::holds_alternative<ObservedStateCallback>(replayed.callbacks[1U]));
  REQUIRE(std::holds_alternative<ObservedMarketCallback>(replayed.callbacks[2U]));
  const auto& synchronizing = std::get<ObservedStateCallback>(replayed.callbacks[0U]);
  const auto& ready = std::get<ObservedStateCallback>(replayed.callbacks[1U]);
  const auto& market = std::get<ObservedMarketCallback>(replayed.callbacks[2U]);

  // ++++++++++++++++++++++++++++++++++++++++
  // Bootstrap and snapshot callbacks share immutable bot-derived attribution and policy identity.
  for (const auto* const context : {&synchronizing.context, &ready.context, &market.context}) {
    CHECK(context->firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
    CHECK(context->desk_id == parse_identifier_or_throw<model::DeskId>("desk.digital-assets"));
    CHECK(context->bot_id ==
          parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"));
    CHECK(context->strategy_id ==
          parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference"));
    CHECK(context->subscription_id == parse_identifier_or_throw<model::SubscriptionId>(
                                          "subscription.deribit-btc-perpetual-book"));
    CHECK(context->configuration_fingerprint == replayed.evidence.configuration_fingerprint);
    CHECK(context->runtime_policy_fingerprint == replayed.evidence.runtime_policy_fingerprint);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Pin callback and owner-turn ordinals together with the exact Ready book produced by snapshot.
  CHECK(synchronizing.context.callback_ordinal ==
        create_ordinal_or_throw<model::CallbackOrdinal>(1U));
  CHECK(synchronizing.event.turn_ordinal == create_ordinal_or_throw<model::TurnOrdinal>(1U));
  CHECK(synchronizing.event.readiness == market_data::MarketReadiness::Synchronizing);
  CHECK(ready.context.callback_ordinal == create_ordinal_or_throw<model::CallbackOrdinal>(2U));
  CHECK(ready.event.turn_ordinal == create_ordinal_or_throw<model::TurnOrdinal>(2U));
  CHECK(ready.event.readiness == market_data::MarketReadiness::Ready);
  CHECK(market.context.callback_ordinal == create_ordinal_or_throw<model::CallbackOrdinal>(3U));
  CHECK(market.commit.turn_ordinal == create_ordinal_or_throw<model::TurnOrdinal>(2U));
  CHECK(market.update.source_sequence() == model::SequenceNumber{100U});
  REQUIRE(market.bids.size() == 1U);
  REQUIRE(market.asks.size() == 1U);
  CHECK(market.bids.front() ==
        market_data::BookLevel{parse_decimal_or_throw<model::Price>("50000"),
                               parse_decimal_or_throw<model::Quantity>("2")});
  CHECK(market.asks.front() ==
        market_data::BookLevel{parse_decimal_or_throw<model::Price>("50000.5"),
                               parse_decimal_or_throw<model::Quantity>("3")});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify the stable disposition/stage/reason matrix plus precise identity and risk-evidence shape.
void check_submission_results(const M3SubmissionReplayResult& replayed) {
  constexpr std::array expected{
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Route,
                                execution::SubmissionReason::RouteNotOwned, false},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::CanonicalValidation,
                                execution::SubmissionReason::PriceTickMismatch, false},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::CanonicalValidation,
                                execution::SubmissionReason::QuantityScaleExceeded, false},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Risk,
                                execution::SubmissionReason::SingleOrderQuantityExceeded, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Encoding,
                                execution::SubmissionReason::EncodingFailed, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Initiation,
                                execution::SubmissionReason::InitiationDefinitelyFailed, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::SubmissionUnknown,
                                execution::SubmissionStage::Initiation,
                                execution::SubmissionReason::InitiationOutcomeUnknown, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::WriteInitiated,
                                execution::SubmissionStage::Initiation,
                                execution::SubmissionReason::None, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Oms,
                                execution::SubmissionReason::DuplicateOrderIdentity, true},
      ExpectedSubmitResultShape{execution::SubmitDisposition::LocallyRejected,
                                execution::SubmissionStage::Oms,
                                execution::SubmissionReason::OmsCapacityExceeded, true},
  };
  REQUIRE(replayed.results.size() == expected.size());
  const auto identities = create_unique_order_ids_or_throw();
  constexpr std::array<std::optional<std::size_t>, 10U> identity_indices{
      std::nullopt, std::nullopt, std::nullopt, 0U, 1U, 2U, 3U, 4U, 4U, 5U};

  // ++++++++++++++++++++++++++++++++++++++++
  // All attempts complete synchronously and only the first three stop before local identity.
  for (std::size_t index = 0U; index < expected.size(); ++index) {
    const auto& observed = replayed.results[index];
    CHECK(observed.disposition == expected[index].disposition);
    CHECK(observed.stage == expected[index].stage);
    CHECK(observed.reason == expected[index].reason);
    REQUIRE(observed.attempt_id.has_value());
    CHECK(*observed.attempt_id == create_ordinal_or_throw<model::SubmissionAttemptId>(index + 1U));
    CHECK(observed.order_id.has_value() == expected[index].has_order_id);
    if (identity_indices[index]) {
      REQUIRE(observed.order_id.has_value());
      CHECK(*observed.order_id == identities[*identity_indices[index]]);
    }
    CHECK(observed.local_path_nanoseconds == std::optional<std::uint64_t>{25U});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Only the risk exit owns typed first-failing Bot-scope evidence; all other results omit it.
  REQUIRE(replayed.results[3U].risk_evidence.has_value());
  const auto& risk_rejection = *replayed.results[3U].risk_evidence;
  CHECK(risk_rejection.scope() == risk::RiskScopeKind::Bot);
  CHECK(risk_rejection.measure_kind() == execution::RiskMeasureKind::Quantity);
  CHECK(risk_rejection.observed_quantity() == parse_decimal_or_throw<model::Quantity>("1000001"));
  CHECK(risk_rejection.quantity_limit() == parse_decimal_or_throw<model::Quantity>("1000000"));
  for (std::size_t index = 0U; index < replayed.results.size(); ++index) {
    if (index != 3U) {
      CHECK_FALSE(replayed.results[index].risk_evidence.has_value());
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify every per-attempt canonical event sequence and exact release/retention count.
void check_submission_trace(const runtime::SubmissionRuntimeEvidence& submission) {
  using Kind = trace::SubmissionTraceEventKind;
  constexpr std::array<std::uint8_t, 10U> records_per_attempt{2U,  3U, 3U, 6U, 9U,
                                                              10U, 9U, 9U, 8U, 8U};
  constexpr std::array<Kind, 67U> expected{
      Kind::Attempt,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskRejected,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsAdmitted,
      Kind::EncodingFailed,
      Kind::ReservationReleased,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsAdmitted,
      Kind::Encoded,
      Kind::InitiationDefinitelyFailed,
      Kind::ReservationReleased,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsAdmitted,
      Kind::Encoded,
      Kind::SubmissionUnknown,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsAdmitted,
      Kind::Encoded,
      Kind::WriteInitiated,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsNonAdmission,
      Kind::ReservationReleased,
      Kind::SubmissionCompleted,
      Kind::Attempt,
      Kind::RouteAuthorized,
      Kind::CanonicalValidated,
      Kind::IdentityGenerated,
      Kind::RiskReserved,
      Kind::OmsNonAdmission,
      Kind::ReservationReleased,
      Kind::SubmissionCompleted,
  };
  REQUIRE(submission.trace_records.size() == expected.size());

  // ++++++++++++++++++++++++++++++++++++++++
  // Record order, attempt grouping, callback identity, and runtime attribution are all canonical.
  const auto requests = create_request_matrix_or_throw();
  std::size_t record_index = 0U;
  for (std::size_t attempt_index = 0U; attempt_index < records_per_attempt.size();
       ++attempt_index) {
    for (std::uint8_t position = 0U; position < records_per_attempt[attempt_index]; ++position) {
      const auto& record = submission.trace_records[record_index];
      CHECK(record.kind() == expected[record_index]);
      CHECK(record.fields().context.attempt_id ==
            create_ordinal_or_throw<model::SubmissionAttemptId>(attempt_index + 1U));
      CHECK(record.fields().context.request == requests[attempt_index]);
      CHECK(record.fields().context.owner_turn_ordinal ==
            create_ordinal_or_throw<model::TurnOrdinal>(2U));
      CHECK(record.fields().context.callback_ordinal ==
            create_ordinal_or_throw<model::CallbackOrdinal>(3U));
      CHECK(record.fields().context.callback_processing_nanoseconds == 1'100U);
      CHECK(record.fields().context.attribution.firm_id ==
            parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
      CHECK(record.fields().context.attribution.bot_id ==
            parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"));
      ++record_index;
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Count terminal event kinds independently so a reordered or duplicated lifecycle is visible.
  const auto event_count = [&submission](Kind kind) {
    return static_cast<std::size_t>(
        std::count_if(submission.trace_records.begin(), submission.trace_records.end(),
                      [kind](const auto& record) { return record.kind() == kind; }));
  };
  CHECK(event_count(Kind::ReservationReleased) == 4U);
  CHECK(event_count(Kind::SubmissionUnknown) == 1U);
  CHECK(event_count(Kind::WriteInitiated) == 1U);
  CHECK(event_count(Kind::ReentryRejected) == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify retained OMS rows, exact BotContext provenance, and terminal local-only states.
void check_oms(const runtime::SubmissionRuntimeEvidence& submission) {
  constexpr std::array expected_states{
      oms::OutboundOrderState::LocallyFailed, oms::OutboundOrderState::LocallyFailed,
      oms::OutboundOrderState::SubmissionUnknown, oms::OutboundOrderState::WriteInitiated};
  constexpr std::array<std::string_view, 4U> expected_quantities{"1", "2", "3", "4"};
  REQUIRE(submission.oms_order_count == expected_states.size());
  REQUIRE(submission.oms_orders.size() == expected_states.size());

  // ++++++++++++++++++++++++++++++++++++++++
  // Admission order is stable and every field came from route/configuration/risk authority.
  for (std::size_t index = 0U; index < submission.oms_orders.size(); ++index) {
    const auto& observed = submission.oms_orders[index];
    const auto& admission = observed.admission;
    const auto& provenance = admission.provenance;
    CHECK(observed.state == expected_states[index]);
    CHECK(admission.attempt_id == create_ordinal_or_throw<model::SubmissionAttemptId>(index + 5U));
    CHECK(admission.reservation_id == create_ordinal_or_throw<model::ReservationId>(index + 5U));
    CHECK(admission.economics.price == parse_decimal_or_throw<model::Price>("100"));
    CHECK(admission.economics.quantity ==
          parse_decimal_or_throw<model::Quantity>(expected_quantities[index]));
    CHECK(admission.exposure.quote_face_notional() ==
          parse_decimal_or_throw<model::Notional>(std::to_string((index + 1U) * 10U)));
    CHECK(provenance.route_id ==
          parse_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual"));
    CHECK(provenance.logical_account_id ==
          parse_identifier_or_throw<model::LogicalAccountId>("account.deribit-testnet-aegis"));
    CHECK(provenance.venue_id == parse_identifier_or_throw<model::VenueId>("deribit"));
    CHECK(provenance.firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-lab"));
    CHECK(provenance.desk_id == parse_identifier_or_throw<model::DeskId>("desk.digital-assets"));
    CHECK(provenance.bot_id ==
          parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"));
    CHECK(provenance.strategy_id ==
          parse_identifier_or_throw<model::StrategyId>("strategy.deterministic-reference"));
    CHECK(provenance.risk_policy_fingerprint == submission.risk_policy_fingerprint.bytes());
    CHECK(provenance.risk_policy_revision == submission.risk_policy_revision);
    CHECK(provenance.submission_policy_fingerprint ==
          submission.submission_policy_fingerprint.bytes());
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify definitive exits released exactly once while uncertain and initiated exposure stayed held.
void check_reservations_and_scopes(const runtime::SubmissionRuntimeEvidence& submission) {
  REQUIRE(submission.held_reservation_count == 2U);
  REQUIRE(submission.held_reservations.size() == 2U);
  const auto& unknown = submission.held_reservations[0U];
  const auto& initiated = submission.held_reservations[1U];
  CHECK(unknown.reservation_id == create_ordinal_or_throw<model::ReservationId>(7U));
  CHECK(unknown.state == risk::ReservationState::Held);
  CHECK(unknown.exposure.order_quantity() == parse_decimal_or_throw<model::Quantity>("3"));
  CHECK(unknown.exposure.quote_face_notional() == parse_decimal_or_throw<model::Notional>("30"));
  CHECK(initiated.reservation_id == create_ordinal_or_throw<model::ReservationId>(8U));
  CHECK(initiated.state == risk::ReservationState::Held);
  CHECK(initiated.exposure.order_quantity() == parse_decimal_or_throw<model::Quantity>("4"));
  CHECK(initiated.exposure.quote_face_notional() == parse_decimal_or_throw<model::Notional>("40"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Each of the baseline firm's seven scopes contains only unknown plus initiated economics.
  REQUIRE(submission.scope_exposures.size() == 14U);
  std::uint32_t baseline_scopes = 0U;
  std::uint32_t subsidiary_scopes = 0U;
  for (const auto& scope : submission.scope_exposures) {
    const auto& exposure = scope.exposure;
    if (scope.firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-lab")) {
      ++baseline_scopes;
      CHECK(exposure.open_order_count == 2U);
      CHECK(exposure.gross_reserved_quote_notional ==
            parse_decimal_or_throw<model::Notional>("70"));
      CHECK(exposure.reserved_buy_quantity == parse_decimal_or_throw<model::Quantity>("7"));
      CHECK(exposure.reserved_sell_quantity == parse_decimal_or_throw<model::Quantity>("0"));
      CHECK(exposure.worst_case_position_quantity == parse_decimal_or_throw<model::Quantity>("7"));
      CHECK(exposure.reserved_buy_quote_notional == parse_decimal_or_throw<model::Notional>("70"));
      CHECK(exposure.reserved_sell_quote_notional == parse_decimal_or_throw<model::Notional>("0"));
      CHECK(exposure.instrument_worst_case_quote_notional ==
            parse_decimal_or_throw<model::Notional>("70"));
      CHECK(exposure.worst_case_position_quote_notional ==
            parse_decimal_or_throw<model::Notional>("70"));
    } else {
      CHECK(scope.firm_id == parse_identifier_or_throw<model::FirmId>("firm.aegis-subsidiary"));
      ++subsidiary_scopes;
      CHECK(exposure.open_order_count == 0U);
      CHECK(exposure.gross_reserved_quote_notional == parse_decimal_or_throw<model::Notional>("0"));
      CHECK(exposure.reserved_buy_quantity == parse_decimal_or_throw<model::Quantity>("0"));
      CHECK(exposure.reserved_sell_quantity == parse_decimal_or_throw<model::Quantity>("0"));
      CHECK(exposure.worst_case_position_quantity == parse_decimal_or_throw<model::Quantity>("0"));
      CHECK(exposure.reserved_buy_quote_notional == parse_decimal_or_throw<model::Notional>("0"));
      CHECK(exposure.reserved_sell_quote_notional == parse_decimal_or_throw<model::Notional>("0"));
      CHECK(exposure.instrument_worst_case_quote_notional ==
            parse_decimal_or_throw<model::Notional>("0"));
      CHECK(exposure.worst_case_position_quote_notional ==
            parse_decimal_or_throw<model::Notional>("0"));
    }
  }
  CHECK(baseline_scopes == 7U);
  CHECK(subsidiary_scopes == 7U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify fake acceptance copied exact bytes only for unknown and local-initiation attempts.
void check_fake_writes(const runtime::SubmissionRuntimeEvidence& submission) {
  REQUIRE(submission.encoder_invocations_consumed == 4U);
  REQUIRE(submission.initiator_invocations_consumed == 3U);
  REQUIRE(submission.accepted_writes.size() == 2U);
  const auto identities = create_unique_order_ids_or_throw();
  constexpr std::array<std::uint64_t, 2U> attempts{7U, 8U};
  constexpr std::array<std::uint64_t, 2U> encoder_invocations{3U, 4U};
  constexpr std::array<std::uint64_t, 2U> initiator_invocations{2U, 3U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Two accepted slots and only two prove no automatic retry followed the uncertain result.
  for (std::size_t index = 0U; index < submission.accepted_writes.size(); ++index) {
    const auto& write = submission.accepted_writes[index];
    CHECK(write.attempt_id == create_ordinal_or_throw<model::SubmissionAttemptId>(attempts[index]));
    CHECK(write.encoder_invocation_ordinal ==
          create_ordinal_or_throw<model::EncoderInvocationOrdinal>(encoder_invocations[index]));
    CHECK(write.initiator_invocation_ordinal ==
          create_ordinal_or_throw<model::InitiatorInvocationOrdinal>(initiator_invocations[index]));
    CHECK(write.write_ordinal == create_ordinal_or_throw<model::FakeWriteOrdinal>(index + 1U));
    CHECK(write.bytes.size() <= execution::maximum_encoded_fake_order_bytes);
    CHECK_FALSE(write.bytes.empty());
    CHECK(submission.oms_orders[index + 2U].admission.order_id == identities[index + 3U]);
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify diagnostic order proves four exact releases and one conservative unknown retention.
void check_submission_diagnostics(const runtime::SubmissionRuntimeEvidence& submission) {
  constexpr std::array expected_kinds{
      runtime::SubmissionDiagnosticKind::ReservationReleased,
      runtime::SubmissionDiagnosticKind::ReservationReleased,
      runtime::SubmissionDiagnosticKind::UnknownExposureRetained,
      runtime::SubmissionDiagnosticKind::ReservationReleased,
      runtime::SubmissionDiagnosticKind::ReservationReleased,
  };
  constexpr std::array<std::uint64_t, 5U> expected_attempts{5U, 6U, 7U, 9U, 10U};
  constexpr std::array expected_stages{
      execution::SubmissionStage::Encoding, execution::SubmissionStage::Initiation,
      execution::SubmissionStage::Initiation, execution::SubmissionStage::Oms,
      execution::SubmissionStage::Oms};
  constexpr std::array expected_reasons{execution::SubmissionReason::EncodingFailed,
                                        execution::SubmissionReason::InitiationDefinitelyFailed,
                                        execution::SubmissionReason::InitiationOutcomeUnknown,
                                        execution::SubmissionReason::DuplicateOrderIdentity,
                                        execution::SubmissionReason::OmsCapacityExceeded};
  REQUIRE(submission.diagnostics.size() == expected_kinds.size());
  CHECK(submission.dropped_diagnostics == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Match every retained diagnostic to its attempt, release identity, cause, and sealed provenance.
  for (std::size_t index = 0U; index < submission.diagnostics.size(); ++index) {
    const auto& diagnostic = submission.diagnostics[index];
    CHECK(diagnostic.ordinal == index + 1U);
    CHECK(diagnostic.kind == expected_kinds[index]);
    CHECK(diagnostic.fields.attempt_id ==
          create_ordinal_or_throw<model::SubmissionAttemptId>(expected_attempts[index]));
    CHECK(diagnostic.fields.reservation_id ==
          create_ordinal_or_throw<model::ReservationId>(expected_attempts[index]));
    CHECK(diagnostic.fields.stage == expected_stages[index]);
    CHECK(diagnostic.fields.reason == expected_reasons[index]);
    CHECK(diagnostic.fields.occurrence_count == 1U);
    CHECK(diagnostic.provenance.risk_policy_fingerprint ==
          submission.risk_policy_fingerprint.bytes());
    CHECK(diagnostic.provenance.submission_policy_fingerprint ==
          submission.submission_policy_fingerprint.bytes());
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Verify final runtime lifecycle, source state, bounded counters, and absence of hidden failures.
void check_runtime_evidence(const M3SubmissionReplayResult& replayed) {
  const auto& evidence = replayed.evidence;
  CHECK_FALSE(evidence.fault.has_value());
  CHECK(evidence.diagnostics.empty());
  CHECK(evidence.dropped_diagnostics == 0U);
  CHECK(evidence.trace_records.size() == 6U);
  const auto runtime_trace_count = [&evidence](trace::RuntimeTraceEventKind kind) {
    return static_cast<std::size_t>(
        std::count_if(evidence.trace_records.begin(), evidence.trace_records.end(),
                      [kind](const auto& record) { return record.kind() == kind; }));
  };
  CHECK(runtime_trace_count(trace::RuntimeTraceEventKind::InputDisposition) == 1U);
  CHECK(runtime_trace_count(trace::RuntimeTraceEventKind::MarketStateTransition) == 2U);
  CHECK(runtime_trace_count(trace::RuntimeTraceEventKind::StateCallback) == 2U);
  CHECK(runtime_trace_count(trace::RuntimeTraceEventKind::MarketCallback) == 1U);
  CHECK(runtime_trace_count(trace::RuntimeTraceEventKind::ReentryDetected) == 0U);
  CHECK(evidence.executor.completed_turns == 2U);
  CHECK(evidence.executor.pending_commands == 0U);
  CHECK(evidence.executor.pending_fences == 0U);
  CHECK(evidence.executor.closed);
  CHECK_FALSE(evidence.executor.owner_bound);
  REQUIRE(evidence.sources.size() == 1U);
  const auto& source = evidence.sources.front();
  CHECK(source.source_ordinal == model::MarketSourceOrdinal::create_initial());
  CHECK(source.readiness == market_data::MarketReadiness::Ready);
  CHECK(source.active_session == model::SessionEpoch{1U});
  CHECK(source.last_source_sequence == model::SequenceNumber{100U});
  REQUIRE(source.book_identity.has_value());
  CHECK(source.book_identity->generation().value() == 1U);
  CHECK(source.book_identity->revision().value() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the optional submission bundle and prove it carries no hidden terminal fault.
  REQUIRE(evidence.submission.has_value());
  CHECK_FALSE(evidence.submission->runtime_faulted);
  CHECK_FALSE(evidence.submission->terminal_error.has_value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The complete matrix must reproduce every callback, local result, canonical byte, and cold state.
TEST_CASE("the complete M3 reference workload is byte-identical across serialized owners",
          "[m3][deterministic_scenario][runtime][submission]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Two fresh manual owners and one dedicated owner consume identical deterministic authorities.
  const auto first = execute_replay_or_throw(M3SubmissionReplayMode::Manual);
  const auto second = execute_replay_or_throw(M3SubmissionReplayMode::Manual);
  const auto dedicated = execute_replay_or_throw(M3SubmissionReplayMode::Dedicated);

  // ++++++++++++++++++++++++++++++++++++++++
  // Full structural equality includes callbacks, normalized results, both traces, diagnostics,
  // OMS rows, reservations, risk cells, fake slots, and all quiescent runtime counters.
  CHECK(first == second);
  CHECK(first == dedicated);
  CHECK(first.unrelated_callback_count == 0U);
  check_callback_sequence(first);
  check_submission_results(first);
  check_runtime_evidence(first);
  REQUIRE(first.evidence.submission.has_value());
  const auto& submission = *first.evidence.submission;
  check_submission_trace(submission);
  check_submission_diagnostics(submission);
  check_oms(submission);
  check_reservations_and_scopes(submission);
  check_fake_writes(submission);

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact byte sizes and digests pin the complete AEGISRTS and AEGISSTS schema-one streams.
  CHECK(first.evidence.canonical_trace_bytes.size() == 2'494U);
  CHECK(digest_to_hex(first.evidence.canonical_trace_digest) ==
        "4d62d2c42bce7eeea8901b1c28baf4059190a21a46747f7fd70ef8fc67d71ed7");
  CHECK(submission.canonical_trace_bytes.size() == 35'616U);
  CHECK(digest_to_hex(submission.canonical_trace_digest) ==
        "ace82ff42d02074d512f743f9c3b5d8dc040911e57031e1d438db2715780943d");
  CHECK(first.evidence.configuration_fingerprint.to_hex() ==
        "442dbeb26f2a1251f8badb9cff75e020940ad63d743e8b29175b50749793e908");
  CHECK(first.evidence.runtime_policy_fingerprint.to_hex() ==
        "78b64db91f7fc64914f8441cb5b883993c89b3f7e5b7105f3c0d3b02c74db2d5");
  CHECK(submission.risk_policy_fingerprint.to_hex() ==
        "6fc0d0121e6b51fc103be03318d8b09ce1aba2f8c7da3b6afaa4765ba958ce9e");
  CHECK(submission.submission_policy_fingerprint.to_hex() ==
        "eea132818dc108b9e0aafdcf5befc2dbc14c7332a6370b5ee22e80bae5aec737");

  // ++++++++++++++++++++++++++++++++++++++++
  // Accepted fake slots pin the exact risk-approved AEGISFOE economics independently of traces.
  constexpr std::array<std::string_view, 2U> accepted_write_digests{
      "4e9eae436a7db7e779a560ab2242e63c7816080376d8a59affbd21b2be789674",
      "b19ac0749c2aaa1a55ee14c5b088e282cf83aa24a5379f3c270533af2ad79165"};
  for (std::size_t index = 0U; index < submission.accepted_writes.size(); ++index) {
    const auto& write = submission.accepted_writes[index];
    CHECK(write.bytes.size() == 442U);
    const auto digest = model::calculate_sha256_digest(std::span<const std::byte>{write.bytes});
    CHECK(digest_to_hex(digest) == accepted_write_digests[index]);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // M3 derives a separate enabled fixture; the accepted M1 fingerprint stays byte-for-byte fixed.
  auto m1 = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());
  REQUIRE(m1);
  CHECK(m1.value().fingerprint().to_hex() ==
        "e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
