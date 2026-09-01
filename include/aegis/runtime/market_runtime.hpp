// Purpose: compose recorded fixture ingress, transactional source state, canonical bot dispatch,
// and an optional concrete fake-only submission stack behind one bounded serialized owner.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/market_data/recorded_fixture.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/risk/reservation_ledger.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "aegis/runtime/dedicated_executor_driver.hpp"
#include "aegis/runtime/fake_submission_runtime.hpp"
#include "aegis/runtime/runtime_diagnostics.hpp"
#include "aegis/runtime/serialized_executor.hpp"
#include "aegis/runtime/submission_diagnostics.hpp"
#include "aegis/trace/runtime_trace.hpp"
#include "aegis/trace/submission_trace.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The concrete coordinator remains a runtime-private composition detail owned behind a stable
// pointer; only MarketRuntime's out-of-line implementation requires its complete definition.
class SubmissionCoordinator;

// ########################################################################

// ########################################################################
// Runtime lifecycle distinguishes source bootstrap, ordinary operation, deliberate closure, and
// fail-closed suppression after an owner-turn fault.
enum class MarketRuntimeLifecycle : std::uint8_t {
  Starting = 1,
  Running = 2,
  Closed = 3,
  Faulted = 4,
};

// ########################################################################

// ########################################################################
// A synchronized status copy exposes bounded ingress and callback health without publishing any
// mutable book, queue slot, strategy, or trace append capability.
struct MarketRuntimeStatus {
  MarketRuntimeLifecycle lifecycle;
  std::uint32_t initialized_sources;
  ExecutorQueueSnapshot executor;
  BotRuntimeStatus bots;
  std::optional<BotDispatchReport> last_dispatch;
  std::optional<model::DomainError> fault;
  std::optional<TurnReport> last_completed_turn;
  bool dedicated_driver_started;
  bool dedicated_driver_running;
  bool diagnostic_saturated;
  std::uint64_t dropped_diagnostics;

  // --------------------------------------------------------
  // Structural equality makes complete deterministic lifecycle observations comparable.
  friend bool operator==(const MarketRuntimeStatus&, const MarketRuntimeStatus&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Quiescent source evidence copies owner state only after ownership is released and work is either
// drained by closure or terminally suppressed, so no caller receives a live mutable-state alias.
struct MarketRuntimeSourceEvidence {
  model::MarketSourceOrdinal source_ordinal;
  std::optional<market_data::MarketReadiness> readiness;
  std::optional<market_data::BookIdentity> book_identity;
  std::optional<model::SessionEpoch> active_session;
  std::optional<model::SequenceNumber> last_source_sequence;

  // --------------------------------------------------------
  // Structural equality pins the complete source-state observation.
  friend bool operator==(const MarketRuntimeSourceEvidence&,
                         const MarketRuntimeSourceEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One copied OMS row preserves its complete immutable admission plus the final owner-local state.
struct SubmissionOmsOrderEvidence {
  oms::OutboundOrderAdmission admission;
  oms::OutboundOrderState state;

  // --------------------------------------------------------
  // Structural equality compares the immutable admission and final owner-local OMS state.
  friend bool operator==(const SubmissionOmsOrderEvidence&,
                         const SubmissionOmsOrderEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One copied fake accepted slot owns the exact bytes and every assigned local causal identity.
struct SubmissionAcceptedWriteEvidence {
  model::SubmissionAttemptId attempt_id;
  model::EncoderInvocationOrdinal encoder_invocation_ordinal;
  model::InitiatorInvocationOrdinal initiator_invocation_ordinal;
  model::FakeWriteOrdinal write_ordinal;
  std::vector<std::byte> bytes;

  // --------------------------------------------------------
  // Structural equality compares every causal identity and the complete accepted byte copy.
  friend bool operator==(const SubmissionAcceptedWriteEvidence&,
                         const SubmissionAcceptedWriteEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Quiescent M3 evidence owns all deterministic policy identities, canonical and diagnostic
// prefixes, retained OMS/risk state, accepted fake slots, and exact terminal fault status.
struct SubmissionRuntimeEvidence {
  risk::RiskPolicyFingerprint risk_policy_fingerprint;
  model::RiskPolicyRevision risk_policy_revision;
  execution::SubmissionPolicyFingerprint submission_policy_fingerprint;
  std::vector<trace::SubmissionTraceRecord> trace_records;
  std::vector<std::byte> canonical_trace_bytes;
  model::Sha256Digest canonical_trace_digest;
  std::vector<SubmissionDiagnosticRecord> diagnostics;
  std::uint64_t dropped_diagnostics;
  std::vector<SubmissionOmsOrderEvidence> oms_orders;
  std::uint32_t oms_order_count;
  std::vector<risk::ReservationEvidence> held_reservations;
  std::uint32_t held_reservation_count;
  std::vector<risk::RiskScopeExposureEvidence> scope_exposures;
  std::vector<SubmissionAcceptedWriteEvidence> accepted_writes;
  std::uint64_t encoder_invocations_consumed;
  std::uint64_t initiator_invocations_consumed;
  bool runtime_faulted;
  std::optional<model::DomainError> terminal_error;

  // --------------------------------------------------------
  // Structural equality compares the complete detached M3 submission evidence bundle.
  friend bool operator==(const SubmissionRuntimeEvidence&,
                         const SubmissionRuntimeEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Final evidence owns M2 canonical replay state plus the optional M3 fake-submission bundle so it
// remains valid independently of the runtime object's later destruction.
struct MarketRuntimeEvidence {
  configuration::ConfigurationFingerprint configuration_fingerprint;
  RuntimePolicyFingerprint runtime_policy_fingerprint;
  std::vector<trace::RuntimeTraceRecord> trace_records;
  std::vector<std::byte> canonical_trace_bytes;
  model::Sha256Digest canonical_trace_digest;
  std::vector<RuntimeDiagnosticRecord> diagnostics;
  std::uint64_t dropped_diagnostics;
  std::vector<MarketRuntimeSourceEvidence> sources;
  ExecutorQueueSnapshot executor;
  std::optional<BotDispatchReport> last_dispatch;
  std::optional<model::DomainError> fault;
  std::optional<TurnReport> last_completed_turn;
  std::optional<SubmissionRuntimeEvidence> submission;

  // --------------------------------------------------------
  // Structural equality makes whole cold replay bundles directly comparable across drivers.
  friend bool operator==(const MarketRuntimeEvidence&, const MarketRuntimeEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// MarketRuntime is the stable heap-owned composition root and the sole handler allowed to mutate
// source state or install direct fake submission in accepted owner turns.
class MarketRuntime final : public SourceDiscontinuityHandler {
public:

  // --------------------------------------------------------
  // Validate one sealed composition and enqueue the first genuine source-bootstrap owner turn.
  [[nodiscard]] static model::Result<std::unique_ptr<MarketRuntime>>
  create_market_runtime(configuration::StartupConfiguration configuration, RuntimePolicy policy,
                        model::ClockProvider& executor_clock,
                        model::ClockProvider& callback_measurement_clock,
                        std::vector<BotStrategyRegistration> strategies);

  // --------------------------------------------------------
  // Validate and own one concrete credential-free fake stack before any callback can submit.
  [[nodiscard]] static model::Result<std::unique_ptr<MarketRuntime>>
  create_with_fake_submission(configuration::StartupConfiguration configuration,
                              RuntimePolicy policy, model::ClockProvider& executor_clock,
                              model::ClockProvider& callback_measurement_clock,
                              std::vector<BotStrategyRegistration> strategies,
                              FakeSubmissionRuntimeParams submission_params);

  // --------------------------------------------------------
  // Stop a dedicated owner, if present, before borrowed clocks and owned executor state disappear.
  ~MarketRuntime() override;

  // --------------------------------------------------------
  // Stable handler, strategy, and queue addresses make the composition non-copyable and
  // non-movable for its complete lifetime.
  MarketRuntime(const MarketRuntime&) = delete;
  MarketRuntime& operator=(const MarketRuntime&) = delete;
  MarketRuntime(MarketRuntime&&) = delete;
  MarketRuntime& operator=(MarketRuntime&&) = delete;

  // --------------------------------------------------------
  // Admit one caller-owned bounded recorded frame without blocking; optional untrusted source
  // attribution names a loss fence only when it resolves in the sealed runtime policy.
  [[nodiscard]] model::Result<AdmissionDecision>
  try_admit(market_data::IngressFrameAttempt attempt);

  // --------------------------------------------------------
  // Admit one explicit owner-local continuity reset without fabricating recorded-frame identity.
  [[nodiscard]] model::Result<AdmissionDecision>
  try_resynchronize(const model::MarketSourceId& source_id);

  // --------------------------------------------------------
  // Bind deterministic progression to the caller after ensuring no dedicated owner is active.
  [[nodiscard]] model::Result<void> bind_to_current_thread();

  // --------------------------------------------------------
  // Run at most one shared executor turn, recording callback-local recursive drive attempts.
  [[nodiscard]] model::Result<std::optional<TurnReport>> execute_next_turn();

  // --------------------------------------------------------
  // Run no more than the policy bound through the same one-turn wrapper and preserve its last
  // complete queue-age report.
  [[nodiscard]] model::Result<PendingTurnExecutionReport>
  execute_pending_turns(std::size_t maximum_turns);

  // --------------------------------------------------------
  // Release deterministic ownership only outside a callback and active owner turn.
  [[nodiscard]] model::Result<void> release_from_current_thread();

  // --------------------------------------------------------
  // Start one dedicated owner that consumes the same queued bootstrap, frame, and fence turns.
  [[nodiscard]] model::Result<void> start_dedicated();

  // --------------------------------------------------------
  // Graceful dedicated closure rejects later ingress and drains the accepted/fenced prefix.
  // External callers wait for release; the dedicated owner requests stop and returns from its turn.
  void close_and_wait() noexcept;

  // --------------------------------------------------------
  // Close admission without waiting; deterministic callers may continue draining explicitly.
  void close() noexcept;

  // --------------------------------------------------------
  // Copy one synchronized external lifecycle observation.
  [[nodiscard]] MarketRuntimeStatus status() const;

  // --------------------------------------------------------
  // Copy exact replay evidence after owner release and either complete drainage or terminal
  // suppression of the preserved pending prefix.
  [[nodiscard]] model::Result<MarketRuntimeEvidence> collect_quiescent_evidence() const;

  // --------------------------------------------------------
  // Apply an ordered attributable capacity-loss fence before later work for that source.
  [[nodiscard]] model::Result<void>
  on_source_discontinuity(const SourceDiscontinuity& discontinuity,
                          const ControlTurnContext& context) noexcept override;

  // --------------------------------------------------------
private:

  // ########################################################################
  // One construction-time slot transfers an accepted attempt from a serialized producer into its
  // eventual owner turn without retaining caller storage through a raw pointer.
  struct FrameSlot {
    std::optional<market_data::IngressFrameAttempt> attempt;
  };

  // ########################################################################

  // ########################################################################
  // A frame command contains only the stable runtime handle and bounded slot index.
  struct FrameCommand {
    MarketRuntime* runtime;
    std::uint32_t slot_index;
  };

  // ########################################################################

  // ########################################################################
  // Bootstrap commands name one canonical configured source at a stable runtime address.
  struct BootstrapCommand {
    MarketRuntime* runtime;
    std::uint32_t source_index;
  };

  // ########################################################################

  // ########################################################################
  // Resynchronization commands defer one explicit source reset to the serialized owner.
  struct ResynchronizeCommand {
    MarketRuntime* runtime;
    std::uint32_t source_index;
  };

  // ########################################################################

  // ########################################################################
  // A pending noncanonical diagnostic is validated before state work and appended only after the
  // state owner has returned a complete outcome.
  struct PendingDiagnostic {
    RuntimeDiagnosticKind kind;
    RuntimeDiagnosticFields fields;
  };

  // ########################################################################

  // --------------------------------------------------------
  // Construct immutable provenance and fixed sinks at the final stable object address.
  MarketRuntime(configuration::StartupConfiguration configuration, RuntimePolicy policy,
                model::ClockProvider& executor_clock,
                model::ClockProvider& callback_measurement_clock);

  // --------------------------------------------------------
  // Share stable-address construction while keeping observation-only and fake-only entry points
  // explicit and preventing a generic transport dependency from entering the composition root.
  [[nodiscard]] static model::Result<std::unique_ptr<MarketRuntime>>
  create_market_runtime_composition(configuration::StartupConfiguration configuration,
                                    RuntimePolicy policy, model::ClockProvider& executor_clock,
                                    model::ClockProvider& callback_measurement_clock,
                                    std::vector<BotStrategyRegistration> strategies,
                                    std::optional<FakeSubmissionRuntimeParams> submission_params);

  // --------------------------------------------------------
  // Copy the complete optional submission owner only after the enclosing runtime is quiescent.
  [[nodiscard]] model::Result<SubmissionRuntimeEvidence> copy_submission_evidence() const;

  // --------------------------------------------------------
  // Bridge fixed inline executor commands into stable-address owner methods.
  [[nodiscard]] static model::Result<void>
  execute_frame_command(const FrameCommand& command, const AcceptedTurnContext& context) noexcept;
  [[nodiscard]] static model::Result<void>
  execute_bootstrap_command(const BootstrapCommand& command,
                            const AcceptedTurnContext& context) noexcept;
  [[nodiscard]] static model::Result<void>
  execute_resynchronize_command(const ResynchronizeCommand& command,
                                const AcceptedTurnContext& context) noexcept;

  // --------------------------------------------------------
  // Consume one accepted slot and contain parse/normalization failure inside its source boundary.
  [[nodiscard]] model::Result<void> execute_frame_turn(std::uint32_t slot_index,
                                                       const AcceptedTurnContext& context);

  // --------------------------------------------------------
  // Initialize sources in canonical order and enqueue only the next source after this one commits.
  [[nodiscard]] model::Result<void> execute_bootstrap_turn(std::uint32_t source_index,
                                                           const AcceptedTurnContext& context);

  // --------------------------------------------------------
  // Apply one explicit source reset on the serialized owner.
  [[nodiscard]] model::Result<void>
  execute_resynchronization_turn(std::uint32_t source_index, const AcceptedTurnContext& context);

  // --------------------------------------------------------
  // Route one complete normalized command through preflight, transactional state, and dispatch.
  [[nodiscard]] model::Result<void>
  execute_normalized_market_turn(market_data::NormalizedRecordedMarketCommand command,
                                 const AcceptedTurnContext& context);

  // --------------------------------------------------------
  // Convert one attributable malformed/unsupported frame into a sanitized state outcome.
  [[nodiscard]] model::Result<void> execute_attributable_failure_turn(
      const RuntimeSource& source, model::SessionEpoch session_epoch,
      const AdmissionReceipt& receipt, trace::RuntimeInputDisposition disposition,
      PendingDiagnostic diagnostic, const AcceptedTurnContext& context);

  // --------------------------------------------------------
  // Dispatch one successful state-machine outcome and convert every later failure into a latched
  // close while preserving the applied turn's successful executor report.
  void finalize_market_turn_outcome(const BotDispatchPlan& plan,
                                    market_data::MarketTurnOutcome& outcome,
                                    std::optional<PendingDiagnostic> diagnostic) noexcept;

  // --------------------------------------------------------
  // Preserve the first runtime fault, reject later ingress, and suppress subsequent callbacks.
  void latch_runtime_fault(model::DomainError error) noexcept;

  // --------------------------------------------------------
  // Reject recursive owner progression through BotRuntime's callback-local evidence path.
  [[nodiscard]] std::optional<model::DomainError> record_owner_reentry() noexcept;

  // --------------------------------------------------------
  // Copy a completed deterministic report for later status and quiescent evidence.
  void record_completed_turn_report(const std::optional<TurnReport>& report);

  // --------------------------------------------------------
  // Publish synchronized copies of owner-local bot and diagnostic health for concurrent status
  // readers without granting them access to either mutable subsystem.
  void publish_owner_observations(
      std::optional<BotDispatchReport> completed_dispatch = std::nullopt) noexcept;

  // --------------------------------------------------------
  configuration::StartupConfiguration configuration_;
  RuntimePolicy policy_;
  model::ClockProvider* executor_clock_;
  model::ClockProvider* callback_measurement_clock_;
  trace::RuntimeTraceSink trace_sink_;
  RuntimeDiagnosticSink diagnostics_;
  std::vector<market_data::MarketStateMachine> market_states_;
  std::unique_ptr<SubmissionCoordinator> submission_coordinator_;
  std::unique_ptr<BotRuntime> bot_runtime_;
  std::unique_ptr<SerializedExecutor> executor_;
  std::unique_ptr<DeterministicExecutorDriver> deterministic_driver_;

  mutable std::mutex ingress_mutex_;
  std::vector<FrameSlot> frame_slots_;
  MarketRuntimeLifecycle lifecycle_{MarketRuntimeLifecycle::Starting};
  std::uint32_t initialized_sources_{0U};
  std::optional<model::DomainError> fault_;
  std::optional<TurnReport> last_completed_turn_;
  BotRuntimeStatus published_bot_status_{};
  std::optional<BotDispatchReport> published_last_dispatch_;
  bool published_diagnostic_saturated_{false};
  std::uint64_t published_dropped_diagnostics_{0U};

  mutable std::mutex driver_mutex_;
  std::unique_ptr<DedicatedExecutorDriver> dedicated_driver_;
};

// ########################################################################

} // namespace aegis::runtime
