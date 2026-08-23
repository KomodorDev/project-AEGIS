// Purpose: own one strategy instance and persistent bot-bound context per configured bot, then
// dispatch coherent market/state events in canonical subscription order.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/execution/order_submission.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/organization/organization.hpp"
#include "aegis/runtime/runtime_diagnostics.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/trace/runtime_trace.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// Keep this public header on leaf order/result contracts; concrete submission composition is needed
// only by out-of-line runtime implementation and stable non-owning pointers.
class SubmissionCoordinator;

// ########################################################################

// ########################################################################
// Forward-declare the strategy owner so turn-scoped context and plan capabilities can restrict
// construction without exposing its private implementation.
class BotRuntime;

// ########################################################################
// BotContext owns immutable startup attribution and provenance for one bot. BotRuntime activates
// its callback-local observation fields; the normalized submission method fails closed when the
// observation-only composition has installed no M3 authority.
class BotContext final {
public:

  // --------------------------------------------------------
  // A bot-bound capability cannot be copied, moved, or retained as a detached authority value.
  BotContext(const BotContext&) = delete;
  BotContext& operator=(const BotContext&) = delete;
  BotContext(BotContext&&) = delete;
  BotContext& operator=(BotContext&&) = delete;

  // --------------------------------------------------------
  // Borrow the configured firm attribution for this callback.
  [[nodiscard]] const model::FirmId& firm_id() const noexcept { return attribution_.firm_id; }

  // --------------------------------------------------------
  // Borrow the configured desk attribution for this callback.
  [[nodiscard]] const model::DeskId& desk_id() const noexcept { return attribution_.desk_id; }

  // --------------------------------------------------------
  // Borrow the configured bot identity that owns the strategy instance.
  [[nodiscard]] const model::BotId& bot_id() const noexcept { return attribution_.bot_id; }

  // --------------------------------------------------------
  // Borrow the immutable strategy identity assigned to the configured bot.
  [[nodiscard]] const model::StrategyId& strategy_id() const noexcept {
    return attribution_.strategy_id;
  }

  // --------------------------------------------------------
  // Borrow the exact observation grant that caused this callback.
  [[nodiscard]] const model::SubscriptionId& subscription_id() const noexcept {
    return *subscription_id_;
  }

  // --------------------------------------------------------
  // Return the runtime-issued global callback position.
  [[nodiscard]] model::CallbackOrdinal callback_ordinal() const noexcept {
    return callback_ordinal_;
  }

  // --------------------------------------------------------
  // Borrow the sealed startup-configuration identity associated with this callback.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow the immutable runtime-policy identity associated with this callback.
  [[nodiscard]] const RuntimePolicyFingerprint& runtime_policy_fingerprint() const noexcept {
    return runtime_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Preserve one public submission vocabulary across runtime modes; the M2 composition has no
  // authority and therefore returns a definite capability rejection without consuming identity.
  [[nodiscard]] execution::SubmitResult submit(const execution::OrderRequest& request) noexcept;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only canonical dispatch can bind configured attribution to one callback identity.
  friend class BotRuntime;

  // ########################################################################

  // --------------------------------------------------------
  // Bind immutable configured identity once; callback-local values are installed only by dispatch.
  BotContext(organization::BotAttribution attribution,
             configuration::ConfigurationFingerprint configuration_fingerprint,
             RuntimePolicyFingerprint runtime_policy_fingerprint,
             SubmissionCoordinator* submission_coordinator) noexcept;

  // --------------------------------------------------------
  // Activate one persistent context immediately before its owning synchronous strategy callback.
  void activate(const model::SubscriptionId& subscription_id, model::TurnOrdinal owner_turn_ordinal,
                model::CallbackOrdinal callback_ordinal,
                model::ProcessingTimestamp processing_timestamp) noexcept;

  // --------------------------------------------------------
  // Remove callback-local observation authority before another callback can reuse the object.
  void deactivate() noexcept;

  // --------------------------------------------------------
  // Return one process-local token whose thread-local storage address distinguishes owner threads.
  [[nodiscard]] static std::uintptr_t current_thread_token() noexcept;

  // --------------------------------------------------------
  organization::BotAttribution attribution_;
  const model::SubscriptionId* subscription_id_{nullptr};
  model::TurnOrdinal owner_turn_ordinal_{model::TurnOrdinal::initial()};
  model::CallbackOrdinal callback_ordinal_{model::CallbackOrdinal::initial()};
  model::ProcessingTimestamp processing_timestamp_{0U};
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  RuntimePolicyFingerprint runtime_policy_fingerprint_;
  SubmissionCoordinator* submission_coordinator_;
  execution::SteadySubmissionMeasurementClock capability_absent_measurement_clock_;
  execution::SubmissionMeasurementClock* submission_measurement_clock_;
  std::atomic<std::uintptr_t> active_owner_token_{0U};
};

// ########################################################################
// Strategy callbacks receive prices only with a ReadyBookView and receive non-ready changes only
// through the sanitized state event. The noexcept contract makes run-to-completion explicit.
class Strategy {
public:

  // --------------------------------------------------------
  // Strategy instances have one stable owner and cannot be copied or moved between bot runtimes.
  Strategy() = default;
  Strategy(const Strategy&) = delete;
  Strategy& operator=(const Strategy&) = delete;
  Strategy(Strategy&&) = delete;
  Strategy& operator=(Strategy&&) = delete;
  virtual ~Strategy() = default;

  // --------------------------------------------------------
  // Observe one atomically committed Ready update and its coherent turn-scoped book view.
  virtual void on_market_data(const market_data::MarketEvent& event,
                              const market_data::ReadyBookView& book,
                              BotContext& context) noexcept = 0;

  // --------------------------------------------------------
  // Observe one sanitized readiness transition without access to hidden book storage.
  virtual void on_market_state(const market_data::MarketStateEvent& event,
                               BotContext& context) noexcept = 0;
};

// ########################################################################
// Startup registration transfers exactly one mutable strategy instance for one configured bot.
struct BotStrategyRegistration {
  model::BotId bot_id;
  std::unique_ptr<Strategy> strategy;
};

// ########################################################################
// A turn-scoped proof binds source fan-out and callback headroom to the exact BotRuntime that
// checked them. Callers may copy the opaque proof but cannot construct or alter one.
class BotDispatchPlan final {
public:

  // --------------------------------------------------------
  // Return the configured source whose canonical grant range was preflighted.
  [[nodiscard]] model::MarketSourceOrdinal source_ordinal() const noexcept {
    return source_ordinal_;
  }

  // --------------------------------------------------------
  // Return the exact serialized owner turn this proof authorizes.
  [[nodiscard]] model::TurnOrdinal turn_ordinal() const noexcept { return turn_ordinal_; }

  // --------------------------------------------------------
  // Return the exact number of matching configured observation grants.
  [[nodiscard]] std::uint32_t matching_subscription_count() const noexcept {
    return matching_subscription_count_;
  }

  // --------------------------------------------------------
  // Return the exact per-grant event count classified before owner mutation.
  [[nodiscard]] std::uint32_t event_count() const noexcept { return event_count_; }

  // --------------------------------------------------------
  // Return the exact callback count reserved by this proof.
  [[nodiscard]] std::uint32_t callback_count() const noexcept { return callback_count_; }

  // --------------------------------------------------------
  // Return exact callback plus possible first-reentry canonical trace headroom.
  [[nodiscard]] std::uint32_t callback_trace_record_count() const noexcept {
    return callback_trace_record_count_;
  }

  // --------------------------------------------------------
private:

  // ########################################################################
  // Interesting syntax: private construction and friendship make a successful BotRuntime
  // preflight the only authority able to mint a dispatch proof.
  friend class BotRuntime;

  // ########################################################################

  // --------------------------------------------------------
  // Capture every private freshness and owner binding only after preflight succeeds.
  BotDispatchPlan(const BotRuntime& owner, model::MarketSourceOrdinal source_ordinal,
                  model::TurnOrdinal turn_ordinal, std::uint32_t matching_subscription_count,
                  std::uint32_t event_count, std::uint32_t callback_count,
                  std::uint32_t callback_trace_record_count,
                  std::optional<model::CallbackOrdinal> callback_counter_predecessor,
                  std::uint64_t dispatch_counter_predecessor) noexcept
      : owner_{&owner}, source_ordinal_{source_ordinal}, turn_ordinal_{turn_ordinal},
        matching_subscription_count_{matching_subscription_count}, event_count_{event_count},
        callback_count_{callback_count}, callback_trace_record_count_{callback_trace_record_count},
        callback_counter_predecessor_{callback_counter_predecessor},
        dispatch_counter_predecessor_{dispatch_counter_predecessor} {}

  // --------------------------------------------------------
  const BotRuntime* owner_;
  model::MarketSourceOrdinal source_ordinal_;
  model::TurnOrdinal turn_ordinal_;
  std::uint32_t matching_subscription_count_;
  std::uint32_t event_count_;
  std::uint32_t callback_count_;
  std::uint32_t callback_trace_record_count_;
  std::optional<model::CallbackOrdinal> callback_counter_predecessor_;
  std::uint64_t dispatch_counter_predecessor_;
};

// ########################################################################

// ########################################################################
// Latched status keeps post-callback measurement and evidence faults externally observable.
// Noncanonical faults never truncate a preflighted fan-out; critical trace loss fails closed.
struct BotRuntimeStatus {
  bool canonical_trace_failure_latched{false};
  bool callback_clock_regression_latched{false};
  bool diagnostic_evidence_failure_latched{false};

  // --------------------------------------------------------
  // Report whether no post-callback fault has been latched.
  [[nodiscard]] bool healthy() const noexcept {
    return !canonical_trace_failure_latched && !callback_clock_regression_latched &&
           !diagnostic_evidence_failure_latched;
  }

  // --------------------------------------------------------
  // Structural equality lets owner-status evidence be compared without exposing mutation.
  friend bool operator==(const BotRuntimeStatus&, const BotRuntimeStatus&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Dispatch reports exact synchronous invocations and callback identities for scenario assertions.
struct BotDispatchReport {
  std::uint32_t state_callbacks;
  std::uint32_t market_callbacks;
  std::uint32_t callback_budget_exceeded;
  std::uint32_t callback_clock_regressions;
  std::uint64_t diagnostic_evidence_failures;
  std::uint64_t diagnostic_observations_dropped;
  std::optional<model::CallbackOrdinal> first_callback_ordinal;
  std::optional<model::CallbackOrdinal> last_callback_ordinal;

  // --------------------------------------------------------
  // Return the exact number of callbacks completed in this dispatch.
  [[nodiscard]] std::uint32_t callback_count() const noexcept {
    return state_callbacks + market_callbacks;
  }

  // --------------------------------------------------------
  // Structural equality supports deterministic dispatch-report assertions.
  friend bool operator==(const BotDispatchReport&, const BotDispatchReport&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Construction-only state supports deterministic callback-counter boundary qualification without
// exposing a mutable production counter after strategy dispatch begins.
struct BotRuntimeCounterSeed {
  std::optional<model::CallbackOrdinal> last_callback_ordinal;
  std::uint64_t completed_dispatch_count{0U};
};

// ########################################################################
// BotRuntime owns strategy state, canonical grant routing, callback ordinals, and dispatch re-entry
// state. It is invoked only by the serialized owner.
class BotRuntime final {
public:

  // --------------------------------------------------------
  // Validate exact bot coverage and build source/subscription routing before any callback is legal.
  [[nodiscard]] static model::Result<BotRuntime>
  create(const configuration::StartupConfiguration& configuration, const RuntimePolicy& policy,
         model::ClockProvider& measurement_clock, trace::RuntimeTraceSink& trace_sink,
         RuntimeDiagnosticSink& diagnostics, std::vector<BotStrategyRegistration> registrations,
         BotRuntimeCounterSeed counter_seed = {},
         SubmissionCoordinator* submission_coordinator = nullptr);

  // --------------------------------------------------------
  // One-time moves publish the factory result before any plan can bind to the final object address.
  BotRuntime(const BotRuntime&) = delete;
  BotRuntime& operator=(const BotRuntime&) = delete;
  BotRuntime(BotRuntime&&) noexcept = default;
  BotRuntime& operator=(BotRuntime&&) noexcept = default;

  // --------------------------------------------------------
  // Validate source routing, exact callback/trace capacity, and callback-counter headroom for the
  // classified zero-to-two event shape. This observation does not mutate BotRuntime.
  [[nodiscard]] model::Result<BotDispatchPlan> preflight(model::MarketSourceOrdinal source_ordinal,
                                                         model::TurnOrdinal turn_ordinal,
                                                         std::uint32_t event_count) const;

  // --------------------------------------------------------
  // Preserve a no-fail coordinator rollback seam. Preflight is observational, so cancellation is a
  // deliberate no-op and a failed market turn leaves BotRuntime bit-for-bit unchanged.
  void cancel(const BotDispatchPlan& plan) noexcept;

  // --------------------------------------------------------
  // Validate the exact opaque preflight proof, prepare all owned trace identities, then invoke
  // state-before-market callbacks in canonical grant order.
  [[nodiscard]] model::Result<BotDispatchReport>
  dispatch(const BotDispatchPlan& plan, const market_data::MarketTurnOutcome& outcome);

  // --------------------------------------------------------
  // Coalesce an executor owner-drive recursion attempt into the active callback's one prebuilt
  // re-entry evidence slot. Calls outside strategy execution fail without changing trace state.
  [[nodiscard]] model::Result<void> record_owner_reentry() noexcept;

  // --------------------------------------------------------
  // Report whether a synchronous callback fan-out is currently on the owner stack.
  [[nodiscard]] bool dispatch_active() const noexcept { return dispatch_active_; }

  // --------------------------------------------------------
  // Return the final global callback ordinal, absent before the first callback.
  [[nodiscard]] std::optional<model::CallbackOrdinal> last_callback_ordinal() const noexcept {
    return last_callback_ordinal_;
  }

  // --------------------------------------------------------
  // Count completed outcomes, including zero-callback outcomes, so every used plan changes one
  // predecessor exactly once.
  [[nodiscard]] constexpr std::uint64_t completed_dispatch_count() const noexcept {
    return completed_dispatch_count_;
  }

  // --------------------------------------------------------
  // Return post-callback faults that suppress all later preflight and dispatch attempts.
  [[nodiscard]] constexpr const BotRuntimeStatus& status() const noexcept { return status_; }

  // --------------------------------------------------------
private:

  // ########################################################################
  // Own one configured bot identifier and its sole mutable strategy instance.
  struct StrategyEntry {
    model::BotId bot_id;
    organization::BotAttribution attribution;
    std::unique_ptr<Strategy> strategy;
    std::unique_ptr<BotContext> context;
  };

  // ########################################################################

  // ########################################################################
  // Bind one canonical subscription and full attribution to its owning strategy and source.
  struct Grant {
    model::MarketSourceOrdinal source_ordinal;
    trace::RuntimeTraceSource trace_source;
    market_data::Subscription subscription;
    organization::BotAttribution attribution;
    Strategy* strategy;
    BotContext* context;
  };

  // ########################################################################

  // ########################################################################
  // Own validated callback and possible re-entry trace fields before strategy execution begins.
  struct PreparedCallback {
    Grant* grant;
    model::CallbackOrdinal callback_ordinal;
    trace::RuntimeTraceFields trace_fields;
    trace::RuntimeTraceFields reentry_trace_fields;
    bool state_callback;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Publish only startup-validated ownership, routing, limits, clocks, and evidence sinks.
  BotRuntime(configuration::ConfigurationFingerprint configuration_fingerprint,
             RuntimePolicyFingerprint runtime_policy_fingerprint,
             std::uint32_t maximum_callbacks_per_turn, std::uint64_t callback_budget_nanoseconds,
             model::ClockProvider& measurement_clock, trace::RuntimeTraceSink& trace_sink,
             RuntimeDiagnosticSink& diagnostics, std::vector<StrategyEntry> strategies,
             std::vector<Grant> grants, std::vector<std::size_t> source_offsets,
             BotRuntimeCounterSeed counter_seed)
      : configuration_fingerprint_{std::move(configuration_fingerprint)},
        runtime_policy_fingerprint_{std::move(runtime_policy_fingerprint)},
        maximum_callbacks_per_turn_{maximum_callbacks_per_turn},
        callback_budget_nanoseconds_{callback_budget_nanoseconds},
        measurement_clock_{&measurement_clock}, trace_sink_{&trace_sink},
        diagnostics_{&diagnostics}, strategies_{std::move(strategies)}, grants_{std::move(grants)},
        source_offsets_{std::move(source_offsets)},
        last_callback_ordinal_{counter_seed.last_callback_ordinal},
        completed_dispatch_count_{counter_seed.completed_dispatch_count} {
    prepared_callbacks_.reserve(maximum_callbacks_per_turn_);
  }

  // --------------------------------------------------------
  // Map any latched post-callback fault to one stable fail-closed domain error.
  [[nodiscard]] model::Result<void> require_healthy() const;

  // --------------------------------------------------------
  // Validate owner identity, freshness, source fan-out, and callback predecessor without changing
  // the supplied opaque plan.
  [[nodiscard]] model::Result<void> validate_plan(const BotDispatchPlan& plan) const;

  // --------------------------------------------------------
  // Coalesce one or more nested attempts into bounded canonical and diagnostic evidence.
  [[nodiscard]] model::Result<BotDispatchReport> reject_reentry();

  // --------------------------------------------------------
  // Record one owner- or dispatch-reentry attempt using the shared callback-local reservation and
  // preserve the first diagnostic kind when attempts are mixed.
  [[nodiscard]] model::Result<void> record_reentry(bool owner_reentry) noexcept;

  // --------------------------------------------------------
  // Append a factory-shaped callback diagnostic and latch evidence failure without interrupting
  // the current preflighted fan-out.
  void append_callback_diagnostic(RuntimeDiagnosticKind kind, const RuntimeDiagnosticFields& fields,
                                  BotDispatchReport& report) noexcept;

  // --------------------------------------------------------
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  RuntimePolicyFingerprint runtime_policy_fingerprint_;
  std::uint32_t maximum_callbacks_per_turn_;
  std::uint64_t callback_budget_nanoseconds_;
  model::ClockProvider* measurement_clock_;
  trace::RuntimeTraceSink* trace_sink_;
  RuntimeDiagnosticSink* diagnostics_;
  std::vector<StrategyEntry> strategies_;
  std::vector<Grant> grants_;
  std::vector<std::size_t> source_offsets_;
  std::vector<PreparedCallback> prepared_callbacks_;
  std::optional<model::CallbackOrdinal> last_callback_ordinal_;
  BotRuntimeStatus status_;
  std::uint64_t completed_dispatch_count_{0U};
  bool dispatch_active_{false};
  Grant* active_grant_{nullptr};
  PreparedCallback* active_prepared_callback_{nullptr};
  std::optional<model::TurnOrdinal> active_turn_ordinal_;
  std::optional<model::CallbackOrdinal> active_callback_ordinal_;
  bool active_reentry_traced_{false};
  bool active_reentry_trace_failed_{false};
  std::uint64_t active_reentry_attempts_{0U};
  std::optional<RuntimeDiagnosticKind> active_reentry_diagnostic_kind_;
};

// ########################################################################

} // namespace aegis::runtime
