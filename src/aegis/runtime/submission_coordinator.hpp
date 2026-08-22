// Purpose: compose the complete deterministic M3 submission stack at the runtime layer and execute
// route, risk, OMS, exact encoding, and offline fake initiation synchronously on one owner turn.

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

namespace aegis::runtime {

// ########################################################################
// BotContext is the sole runtime authority allowed to enter the private submission coordinator.
class BotContext;

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
// BotContext. It exposes inspection only as immutable evidence and lower-layer state views.
class SubmissionCoordinator final {
public:

  // --------------------------------------------------------
  // Validate route projection and both immutable policies, then preallocate every owner-local
  // table/sink/fake before returning a submission-capable stack.
  [[nodiscard]] static model::Result<std::unique_ptr<SubmissionCoordinator>>
  create(const configuration::StartupConfiguration& configuration,
         const runtime::RuntimePolicy& runtime_policy, FakeSubmissionRuntimeParams params);

  // --------------------------------------------------------
  SubmissionCoordinator(const SubmissionCoordinator&) = delete;
  SubmissionCoordinator& operator=(const SubmissionCoordinator&) = delete;
  SubmissionCoordinator(SubmissionCoordinator&&) = delete;
  SubmissionCoordinator& operator=(SubmissionCoordinator&&) = delete;

  [[nodiscard]] const execution::OwnerLocalRouteCatalog& routes() const noexcept { return routes_; }

  // --------------------------------------------------------
  [[nodiscard]] const risk::ReservationLedger& reservations() const noexcept { return ledger_; }

  // --------------------------------------------------------
  [[nodiscard]] const execution::SubmissionPolicy& policy() const noexcept { return policy_; }

  // --------------------------------------------------------
  [[nodiscard]] const oms::OutboundOms& outbound_oms() const noexcept { return outbound_oms_; }

  // --------------------------------------------------------
  [[nodiscard]] const execution::DeterministicFakeOrderEncoder& encoder() const noexcept {
    return encoder_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const execution::DeterministicFakeWriteInitiator& initiator() const noexcept {
    return initiator_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const trace::SubmissionTraceSink& trace_sink() const noexcept {
    return trace_sink_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const runtime::SubmissionDiagnosticSink& diagnostics() const noexcept {
    return diagnostics_;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool runtime_faulted() const noexcept { return runtime_faulted_; }

  // --------------------------------------------------------
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
    enum class State : std::uint8_t {
      Armed = 1,
      Released = 2,
      Retained = 3,
      FaultedConsumed = 4,
    };

    // ########################################################################

    // --------------------------------------------------------
    ReservationRollbackGuard(SubmissionCoordinator& coordinator,
                             model::ReservationId reservation_id, CallbackBinding binding,
                             model::OrderId order_id) noexcept;

    // --------------------------------------------------------
    ReservationRollbackGuard(const ReservationRollbackGuard&) = delete;
    ReservationRollbackGuard& operator=(const ReservationRollbackGuard&) = delete;
    ReservationRollbackGuard(ReservationRollbackGuard&&) = delete;
    ReservationRollbackGuard& operator=(ReservationRollbackGuard&&) = delete;
    ~ReservationRollbackGuard() noexcept;

    // --------------------------------------------------------
    // Exercise the rollback right at most once, disarming before the lower-layer transition.
    [[nodiscard]] bool release(execution::SubmissionStage stage,
                               execution::SubmissionReason reason) noexcept;

    // --------------------------------------------------------
    // Retain conservative exposure exactly once after directly validating the fake's accepted slot.
    [[nodiscard]] bool
    retain_after_acceptance(const execution::FakeInitiationResult& initiation) noexcept;

    // --------------------------------------------------------
    [[nodiscard]] State state() const noexcept { return state_; }

    // --------------------------------------------------------
    [[nodiscard]] model::ReservationId reservation_id() const noexcept { return reservation_id_; }

    // --------------------------------------------------------
  private:
    SubmissionCoordinator* coordinator_;
    model::ReservationId reservation_id_;
    CallbackBinding binding_;
    model::OrderId order_id_;
    State state_{State::Armed};
  };

  // ########################################################################

  // --------------------------------------------------------
  // Execute the complete immediate route-to-fake path using BotContext's private binding and entry
  // timestamp; no queue, executor, coroutine, or I/O boundary is used.
  [[nodiscard]] execution::SubmitResult submit(const CallbackBinding& binding,
                                               const execution::OrderRequest& request,
                                               std::optional<std::uint64_t> entry_started) noexcept;

  // --------------------------------------------------------
  // Read the dedicated thread-safe clock without consulting or mutating any owner-local state.
  [[nodiscard]] std::optional<std::uint64_t> measurement_now() noexcept {
    return measurement_clock_->now_nanoseconds();
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
  execution::OwnerLocalRouteCatalog routes_;
  risk::ReservationLedger ledger_;
  execution::SubmissionPolicy policy_;
  oms::OutboundOms outbound_oms_;
  execution::DeterministicFakeOrderEncoder encoder_;
  execution::DeterministicFakeWriteInitiator initiator_;
  std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock_;
  model::DeterministicOrderIdSource order_ids_;
  trace::SubmissionTraceSink trace_sink_;
  runtime::SubmissionDiagnosticSink diagnostics_;
  std::uint64_t attempts_consumed_{0U};
  bool submit_active_{false};
  bool reentry_traced_{false};
  bool runtime_faulted_{false};
  std::optional<model::DomainError> terminal_error_;
  std::optional<trace::SubmissionTraceContext> active_trace_context_;
  std::optional<ReentryProbe> reentry_probe_;
  std::optional<TraceAppendFaultPointForTest> trace_append_fault_for_test_;
};

// ########################################################################

} // namespace aegis::runtime
