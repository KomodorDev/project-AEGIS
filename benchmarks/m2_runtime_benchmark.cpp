// Purpose: measure the three named M2 executor, callback, and coherent market-owner workloads with
// explicit latency distributions and scoped C++ heap-allocation counts.

#include "aegis/runtime/market_runtime.hpp"
#include "reference_configuration.hpp"
#include "support/allocation_tracking.hpp"
#include "support/distribution_metrics.hpp"

#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// Fixed iteration counts make p99.9 meaningful and bound the trace capacity needed by each fresh
// benchmark fixture, independently of ambient benchmark-runner timing heuristics.
constexpr std::int64_t distribution_iterations = 10'000;
constexpr std::uint32_t distribution_trace_capacity = 20'016U;
constexpr std::size_t retained_levels_per_side = 20U;

// ########################################################################
// The probe transfers copied callback observations to the outer benchmark loop without retaining
// any turn-scoped event, book view, or context.
struct CallbackMeasurementProbe {
  std::uint64_t callback_count{0U};
  std::uint64_t state_callback_count{0U};
  std::uint64_t checksum{0U};
  std::size_t last_bid_count{0U};
  std::size_t last_ask_count{0U};
  std::optional<market_data::MarketUpdateKind> last_update_kind;

  // --------------------------------------------------------
  // Remove setup callback counts before either workload begins its measured iterations.
  void reset_callback_observations() noexcept {
    callback_count = 0U;
    state_callback_count = 0U;
  }

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// A trivially copied command gives BENCH-M2-EXEC-001 no behavior beyond proving handler invocation.
struct NoopCommand {
  std::uint64_t token;
};

// ########################################################################

// --------------------------------------------------------
// Fail immediately when a benchmark fixture contains an invalid nominal identifier literal.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M2 benchmark fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Execute one copied no-op command while keeping its invocation observable to the optimizer.
[[nodiscard]] model::Result<void>
execute_noop_command(const NoopCommand& command,
                     const runtime::AcceptedTurnContext& context) noexcept {
  auto token = command.token;
  benchmark::DoNotOptimize(token);
  benchmark::DoNotOptimize(context.turn_ordinal.value());
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Encode the sealed M1 and M2 identities in the exact grammar consumed by evidence tooling.
[[nodiscard]] std::string
create_benchmark_evidence_label(const configuration::StartupConfiguration& configuration,
                                const runtime::RuntimePolicy& policy) {
  return "configuration_fingerprint_sha256=" + configuration.fingerprint().to_hex() +
         ";runtime_policy_fingerprint_sha256=" + policy.fingerprint().to_hex();
}

// ########################################################################
// This injected measurement clock brackets the exact strategy call made by BotRuntime. Its first
// observation occurs immediately before virtual dispatch and its second immediately after return.
class CallbackMeasurementClock final : public model::ClockProvider {
public:

  // --------------------------------------------------------
  // Keep a stable local origin so every returned clock value is unsigned and process-local.
  CallbackMeasurementClock() noexcept : origin_{std::chrono::steady_clock::now()} {}

  // --------------------------------------------------------
  // Request measurement of exactly one complete callback invocation.
  void arm_callback_measurement() noexcept {
    armed_ = true;
    active_ = false;
    completed_ = false;
    duration_nanoseconds_ = 0U;
    allocation_count_ = 0U;
  }

  // --------------------------------------------------------
  // Report whether both observations enclosing the requested callback were received.
  [[nodiscard]] bool is_completed() const noexcept { return completed_; }

  // --------------------------------------------------------
  // Return the measured immediately-before to immediately-after callback duration.
  [[nodiscard]] std::uint64_t duration_nanoseconds() const noexcept {
    return duration_nanoseconds_;
  }

  // --------------------------------------------------------
  // Return successful C++ heap allocations in the same callback-only interval.
  [[nodiscard]] std::uint64_t allocation_count() const noexcept { return allocation_count_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Turn consecutive armed observations into the callback start and finish boundaries while
  // unarmed observations still provide the monotonic clock required by ordinary runtime setup.
  [[nodiscard]] std::uint64_t monotonic_nanoseconds() noexcept override {
    if (armed_ && !active_) {
      aegis_benchmark_support::allocation_tracking::begin_allocation_interval();
      started_ = std::chrono::steady_clock::now();
      active_ = true;
      return calculate_nanoseconds_since_origin(started_);
    }

    const auto observed = std::chrono::steady_clock::now();
    if (armed_ && active_) {
      duration_nanoseconds_ =
          aegis_benchmark_support::calculate_elapsed_nanoseconds(started_, observed);
      allocation_count_ =
          aegis_benchmark_support::allocation_tracking::finish_allocation_interval();
      completed_ = true;
      armed_ = false;
      active_ = false;
    }
    return calculate_nanoseconds_since_origin(observed);
  }

  // --------------------------------------------------------
  // Convert one steady observation into the provider's nonnegative local clock domain.
  [[nodiscard]] std::uint64_t calculate_nanoseconds_since_origin(
      std::chrono::steady_clock::time_point observed) const noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(observed - origin_).count();
    return static_cast<std::uint64_t>(elapsed);
  }

  // --------------------------------------------------------
  std::chrono::steady_clock::time_point origin_;
  std::chrono::steady_clock::time_point started_{};
  bool armed_{false};
  bool active_{false};
  bool completed_{false};
  std::uint64_t duration_nanoseconds_{0U};
  std::uint64_t allocation_count_{0U};
};

// ########################################################################

// --------------------------------------------------------
// Mix one exact unsigned observation into the benchmark-local deterministic strategy checksum.
[[nodiscard]] std::uint64_t mix_checksum(std::uint64_t checksum, std::uint64_t value) noexcept {
  return checksum ^ (value + 0x9e3779b97f4a7c15ULL + (checksum << 6U) + (checksum >> 2U));
}

// ########################################################################
// The observe-only reference strategy consumes the complete coherent 20x20 view without retaining
// a turn-scoped event, view, or context after synchronous return.
class DeterministicReferenceStrategy final : public runtime::Strategy {
public:

  // --------------------------------------------------------
  // Borrow one probe whose lifetime encloses the strategy and owning market runtime.
  explicit DeterministicReferenceStrategy(CallbackMeasurementProbe& probe) noexcept
      : probe_{&probe} {}

  // --------------------------------------------------------
  // Consume every visible level in canonical order and publish only copied scalar observations.
  void on_market_data(const market_data::MarketEvent& event, const market_data::ReadyBookView& book,
                      runtime::BotContext& context) noexcept override {

    // ++++++++++++++++++++++++++++++++++++++++
    // Read the complete coherent book and attribution into a stable, allocation-free checksum.
    auto checksum = mix_checksum(probe_->checksum, event.update().source_sequence().value());
    checksum = mix_checksum(checksum, event.context().book_generation.value());
    checksum = mix_checksum(checksum, event.context().book_revision.value());
    checksum = mix_checksum(checksum, context.callback_ordinal().value());
    checksum = mix_checksum(checksum,
                            static_cast<std::uint64_t>(context.subscription_id().value().size()));
    checksum = mix_checksum(checksum, static_cast<std::uint64_t>(book.bid_count()));
    checksum = mix_checksum(checksum, static_cast<std::uint64_t>(book.ask_count()));
    for (std::size_t index = 0U; index < book.bid_count(); ++index) {
      const auto level = book.bid_at(index);
      if (level) {
        checksum = mix_checksum(checksum, static_cast<std::uint64_t>(level->price.coefficient()));
        checksum =
            mix_checksum(checksum, static_cast<std::uint64_t>(level->quantity.coefficient()));
      }
    }
    for (std::size_t index = 0U; index < book.ask_count(); ++index) {
      const auto level = book.ask_at(index);
      if (level) {
        checksum = mix_checksum(checksum, static_cast<std::uint64_t>(level->price.coefficient()));
        checksum =
            mix_checksum(checksum, static_cast<std::uint64_t>(level->quantity.coefficient()));
      }
    }
    probe_->checksum = checksum;
    probe_->last_bid_count = book.bid_count();
    probe_->last_ask_count = book.ask_count();
    probe_->last_update_kind = event.update().kind();
    ++probe_->callback_count;

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Fold sanitized readiness into the same scalar sink without receiving book access.
  void on_market_state(const market_data::MarketStateEvent& event,
                       runtime::BotContext& context) noexcept override {
    probe_->checksum =
        mix_checksum(probe_->checksum, static_cast<std::uint64_t>(event.fields().readiness));
    probe_->checksum = mix_checksum(probe_->checksum, context.callback_ordinal().value());
    ++probe_->state_callback_count;
  }

  // --------------------------------------------------------
private:
  CallbackMeasurementProbe* probe_;
};

// ########################################################################

// --------------------------------------------------------
// Seal the accepted credential-free reference configuration before constructing M2 policy.
[[nodiscard]] configuration::StartupConfiguration create_reference_configuration_or_throw() {
  auto created = configuration::StartupConfiguration::create_startup_configuration(
      test_support::create_reference_configuration_params_or_throw());
  if (!created) {
    throw std::logic_error{"invalid startup configuration in M2 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Define the sole public BTC perpetual source used by both runtime workloads.
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
// Fix all runtime capacities once, including enough canonical trace space for every fixed sample.
[[nodiscard]] runtime::RuntimePolicy
create_reference_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{1U, 4'096U, 64U, 20U, 1'000'000'000U, 2U, 32U,
                                       distribution_trace_capacity, 32U, 1'000'000'000U},
          {create_reference_source_or_throw()},
      });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M2 benchmark fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Append one half-tick BTC price without binary floating-point conversion.
void append_half_tick_price(std::string& frame, std::uint32_t half_ticks) {
  frame.append(std::to_string(half_ticks / 2U));
  frame.append(half_ticks % 2U == 0U ? ".0" : ".5");
}

// --------------------------------------------------------
// Build the authoritative 20-bid/20-ask snapshot entirely outside timed benchmark regions.
[[nodiscard]] std::string create_snapshot_frame() {
  std::string frame =
      "AEGISMD|1|source.deribit-btc-perpetual|snapshot|100|none|1000|1|ok:bench-20x20|40";
  frame.reserve(1'024U);
  for (std::uint32_t index = 0U; index < retained_levels_per_side; ++index) {
    frame.append("|B,");
    append_half_tick_price(frame, 100'000U - index);
    frame.push_back(',');
    frame.append(std::to_string(index + 1U));
  }
  for (std::uint32_t index = 0U; index < retained_levels_per_side; ++index) {
    frame.append("|A,");
    append_half_tick_price(frame, 100'001U + index);
    frame.push_back(',');
    frame.append(std::to_string(index + 1U));
  }
  return frame;
}

// --------------------------------------------------------
// Build one valid absolute delta while preserving the fixed 20x20 depth and top-level identities.
[[nodiscard]] std::string create_delta_frame(std::uint64_t sequence) {
  const auto predecessor = sequence - 1U;
  const auto bid_quantity = sequence % 2U == 0U ? 23U : 21U;
  const auto ask_quantity = sequence % 2U == 0U ? 24U : 22U;
  return "AEGISMD|1|source.deribit-btc-perpetual|delta|" + std::to_string(sequence) + "|" +
         std::to_string(predecessor) + "|" + std::to_string(1'000U + sequence) +
         "|1|ok:bench-delta|2|B,50000.0," + std::to_string(bid_quantity) + "|A,50000.5," +
         std::to_string(ask_quantity);
}

// --------------------------------------------------------
// Transfer one bounded recorded frame into a caller-owned credential-free ingress attempt.
[[nodiscard]] market_data::IngressFrameAttempt create_ingress_attempt_or_throw(std::string frame) {
  auto created = market_data::IngressFrameAttempt::create_ingress_frame_attempt(
      parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
      model::SessionEpoch{1U}, std::move(frame));
  if (!created) {
    throw std::logic_error{"invalid recorded frame in M2 benchmark fixture"};
  }
  return std::move(created).value();
}

// ########################################################################
// One prebuilt runtime retains the fixed book and admits a fresh contiguous delta before each
// measured owner turn; fixture construction, bootstrap, snapshot, and frame creation stay untimed.
class MarketBenchmarkHarness final {
public:

  // --------------------------------------------------------
  // Construct and bootstrap the single-firm, single-bot reference runtime at a coherent 20x20 book.
  MarketBenchmarkHarness() : executor_clock_{100U} {

    // ++++++++++++++++++++++++++++++++++++++++
    // Seal immutable configuration and policy before transferring strategy ownership.
    auto configuration = create_reference_configuration_or_throw();
    auto policy = create_reference_policy_or_throw(configuration);
    evidence_label_ = create_benchmark_evidence_label(configuration, policy);
    std::vector<runtime::BotStrategyRegistration> strategies;
    strategies.push_back(runtime::BotStrategyRegistration{
        parse_identifier_or_throw<model::BotId>("bot.deribit-btc-perpetual-reference"),
        std::make_unique<DeterministicReferenceStrategy>(probe_),
    });
    auto created = runtime::MarketRuntime::create_market_runtime(
        std::move(configuration), std::move(policy), executor_clock_, callback_measurement_clock_,
        std::move(strategies));
    if (!created) {
      throw std::logic_error{"invalid market runtime in M2 benchmark fixture"};
    }
    runtime_ = std::move(created).value();

    // ++++++++++++++++++++++++++++++++++++++++
    // Run genuine bootstrap and snapshot turns before publishing the reusable benchmark fixture.
    if (!runtime_->bind_to_current_thread()) {
      throw std::logic_error{"failed to bind M2 benchmark runtime"};
    }
    const auto bootstrap = runtime_->execute_next_turn();
    if (!bootstrap || !bootstrap.value()) {
      throw std::logic_error{"failed to bootstrap M2 benchmark runtime"};
    }
    require_accepted_frame(create_snapshot_frame());
    const auto snapshot = runtime_->execute_next_turn();
    if (!snapshot || !snapshot.value() || probe_.last_bid_count != retained_levels_per_side ||
        probe_.last_ask_count != retained_levels_per_side) {
      throw std::logic_error{"failed to establish the M2 benchmark 20x20 book"};
    }
    probe_.reset_callback_observations();

    // ++++++++++++++++++++++++++++++++++++++++
  }

  // --------------------------------------------------------
  // Close and release deterministic ownership before the runtime destroys borrowed clock state.
  ~MarketBenchmarkHarness() {
    runtime_->close();
    static_cast<void>(runtime_->release_from_current_thread());
  }

  // --------------------------------------------------------
  // Admit the next contiguous delta outside its later owner-execution measurement interval.
  void require_accepted_delta(std::uint64_t sequence) {
    require_accepted_frame(create_delta_frame(sequence));
  }

  // --------------------------------------------------------
  // Execute one already-admitted delta through the shared serialized owner.
  [[nodiscard]] model::Result<std::optional<runtime::TurnReport>> execute_admitted_delta_turn() {
    return runtime_->execute_next_turn();
  }

  // --------------------------------------------------------
  // Borrow the scalar-only callback probe used to verify the coherent strategy observation.
  [[nodiscard]] CallbackMeasurementProbe& callback_probe() noexcept { return probe_; }

  // --------------------------------------------------------
  // Borrow the injected clock that brackets one callback at BotRuntime's invocation boundary.
  [[nodiscard]] CallbackMeasurementClock& callback_measurement_clock() noexcept {
    return callback_measurement_clock_;
  }

  // --------------------------------------------------------
  // Borrow the immutable configuration/runtime-policy identity attached to this workload record.
  [[nodiscard]] const std::string& evidence_label() const noexcept { return evidence_label_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Require one nonblocking accepted decision while setup remains outside workload timing.
  void require_accepted_frame(std::string frame) {
    auto admitted = runtime_->try_admit(create_ingress_attempt_or_throw(std::move(frame)));
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted) {
      throw std::logic_error{"failed to admit M2 benchmark frame"};
    }
  }

  // --------------------------------------------------------
  CallbackMeasurementProbe probe_;
  model::DeterministicClockProvider executor_clock_;
  CallbackMeasurementClock callback_measurement_clock_;
  std::string evidence_label_;
  std::unique_ptr<runtime::MarketRuntime> runtime_;
};

// ########################################################################

// --------------------------------------------------------
// BENCH-M2-EXEC-001 measures successful admission plus one completed no-op serialized owner turn.
void benchmark_executor_turn_or_throw(benchmark::State& state) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal common provenance, then construct, preallocate, and bind the executor before timing.
  auto configuration = create_reference_configuration_or_throw();
  auto policy = create_reference_policy_or_throw(configuration);
  state.SetLabel(create_benchmark_evidence_label(configuration, policy));
  model::DeterministicClockProvider clock{100U};
  runtime::SerializedExecutor executor{policy.limits().ingress_capacity, clock,
                                       policy.limits().maximum_drive_turns};
  if (!executor.bind_to_current_thread()) {
    state.SkipWithError("failed to bind BENCH-M2-EXEC-001 executor");
    return;
  }
  std::uint64_t token = 0U;
  std::uint64_t allocation_count = 0U;
  std::uint64_t total_nanoseconds = 0U;

  // ++++++++++++++++++++++++++++++++++++++++
  // Require successful admission first, then time only owner execution through turn completion.
  for ([[maybe_unused]] const auto iteration : state) {
    ++token;
    auto admitted = executor.try_admit(
        runtime::InlineCommandWorkItem::create_inline_command_work_item<execute_noop_command>(
            NoopCommand{token}));
    if (!admitted || admitted.value().outcome != runtime::AdmissionOutcome::Accepted) {
      state.SkipWithError("BENCH-M2-EXEC-001 admission did not succeed");
      break;
    }
    aegis_benchmark_support::allocation_tracking::begin_allocation_interval();
    const auto started = std::chrono::steady_clock::now();
    auto completed = executor.execute_next_turn();
    const auto finished = std::chrono::steady_clock::now();
    allocation_count += aegis_benchmark_support::allocation_tracking::finish_allocation_interval();
    const auto duration = aegis_benchmark_support::calculate_elapsed_nanoseconds(started, finished);
    total_nanoseconds += duration;
    state.SetIterationTime(aegis_benchmark_support::nanoseconds_to_seconds(duration));
    if (!completed || !completed.value()) {
      state.SkipWithError("BENCH-M2-EXEC-001 turn did not complete");
      break;
    }
    benchmark::DoNotOptimize(completed.value()->completed_turns);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the exact required units using the completed iteration count.
  const auto iterations = static_cast<double>(state.iterations());
  if (iterations != 0.0) {
    state.counters["ns_per_turn"] = static_cast<double>(total_nanoseconds) / iterations;
    state.counters["turns_per_second"] =
        benchmark::Counter{iterations, benchmark::Counter::kIsRate};
    state.counters["allocations_per_turn"] = static_cast<double>(allocation_count) / iterations;
    state.counters["sample_count"] = iterations;
    state.SetItemsProcessed(state.iterations());
  }
  static_cast<void>(executor.release_from_current_thread());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// BENCH-M2-CALLBACK-001 reports only entry-to-return strategy time on each coherent Ready view.
void benchmark_reference_callback_or_throw(benchmark::State& state) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the runtime and fixed book once, then reserve all percentile storage before measurement.
  MarketBenchmarkHarness harness;
  state.SetLabel(harness.evidence_label());
  std::vector<std::uint64_t> samples;
  samples.reserve(static_cast<std::size_t>(distribution_iterations));
  std::uint64_t allocation_count = 0U;
  std::uint64_t sequence = 101U;

  // ++++++++++++++++++++++++++++++++++++++++
  // Let the owner prepare and invoke each callback; the injected measurement clock brackets the
  // virtual strategy call and excludes parsing, book mutation, trace preparation, and dispatch.
  for ([[maybe_unused]] const auto iteration : state) {
    harness.require_accepted_delta(sequence);
    const auto callback_count_before = harness.callback_probe().callback_count;
    const auto state_callback_count_before = harness.callback_probe().state_callback_count;
    harness.callback_measurement_clock().arm_callback_measurement();
    auto completed = harness.execute_admitted_delta_turn();
    if (!completed || !completed.value()) {
      state.SkipWithError("BENCH-M2-CALLBACK-001 owner turn did not complete");
      break;
    }
    if (!harness.callback_measurement_clock().is_completed()) {
      state.SkipWithError("BENCH-M2-CALLBACK-001 callback measurement did not close");
      break;
    }
    if (harness.callback_probe().callback_count != callback_count_before + 1U ||
        harness.callback_probe().state_callback_count != state_callback_count_before ||
        harness.callback_probe().last_update_kind != market_data::MarketUpdateKind::Delta ||
        harness.callback_probe().last_bid_count != retained_levels_per_side ||
        harness.callback_probe().last_ask_count != retained_levels_per_side) {
      state.SkipWithError("BENCH-M2-CALLBACK-001 strategy did not observe one 20x20 view");
      break;
    }
    const auto duration = harness.callback_measurement_clock().duration_nanoseconds();
    samples.push_back(duration);
    allocation_count += harness.callback_measurement_clock().allocation_count();
    state.SetIterationTime(aegis_benchmark_support::nanoseconds_to_seconds(duration));
    benchmark::DoNotOptimize(harness.callback_probe().checksum);
    ++sequence;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the required callback percentiles, rate, and callback-local allocation mean.
  aegis_benchmark_support::publish_latency_distribution(
      state, samples, allocation_count, "callbacks_per_second", "allocations_per_callback");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// BENCH-M2-MD-001 measures one complete owner turn from delta execution through callback return.
void benchmark_market_data_turn_or_throw(benchmark::State& state) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish the fixed 20x20 book and preallocate percentile storage before timed owner turns.
  MarketBenchmarkHarness harness;
  state.SetLabel(harness.evidence_label());
  std::vector<std::uint64_t> samples;
  samples.reserve(static_cast<std::size_t>(distribution_iterations));
  std::uint64_t allocation_count = 0U;
  std::uint64_t sequence = 101U;

  // ++++++++++++++++++++++++++++++++++++++++
  // Admission stays outside the specified interval; time the full synchronous owner turn that
  // parses, applies, commits, traces, dispatches one callback, and returns its completion report.
  for ([[maybe_unused]] const auto iteration : state) {
    harness.require_accepted_delta(sequence);
    const auto callback_count_before = harness.callback_probe().callback_count;
    const auto state_callback_count_before = harness.callback_probe().state_callback_count;
    aegis_benchmark_support::allocation_tracking::begin_allocation_interval();
    const auto started = std::chrono::steady_clock::now();
    auto completed = harness.execute_admitted_delta_turn();
    const auto finished = std::chrono::steady_clock::now();
    allocation_count += aegis_benchmark_support::allocation_tracking::finish_allocation_interval();
    const auto duration = aegis_benchmark_support::calculate_elapsed_nanoseconds(started, finished);
    if (!completed || !completed.value() ||
        harness.callback_probe().callback_count != callback_count_before + 1U ||
        harness.callback_probe().state_callback_count != state_callback_count_before ||
        harness.callback_probe().last_update_kind != market_data::MarketUpdateKind::Delta ||
        harness.callback_probe().last_bid_count != retained_levels_per_side ||
        harness.callback_probe().last_ask_count != retained_levels_per_side) {
      state.SkipWithError("BENCH-M2-MD-001 owner turn did not commit one 20x20 callback");
      break;
    }
    samples.push_back(duration);
    state.SetIterationTime(aegis_benchmark_support::nanoseconds_to_seconds(duration));
    benchmark::DoNotOptimize(harness.callback_probe().checksum);
    ++sequence;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the required full-turn percentiles, rate, and owner-interval allocation mean.
  aegis_benchmark_support::publish_latency_distribution(
      state, samples, allocation_count, "events_per_second", "allocations_per_event");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Register the fixed-size executor workload with explicit manual nanosecond timing.
BENCHMARK(benchmark_executor_turn_or_throw)
    ->Name("BENCH-M2-EXEC-001/executor.admit-and-run-noop")
    ->Iterations(distribution_iterations)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

// --------------------------------------------------------
// Register callback-only timing with enough fixed samples for a stable p99.9 observation.
BENCHMARK(benchmark_reference_callback_or_throw)
    ->Name("BENCH-M2-CALLBACK-001/strategy.reference-ready-view")
    ->Iterations(distribution_iterations)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// --------------------------------------------------------
// Register the full synchronous 20x20 market owner-turn workload.
BENCHMARK(benchmark_market_data_turn_or_throw)
    ->Name("BENCH-M2-MD-001/market.delta-20x20-and-callback")
    ->Iterations(distribution_iterations)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);

// --------------------------------------------------------
