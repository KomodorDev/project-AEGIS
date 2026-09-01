// Purpose: compose the deterministic M3 submission stack, install one recovery-bound read-only M4
// planning child while pristine, and execute the unchanged synchronous submission path.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/execution/submission_policy.hpp"
#include "aegis/execution/submission_route.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/organization/organization.hpp"
#include "aegis/risk/reservation_ledger.hpp"
#include "aegis/risk/risk_policy.hpp"
#include "aegis/runtime/fake_submission_runtime.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/runtime/submission_diagnostics.hpp"
#include "aegis/trace/submission_trace.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace aegis::recovery {

namespace detail {

// ########################################################################
// Opaque lease control keeps the installed recovery incarnation live without exposing its medium.
struct FakeJournalLeaseControl;

// ########################################################################

} // namespace detail

// ########################################################################
// A completed bootstrap supplies the acknowledged namespace and lease consumed during M4 install.
class RecoveryBootstrap;

// ########################################################################

} // namespace aegis::recovery

namespace aegis::runtime {

// ########################################################################
// BotContext is the sole runtime authority allowed to enter the private submission coordinator.
class BotContext;

// ########################################################################

// ########################################################################
// BotRuntime alone may close recovery installation before callback authority becomes reachable.
class BotRuntime;

// ########################################################################

// ########################################################################
// The immutable M4 policy is borrowed only while an owner-bound planning child validates and copies
// it.
class M4Policy;

// ########################################################################

// ########################################################################
// The source-private identity planner remains incomplete at this public M3 boundary.
class PrivateOrderReconciler;

// ########################################################################

// ########################################################################
// Source-private fault points let focused tests force exact canonical-append containment branches
// without installing a callback, polymorphic fault source, or public runtime capability.
enum class TraceAppendFaultPointForTest : std::uint8_t {
  FirstReentryRejected = 1,
  RiskReservedBeforeOms = 2,
  WriteInitiatedAfterAcceptance = 3,
  SubmissionCompletedAfterInitiation = 4,
};

// ########################################################################

// ########################################################################
// SubmissionCoordinator owns every mutable M3 component and is the sole direct-path entry below
// BotContext. Before any M3 activity or callback-capable BotRuntime exists, it may consume one
// acknowledged recovery bootstrap into the active identity stream and one read-only M4 planning
// child, which is destroyed first; no public operation grants it event-application or mutation
// authority.
class SubmissionCoordinator final {
public:

  // --------------------------------------------------------
  // Validate route projection and both immutable policies, then preallocate every owner-local
  // table/sink/fake before returning a submission-capable stack.
  [[nodiscard]] static model::Result<std::unique_ptr<SubmissionCoordinator>>
  create_submission_coordinator(const configuration::StartupConfiguration& configuration,
                                const runtime::RuntimePolicy& runtime_policy,
                                FakeSubmissionRuntimeParams params);

  // --------------------------------------------------------
  // Keep the final coordinator address and every uniquely owned M3/M4 component stable.
  SubmissionCoordinator(const SubmissionCoordinator&) = delete;
  SubmissionCoordinator& operator=(const SubmissionCoordinator&) = delete;
  SubmissionCoordinator(SubmissionCoordinator&&) = delete;
  SubmissionCoordinator& operator=(SubmissionCoordinator&&) = delete;

  // --------------------------------------------------------
  // Destroy the source-private child while its complete type and every borrowed owner component are
  // still available; destruction performs no event processing.
  ~SubmissionCoordinator();

  // --------------------------------------------------------
  // Consume one namespace-acknowledged recovery bootstrap and install a fully allocated planning
  // child only before callback authority attaches and while every M3 activity, evidence, fault,
  // and test-probe field remains pristine. Any reported failure leaves both owner and bootstrap
  // unchanged; success replaces the unused construction-time identity stream before publishing
  // the read-only child.
  [[nodiscard]] model::Result<void> install_recovery_bound_private_order_reconciler(
      const configuration::StartupConfiguration& configuration, const M4Policy& policy,
      recovery::RecoveryBootstrap&& recovery_bootstrap);

  // --------------------------------------------------------
  // Borrow the installed read-only planning child, or return null before successful installation.
  [[nodiscard]] const PrivateOrderReconciler* private_order_reconciler() const noexcept {
    return private_order_reconciler_.get();
  }

  // --------------------------------------------------------
  // Borrow the exact installed route catalog used by the synchronous M3 owner.
  [[nodiscard]] const execution::OwnerLocalRouteCatalog& routes() const noexcept { return routes_; }

  // --------------------------------------------------------
  // Borrow the exact owner-local reservation ledger used by synchronous submission.
  [[nodiscard]] const risk::ReservationLedger& reservations() const noexcept { return ledger_; }

  // --------------------------------------------------------
  // Borrow the immutable submission policy that sized and authorized this owner.
  [[nodiscard]] const execution::SubmissionPolicy& policy() const noexcept { return policy_; }

  // --------------------------------------------------------
  // Borrow the exact outbound OMS owned by the synchronous submission path.
  [[nodiscard]] const oms::OutboundOms& outbound_oms() const noexcept { return outbound_oms_; }

  // --------------------------------------------------------
  // Borrow the deterministic credential-free encoder used by this owner.
  [[nodiscard]] const execution::DeterministicFakeOrderEncoder& encoder() const noexcept {
    return encoder_;
  }

  // --------------------------------------------------------
  // Borrow the deterministic credential-free write initiator used by this owner.
  [[nodiscard]] const execution::DeterministicFakeWriteInitiator& initiator() const noexcept {
    return initiator_;
  }

  // --------------------------------------------------------
  // Borrow the bounded canonical submission-trace prefix retained by this owner.
  [[nodiscard]] const trace::SubmissionTraceSink& trace_sink() const noexcept {
    return trace_sink_;
  }

  // --------------------------------------------------------
  // Borrow the bounded submission-diagnostic prefix retained by this owner.
  [[nodiscard]] const runtime::SubmissionDiagnosticSink& diagnostics() const noexcept {
    return diagnostics_;
  }

  // --------------------------------------------------------
  // Return whether an impossible lower-layer invariant has permanently faulted this owner.
  [[nodiscard]] bool is_runtime_faulted() const noexcept { return runtime_faulted_; }

  // --------------------------------------------------------
  // Borrow the first terminal owner error, or typed absence while no fault is latched.
  [[nodiscard]] const std::optional<model::DomainError>& terminal_error() const noexcept {
    return terminal_error_;
  }

  // --------------------------------------------------------
  // Arm one private-header-only, copied-data probe. It exposes no callback binding, submit entry,
  // or mutable-state alias and accepts only the fixed bounded count used to prove genuine re-entry.
  [[nodiscard]] bool arm_reentry_probe_for_test(execution::OrderRequest request,
                                                std::uint32_t requested_attempts);

  // --------------------------------------------------------
  // Arm one source-private, fixed-enum, one-shot canonical append failure while no submit is
  // active.
  [[nodiscard]] bool arm_trace_append_fault_for_test(TraceAppendFaultPointForTest point) noexcept;

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only BotContext may mint callback authority, read the entry clock, or enter the coordinator.
  friend class BotContext;

  // ########################################################################

  // ########################################################################
  // Only a successfully constructed BotRuntime may permanently close the recovery-install seam.
  friend class BotRuntime;

  // ########################################################################

  // --------------------------------------------------------
  // Permanently reject later recovery installation before callback authority can escape its
  // successful factory; repeated closure is an idempotent startup-composition operation.
  void close_recovery_installation_before_callback_authority() noexcept {
    recovery_installation_closed_ = true;
  }

  // --------------------------------------------------------

  // ########################################################################
  // BotContext supplies this private binding from its active context; OrderRequest cannot author
  // attribution, owner-turn, callback, or processing-time fields.
  struct CallbackBinding {
    BotContext* context;
    const organization::BotAttribution* attribution;
    model::TurnOrdinal owner_turn_ordinal;
    model::CallbackOrdinal callback_ordinal;
    model::ProcessingTimestamp processing_timestamp;
  };

  // ########################################################################

  // ########################################################################
  // This private fixed-data probe lets focused tests request bounded genuine BotContext re-entry at
  // the identity seam without installing a callback, virtual provider, or public submission seam.
  struct ReentryProbe {
    static constexpr std::uint32_t maximum_attempts = 3U;

    execution::OrderRequest request;
    std::uint32_t requested_attempts;
    bool armed{true};
  };

  // ########################################################################

  // ########################################################################
  // One post-risk guard owns the sole rollback right. Explicit release or accepted-copy retention
  // disarms it before any return; its destructor contains an otherwise impossible missed branch.
  class ReservationRollbackGuard final {
  public:

    // ########################################################################
    // The guard distinguishes an unused rollback right, an exact release, conservative retention,
    // and a consumed lower-layer fault that must never be retried.
    enum class RollbackState : std::uint8_t {
      Armed = 1,
      Released = 2,
      Retained = 3,
      FaultedConsumed = 4,
    };

    // ########################################################################

    // --------------------------------------------------------
    // Bind one armed rollback right to the exact reservation and callback-local owner state.
    ReservationRollbackGuard(SubmissionCoordinator& coordinator,
                             model::ReservationId reservation_id, CallbackBinding binding,
                             model::OrderId order_id) noexcept;

    // --------------------------------------------------------
    // Prevent duplicate rollback rights or reseating, and exercise any still-armed right at
    // destruction.
    ReservationRollbackGuard(const ReservationRollbackGuard&) = delete;
    ReservationRollbackGuard& operator=(const ReservationRollbackGuard&) = delete;
    ReservationRollbackGuard(ReservationRollbackGuard&&) = delete;
    ReservationRollbackGuard& operator=(ReservationRollbackGuard&&) = delete;
    ~ReservationRollbackGuard() noexcept;

    // --------------------------------------------------------
    // Exercise the rollback right at most once, disarming before the lower-layer transition.
    [[nodiscard]] bool release_reservation(execution::SubmissionStage stage,
                                           execution::SubmissionReason reason) noexcept;

    // --------------------------------------------------------
    // Retain conservative exposure exactly once after directly validating the fake's accepted slot.
    [[nodiscard]] bool
    retain_after_acceptance(const execution::FakeInitiationResult& initiation) noexcept;

    // --------------------------------------------------------
    // Return the guard's exact one-way lifecycle state.
    [[nodiscard]] RollbackState rollback_state() const noexcept { return state_; }

    // --------------------------------------------------------
    // Return the exact reservation protected by this guard.
    [[nodiscard]] model::ReservationId reservation_id() const noexcept { return reservation_id_; }

    // --------------------------------------------------------
  private:
    SubmissionCoordinator* coordinator_;
    model::ReservationId reservation_id_;
    CallbackBinding binding_;
    model::OrderId order_id_;
    RollbackState state_{RollbackState::Armed};
  };

  // ########################################################################

  // --------------------------------------------------------
  // Execute the complete immediate route-to-fake path using BotContext's private binding and entry
  // timestamp; no queue, executor, coroutine, or I/O boundary is used.
  [[nodiscard]] execution::SubmitResult
  submit_order(const CallbackBinding& binding, const execution::OrderRequest& request,
               std::optional<std::uint64_t> entry_started) noexcept;

  // --------------------------------------------------------
  // Take one dedicated thread-safe clock sample without consulting or mutating owner-local state;
  // deterministic clocks visibly consume at most one scripted reading.
  [[nodiscard]] std::optional<std::uint64_t> take_measurement_nanosecond_reading() noexcept {
    return measurement_clock_->take_nanosecond_reading();
  }

  // --------------------------------------------------------
  // Publish only one fully constructed stack whose component references can never be reseated.
  SubmissionCoordinator(execution::OwnerLocalRouteCatalog routes, risk::ReservationLedger ledger,
                        execution::SubmissionPolicy policy, oms::OutboundOms outbound_oms,
                        execution::DeterministicFakeOrderEncoder encoder,
                        execution::DeterministicFakeWriteInitiator initiator,
                        std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock,
                        model::DeterministicOrderIdSource order_ids,
                        trace::SubmissionTraceProvenance trace_provenance,
                        runtime::SubmissionDiagnosticProvenance diagnostic_provenance);

  // --------------------------------------------------------
  // Preserve the first exact lower-layer invariant failure for enclosing-runtime publication.
  void latch_runtime_fault(model::DomainError error) noexcept;

  // --------------------------------------------------------
  // Append one canonical event or latch the submission runtime when an impossible post-preflight
  // evidence invariant occurs.
  [[nodiscard]] bool append_trace(trace::SubmissionTraceEventKind kind,
                                  const trace::SubmissionTraceFields& fields) noexcept;

  // --------------------------------------------------------
  // Consume exactly one matching source-private fault point without changing the accepted prefix.
  [[nodiscard]] bool
  consume_trace_append_fault_for_test(trace::SubmissionTraceEventKind kind,
                                      const trace::SubmissionTraceFields& fields) noexcept;

  // --------------------------------------------------------
  // Calculate one checked noncanonical duration and emit the sole permitted unavailable-time
  // observation after owner, attempt, and trace-preflight authority all exist.
  [[nodiscard]] std::optional<std::uint64_t>
  finish_measurement(std::optional<std::uint64_t> entry_started,
                     std::optional<std::uint64_t> measurement_finished,
                     const trace::SubmissionTraceFields& fields, const CallbackBinding& binding,
                     execution::SubmissionStage stage, execution::SubmissionReason reason) noexcept;

  // --------------------------------------------------------
  // Return one measured rejection after appending its canonical completion snapshot.
  [[nodiscard]] execution::SubmitResult
  complete_rejection(trace::SubmissionTraceFields& fields, execution::SubmissionStage stage,
                     execution::SubmissionReason reason, std::optional<model::OrderId> order_id,
                     std::optional<execution::RiskLimitEvidence> risk_evidence,
                     const CallbackBinding& binding,
                     std::optional<std::uint64_t> entry_started) noexcept;

  // --------------------------------------------------------
  // Latch an impossible current-attempt fault and conservatively preserve post-copy uncertainty.
  [[nodiscard]] execution::SubmitResult fail_internal(
      trace::SubmissionTraceFields& fields, ReservationRollbackGuard* rollback,
      std::optional<model::OrderId> order_id, const CallbackBinding& binding,
      std::optional<std::uint64_t> entry_started, bool canonical_append_failed = false,
      const std::optional<std::uint64_t>* captured_measurement_finished = nullptr) noexcept;

  // --------------------------------------------------------
  // Retain every M3 owner component, activity/fault latch, and the last-declared M4 planning child;
  // the lease declared before the identity stream outlives that provider during reverse-order
  // destruction, and all mutable fields remain source-private to the serialized submission path.
  execution::OwnerLocalRouteCatalog routes_;
  risk::ReservationLedger ledger_;
  execution::SubmissionPolicy policy_;
  oms::OutboundOms outbound_oms_;
  execution::DeterministicFakeOrderEncoder encoder_;
  execution::DeterministicFakeWriteInitiator initiator_;
  std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock_;
  std::shared_ptr<recovery::detail::FakeJournalLeaseControl> recovery_identity_lease_;
  model::DeterministicOrderIdSource order_ids_;
  trace::SubmissionTraceSink trace_sink_;
  runtime::SubmissionDiagnosticSink diagnostics_;
  bool recovery_installation_closed_{false};
  std::uint64_t attempts_consumed_{0U};
  bool submit_active_{false};
  bool reentry_traced_{false};
  bool runtime_faulted_{false};
  std::optional<model::DomainError> terminal_error_;
  std::optional<trace::SubmissionTraceContext> active_trace_context_;
  std::optional<ReentryProbe> reentry_probe_;
  std::optional<TraceAppendFaultPointForTest> trace_append_fault_for_test_;
  // Declaring the source-private child last makes default destruction release it before every M3
  // component whose immutable views it retains.
  std::unique_ptr<PrivateOrderReconciler> private_order_reconciler_;
};

// ########################################################################

} // namespace aegis::runtime
