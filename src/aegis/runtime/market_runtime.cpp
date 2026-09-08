// Purpose: compose bounded recorded ingress, transactional market state, canonical strategy
// dispatch, and optional fake submission/private retention on either serialized executor driver.

#include "aegis/runtime/market_runtime.hpp"

#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/model/domain_error.hpp"
#include "private_order_reconciler.hpp"
#include "submission_coordinator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// Diagnostic detail namespaces keep parser compatibility codes distinct from domain validation
// codes while retaining each assigned low sixteen-bit value unchanged.
inline constexpr std::uint32_t parser_detail_namespace = 0x0001'0000U;
inline constexpr std::uint32_t domain_detail_namespace = 0x0002'0000U;

// ########################################################################

// --------------------------------------------------------
// Build one stable coordinator failure without leaking untrusted fixture text.
template <typename Value>
[[nodiscard]] model::Result<Value> create_market_runtime_failure_result(DomainErrorCode code,
                                                                        std::string field) {
  return model::Result<Value>::create_failure(DomainError::create_at_field(code, std::move(field)));
}

// --------------------------------------------------------
// Encode an assigned parser failure in its non-overlapping diagnostic namespace.
[[nodiscard]] constexpr std::uint32_t
parser_detail(market_data::RecordedFixtureParseCode code) noexcept {
  return parser_detail_namespace | static_cast<std::uint32_t>(code);
}

// --------------------------------------------------------
// Encode an assigned domain failure in its non-overlapping diagnostic namespace.
[[nodiscard]] constexpr std::uint32_t domain_detail(DomainErrorCode code) noexcept {
  return domain_detail_namespace | static_cast<std::uint32_t>(code);
}

// --------------------------------------------------------
// Unsupported schema and message kinds remain distinct from malformed fixture syntax.
[[nodiscard]] constexpr bool is_unsupported(market_data::RecordedFixtureParseCode code) noexcept {
  return code == market_data::RecordedFixtureParseCode::UnsupportedVersion ||
         code == market_data::RecordedFixtureParseCode::UnsupportedMessageType;
}

// --------------------------------------------------------
// Project the accepted executor authority into the narrower market-state context.
[[nodiscard]] constexpr market_data::AcceptedMarketTurnContext
accepted_market_turn_context_from_accepted_turn(const AcceptedTurnContext& context) noexcept {
  return market_data::AcceptedMarketTurnContext{context.receipt.attempt_ordinal,
                                                context.turn_ordinal, context.processing_timestamp};
}

// --------------------------------------------------------
// Project owner-control authority without fabricating an admission or receive identity.
[[nodiscard]] constexpr market_data::OwnerMarketTurnContext
owner_market_turn_context_from_control_turn(const ControlTurnContext& context) noexcept {
  return market_data::OwnerMarketTurnContext{context.turn_ordinal, context.processing_timestamp};
}

// --------------------------------------------------------
// Bootstrap and resynchronization commands use accepted scheduling but owner-control semantics.
[[nodiscard]] constexpr market_data::OwnerMarketTurnContext
owner_market_turn_context_from_accepted_turn(const AcceptedTurnContext& context) noexcept {
  return market_data::OwnerMarketTurnContext{context.turn_ordinal, context.processing_timestamp};
}

// --------------------------------------------------------
// Read source attribution uniformly across the closed normalized command variant.
[[nodiscard]] model::MarketSourceOrdinal
command_source_ordinal(const market_data::NormalizedRecordedMarketCommand& command) noexcept {
  return std::visit(
      [](const auto& value) {
        if constexpr (requires { value.source(); }) {
          return value.source().source_ordinal();
        } else {
          return value.source.source_ordinal();
        }
      },
      command);
}

// --------------------------------------------------------
// Preserve the newest complete report when deterministic and dedicated observations meet.
[[nodiscard]] std::optional<TurnReport> newer_report(std::optional<TurnReport> first,
                                                     const std::optional<TurnReport>& second) {
  if (!second || (first && first->completed_turns >= second->completed_turns)) {
    return first;
  }
  return second;
}

// --------------------------------------------------------
// Map BotRuntime's published fail-closed precedence without reading its mutable owner state.
[[nodiscard]] std::optional<DomainError> published_bot_fault(const BotRuntimeStatus& status) {
  if (status.canonical_trace_failure_latched) {
    return DomainError::create_at_field(DomainErrorCode::RuntimeEvidenceExhausted,
                                        "market_runtime.canonical_trace_evidence");
  }
  if (status.callback_clock_regression_latched) {
    return DomainError::create_at_field(DomainErrorCode::InvalidTimestampOrder,
                                        "market_runtime.callback_clock_regression");
  }
  if (status.diagnostic_evidence_failure_latched) {
    return DomainError::create_at_field(DomainErrorCode::RuntimeEvidenceExhausted,
                                        "market_runtime.diagnostic_evidence");
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Canonical sink exhaustion becomes the single runtime-level fail-closed error promised by M2.
[[nodiscard]] DomainError runtime_error_from_turn_error(const DomainError& error) {
  if (error.code == DomainErrorCode::TraceCapacityExceeded) {
    return DomainError::create_at_field(DomainErrorCode::RuntimeEvidenceExhausted,
                                        "market_runtime.canonical_trace_evidence");
  }
  return error;
}

// --------------------------------------------------------

// ########################################################################
// ExactBotDispatchPreflight converts the market-data-neutral classified turn shape into the sole
// BotRuntime plan that can authorize callback counters and trace headroom before domain commit.
class ExactBotDispatchPreflight final : public market_data::MarketTurnPreflightAuthority {
public:

  // --------------------------------------------------------
  // Borrow the owner-local bot runtime for exactly one state-machine call.
  explicit ExactBotDispatchPreflight(BotRuntime& bot_runtime) noexcept
      : bot_runtime_{&bot_runtime} {}

  // --------------------------------------------------------
  // Mint exactly one plan and cross-check every shared fan-out count before authorizing commit.
  [[nodiscard]] model::Result<void>
  authorize_market_turn(const market_data::MarketTurnPreflight& request) override {
    if (plan_) {
      return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidRelationship,
                                                        "market_runtime.preflight_repeated");
    }
    auto planned = bot_runtime_->preflight_dispatch_callbacks(
        request.source_ordinal, request.turn_ordinal, request.event_count);
    if (!planned) {
      return model::Result<void>::create_failure(planned.error());
    }
    const auto& value = planned.value();
    const auto recomputed_trace_records =
        static_cast<std::uint64_t>(request.state_trace_record_count) +
        request.callback_trace_record_count;
    if (value.source_ordinal() != request.source_ordinal ||
        value.turn_ordinal() != request.turn_ordinal ||
        value.event_count() != request.event_count ||
        value.matching_subscription_count() != request.matching_subscription_count ||
        value.callback_count() != request.callback_count ||
        value.callback_trace_record_count() != request.callback_trace_record_count ||
        recomputed_trace_records != request.total_trace_record_count) {
      return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidRelationship,
                                                        "market_runtime.preflight_shape");
    }
    plan_.emplace(std::move(planned).value());
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Borrow the plan whose successful authorization is a prerequisite of every successful outcome.
  [[nodiscard]] const BotDispatchPlan& plan() const noexcept { return *plan_; }

  // --------------------------------------------------------
  // Preserve an explicit rollback seam when later trace validation rejects before domain commit.
  void cancel_dispatch_plan() noexcept {
    if (plan_) {
      bot_runtime_->cancel_dispatch_callbacks(*plan_);
    }
  }

  // --------------------------------------------------------
private:
  BotRuntime* bot_runtime_;
  std::optional<BotDispatchPlan> plan_;
};

// ########################################################################

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Construct immutable provenance and fixed sinks before any stable-address command can be queued.
MarketRuntime::MarketRuntime(configuration::StartupConfiguration configuration,
                             RuntimePolicy policy, model::ClockProvider& executor_clock,
                             model::ClockProvider& callback_measurement_clock)
    : configuration_{std::move(configuration)}, policy_{std::move(policy)},
      executor_clock_{&executor_clock}, callback_measurement_clock_{&callback_measurement_clock},
      trace_sink_{policy_}, diagnostics_{policy_} {
  // Interesting syntax: retaining and explicitly observing these non-owning handles documents that
  // both injected clocks must enclose the complete runtime lifetime.
  static_cast<void>(executor_clock_);
  static_cast<void>(callback_measurement_clock_);
}

// --------------------------------------------------------
// Preserve the observation-only M2 composition by installing no submission coordinator.
model::Result<std::unique_ptr<MarketRuntime>>
MarketRuntime::create_market_runtime(configuration::StartupConfiguration configuration,
                                     RuntimePolicy policy, model::ClockProvider& executor_clock,
                                     model::ClockProvider& callback_measurement_clock,
                                     std::vector<BotStrategyRegistration> strategies) {
  return create_market_runtime_composition(std::move(configuration), std::move(policy),
                                           executor_clock, callback_measurement_clock,
                                           std::move(strategies), std::nullopt);
}

// --------------------------------------------------------
// Install only the concrete validated in-memory fake stack named by the M3 composition boundary.
model::Result<std::unique_ptr<MarketRuntime>> MarketRuntime::create_with_fake_submission(
    configuration::StartupConfiguration configuration, RuntimePolicy policy,
    model::ClockProvider& executor_clock, model::ClockProvider& callback_measurement_clock,
    std::vector<BotStrategyRegistration> strategies,
    FakeSubmissionRuntimeParams submission_params) {
  return create_market_runtime_composition(
      std::move(configuration), std::move(policy), executor_clock, callback_measurement_clock,
      std::move(strategies),
      std::optional<FakeSubmissionRuntimeParams>{std::move(submission_params)});
}

// --------------------------------------------------------
// Opt into recovery-bound private preparation without publishing an economic-consumption claim.
model::Result<std::unique_ptr<MarketRuntime>>
MarketRuntime::create_with_fake_private_identity_retention(
    configuration::StartupConfiguration configuration, RuntimePolicy policy,
    model::ClockProvider& executor_clock, model::ClockProvider& callback_measurement_clock,
    std::vector<BotStrategyRegistration> strategies, FakeSubmissionRuntimeParams submission_params,
    const M4Policy& m4_policy, recovery::RecoveryBootstrap&& recovery_bootstrap) {
  return create_market_runtime_composition(
      std::move(configuration), std::move(policy), executor_clock, callback_measurement_clock,
      std::move(strategies),
      std::optional<FakeSubmissionRuntimeParams>{std::move(submission_params)}, &m4_policy,
      &recovery_bootstrap);
}

// --------------------------------------------------------
// Validate the complete composition, preallocate owner state, and enqueue only the first bootstrap.
model::Result<std::unique_ptr<MarketRuntime>> MarketRuntime::create_market_runtime_composition(
    configuration::StartupConfiguration configuration, RuntimePolicy policy,
    model::ClockProvider& executor_clock, model::ClockProvider& callback_measurement_clock,
    std::vector<BotStrategyRegistration> strategies,
    std::optional<FakeSubmissionRuntimeParams> submission_params, const M4Policy* m4_policy,
    recovery::RecoveryBootstrap* recovery_bootstrap) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject mixed startup and runtime provenance before constructing any mutable subsystem.
  if (configuration.fingerprint() != policy.configuration_fingerprint()) {
    return create_market_runtime_failure_result<std::unique_ptr<MarketRuntime>>(
        DomainErrorCode::InvalidRelationship, "market_runtime.provenance");
  }
  if (policy.source_capacity() >
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return create_market_runtime_failure_result<std::unique_ptr<MarketRuntime>>(
        DomainErrorCode::InvalidRuntimePolicy, "market_runtime.source_capacity");
  }
  if ((m4_policy == nullptr) != (recovery_bootstrap == nullptr) ||
      (m4_policy != nullptr && !submission_params)) {
    return create_market_runtime_failure_result<std::unique_ptr<MarketRuntime>>(
        DomainErrorCode::InvalidM4Policy, "market_runtime.private_identity_composition");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate private lane capacities while the caller's acknowledged bootstrap remains intact.
  std::optional<PrivateAdmissionConfiguration> private_configuration;
  if (m4_policy != nullptr) {
    auto created = PrivateAdmissionConfiguration::create_private_admission_configuration(
        configuration, *m4_policy);
    if (!created) {
      return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(created.error());
    }
    private_configuration.emplace(std::move(created).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate the composition root first so every queued command can retain its final address.
  auto runtime = std::unique_ptr<MarketRuntime>{new MarketRuntime{
      std::move(configuration), std::move(policy), executor_clock, callback_measurement_clock}};

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate and allocate the entire fake-only stack before constructing callback capabilities.
  if (submission_params) {
    auto submission = SubmissionCoordinator::create_submission_coordinator(
        runtime->configuration_, runtime->policy_, std::move(*submission_params));
    if (!submission) {
      return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(submission.error());
    }
    runtime->submission_coordinator_ = std::move(submission).value();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preallocate and construct every source state before callback dispatch becomes reachable.
  runtime->market_states_.reserve(runtime->policy_.source_capacity());
  for (const auto& source : runtime->policy_.sources()) {
    const auto* const metadata = runtime->configuration_.find_instrument_metadata(
        source.definition().venue_id, source.definition().instrument_id);
    if (metadata == nullptr) {
      return create_market_runtime_failure_result<std::unique_ptr<MarketRuntime>>(
          DomainErrorCode::DanglingReference, "market_runtime.source_metadata");
    }
    auto state = market_data::MarketStateMachine::create_market_state_machine(runtime->policy_,
                                                                              source, *metadata);
    if (!state) {
      return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(state.error());
    }
    runtime->market_states_.push_back(std::move(state).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prepare transfer slots before consuming recovery authority; extra slots permit a producer to
  // reuse released storage while the active command retains its detached immutable input.
  const auto ingress_capacity =
      static_cast<std::size_t>(runtime->policy_.limits().ingress_capacity);
  runtime->frame_slots_.resize(ingress_capacity + 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Install the private owner only after ordinary source preparations and before BotRuntime closes
  // the recovery installation seam. The executor later borrows this nonmoving coordinator child.
  if (m4_policy != nullptr) {
    auto installed =
        runtime->submission_coordinator_->install_recovery_bound_private_order_reconciler(
            runtime->configuration_, *m4_policy, std::move(*recovery_bootstrap));
    if (!installed) {
      return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(installed.error());
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal exact strategy coverage before creating the executor that can make callbacks reachable.
  auto bots = BotRuntime::create_bot_runtime(
      runtime->configuration_, runtime->policy_, callback_measurement_clock, runtime->trace_sink_,
      runtime->diagnostics_, std::move(strategies), {}, runtime->submission_coordinator_.get());
  if (!bots) {
    return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(bots.error());
  }
  runtime->bot_runtime_ = std::make_unique<BotRuntime>(std::move(bots).value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Borrow the installed owner only after callback composition validates; reverse destruction
  // releases the executor and every callback context before the owning coordinator disappears.
  if (private_configuration) {
    runtime->executor_ = std::make_unique<SerializedExecutor>(
        ingress_capacity, runtime->policy_.source_capacity(), executor_clock, *runtime,
        std::move(*private_configuration),
        *runtime->submission_coordinator_->private_admission_owner(),
        static_cast<std::size_t>(runtime->policy_.limits().maximum_drive_turns));
  } else {
    runtime->executor_ = std::make_unique<SerializedExecutor>(
        ingress_capacity, runtime->policy_.source_capacity(), executor_clock, *runtime,
        static_cast<std::size_t>(runtime->policy_.limits().maximum_drive_turns));
  }
  runtime->deterministic_driver_ =
      std::make_unique<DeterministicExecutorDriver>(*runtime->executor_);

  // ++++++++++++++++++++++++++++++++++++++++
  // One source-less accepted command begins canonical one-at-a-time source initialization.
  const auto bootstrap = InlineCommandWorkItem::create_inline_command_work_item<
      &MarketRuntime::execute_bootstrap_command>(BootstrapCommand{runtime.get(), 0U});
  auto admitted = runtime->executor_->try_admit(bootstrap);
  if (!admitted) {
    return model::Result<std::unique_ptr<MarketRuntime>>::create_failure(admitted.error());
  }
  if (admitted.value().outcome != AdmissionOutcome::Accepted ||
      !admitted.value().receipt.has_value()) {
    return create_market_runtime_failure_result<std::unique_ptr<MarketRuntime>>(
        DomainErrorCode::InvalidRuntimePolicy, "market_runtime.bootstrap_admission");
  }
  runtime->publish_owner_observations();
  return model::Result<std::unique_ptr<MarketRuntime>>::create_success(std::move(runtime));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Close and join any dedicated owner before its borrowed clocks or executor are destroyed.
MarketRuntime::~MarketRuntime() { close_and_wait(); }

// --------------------------------------------------------
// Admit one bounded frame while transferring its storage through a stable preallocated slot.
model::Result<AdmissionDecision>
MarketRuntime::try_admit(market_data::IngressFrameAttempt attempt) {
  std::lock_guard lock{ingress_mutex_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Source initialization is an explicit owner prefix; ordinary input cannot overtake it.
  if (lifecycle_ == MarketRuntimeLifecycle::Starting) {
    return create_market_runtime_failure_result<AdmissionDecision>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.starting");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve only trustworthy policy attribution, independently of policy frame-size rejection.
  std::optional<model::MarketSourceOrdinal> source_ordinal;
  if (attempt.source_id()) {
    const auto* const source = policy_.find_source(*attempt.source_id());
    if (source != nullptr) {
      source_ordinal = source->ordinal();
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve one transfer slot before publishing its index to the executor.
  const auto available =
      std::find_if(frame_slots_.begin(), frame_slots_.end(),
                   [](const FrameSlot& slot) { return !slot.attempt.has_value(); });
  if (available == frame_slots_.end()) {
    return create_market_runtime_failure_result<AdmissionDecision>(
        DomainErrorCode::InvalidMarketState, "market_runtime.frame_slots");
  }
  const auto slot_index = static_cast<std::size_t>(available - frame_slots_.begin());
  available->attempt.emplace(std::move(attempt));
  const auto work =
      InlineCommandWorkItem::create_inline_command_work_item<&MarketRuntime::execute_frame_command>(
          FrameCommand{this, static_cast<std::uint32_t>(slot_index)});
  auto decision = executor_->try_admit(work, source_ordinal);

  // ++++++++++++++++++++++++++++++++++++++++
  // Only an accepted queue entry retains the slot; all other paths release it immediately.
  if (!decision) {
    available->attempt.reset();
    if (!fault_) {
      fault_.emplace(decision.error());
    }
    lifecycle_ = MarketRuntimeLifecycle::Faulted;
    return model::Result<AdmissionDecision>::create_failure(decision.error());
  }
  if (decision.value().outcome != AdmissionOutcome::Accepted) {
    available->attempt.reset();
  }
  return decision;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preserve source ownership until the executor copies one fact and track nominal admission faults.
model::Result<PrivateAdmissionDecision>
MarketRuntime::try_admit_private(const oms::PrivateOrderIngressAttempt& attempt) {
  std::lock_guard lock{ingress_mutex_};
  if (lifecycle_ == MarketRuntimeLifecycle::Starting) {
    return create_market_runtime_failure_result<PrivateAdmissionDecision>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.starting");
  }
  if (!submission_coordinator_ || submission_coordinator_->private_order_reconciler() == nullptr) {
    return create_market_runtime_failure_result<PrivateAdmissionDecision>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.private_identity_composition");
  }
  auto decision = executor_->try_admit_private(attempt);
  if (!decision) {
    if (auto terminal = executor_->terminal_error()) {
      if (!fault_) {
        fault_.emplace(std::move(*terminal));
      }
      lifecycle_ = MarketRuntimeLifecycle::Faulted;
    }
  }
  return decision;
}

// --------------------------------------------------------
// Delegate ordinary copied/retained observation to the executor's synchronized lane-specific
// oracle.
std::optional<PrivateAdmissionObservation>
MarketRuntime::private_admission_observation(model::AdmissionOrdinal attempt_ordinal) const {
  return executor_->private_admission_observation(attempt_ordinal);
}

// --------------------------------------------------------
// Admit one owner-local reset without turning rejected control work into a source-loss fence.
model::Result<AdmissionDecision>
MarketRuntime::try_resynchronize(const model::MarketSourceId& source_id) {
  std::lock_guard lock{ingress_mutex_};
  if (lifecycle_ == MarketRuntimeLifecycle::Starting) {
    return create_market_runtime_failure_result<AdmissionDecision>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.starting");
  }
  const auto* const source = policy_.find_source(source_id);
  if (source == nullptr) {
    return create_market_runtime_failure_result<AdmissionDecision>(
        DomainErrorCode::RuntimeSourceNotConfigured, "market_runtime.resynchronize_source");
  }
  const auto source_index = static_cast<std::uint32_t>(source->ordinal().value() - 1U);
  const auto work = InlineCommandWorkItem::create_inline_command_work_item<
      &MarketRuntime::execute_resynchronize_command>(ResynchronizeCommand{this, source_index});
  auto decision = executor_->try_admit(work);
  if (!decision) {
    if (!fault_) {
      fault_.emplace(decision.error());
    }
    lifecycle_ = MarketRuntimeLifecycle::Faulted;
  }
  return decision;
}

// --------------------------------------------------------
// Bind deterministic ownership only when no dedicated driver object owns the lifecycle seam.
model::Result<void> MarketRuntime::bind_to_current_thread() {
  std::lock_guard lock{driver_mutex_};
  if (dedicated_driver_) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::ExecutionNotPermitted,
                                                      "market_runtime.dedicated_driver");
  }
  return deterministic_driver_->bind_to_current_thread();
}

// --------------------------------------------------------
// Execute one deterministic turn and translate same-thread callback recursion into bot evidence.
model::Result<std::optional<TurnReport>> MarketRuntime::execute_next_turn() {
  auto turn = deterministic_driver_->execute_next_turn();
  if (!turn) {
    if (turn.error().code == DomainErrorCode::ExecutorReentryDetected) {
      if (auto reentry = record_owner_reentry()) {
        return model::Result<std::optional<TurnReport>>::create_failure(std::move(*reentry));
      }
    }
    if (executor_->terminal_error().has_value()) {
      latch_runtime_fault(turn.error());
    }
    return model::Result<std::optional<TurnReport>>::create_failure(turn.error());
  }
  record_completed_turn_report(turn.value());
  return turn;
}

// --------------------------------------------------------
// Progress through the one-turn wrapper so the last complete queue-age report is never skipped.
model::Result<PendingTurnExecutionReport>
MarketRuntime::execute_pending_turns(std::size_t maximum_turns) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate ownership and recursion before a nonzero request can partially advance the queue.
  auto validation = deterministic_driver_->execute_pending_turns(0U);
  if (!validation) {
    if (validation.error().code == DomainErrorCode::ExecutorReentryDetected) {
      if (auto reentry = record_owner_reentry()) {
        return model::Result<PendingTurnExecutionReport>::create_failure(std::move(*reentry));
      }
    }
    return model::Result<PendingTurnExecutionReport>::create_failure(validation.error());
  }
  if (maximum_turns > static_cast<std::size_t>(policy_.limits().maximum_drive_turns)) {
    return create_market_runtime_failure_result<PendingTurnExecutionReport>(
        DomainErrorCode::InvalidRuntimePolicy, "maximum_drive_turns");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Stop at the caller bound or the first empty merged command/fence observation.
  std::size_t turns_executed = 0U;
  while (turns_executed < maximum_turns) {
    auto turn = execute_next_turn();
    if (!turn) {
      return model::Result<PendingTurnExecutionReport>::create_failure(turn.error());
    }
    if (!turn.value()) {
      break;
    }
    ++turns_executed;
  }
  const auto snapshot = executor_->queue_snapshot();
  return model::Result<PendingTurnExecutionReport>::create_success(PendingTurnExecutionReport{
      turns_executed, snapshot.pending_commands, snapshot.pending_fences, snapshot.command_capacity,
      snapshot.maximum_drive_turns, snapshot.completed_turns});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Release deterministic ownership and record only genuine same-thread active-turn recursion.
model::Result<void> MarketRuntime::release_from_current_thread() {
  auto released = deterministic_driver_->release_from_current_thread();
  if (!released && released.error().code == DomainErrorCode::ExecutorReentryDetected) {
    if (auto reentry = record_owner_reentry()) {
      return model::Result<void>::create_failure(std::move(*reentry));
    }
  }
  return released;
}

// --------------------------------------------------------
// Start the dedicated owner only when the runtime remains open and no prior driver exists.
model::Result<void> MarketRuntime::start_dedicated() {
  std::lock_guard driver_lock{driver_mutex_};
  if (dedicated_driver_) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::ExecutionNotPermitted,
                                                      "market_runtime.dedicated_driver");
  }
  {
    std::lock_guard ingress_lock{ingress_mutex_};
    if (lifecycle_ == MarketRuntimeLifecycle::Closed ||
        lifecycle_ == MarketRuntimeLifecycle::Faulted) {
      return create_market_runtime_failure_result<void>(DomainErrorCode::ExecutionNotPermitted,
                                                        "market_runtime.lifecycle");
    }
  }

  auto driver = std::make_unique<DedicatedExecutorDriver>(*executor_);
  if (!driver->has_started_successfully()) {
    auto error = driver->terminal_error().value_or(DomainError::create_at_field(
        DomainErrorCode::ExecutorNotBound, "market_runtime.dedicated_driver"));
    return model::Result<void>::create_failure(std::move(error));
  }
  dedicated_driver_ = std::move(driver);
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Close ingress and request prefix drainage; external callers join while the owner stack returns.
void MarketRuntime::close_and_wait() noexcept {
  DedicatedExecutorDriver* driver = nullptr;
  {
    std::lock_guard driver_lock{driver_mutex_};
    if (!executor_) {
      return;
    }
    {
      std::lock_guard ingress_lock{ingress_mutex_};
      executor_->close();
      if (!fault_) {
        lifecycle_ = MarketRuntimeLifecycle::Closed;
      }
    }
    driver = dedicated_driver_.get();
  }
  if (driver == nullptr) {
    return;
  }

  driver->request_stop();
  if (executor_->is_current_thread_owner()) {
    return;
  }
  driver->wait_until_stopped();
  record_completed_turn_report(driver->last_turn_report());
  if (auto terminal = driver->terminal_error()) {
    latch_runtime_fault(std::move(*terminal));
  }
}

// --------------------------------------------------------
// Close admission atomically with producer-visible lifecycle publication without waiting.
void MarketRuntime::close() noexcept {
  std::lock_guard driver_lock{driver_mutex_};
  std::lock_guard ingress_lock{ingress_mutex_};
  executor_->close();
  if (!fault_) {
    lifecycle_ = MarketRuntimeLifecycle::Closed;
  }
}

// --------------------------------------------------------
// Copy synchronized lifecycle state without reading live owner-local bot or diagnostic objects.
MarketRuntimeStatus MarketRuntime::status() const {
  std::lock_guard driver_lock{driver_mutex_};
  const bool dedicated_started =
      dedicated_driver_ != nullptr && dedicated_driver_->has_started_successfully();
  const bool dedicated_running = dedicated_driver_ != nullptr && dedicated_driver_->is_running();
  const auto dedicated_error =
      dedicated_driver_ ? dedicated_driver_->terminal_error() : std::nullopt;
  const auto dedicated_report =
      dedicated_driver_ ? dedicated_driver_->last_turn_report() : std::nullopt;

  std::lock_guard ingress_lock{ingress_mutex_};
  const auto executor_state = executor_->queue_snapshot();
  auto reported_fault = fault_;
  if (!reported_fault) {
    reported_fault = published_bot_fault(published_bot_status_);
  }
  if (!reported_fault && dedicated_error) {
    reported_fault = dedicated_error;
  }
  if (!reported_fault) {
    reported_fault = executor_->terminal_error();
  }
  const auto reported_lifecycle = reported_fault ? MarketRuntimeLifecycle::Faulted : lifecycle_;
  return MarketRuntimeStatus{reported_lifecycle,
                             initialized_sources_,
                             executor_state,
                             published_bot_status_,
                             published_last_dispatch_,
                             std::move(reported_fault),
                             newer_report(last_completed_turn_, dedicated_report),
                             dedicated_started,
                             dedicated_running,
                             published_diagnostic_saturated_,
                             published_dropped_diagnostics_};
}

// --------------------------------------------------------
// Copy all M3 evidence through immutable lower-layer inspection after owner release.
model::Result<SubmissionRuntimeEvidence> MarketRuntime::copy_submission_evidence() const {
  if (!submission_coordinator_) {
    return create_market_runtime_failure_result<SubmissionRuntimeEvidence>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.submission_capability");
  }
  const auto& coordinator = *submission_coordinator_;

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize AEGISSTS bytes and digest from the same accepted canonical record prefix.
  auto canonical_bytes = coordinator.trace_sink().encode_canonical_bytes();
  if (!canonical_bytes) {
    return model::Result<SubmissionRuntimeEvidence>::create_failure(canonical_bytes.error());
  }
  auto canonical_digest = coordinator.trace_sink().derive_digest();
  if (!canonical_digest) {
    return model::Result<SubmissionRuntimeEvidence>::create_failure(canonical_digest.error());
  }
  const auto trace_span = coordinator.trace_sink().records();
  std::vector<trace::SubmissionTraceRecord> trace_records{trace_span.begin(), trace_span.end()};

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the retained noncanonical prefix and exact saturation accounting.
  std::vector<SubmissionDiagnosticRecord> diagnostics;
  diagnostics.reserve(coordinator.diagnostics().diagnostic_count());
  for (std::size_t index = 0U; index < coordinator.diagnostics().diagnostic_count(); ++index) {
    const auto* const record = coordinator.diagnostics().diagnostic_at(index);
    if (record != nullptr) {
      diagnostics.push_back(*record);
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy every permanent OMS row in admission order, including terminal LocallyFailed records.
  const auto& outbound = coordinator.outbound_oms();
  std::vector<SubmissionOmsOrderEvidence> oms_orders;
  oms_orders.reserve(outbound.order_count());
  for (std::size_t index = 0U; index < outbound.order_count(); ++index) {
    const auto* const record = outbound.record_at(index);
    if (record == nullptr) {
      return create_market_runtime_failure_result<SubmissionRuntimeEvidence>(
          DomainErrorCode::InvalidOmsState, "market_runtime.submission_oms_evidence");
    }
    oms_orders.push_back(SubmissionOmsOrderEvidence{record->admission(), record->state()});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Select every currently Held reusable slot and preserve the full canonical scope-cell view.
  const auto& reservations = coordinator.reservations();
  std::vector<risk::ReservationEvidence> held_reservations;
  held_reservations.reserve(reservations.held_reservation_count());
  for (std::size_t index = 0U; index < reservations.capacity(); ++index) {
    const auto* const reservation = reservations.reservation_at(index);
    if (reservation != nullptr && reservation->state == risk::ReservationState::Held) {
      held_reservations.push_back(*reservation);
    }
  }
  if (held_reservations.size() != reservations.held_reservation_count()) {
    return create_market_runtime_failure_result<SubmissionRuntimeEvidence>(
        DomainErrorCode::InvalidRiskReservationState, "market_runtime.submission_reservations");
  }
  std::vector<risk::RiskScopeExposureEvidence> scope_exposures;
  scope_exposures.reserve(reservations.scope_evidence_count());
  for (std::size_t index = 0U; index < reservations.scope_evidence_count(); ++index) {
    auto exposure = reservations.scope_evidence_at(index);
    if (!exposure) {
      return create_market_runtime_failure_result<SubmissionRuntimeEvidence>(
          DomainErrorCode::InvalidRiskReservationState, "market_runtime.submission_scope_evidence");
    }
    scope_exposures.push_back(std::move(*exposure));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Own each fake accepted slot's exact bytes so evidence outlives the runtime and fake buffers.
  std::vector<SubmissionAcceptedWriteEvidence> accepted_writes;
  accepted_writes.reserve(coordinator.initiator().accepted_writes().size());
  for (const auto& write : coordinator.initiator().accepted_writes()) {
    const auto bytes = write.bytes();
    accepted_writes.push_back(
        SubmissionAcceptedWriteEvidence{write.attempt_id(), write.encoder_invocation_ordinal(),
                                        write.initiator_invocation_ordinal(), write.write_ordinal(),
                                        std::vector<std::byte>{bytes.begin(), bytes.end()}});
  }

  return model::Result<SubmissionRuntimeEvidence>::create_success(SubmissionRuntimeEvidence{
      reservations.policy().fingerprint(),
      reservations.policy().revision(),
      coordinator.policy().fingerprint(),
      std::move(trace_records),
      std::move(canonical_bytes).value(),
      std::move(canonical_digest).value(),
      std::move(diagnostics),
      coordinator.diagnostics().dropped_count(),
      std::move(oms_orders),
      outbound.order_count(),
      std::move(held_reservations),
      reservations.held_reservation_count(),
      std::move(scope_exposures),
      std::move(accepted_writes),
      coordinator.encoder().invocations_consumed(),
      coordinator.initiator().invocations_consumed(),
      coordinator.is_runtime_faulted(),
      coordinator.terminal_error(),
  });

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Copy complete replay evidence only after no owner or future admission can mutate it.
model::Result<MarketRuntimeEvidence> MarketRuntime::collect_quiescent_evidence() const {
  std::lock_guard driver_lock{driver_mutex_};
  if (dedicated_driver_ && dedicated_driver_->is_running()) {
    return create_market_runtime_failure_result<MarketRuntimeEvidence>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.evidence_owner");
  }
  const auto dedicated_report =
      dedicated_driver_ ? dedicated_driver_->last_turn_report() : std::nullopt;
  const auto dedicated_error =
      dedicated_driver_ ? dedicated_driver_->terminal_error() : std::nullopt;

  std::lock_guard ingress_lock{ingress_mutex_};
  const auto executor_state = executor_->queue_snapshot();
  const bool suppresses_pending_prefix = executor_state.faulted;
  const auto& private_lane = executor_state.private_lane;
  if ((!executor_state.closed && !suppresses_pending_prefix) || executor_state.owner_bound ||
      executor_state.turn_active || executor_state.in_flight_fences != 0U ||
      private_lane.in_flight_slots != 0U || private_lane.reconciliation_in_flight_slots != 0U ||
      private_lane.in_flight_account_fences != 0U || private_lane.global_fence_in_flight ||
      (!suppresses_pending_prefix &&
       (executor_state.pending_commands != 0U || executor_state.pending_fences != 0U ||
        private_lane.queued_slots != 0U || private_lane.reconciliation_queued_slots != 0U ||
        private_lane.pending_account_fences != 0U ||
        (private_lane.global_fence_active && !private_lane.global_fence_owner_applied)))) {
    return create_market_runtime_failure_result<MarketRuntimeEvidence>(
        DomainErrorCode::ExecutionNotPermitted, "market_runtime.evidence_quiescence");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize the canonical stream and digest from the same immutable accepted trace prefix.
  auto canonical_bytes = trace_sink_.encode_canonical_bytes();
  if (!canonical_bytes) {
    return model::Result<MarketRuntimeEvidence>::create_failure(canonical_bytes.error());
  }
  auto canonical_digest = trace_sink_.derive_digest();
  if (!canonical_digest) {
    return model::Result<MarketRuntimeEvidence>::create_failure(canonical_digest.error());
  }
  const auto trace_span = trace_sink_.records();
  std::vector<trace::RuntimeTraceRecord> trace_records{trace_span.begin(), trace_span.end()};

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the retained diagnostic prefix and every source's final hidden-state summary.
  std::vector<RuntimeDiagnosticRecord> diagnostic_records;
  diagnostic_records.reserve(diagnostics_.diagnostic_count());
  for (std::size_t index = 0U; index < diagnostics_.diagnostic_count(); ++index) {
    const auto* const record = diagnostics_.diagnostic_at(index);
    if (record != nullptr) {
      diagnostic_records.push_back(*record);
    }
  }
  std::vector<MarketRuntimeSourceEvidence> sources;
  sources.reserve(market_states_.size());
  for (std::size_t index = 0U; index < market_states_.size(); ++index) {
    const auto source_ordinal = model::MarketSourceOrdinal::from_value(index + 1U);
    if (!source_ordinal) {
      return model::Result<MarketRuntimeEvidence>::create_failure(source_ordinal.error());
    }
    const auto& state = market_states_[index];
    sources.push_back(MarketRuntimeSourceEvidence{source_ordinal.value(), state.readiness(),
                                                  state.book_identity(), state.active_session(),
                                                  state.last_source_sequence()});
  }

  auto reported_fault = fault_;
  if (!reported_fault) {
    reported_fault = published_bot_fault(published_bot_status_);
  }
  if (!reported_fault && dedicated_error) {
    reported_fault = dedicated_error;
  }
  if (!reported_fault) {
    reported_fault = executor_->terminal_error();
  }

  std::optional<SubmissionRuntimeEvidence> submission;
  if (submission_coordinator_) {
    auto copied = copy_submission_evidence();
    if (!copied) {
      return model::Result<MarketRuntimeEvidence>::create_failure(copied.error());
    }
    submission.emplace(std::move(copied).value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy fixed registry observations only after the shared owner and every unsuppressed lane stop.
  std::optional<PrivateIdentityRetentionRuntimeEvidence> private_identity_retention;
  if (submission_coordinator_) {
    const auto* const reconciler = submission_coordinator_->private_order_reconciler();
    if (reconciler != nullptr) {
      private_identity_retention.emplace(PrivateIdentityRetentionRuntimeEvidence{
          reconciler->m4_policy().fingerprint(), reconciler->recovery_lineage_id(),
          reconciler->runtime_epoch_id(), reconciler->registered_order_namespace(),
          reconciler->event_identity_record_capacity(), reconciler->event_identity_record_count(),
          reconciler->trade_identity_record_capacity(), reconciler->trade_identity_record_count(),
          reconciler->exchange_order_mapping_capacity(), reconciler->exchange_order_mapping_count(),
          reconciler->identity_preparations().event_record_count(),
          reconciler->identity_preparations().trade_record_count(),
          reconciler->identity_preparations().mapping_candidate_count(),
          reconciler->retained_identity_turn_count()});
    }
  }

  return model::Result<MarketRuntimeEvidence>::create_success(MarketRuntimeEvidence{
      configuration_.fingerprint(), policy_.fingerprint(), std::move(trace_records),
      std::move(canonical_bytes).value(), std::move(canonical_digest).value(),
      std::move(diagnostic_records), diagnostics_.dropped_count(), std::move(sources),
      executor_state, published_last_dispatch_, std::move(reported_fault),
      newer_report(last_completed_turn_, dedicated_report), std::move(submission),
      std::move(private_identity_retention)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Apply an ordered loss fence through preflight, transactional state, and complete dispatch.
model::Result<void>
MarketRuntime::on_source_discontinuity(const SourceDiscontinuity& discontinuity,
                                       const ControlTurnContext& context) noexcept {
  const auto source_index = static_cast<std::size_t>(discontinuity.source_ordinal.value() - 1U);
  if (source_index >= market_states_.size() || discontinuity.lost_attempt_count == 0U) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                                                      "market_runtime.discontinuity_source");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate bounded noncanonical loss evidence before the market owner changes readiness.
  PendingDiagnostic diagnostic;
  diagnostic.kind = RuntimeDiagnosticKind::SourceDiscontinuity;
  diagnostic.fields.source_ordinal = discontinuity.source_ordinal;
  diagnostic.fields.admission_ordinal = discontinuity.earliest_failed_attempt;
  diagnostic.fields.turn_ordinal = context.turn_ordinal;
  diagnostic.fields.occurrence_count = discontinuity.lost_attempt_count;
  auto diagnostic_valid = diagnostics_.validate_diagnostic(diagnostic.kind, diagnostic.fields);
  if (!diagnostic_valid) {
    return diagnostic_valid;
  }
  ExactBotDispatchPreflight preflight{*bot_runtime_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact authorization and trace reservation precede state mutation; later dispatch faults
  // preserve this applied turn.
  auto outcome = market_states_[source_index].apply_source_discontinuity(
      discontinuity.earliest_failed_attempt, owner_market_turn_context_from_control_turn(context),
      trace_sink_, preflight);
  if (!outcome) {
    preflight.cancel_dispatch_plan();
    return model::Result<void>::create_failure(runtime_error_from_turn_error(outcome.error()));
  }
  auto value = std::move(outcome).value();
  finalize_market_turn_outcome(preflight.plan(), value, std::move(diagnostic));
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Bridge the fixed inline frame command into its stable runtime owner.
model::Result<void>
MarketRuntime::execute_frame_command(const FrameCommand& command,
                                     const AcceptedTurnContext& context) noexcept {
  if (command.runtime == nullptr) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidRelationship,
                                                      "market_runtime.frame_command");
  }
  auto processed = command.runtime->execute_frame_turn(command.slot_index, context);
  if (!processed) {
    return model::Result<void>::create_failure(runtime_error_from_turn_error(processed.error()));
  }
  return processed;
}

// --------------------------------------------------------
// Bridge the fixed inline bootstrap command into canonical source initialization.
model::Result<void>
MarketRuntime::execute_bootstrap_command(const BootstrapCommand& command,
                                         const AcceptedTurnContext& context) noexcept {
  if (command.runtime == nullptr) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidRelationship,
                                                      "market_runtime.bootstrap_command");
  }
  auto processed = command.runtime->execute_bootstrap_turn(command.source_index, context);
  if (!processed) {
    return model::Result<void>::create_failure(runtime_error_from_turn_error(processed.error()));
  }
  return processed;
}

// --------------------------------------------------------
// Bridge the fixed inline resynchronization command into one owner-local source reset.
model::Result<void>
MarketRuntime::execute_resynchronize_command(const ResynchronizeCommand& command,
                                             const AcceptedTurnContext& context) noexcept {
  if (command.runtime == nullptr) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidRelationship,
                                                      "market_runtime.resynchronize_command");
  }
  auto processed = command.runtime->execute_resynchronization_turn(command.source_index, context);
  if (!processed) {
    return model::Result<void>::create_failure(runtime_error_from_turn_error(processed.error()));
  }
  return processed;
}

// --------------------------------------------------------
// Consume one accepted slot and contain every envelope, parser, and normalizer failure.
model::Result<void> MarketRuntime::execute_frame_turn(std::uint32_t slot_index,
                                                      const AcceptedTurnContext& context) {
  std::optional<market_data::IngressFrameAttempt> attempt;
  {
    std::lock_guard lock{ingress_mutex_};
    if (slot_index >= frame_slots_.size() || !frame_slots_[slot_index].attempt) {
      return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidMarketState,
                                                        "market_runtime.frame_slot");
    }
    attempt.emplace(std::move(*frame_slots_[slot_index].attempt));
    frame_slots_[slot_index].attempt.reset();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Trust only policy lookup for attribution, even when post-admission frame minting rejects size.
  const RuntimeSource* source = nullptr;
  if (attempt->source_id()) {
    source = policy_.find_source(*attempt->source_id());
  }
  const auto frame_size = static_cast<std::uint64_t>(attempt->frame().size());
  const auto session_epoch = attempt->session_epoch();
  auto frame = market_data::RecordedFrame::create_recorded_frame(
      std::move(*attempt), policy_, context.receipt.receive_sequence, context.receipt.received_at);
  if (!frame) {
    PendingDiagnostic diagnostic;
    diagnostic.kind = source != nullptr ? RuntimeDiagnosticKind::MalformedInput
                                        : RuntimeDiagnosticKind::UnsupportedInput;
    diagnostic.fields.source_ordinal =
        source != nullptr ? std::optional<model::MarketSourceOrdinal>{source->ordinal()}
                          : std::nullopt;
    diagnostic.fields.admission_ordinal = context.receipt.attempt_ordinal;
    diagnostic.fields.turn_ordinal = context.turn_ordinal;
    diagnostic.fields.detail_code = domain_detail(frame.error().code);
    diagnostic.fields.observed_value = frame_size;
    diagnostic.fields.limit_value = policy_.limits().maximum_frame_bytes;
    if (source != nullptr) {
      return execute_attributable_failure_turn(*source, session_epoch, context.receipt,
                                               trace::RuntimeInputDisposition::MalformedRejected,
                                               std::move(diagnostic), context);
    }
    auto valid = diagnostics_.validate_diagnostic(diagnostic.kind, diagnostic.fields);
    if (!valid) {
      return valid;
    }
    auto appended = diagnostics_.append_diagnostic(diagnostic.kind, std::move(diagnostic.fields));
    publish_owner_observations();
    return appended;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Parse into temporary storage and map only assigned code/offset details on failure.
  auto parsed = market_data::parse_recorded_fixture(frame.value());
  if (!parsed) {
    const auto parse_error = parsed.error();
    const auto disposition = is_unsupported(parse_error.code)
                                 ? trace::RuntimeInputDisposition::UnsupportedRejected
                                 : trace::RuntimeInputDisposition::MalformedRejected;
    PendingDiagnostic diagnostic;
    diagnostic.kind = is_unsupported(parse_error.code) ? RuntimeDiagnosticKind::UnsupportedInput
                                                       : RuntimeDiagnosticKind::MalformedInput;
    diagnostic.fields.source_ordinal = source->ordinal();
    diagnostic.fields.admission_ordinal = context.receipt.attempt_ordinal;
    diagnostic.fields.turn_ordinal = context.turn_ordinal;
    diagnostic.fields.detail_code = parser_detail(parse_error.code);
    diagnostic.fields.observed_value = parse_error.byte_offset;
    return execute_attributable_failure_turn(*source, session_epoch, context.receipt, disposition,
                                             std::move(diagnostic), context);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Assigned semantic normalization failures are attributable malformed content and never reach
  // mutable book state; impossible policy/source failures remain terminal contract errors.
  auto normalized = market_data::normalize_recorded_fixture(std::move(parsed).value(), policy_);
  if (!normalized) {
    if (normalized.error().code != DomainErrorCode::InvalidMarketEvent &&
        normalized.error().code != DomainErrorCode::MarketBookCapacityExceeded) {
      return model::Result<void>::create_failure(normalized.error());
    }
    PendingDiagnostic diagnostic;
    diagnostic.kind = RuntimeDiagnosticKind::MalformedInput;
    diagnostic.fields.source_ordinal = source->ordinal();
    diagnostic.fields.admission_ordinal = context.receipt.attempt_ordinal;
    diagnostic.fields.turn_ordinal = context.turn_ordinal;
    diagnostic.fields.detail_code = domain_detail(normalized.error().code);
    if (normalized.error().context.collection_index) {
      diagnostic.fields.observed_value =
          static_cast<std::uint64_t>(*normalized.error().context.collection_index) + 1U;
    }
    return execute_attributable_failure_turn(*source, session_epoch, context.receipt,
                                             trace::RuntimeInputDisposition::MalformedRejected,
                                             std::move(diagnostic), context);
  }
  return execute_normalized_market_turn(std::move(normalized).value(), context);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Initialize one source, dispatch its transition, and enqueue only its canonical successor.
model::Result<void> MarketRuntime::execute_bootstrap_turn(std::uint32_t source_index,
                                                          const AcceptedTurnContext& context) {
  if (source_index >= market_states_.size() || source_index >= policy_.sources().size()) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                                                      "market_runtime.bootstrap_source");
  }
  {
    std::lock_guard lock{ingress_mutex_};
    if (source_index != initialized_sources_) {
      return create_market_runtime_failure_result<void>(DomainErrorCode::InvalidMarketState,
                                                        "market_runtime.bootstrap_order");
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preflight exact state-only fan-out before initialization publishes Synchronizing.
  ExactBotDispatchPreflight preflight{*bot_runtime_};
  auto outcome = market_states_[source_index].initialize_market_state(
      owner_market_turn_context_from_accepted_turn(context), trace_sink_, preflight);
  if (!outcome) {
    preflight.cancel_dispatch_plan();
    return model::Result<void>::create_failure(outcome.error());
  }
  auto value = std::move(outcome).value();
  finalize_market_turn_outcome(preflight.plan(), value, std::nullopt);

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish completed initialization and enqueue at most one later source after this commit.
  std::lock_guard lock{ingress_mutex_};
  ++initialized_sources_;
  if (initialized_sources_ == market_states_.size()) {
    if (!fault_ && lifecycle_ == MarketRuntimeLifecycle::Starting) {
      lifecycle_ = MarketRuntimeLifecycle::Running;
    }
    return model::Result<void>::create_success();
  }
  if (fault_ || lifecycle_ == MarketRuntimeLifecycle::Closed) {
    return model::Result<void>::create_success();
  }

  const auto next = InlineCommandWorkItem::create_inline_command_work_item<
      &MarketRuntime::execute_bootstrap_command>(BootstrapCommand{this, initialized_sources_});
  auto admitted = executor_->try_admit(next);
  if (!admitted) {
    fault_.emplace(admitted.error());
    lifecycle_ = MarketRuntimeLifecycle::Faulted;
    return model::Result<void>::create_success();
  }
  if (admitted.value().outcome == AdmissionOutcome::Closed) {
    lifecycle_ = MarketRuntimeLifecycle::Closed;
    return model::Result<void>::create_success();
  }
  if (admitted.value().outcome != AdmissionOutcome::Accepted ||
      !admitted.value().receipt.has_value()) {
    auto error = DomainError::create_at_field(DomainErrorCode::InvalidMarketState,
                                              "market_runtime.bootstrap_admission");
    const auto requested = executor_->request_owner_fault(error);
    static_cast<void>(requested);
    fault_.emplace(std::move(error));
    lifecycle_ = MarketRuntimeLifecycle::Faulted;
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Apply one explicit control reset through the same preflight and dispatch ordering.
model::Result<void>
MarketRuntime::execute_resynchronization_turn(std::uint32_t source_index,
                                              const AcceptedTurnContext& context) {
  if (source_index >= market_states_.size()) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                                                      "market_runtime.resynchronize_source");
  }
  ExactBotDispatchPreflight preflight{*bot_runtime_};
  auto outcome = market_states_[source_index].resynchronize_source(
      owner_market_turn_context_from_accepted_turn(context), trace_sink_, preflight);
  if (!outcome) {
    preflight.cancel_dispatch_plan();
    return model::Result<void>::create_failure(outcome.error());
  }
  auto value = std::move(outcome).value();
  finalize_market_turn_outcome(preflight.plan(), value, std::nullopt);
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Preflight fan-out, invoke the matching transactional overload, and dispatch the complete outcome.
model::Result<void>
MarketRuntime::execute_normalized_market_turn(market_data::NormalizedRecordedMarketCommand command,
                                              const AcceptedTurnContext& context) {
  const auto source_ordinal = command_source_ordinal(command);
  const auto source_index = static_cast<std::size_t>(source_ordinal.value() - 1U);
  if (source_index >= market_states_.size()) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                                                      "market_runtime.normalized_source");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A fixture-authored freshness timestamp cannot impersonate the accepted owner-turn clock.
  if (const auto* const staleness = std::get_if<market_data::StalenessCheck>(&command);
      staleness != nullptr && staleness->processing_timestamp != context.processing_timestamp) {
    PendingDiagnostic diagnostic;
    diagnostic.kind = RuntimeDiagnosticKind::MalformedInput;
    diagnostic.fields.source_ordinal = source_ordinal;
    diagnostic.fields.admission_ordinal = context.receipt.attempt_ordinal;
    diagnostic.fields.turn_ordinal = context.turn_ordinal;
    diagnostic.fields.detail_code = domain_detail(DomainErrorCode::InvalidMarketEvent);
    diagnostic.fields.observed_value = staleness->processing_timestamp.nanoseconds();
    diagnostic.fields.limit_value = context.processing_timestamp.nanoseconds();
    return execute_attributable_failure_turn(
        policy_.sources()[source_index], staleness->session_epoch, context.receipt,
        trace::RuntimeInputDisposition::MalformedRejected, std::move(diagnostic), context);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prevalidate possible structural diagnostics before exact classified callback authorization.
  PendingDiagnostic structural;
  structural.kind = RuntimeDiagnosticKind::StructuralBookRejected;
  structural.fields.source_ordinal = source_ordinal;
  structural.fields.admission_ordinal = context.receipt.attempt_ordinal;
  structural.fields.turn_ordinal = context.turn_ordinal;
  structural.fields.detail_code = domain_detail(DomainErrorCode::MarketBookInvalid);
  auto structural_valid = diagnostics_.validate_diagnostic(structural.kind, structural.fields);
  if (!structural_valid) {
    return structural_valid;
  }
  ExactBotDispatchPreflight preflight{*bot_runtime_};

  // ++++++++++++++++++++++++++++++++++++++++
  // Visit exactly one state overload; every failure promises no state mutation and cancels no-op
  // observational preflight before the executor latches a terminal handler failure.
  const auto market_context = accepted_market_turn_context_from_accepted_turn(context);
  auto outcome = std::visit(
      [&](auto&& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::same_as<Value, market_data::NormalizedMarketUpdate>) {
          return market_states_[source_index].apply_market_update(std::move(value), market_context,
                                                                  trace_sink_, preflight);
        } else if constexpr (std::same_as<Value, market_data::SessionStarted>) {
          return market_states_[source_index].apply_session_start(value, market_context,
                                                                  trace_sink_, preflight);
        } else {
          return market_states_[source_index].apply_staleness_check(value, market_context,
                                                                    trace_sink_, preflight);
        }
      },
      std::move(command));
  if (!outcome) {
    preflight.cancel_dispatch_plan();
    return model::Result<void>::create_failure(outcome.error());
  }
  auto value = std::move(outcome).value();
  std::optional<PendingDiagnostic> diagnostic;
  if (value.disposition() == trace::RuntimeInputDisposition::StructuralBookRejected) {
    diagnostic.emplace(std::move(structural));
  }
  finalize_market_turn_outcome(preflight.plan(), value, std::move(diagnostic));
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Sanitize one attributable failure, transition only its source, and dispatch no malformed bytes.
model::Result<void> MarketRuntime::execute_attributable_failure_turn(
    const RuntimeSource& source, model::SessionEpoch session_epoch, const AdmissionReceipt& receipt,
    trace::RuntimeInputDisposition disposition, PendingDiagnostic diagnostic,
    const AcceptedTurnContext& context) {
  if (receipt != context.receipt || diagnostic.fields.source_ordinal != source.ordinal()) {
    return create_market_runtime_failure_result<void>(
        DomainErrorCode::InvalidRelationship, "market_runtime.attributable_failure_context");
  }
  const auto source_index = static_cast<std::size_t>(source.ordinal().value() - 1U);
  if (source_index >= market_states_.size()) {
    return create_market_runtime_failure_result<void>(DomainErrorCode::RuntimeSourceNotConfigured,
                                                      "market_runtime.attributable_failure_source");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Both noncanonical detail and state-only callback headroom are proven before readiness changes.
  auto diagnostic_valid = diagnostics_.validate_diagnostic(diagnostic.kind, diagnostic.fields);
  if (!diagnostic_valid) {
    return diagnostic_valid;
  }
  ExactBotDispatchPreflight preflight{*bot_runtime_};
  const auto rejected = market_data::AttributableMarketFailure{
      market_data::MarketSourceIdentity::from_runtime_source(source), session_epoch,
      receipt.receive_sequence, receipt.received_at, disposition};
  auto outcome = market_states_[source_index].apply_attributable_failure(
      rejected, accepted_market_turn_context_from_accepted_turn(context), trace_sink_, preflight);
  if (!outcome) {
    preflight.cancel_dispatch_plan();
    return model::Result<void>::create_failure(outcome.error());
  }
  auto value = std::move(outcome).value();
  finalize_market_turn_outcome(preflight.plan(), value, std::move(diagnostic));
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Complete callback fan-out and turn later failures into an owner-requested terminal boundary.
void MarketRuntime::finalize_market_turn_outcome(
    const BotDispatchPlan& plan, market_data::MarketTurnOutcome& outcome,
    std::optional<PendingDiagnostic> diagnostic) noexcept {
  std::optional<DomainError> post_commit_fault;
  std::optional<BotDispatchReport> completed_dispatch;

  // ++++++++++++++++++++++++++++++++++++++++
  // Dispatch the full preflighted callback set before noncanonical diagnostic saturation can be
  // observed, preserving state-before-market and run-to-completion guarantees.
  auto dispatched = bot_runtime_->dispatch_callbacks(plan, outcome);
  if (!dispatched) {
    post_commit_fault.emplace(runtime_error_from_turn_error(dispatched.error()));
  } else {
    completed_dispatch.emplace(dispatched.value());
  }
  if (diagnostic) {
    auto appended = diagnostics_.append_diagnostic(diagnostic->kind, std::move(diagnostic->fields));
    if (!appended && !post_commit_fault) {
      post_commit_fault.emplace(appended.error());
    }
  }
  if (!post_commit_fault) {
    if (submission_coordinator_ && submission_coordinator_->is_runtime_faulted()) {
      post_commit_fault =
          submission_coordinator_->terminal_error().value_or(DomainError::create_at_field(
              DomainErrorCode::SubmissionEvidenceExhausted, "market_runtime.submission_runtime"));
    }
  }
  if (!post_commit_fault) {
    post_commit_fault = published_bot_fault(bot_runtime_->status());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The executor must report the applied turn successfully, then expose the fault before another
  // queued command can begin.
  if (post_commit_fault) {
    const auto requested = executor_->request_owner_fault(*post_commit_fault);
    static_cast<void>(requested);
    latch_runtime_fault(*post_commit_fault);
  }
  publish_owner_observations(std::move(completed_dispatch));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preserve the first runtime fault and close later admission outside the publication mutex.
void MarketRuntime::latch_runtime_fault(DomainError error) noexcept {
  {
    std::lock_guard lock{ingress_mutex_};
    if (!fault_) {
      fault_.emplace(std::move(error));
    }
    lifecycle_ = MarketRuntimeLifecycle::Faulted;
  }
  executor_->close();
}

// --------------------------------------------------------
// Record recursive deterministic progression only after executor validation proves same ownership.
std::optional<DomainError> MarketRuntime::record_owner_reentry() noexcept {
  if (!bot_runtime_->is_dispatch_active()) {
    return std::nullopt;
  }
  auto recorded = bot_runtime_->record_owner_reentry();
  publish_owner_observations();
  if (!recorded) {
    return recorded.error();
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Retain the newest complete deterministic report for status and cold evidence.
void MarketRuntime::record_completed_turn_report(const std::optional<TurnReport>& report) {
  if (!report) {
    return;
  }
  std::lock_guard lock{ingress_mutex_};
  last_completed_turn_ = newer_report(last_completed_turn_, report);
}

// --------------------------------------------------------
// Publish owner-local health copies under the sole mutex read by concurrent status callers.
void MarketRuntime::publish_owner_observations(
    std::optional<BotDispatchReport> completed_dispatch) noexcept {
  std::lock_guard lock{ingress_mutex_};
  if (completed_dispatch) {
    published_last_dispatch_.emplace(std::move(*completed_dispatch));
  }
  if (bot_runtime_) {
    published_bot_status_ = bot_runtime_->status();
  }
  published_diagnostic_saturated_ = diagnostics_.is_saturated();
  published_dropped_diagnostics_ = diagnostics_.dropped_count();
}

// --------------------------------------------------------

} // namespace aegis::runtime
