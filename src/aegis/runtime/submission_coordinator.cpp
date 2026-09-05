// Purpose: construct the credential-free M3 submission stack, recovery-bind one M4 private owner
// while pristine, and reject unsafe accounts before synchronous risk reservation.

#include "submission_coordinator.hpp"

#include "aegis/execution/order_validation.hpp"
#include "aegis/model/domain_error.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/runtime/bot_runtime.hpp"
#include "private_order_reconciler.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::runtime {

using execution::DeterministicFakeOrderEncoder;
using execution::DeterministicFakeWriteInitiator;
using execution::FakeInitiationOutcome;
using execution::InstalledSubmissionRoute;
using execution::OrderRequest;
using execution::OwnerLocalRouteCatalog;
using execution::RiskLimitEvidence;
using execution::SubmissionCapability;
using execution::SubmissionMeasurementClock;
using execution::SubmissionPolicy;
using execution::SubmissionPolicyParams;
using execution::SubmissionReason;
using execution::SubmissionRouteInput;
using execution::SubmissionStage;
using execution::SubmitResult;

namespace {

// ########################################################################
// Reset the coordinator's callback-local recursion state on every ordinary return path.
class SubmissionActiveGuard final {
public:

  // --------------------------------------------------------
  // Mark one callback-local submission active and retain its trace context for the guarded scope.
  SubmissionActiveGuard(bool& active,
                        std::optional<trace::SubmissionTraceContext>& context) noexcept
      : active_{active}, context_{context} {
    active_ = true;
  }

  // --------------------------------------------------------
  // Clear the callback-local trace context before reopening the owner to another submission.
  ~SubmissionActiveGuard() {
    context_.reset();
    active_ = false;
  }

  // --------------------------------------------------------
private:
  // Retain aliases to the two callback-local fields whose cleanup this scope guarantees.
  bool& active_;
  std::optional<trace::SubmissionTraceContext>& context_;
};

// ########################################################################

// --------------------------------------------------------
// Project StartupConfiguration into the narrow dependency-safe owner-local route input contract.
[[nodiscard]] model::Result<OwnerLocalRouteCatalog>
create_route_catalog(const configuration::StartupConfiguration& configuration) {
  std::vector<SubmissionRouteInput> inputs;
  inputs.reserve(configuration.routes().routes().size());
  for (const auto& route : configuration.routes().routes()) {
    const auto* const attribution = configuration.organization().find_bot(route.bot_id);
    const auto* const metadata =
        configuration.find_instrument_metadata(route.venue_id, route.instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      return model::Result<OwnerLocalRouteCatalog>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidRelationship,
                                              "submission_coordinator.routes"));
    }
    inputs.push_back(SubmissionRouteInput{route, *attribution, *metadata});
  }
  const auto& provenance = configuration.provenance();
  return OwnerLocalRouteCatalog::create_owner_local_route_catalog(
      configuration.fingerprint(), provenance.configuration_revision(),
      provenance.organization_revision(), provenance.route_revision(), std::move(inputs));
}

// --------------------------------------------------------
// Calculate the exact longest positional AEGISFOE record without creating an order or calling the
// encoder; every identifier contributes its two-byte length prefix plus accepted ASCII bytes.
[[nodiscard]] model::Result<std::uint64_t>
calculate_required_encoded_order_bytes(const OwnerLocalRouteCatalog& routes) {
  constexpr std::uint64_t fixed_bytes = 8U + 2U + model::OrderId::byte_size + (9U * 2U) + 3U +
                                        (2U * 9U) + (4U * model::sha256_digest_size) +
                                        (5U * sizeof(std::uint64_t));
  std::uint64_t maximum = fixed_bytes;
  for (const auto& installed : routes.routes()) {
    const auto& route = installed.route();
    const auto& attribution = installed.attribution();
    const std::string_view identifiers[]{
        route.id.value(),
        route.venue_id.value(),
        route.logical_account_id.value(),
        route.instrument_id.value(),
        installed.metadata().venue_instrument_id().value(),
        attribution.firm_id.value(),
        attribution.desk_id.value(),
        attribution.bot_id.value(),
        attribution.strategy_id.value(),
    };
    std::uint64_t candidate = fixed_bytes;
    for (const auto identifier : identifiers) {
      if (identifier.size() > std::numeric_limits<std::uint16_t>::max() ||
          identifier.size() > std::numeric_limits<std::uint64_t>::max() - candidate) {
        return model::Result<std::uint64_t>::create_failure(
            model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                                "submission_policy.required_encoded_order_bytes"));
      }
      candidate += static_cast<std::uint64_t>(identifier.size());
    }
    if (candidate > maximum) {
      maximum = candidate;
    }
  }
  return model::Result<std::uint64_t>::create_success(maximum);
}

// --------------------------------------------------------
// Subtract only available ordered readings so regression and unsigned wrap remain unrepresentable.
[[nodiscard]] std::optional<std::uint64_t>
calculate_local_duration(std::optional<std::uint64_t> started,
                         std::optional<std::uint64_t> finished) noexcept {
  if (!started || !finished || *finished < *started) {
    return std::nullopt;
  }
  return *finished - *started;
}

// --------------------------------------------------------
// Copy the exact installed route projection into canonical submission evidence.
[[nodiscard]] trace::AuthorizedSubmissionProjection
authorized_submission_projection_from_installed_route(const InstalledSubmissionRoute& route) {
  return trace::AuthorizedSubmissionProjection{
      route.route().id,
      route.route().venue_id,
      route.route().logical_account_id,
      route.route().instrument_id,
      route.metadata().venue_instrument_id(),
      route.metadata().revision(),
  };
}

// --------------------------------------------------------
// Initialize every optional causal group explicitly so strict missing-field warnings remain useful.
[[nodiscard]] trace::SubmissionTraceFields
submission_trace_fields_from_context(const trace::SubmissionTraceContext& context) {
  return trace::SubmissionTraceFields{
      context,      std::nullopt,
      std::nullopt, std::nullopt,
      std::nullopt, std::nullopt,
      std::nullopt, std::nullopt,
      std::nullopt, trace::SubmissionReleaseTransition::None,
      std::nullopt,
  };
}

// --------------------------------------------------------
// Bind one admitted order to every immutable startup and policy identity used by the decision.
[[nodiscard]] oms::OutboundOrderProvenance
create_outbound_order_provenance(const InstalledSubmissionRoute& installed,
                                 const risk::RiskPolicySnapshot& risk_policy,
                                 const SubmissionPolicy& submission_policy,
                                 const model::Sha256Digest& runtime_policy_fingerprint) {
  const auto& route = installed.route();
  const auto& attribution = installed.attribution();
  return oms::OutboundOrderProvenance{
      route.id,
      route.venue_id,
      route.logical_account_id,
      route.instrument_id,
      installed.metadata().venue_instrument_id(),
      attribution.firm_id,
      attribution.desk_id,
      attribution.bot_id,
      attribution.strategy_id,
      installed.configuration_fingerprint().bytes(),
      installed.configuration_revision(),
      installed.organization_revision(),
      installed.route_revision(),
      installed.metadata().revision(),
      runtime_policy_fingerprint,
      risk_policy.fingerprint().bytes(),
      risk_policy.revision(),
      submission_policy.fingerprint().bytes(),
  };
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Construct each fallible immutable or bounded component before publishing one stable heap owner.
// Interesting syntax: the function-try-block translates allocation failures from every construction
// phase, including the final coordinator member initialization, through one stable policy error.
model::Result<std::unique_ptr<SubmissionCoordinator>>
SubmissionCoordinator::create_submission_coordinator(
    const configuration::StartupConfiguration& configuration, const RuntimePolicy& runtime_policy,
    FakeSubmissionRuntimeParams params) try {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject absent direct-path authorities and any M2 policy from another sealed configuration.
  if (!params.measurement_clock ||
      runtime_policy.configuration_fingerprint() != configuration.fingerprint()) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidRelationship,
                                            "submission_coordinator.provenance"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Install routes first because risk completeness and encoded byte capacity depend on them.
  auto route_catalog = create_route_catalog(configuration);
  if (!route_catalog) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
        route_catalog.error());
  }
  auto required_bytes = calculate_required_encoded_order_bytes(route_catalog.value());
  if (!required_bytes) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
        required_bytes.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal fixed risk against the same route/configuration authority, then bind AEGISSUP to its hash.
  auto risk_policy = risk::RiskPolicySnapshot::create_risk_policy_snapshot(
      std::move(params.risk_policy), configuration, route_catalog.value());
  if (!risk_policy) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
        risk_policy.error());
  }
  auto submission_policy = SubmissionPolicy::create_submission_policy(SubmissionPolicyParams{
      SubmissionCapability::DeterministicFakeOnly,
      configuration.fingerprint().bytes(),
      runtime_policy.fingerprint().bytes(),
      risk_policy.value().fingerprint().bytes(),
      risk_policy.value().revision(),
      params.capacities,
      required_bytes.value(),
      std::move(params.encoder_script),
      std::move(params.initiator_script),
  });
  if (!submission_policy) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
        submission_policy.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate the ledger, OMS, and two final offline fakes from the single accepted capacity set.
  const auto capacities = submission_policy.value().capacities();
  auto ledger = risk::ReservationLedger::create_reservation_ledger(std::move(risk_policy).value(),
                                                                   capacities.reservation_capacity);
  auto outbound = oms::OutboundOms::create_outbound_oms(capacities.oms_order_capacity);
  auto encoder = DeterministicFakeOrderEncoder::create_deterministic_fake_order_encoder(
      submission_policy.value().encoder_script(), capacities.encoded_byte_capacity);
  auto initiator = DeterministicFakeWriteInitiator::create_deterministic_fake_write_initiator(
      submission_policy.value().initiator_script(), capacities.accepted_write_capacity);
  if (!ledger) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(ledger.error());
  }
  if (!outbound) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(outbound.error());
  }
  if (!encoder) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(encoder.error());
  }
  if (!initiator) {
    return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(initiator.error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Project raw provenance into dependency-light evidence sinks before moving component owners.
  const auto& provenance = configuration.provenance();
  const trace::SubmissionTraceProvenance trace_provenance{
      configuration.fingerprint().bytes(),  provenance.configuration_revision(),
      provenance.organization_revision(),   provenance.route_revision(),
      runtime_policy.fingerprint().bytes(), ledger.value().policy().fingerprint().bytes(),
      ledger.value().policy().revision(),   submission_policy.value().fingerprint().bytes(),
  };
  const SubmissionDiagnosticProvenance diagnostic_provenance{
      configuration.fingerprint().bytes(), ledger.value().policy().fingerprint().bytes(),
      submission_policy.value().fingerprint().bytes()};

  auto coordinator = std::unique_ptr<SubmissionCoordinator>{new SubmissionCoordinator{
      std::move(route_catalog).value(), std::move(ledger).value(),
      std::move(submission_policy).value(), std::move(outbound).value(), std::move(encoder).value(),
      std::move(initiator).value(), std::move(params.measurement_clock),
      std::move(params.order_ids), trace_provenance, diagnostic_provenance}};
  return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_success(
      std::move(coordinator));

  // ++++++++++++++++++++++++++++++++++++++++
} catch (const std::bad_alloc&) {
  return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                          "submission_policy.capacity_allocation"));
} catch (const std::length_error&) {
  return model::Result<std::unique_ptr<SubmissionCoordinator>>::create_failure(
      model::DomainError::create_at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                          "submission_policy.capacity_allocation"));
}

// --------------------------------------------------------
// Initialize non-movable evidence sinks directly after every movable component reaches final state.
SubmissionCoordinator::SubmissionCoordinator(
    OwnerLocalRouteCatalog routes, risk::ReservationLedger ledger, SubmissionPolicy policy,
    oms::OutboundOms outbound_oms, DeterministicFakeOrderEncoder encoder,
    DeterministicFakeWriteInitiator initiator,
    std::unique_ptr<SubmissionMeasurementClock> measurement_clock,
    model::DeterministicOrderIdSource order_ids, trace::SubmissionTraceProvenance trace_provenance,
    SubmissionDiagnosticProvenance diagnostic_provenance)
    : routes_{std::move(routes)}, ledger_{std::move(ledger)}, policy_{std::move(policy)},
      outbound_oms_{std::move(outbound_oms)}, encoder_{std::move(encoder)},
      initiator_{std::move(initiator)}, measurement_clock_{std::move(measurement_clock)},
      order_ids_{std::move(order_ids)},
      trace_sink_{std::move(trace_provenance), policy_.capacities().submission_trace_capacity},
      diagnostics_{std::move(diagnostic_provenance),
                   policy_.capacities().submission_diagnostic_capacity} {}

// --------------------------------------------------------
// Destroy the last-declared private owner before the M3 components it may inspect unwind.
SubmissionCoordinator::~SubmissionCoordinator() = default;

// --------------------------------------------------------
// Expose only the token-gated admission interface while retaining ownership at this stable address.
PrivateAdmissionOwner* SubmissionCoordinator::private_admission_owner() noexcept {
  return private_order_reconciler_.get();
}

// --------------------------------------------------------
// Consume one sealed recovery bootstrap only after the pristine coordinator and every child
// allocation validate; success publishes its acknowledged identity stream before the child.
model::Result<void> SubmissionCoordinator::install_recovery_bound_private_order_reconciler(
    const configuration::StartupConfiguration& configuration, const M4Policy& policy,
    recovery::RecoveryBootstrap&& recovery_bootstrap) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject callback attachment, reinstallation, and every prior, active, faulted, evidenced, or
  // instrumented M3 state before inspecting authority or allocating the private owner.
  if (recovery_installation_closed_ || private_order_reconciler_ || submit_active_ ||
      attempts_consumed_ != 0U || reentry_traced_ || runtime_faulted_ ||
      terminal_error_.has_value() || active_trace_context_.has_value() ||
      reentry_probe_.has_value() || trace_append_fault_for_test_.has_value() ||
      outbound_oms_.order_count() != 0U || ledger_.held_reservation_count() != 0U ||
      encoder_.invocations_consumed() != 0U || initiator_.invocations_consumed() != 0U ||
      trace_sink_.record_count() != 0U || diagnostics_.diagnostic_count() != 0U ||
      diagnostics_.accepted_count() != 0U || diagnostics_.dropped_count() != 0U) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_order_reconciler.install_state"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Build one private transaction so every reported authority or allocation failure leaves both
  // coordinator and bootstrap identity streams untouched and reusable.
  auto prepared = PrivateOrderReconciler::prepare_recovery_bound_private_order_reconciler(
      *this, configuration, policy, recovery_bootstrap);
  if (!prepared) {
    return model::Result<void>::create_failure(std::move(prepared).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Interesting syntax: the fully allocated child is extracted from its successful Result before
  // bootstrap consumption; every operation after consumption is statically no-throw, so the
  // acknowledged provider replaces the unused construction stream before the child becomes
  // visible.
  using PreparedReconciler = PrivateOrderReconciler::PreparedRecoveryBoundPrivateOrderReconciler;
  using ConsumedAuthority = PrivateOrderReconciler::ConsumedRecoveryIdentityAuthority;
  static_assert(std::is_nothrow_move_constructible_v<PreparedReconciler>);
  static_assert(std::is_nothrow_move_constructible_v<ConsumedAuthority>);
  static_assert(noexcept(
      PrivateOrderReconciler::consume_recovery_identity_authority(std::move(recovery_bootstrap))));
  static_assert(std::is_nothrow_constructible_v<model::DeterministicOrderIdSource,
                                                model::DeterministicOrderIdProvider&&>);
  static_assert(std::is_nothrow_move_assignable_v<
                std::shared_ptr<recovery::detail::FakeJournalLeaseControl>>);
  static_assert(std::is_nothrow_move_assignable_v<model::DeterministicOrderIdSource>);
  static_assert(std::is_nothrow_move_assignable_v<std::unique_ptr<PrivateOrderReconciler>>);
  auto prepared_reconciler = std::move(prepared).value();
  auto identity_authority =
      PrivateOrderReconciler::consume_recovery_identity_authority(std::move(recovery_bootstrap));
  recovery_identity_lease_ = std::move(identity_authority.recovery_identity_lease);
  order_ids_ = model::DeterministicOrderIdSource{std::move(identity_authority.order_ids)};
  private_order_reconciler_ = std::move(prepared_reconciler.reconciler);
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Copy only inert request/count data into the private bounded probe before any outer attempt
// starts.
bool SubmissionCoordinator::arm_reentry_probe_for_test(OrderRequest request,
                                                       std::uint32_t requested_attempts) {
  if (requested_attempts == 0U || requested_attempts > ReentryProbe::maximum_attempts ||
      reentry_probe_ || submit_active_) {
    return false;
  }
  reentry_probe_.emplace(ReentryProbe{std::move(request), requested_attempts});
  return true;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Accept only one assigned fixed-data fault point before a submission or terminal latch exists.
bool SubmissionCoordinator::arm_trace_append_fault_for_test(
    TraceAppendFaultPointForTest point) noexcept {
  const auto assigned = point == TraceAppendFaultPointForTest::FirstReentryRejected ||
                        point == TraceAppendFaultPointForTest::RiskReservedBeforeOms ||
                        point == TraceAppendFaultPointForTest::WriteInitiatedAfterAcceptance ||
                        point == TraceAppendFaultPointForTest::SubmissionCompletedAfterInitiation;
  if (!assigned || trace_append_fault_for_test_ || submit_active_ || runtime_faulted_) {
    return false;
  }
  trace_append_fault_for_test_ = point;
  return true;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Arm the only rollback right immediately after atomic risk reservation succeeds.
SubmissionCoordinator::ReservationRollbackGuard::ReservationRollbackGuard(
    SubmissionCoordinator& coordinator, model::ReservationId reservation_id,
    CallbackBinding binding, model::OrderId order_id) noexcept
    : coordinator_{&coordinator}, reservation_id_{reservation_id}, binding_{binding},
      order_id_{std::move(order_id)} {}

// --------------------------------------------------------
// Contain any structurally missed post-reservation return as an internal fault and exact-once
// rollback; every ordinary and accepted path disarms before reaching this fallback.
SubmissionCoordinator::ReservationRollbackGuard::~ReservationRollbackGuard() noexcept {
  if (state_ != RollbackState::Armed) {
    return;
  }
  coordinator_->latch_runtime_fault(
      model::DomainError::create_at_field(model::DomainErrorCode::SubmissionEvidenceExhausted,
                                          "submission_coordinator.rollback_guard"));
  static_cast<void>(
      release_reservation(SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted));
}

// --------------------------------------------------------
// Consume the rollback right before calling the ledger so even a lower-layer invariant failure
// cannot cause a destructor or later branch to retry the release.
bool SubmissionCoordinator::ReservationRollbackGuard::release_reservation(
    SubmissionStage stage, SubmissionReason reason) noexcept {
  if (state_ != RollbackState::Armed) {
    coordinator_->latch_runtime_fault(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidRiskReservationState,
                                            "submission_coordinator.rollback_guard_reused"));
    return false;
  }
  state_ = RollbackState::FaultedConsumed;
  auto released = coordinator_->ledger_.release_reservation(reservation_id_);
  if (!released) {
    coordinator_->latch_runtime_fault(std::move(released).error());
    return false;
  }
  state_ = RollbackState::Released;

  runtime::SubmissionDiagnosticFields diagnostic;
  diagnostic.attempt_id = model::SubmissionAttemptId::from_value(reservation_id_.value()).value();
  diagnostic.owner_turn_ordinal = binding_.owner_turn_ordinal;
  diagnostic.callback_ordinal = binding_.callback_ordinal;
  diagnostic.order_id = order_id_;
  diagnostic.reservation_id = reservation_id_;
  diagnostic.stage = stage;
  diagnostic.reason = reason;
  auto appended = coordinator_->diagnostics_.append_diagnostic(
      runtime::SubmissionDiagnosticKind::ReservationReleased, std::move(diagnostic));
  if (!appended) {
    coordinator_->latch_runtime_fault(std::move(appended).error());
    return false;
  }
  return true;
}

// --------------------------------------------------------
// Consume the rollback right as conservative retained exposure only at the proven accepted
// boundary.
bool SubmissionCoordinator::ReservationRollbackGuard::retain_after_acceptance(
    const execution::FakeInitiationResult& initiation) noexcept {
  if (state_ != RollbackState::Armed || !initiation.is_accepted()) {
    coordinator_->latch_runtime_fault(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidRiskReservationState,
                                            "submission_coordinator.rollback_guard_retain"));
    return false;
  }
  state_ = RollbackState::Retained;
  return true;
}

// --------------------------------------------------------
// Preserve the first exact lower-layer fault so MarketRuntime can terminate with stable evidence.
void SubmissionCoordinator::latch_runtime_fault(model::DomainError error) noexcept {
  runtime_faulted_ = true;
  if (!terminal_error_) {
    terminal_error_.emplace(std::move(error));
  }
}

// --------------------------------------------------------
// Match the one armed semantic point to its exact canonical event and consume it before failure.
bool SubmissionCoordinator::consume_trace_append_fault_for_test(
    trace::SubmissionTraceEventKind kind, const trace::SubmissionTraceFields& fields) noexcept {
  if (!trace_append_fault_for_test_) {
    return false;
  }
  bool matches = false;
  switch (*trace_append_fault_for_test_) {
  case TraceAppendFaultPointForTest::FirstReentryRejected:
    matches = kind == trace::SubmissionTraceEventKind::ReentryRejected;
    break;
  case TraceAppendFaultPointForTest::RiskReservedBeforeOms:
    matches = kind == trace::SubmissionTraceEventKind::RiskReserved;
    break;
  case TraceAppendFaultPointForTest::WriteInitiatedAfterAcceptance:
    matches = kind == trace::SubmissionTraceEventKind::WriteInitiated;
    break;
  case TraceAppendFaultPointForTest::SubmissionCompletedAfterInitiation:
    matches = kind == trace::SubmissionTraceEventKind::SubmissionCompleted && fields.oms_state &&
              *fields.oms_state == oms::OutboundOrderState::WriteInitiated && fields.final_result &&
              fields.final_result->disposition == execution::SubmitDisposition::WriteInitiated;
    break;
  default:
    break;
  }
  if (matches) {
    trace_append_fault_for_test_.reset();
  }
  return matches;
}

// --------------------------------------------------------
// Append through the sole canonical authority and latch any impossible post-preflight failure.
bool SubmissionCoordinator::append_trace(trace::SubmissionTraceEventKind kind,
                                         const trace::SubmissionTraceFields& fields) noexcept {
  auto appended = consume_trace_append_fault_for_test(kind, fields)
                      ? model::Result<void>::create_failure(model::DomainError::create_at_field(
                            model::DomainErrorCode::SubmissionEvidenceExhausted,
                            "submission_trace.injected_append_failure"))
                      : trace_sink_.append_trace_record(kind, fields);
  if (appended) {
    return true;
  }
  latch_runtime_fault(std::move(appended).error());
  runtime::SubmissionDiagnosticFields diagnostic;
  diagnostic.attempt_id = fields.context.attempt_id;
  diagnostic.owner_turn_ordinal = fields.context.owner_turn_ordinal;
  diagnostic.callback_ordinal = fields.context.callback_ordinal;
  diagnostic.order_id = fields.order_id;
  diagnostic.reservation_id = fields.reservation_id;
  diagnostic.stage = SubmissionStage::Internal;
  diagnostic.reason = SubmissionReason::SubmissionRuntimeFaulted;
  static_cast<void>(diagnostics_.append_diagnostic(
      runtime::SubmissionDiagnosticKind::InternalInvariantFailure, std::move(diagnostic)));
  return false;
}

// --------------------------------------------------------
// Preserve noncanonical latency only for ordered available readings and diagnose absence exactly
// once after the caller has established owner, attempt, and evidence-preflight authority.
std::optional<std::uint64_t> SubmissionCoordinator::finish_measurement(
    std::optional<std::uint64_t> entry_started, std::optional<std::uint64_t> measurement_finished,
    const trace::SubmissionTraceFields& fields, const CallbackBinding& binding,
    SubmissionStage stage, SubmissionReason reason) noexcept {
  const auto duration = calculate_local_duration(entry_started, measurement_finished);
  if (duration) {
    return duration;
  }
  runtime::SubmissionDiagnosticFields diagnostic;
  diagnostic.attempt_id = fields.context.attempt_id;
  diagnostic.owner_turn_ordinal = binding.owner_turn_ordinal;
  diagnostic.callback_ordinal = binding.callback_ordinal;
  diagnostic.order_id = fields.order_id;
  diagnostic.reservation_id = fields.reservation_id;
  diagnostic.stage = stage;
  diagnostic.reason = reason;
  auto appended = diagnostics_.append_diagnostic(
      runtime::SubmissionDiagnosticKind::MeasurementUnavailable, std::move(diagnostic));
  if (!appended) {
    latch_runtime_fault(std::move(appended).error());
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Append the terminal canonical decision before capturing a rejection's end-to-end local duration.
SubmitResult SubmissionCoordinator::complete_rejection(
    trace::SubmissionTraceFields& fields, SubmissionStage stage, SubmissionReason reason,
    std::optional<model::OrderId> order_id, std::optional<RiskLimitEvidence> risk_evidence,
    const CallbackBinding& binding, std::optional<std::uint64_t> entry_started) noexcept {
  auto canonical = SubmitResult::create_locally_rejected_result(
      stage, reason, fields.context.attempt_id, order_id, risk_evidence);
  fields.final_result = trace::SubmissionFinalResult::from_submit_result(canonical);
  if (!append_trace(trace::SubmissionTraceEventKind::SubmissionCompleted, fields)) {
    const auto measurement_finished = take_measurement_nanosecond_reading();
    const auto duration =
        finish_measurement(entry_started, measurement_finished, fields, binding,
                           SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted);
    return SubmitResult::create_locally_rejected_result(
        SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted,
        fields.context.attempt_id, std::move(order_id), std::nullopt, duration);
  }
  const auto duration = finish_measurement(entry_started, take_measurement_nanosecond_reading(),
                                           fields, binding, stage, reason);
  return SubmitResult::create_locally_rejected_result(stage, reason, fields.context.attempt_id,
                                                      std::move(order_id), std::move(risk_evidence),
                                                      duration);
}

// --------------------------------------------------------
// Contain an impossible current-attempt fault without ever reclassifying possible acceptance as a
// definite local rejection or releasing its conservative exposure.
SubmitResult SubmissionCoordinator::fail_internal(
    trace::SubmissionTraceFields& fields, ReservationRollbackGuard* rollback,
    std::optional<model::OrderId> order_id, const CallbackBinding& binding,
    std::optional<std::uint64_t> entry_started, bool canonical_append_failed,
    const std::optional<std::uint64_t>* captured_measurement_finished) noexcept {
  const auto reservation_id = rollback == nullptr ? std::optional<model::ReservationId>{}
                                                  : std::optional{rollback->reservation_id()};
  if (!terminal_error_) {
    latch_runtime_fault(
        model::DomainError::create_at_field(model::DomainErrorCode::SubmissionEvidenceExhausted,
                                            "submission_coordinator.internal_invariant"));
  }
  runtime::SubmissionDiagnosticFields diagnostic;
  diagnostic.attempt_id = fields.context.attempt_id;
  diagnostic.owner_turn_ordinal = binding.owner_turn_ordinal;
  diagnostic.callback_ordinal = binding.callback_ordinal;
  diagnostic.order_id = order_id;
  diagnostic.reservation_id = reservation_id;
  diagnostic.stage = SubmissionStage::Internal;
  diagnostic.reason = SubmissionReason::SubmissionRuntimeFaulted;
  static_cast<void>(diagnostics_.append_diagnostic(
      runtime::SubmissionDiagnosticKind::InternalInvariantFailure, std::move(diagnostic)));

  // ++++++++++++++++++++++++++++++++++++++++
  // A Retained guard is the sole proof of accepted-slot uncertainty; never release its exposure or
  // reclassify the result as definite. A failed canonical append permits no invented later record.
  if (rollback != nullptr &&
      rollback->rollback_state() == ReservationRollbackGuard::RollbackState::Retained && order_id) {
    const auto* retained_order = outbound_oms_.find_order(*order_id);
    bool unknown_state_established = false;
    model::Result<void> marked = model::Result<void>::create_success();
    if (retained_order == nullptr) {
      marked = model::Result<void>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidOmsState,
                                              "submission_coordinator.accepted_order_missing"));
    } else if (retained_order->state() == oms::OutboundOrderState::PendingInitiation) {
      marked = outbound_oms_.mark_submission_unknown(*order_id);
      unknown_state_established = marked.has_value();
    } else if (retained_order->state() == oms::OutboundOrderState::WriteInitiated) {
      marked = outbound_oms_.mark_submission_unknown_after_internal_fault(*order_id);
      unknown_state_established = marked.has_value();
    } else if (retained_order->state() == oms::OutboundOrderState::SubmissionUnknown) {
      unknown_state_established = true;
    } else {
      marked = model::Result<void>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidOmsState, "submission_coordinator.accepted_order_state"));
    }
    if (!marked) {
      latch_runtime_fault(std::move(marked).error());
      canonical_append_failed = true;
    }

    if (unknown_state_established) {
      retained_order = outbound_oms_.find_order(*order_id);
      if (retained_order == nullptr ||
          retained_order->state() != oms::OutboundOrderState::SubmissionUnknown) {
        latch_runtime_fault(model::DomainError::create_at_field(
            model::DomainErrorCode::InvalidOmsState,
            "submission_coordinator.accepted_order_containment"));
        canonical_append_failed = true;
        unknown_state_established = false;
      }
    }

    if (unknown_state_established) {
      if (private_order_reconciler_) {
        private_order_reconciler_->record_submission_uncertainty(*retained_order);
      }
      fields.oms_state = oms::OutboundOrderState::SubmissionUnknown;
      fields.release_transition = trace::SubmissionReleaseTransition::Retained;
      fields.final_result.reset();
      if (fields.initiation && fields.initiation->accepted_write_ordinal) {
        fields.initiation->outcome = FakeInitiationOutcome::AcceptedThenOutcomeLost;
      }
      if (!canonical_append_failed) {
        const auto records = trace_sink_.records();
        if (!records.empty() && records.back().kind() == trace::SubmissionTraceEventKind::Encoded &&
            !append_trace(trace::SubmissionTraceEventKind::SubmissionUnknown, fields)) {
          canonical_append_failed = true;
        }
      }
    }

    auto unknown =
        SubmitResult::create_submission_unknown_result(fields.context.attempt_id, *order_id);
    if (!canonical_append_failed && unknown_state_established) {
      const auto records = trace_sink_.records();
      if (!records.empty() &&
          records.back().kind() == trace::SubmissionTraceEventKind::SubmissionUnknown) {
        fields.final_result = trace::SubmissionFinalResult::from_submit_result(unknown);
        if (!append_trace(trace::SubmissionTraceEventKind::SubmissionCompleted, fields)) {
          canonical_append_failed = true;
        }
      }
    }
    const auto measurement_finished = captured_measurement_finished == nullptr
                                          ? take_measurement_nanosecond_reading()
                                          : *captured_measurement_finished;
    const auto duration =
        finish_measurement(entry_started, measurement_finished, fields, binding,
                           SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted);
    return SubmitResult::create_submission_unknown_result(fields.context.attempt_id, *order_id,
                                                          duration);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Before acceptance, consume an Armed rollback exactly once. A successful ledger transition is
  // canonically observable even when its noncanonical diagnostic append subsequently faults.
  if (rollback != nullptr &&
      rollback->rollback_state() == ReservationRollbackGuard::RollbackState::Armed) {
    static_cast<void>(rollback->release_reservation(SubmissionStage::Internal,
                                                    SubmissionReason::SubmissionRuntimeFaulted));
  }
  if (rollback != nullptr &&
      rollback->rollback_state() == ReservationRollbackGuard::RollbackState::Released) {
    fields.release_transition = trace::SubmissionReleaseTransition::Released;
    if (!canonical_append_failed) {
      const auto records = trace_sink_.records();
      if ((records.empty() ||
           records.back().kind() != trace::SubmissionTraceEventKind::ReservationReleased) &&
          !append_trace(trace::SubmissionTraceEventKind::ReservationReleased, fields)) {
        canonical_append_failed = true;
      }
    }
  }
  if (canonical_append_failed) {
    const auto duration =
        finish_measurement(entry_started, take_measurement_nanosecond_reading(), fields, binding,
                           SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted);
    return SubmitResult::create_locally_rejected_result(
        SubmissionStage::Internal, SubmissionReason::SubmissionRuntimeFaulted,
        fields.context.attempt_id, std::move(order_id), std::nullopt, duration);
  }
  return complete_rejection(fields, SubmissionStage::Internal,
                            SubmissionReason::SubmissionRuntimeFaulted, std::move(order_id),
                            std::nullopt, binding, entry_started);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Execute one exact direct-path attempt with stable first-failure precedence and no asynchronous
// handoff; every fallible stage completes before the next stage mutates owner-local state.
SubmitResult
SubmissionCoordinator::submit_order(const CallbackBinding& binding, const OrderRequest& request,
                                    std::optional<std::uint64_t> entry_started) noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Later work fails without identity or evidence once an impossible owner-local invariant latched.
  if (runtime_faulted_) {
    return SubmitResult::create_locally_rejected_result(SubmissionStage::Internal,
                                                        SubmissionReason::SubmissionRuntimeFaulted);
  }
  if (binding.context == nullptr || binding.attribution == nullptr) {
    return SubmitResult::create_locally_rejected_result(SubmissionStage::Context,
                                                        SubmissionReason::ContextInactive);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Coalesce recursive entry into the outer attempt's reserved evidence slot without new identity.
  if (submit_active_) {
    const auto active_attempt =
        active_trace_context_ ? std::optional{active_trace_context_->attempt_id} : std::nullopt;
    if (active_trace_context_) {
      auto nested = submission_trace_fields_from_context(*active_trace_context_);
      nested.context.request = request;
      auto result = SubmitResult::create_locally_rejected_result(
          SubmissionStage::Context, SubmissionReason::SubmissionReentry,
          active_trace_context_->attempt_id);
      nested.final_result = trace::SubmissionFinalResult::from_submit_result(result);
      if (!reentry_traced_) {
        reentry_traced_ = append_trace(trace::SubmissionTraceEventKind::ReentryRejected, nested);
      }
      runtime::SubmissionDiagnosticFields diagnostic;
      diagnostic.attempt_id = active_trace_context_->attempt_id;
      diagnostic.owner_turn_ordinal = active_trace_context_->owner_turn_ordinal;
      diagnostic.callback_ordinal = active_trace_context_->callback_ordinal;
      diagnostic.stage = SubmissionStage::Context;
      diagnostic.reason = SubmissionReason::SubmissionReentry;
      static_cast<void>(diagnostics_.append_diagnostic(
          runtime::SubmissionDiagnosticKind::ReentryDetected, std::move(diagnostic)));
      const auto duration =
          finish_measurement(entry_started, take_measurement_nanosecond_reading(), nested, binding,
                             SubmissionStage::Context, SubmissionReason::SubmissionReentry);
      return SubmitResult::create_locally_rejected_result(
          SubmissionStage::Context, SubmissionReason::SubmissionReentry, active_attempt,
          std::nullopt, std::nullopt, duration);
    }
    return SubmitResult::create_locally_rejected_result(
        SubmissionStage::Context, SubmissionReason::SubmissionReentry, active_attempt);
  }
  SubmissionActiveGuard active_guard{submit_active_, active_trace_context_};
  reentry_traced_ = false;

  // ++++++++++++++++++++++++++++++++++++++++
  // Issue one bounded attempt before canonical evidence preflight; exhaustion touches no sink.
  if (attempts_consumed_ == policy_.capacities().maximum_submission_attempts) {
    return SubmitResult::create_locally_rejected_result(
        SubmissionStage::Evidence, SubmissionReason::SubmissionAttemptExhausted);
  }
  ++attempts_consumed_;
  auto attempt_id = model::SubmissionAttemptId::from_value(attempts_consumed_);
  if (!attempt_id) {
    latch_runtime_fault(std::move(attempt_id).error());
    return SubmitResult::create_locally_rejected_result(SubmissionStage::Internal,
                                                        SubmissionReason::SubmissionRuntimeFaulted);
  }
  active_trace_context_ = trace::SubmissionTraceContext{
      attempt_id.value(),
      binding.owner_turn_ordinal,
      binding.callback_ordinal,
      binding.processing_timestamp.nanoseconds(),
      trace::SubmissionTraceAttribution{binding.attribution->firm_id, binding.attribution->desk_id,
                                        binding.attribution->bot_id,
                                        binding.attribution->strategy_id},
      request,
  };
  auto preflight =
      trace_sink_.preflight_trace_append(trace::maximum_submission_trace_records_per_attempt);
  if (!preflight) {
    runtime::SubmissionDiagnosticFields diagnostic;
    diagnostic.attempt_id = attempt_id.value();
    diagnostic.owner_turn_ordinal = binding.owner_turn_ordinal;
    diagnostic.callback_ordinal = binding.callback_ordinal;
    diagnostic.stage = SubmissionStage::Evidence;
    diagnostic.reason = SubmissionReason::EvidenceCapacityExceeded;
    static_cast<void>(diagnostics_.append_diagnostic(
        runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded, std::move(diagnostic)));
    return SubmitResult::create_locally_rejected_result(
        SubmissionStage::Evidence, SubmissionReason::EvidenceCapacityExceeded, attempt_id.value());
  }

  auto fields = submission_trace_fields_from_context(*active_trace_context_);
  if (!append_trace(trace::SubmissionTraceEventKind::Attempt, fields)) {
    return fail_internal(fields, nullptr, std::nullopt, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the explicit configured route under context-derived attribution before economics.
  const auto authorization = routes_.evaluate_route_authorization(*binding.attribution, request);
  if (!authorization.is_authorized()) {
    return complete_rejection(fields, SubmissionStage::Route, authorization.reason, std::nullopt,
                              std::nullopt, binding, entry_started);
  }
  const auto& installed = *authorization.installed_route;
  fields.authorized_projection = authorized_submission_projection_from_installed_route(installed);
  if (!append_trace(trace::SubmissionTraceEventKind::RouteAuthorized, fields)) {
    return fail_internal(fields, nullptr, std::nullopt, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate the closed limit/GTC economics and preserve the exact canonical decimal values.
  const auto validation = validate_canonical_order(request, installed.metadata());
  if (!validation.is_accepted()) {
    return complete_rejection(fields, SubmissionStage::CanonicalValidation, validation.reason,
                              std::nullopt, std::nullopt, binding, entry_started);
  }
  const auto economics = *validation.economics;
  if (!append_trace(trace::SubmissionTraceEventKind::CanonicalValidated, fields)) {
    return fail_internal(fields, nullptr, std::nullopt, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A private fixed-data test probe can exercise genuine BotContext re-entry at this precise seam;
  // production composition has no authority that can arm it or install executable behavior.
  if (reentry_probe_ && reentry_probe_->armed) {
    auto& probe = *reentry_probe_;
    probe.armed = false;
    for (std::uint32_t attempt = 0U; attempt < probe.requested_attempts; ++attempt) {
      static_cast<void>(binding.context->submit_order(probe.request));
    }
  }
  if (runtime_faulted_) {
    return fail_internal(fields, nullptr, std::nullopt, binding, entry_started, !reentry_traced_);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Consume one collision-safe local identity only after all canonical economics are accepted.
  auto generated_order =
      std::visit([](auto& source) { return source.generate_next_order_id(); }, order_ids_);
  if (!generated_order) {
    return complete_rejection(fields, SubmissionStage::Identity,
                              SubmissionReason::OrderIdentityExhausted, std::nullopt, std::nullopt,
                              binding, entry_started);
  }
  fields.order_id = generated_order.value();
  if (!append_trace(trace::SubmissionTraceEventKind::IdentityGenerated, fields)) {
    return fail_internal(fields, nullptr, fields.order_id, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve route, canonical validation, and fresh-identity precedence while blocking every route
  // sharing an unsafe account before reservation arithmetic or any outbound side effect.
  if (private_order_reconciler_) {
    const auto globally_blocked =
        private_order_reconciler_->is_private_consumption_globally_blocked();
    const auto account_state =
        private_order_reconciler_->account_safety_state(installed.route().logical_account_id);
    if (globally_blocked || account_state != risk::AccountSafetyState::Synchronized) {
      const auto reason =
          !globally_blocked && account_state == risk::AccountSafetyState::ReconciliationRequired
              ? SubmissionReason::AccountReconciliationRequired
              : SubmissionReason::AccountQuarantined;
      if (!append_trace(trace::SubmissionTraceEventKind::RiskRejected, fields)) {
        return fail_internal(fields, nullptr, fields.order_id, binding, entry_started, true);
      }
      return complete_rejection(fields, SubmissionStage::Risk, reason, fields.order_id,
                                std::nullopt, binding, entry_started);
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Compute, check, and atomically reserve every fixed scope or record the first exact risk limit.
  const auto risk_decision = ledger_.check_and_reserve(attempt_id.value(), installed, economics);
  if (!risk_decision.is_reserved()) {
    fields.risk_rejection = risk_decision.risk_evidence();
    if (!append_trace(trace::SubmissionTraceEventKind::RiskRejected, fields)) {
      return fail_internal(fields, nullptr, fields.order_id, binding, entry_started, true);
    }
    return complete_rejection(fields, SubmissionStage::Risk, risk_decision.reason(),
                              fields.order_id, risk_decision.risk_evidence(), binding,
                              entry_started);
  }
  const auto reservation_id = *risk_decision.reservation_id();
  fields.reservation_id = reservation_id;
  fields.approved_exposure = *risk_decision.exposure();
  ReservationRollbackGuard rollback{*this, reservation_id, binding, *fields.order_id};
  if (!append_trace(trace::SubmissionTraceEventKind::RiskReserved, fields)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Admit the complete immutable projection; ordinary duplicate/capacity failure rolls risk back.
  auto admitted = outbound_oms_.admit_outbound_order(oms::OutboundOrderAdmission{
      attempt_id.value(),
      *fields.order_id,
      reservation_id,
      economics,
      *fields.approved_exposure,
      create_outbound_order_provenance(installed, ledger_.policy(), policy_,
                                       policy_.runtime_policy_fingerprint()),
  });
  if (!admitted) {
    latch_runtime_fault(std::move(admitted).error());
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
  }
  if (!admitted.value().is_admitted()) {
    const auto reason = *admitted.value().reason();
    if (!append_trace(trace::SubmissionTraceEventKind::OmsNonAdmission, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    if (!rollback.release_reservation(SubmissionStage::Oms, reason)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
    }
    fields.release_transition = trace::SubmissionReleaseTransition::Released;
    if (!append_trace(trace::SubmissionTraceEventKind::ReservationReleased, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    return complete_rejection(fields, SubmissionStage::Oms, reason, fields.order_id, std::nullopt,
                              binding, entry_started);
  }
  const auto* record = admitted.value().record();
  fields.oms_state = oms::OutboundOrderState::PendingEncoding;
  if (!append_trace(trace::SubmissionTraceEventKind::OmsAdmitted, fields)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode only the admitted OMS record; a scripted failure terminalizes OMS before exact release.
  auto encoded = encoder_.encode_order(*record);
  if (!encoded) {
    latch_runtime_fault(std::move(encoded).error());
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
  }
  if (!encoded.value().is_encoded()) {
    auto marked = outbound_oms_.mark_encoding_failed(*fields.order_id);
    if (!marked) {
      latch_runtime_fault(std::move(marked).error());
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
    }
    fields.oms_state = oms::OutboundOrderState::LocallyFailed;
    if (!append_trace(trace::SubmissionTraceEventKind::EncodingFailed, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    if (!rollback.release_reservation(SubmissionStage::Encoding,
                                      SubmissionReason::EncodingFailed)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
    }
    fields.release_transition = trace::SubmissionReleaseTransition::Released;
    if (!append_trace(trace::SubmissionTraceEventKind::ReservationReleased, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    return complete_rejection(fields, SubmissionStage::Encoding, SubmissionReason::EncodingFailed,
                              fields.order_id, std::nullopt, binding, entry_started);
  }
  const auto& encoded_order = *encoded.value().encoded_order();
  auto encoding_marked = outbound_oms_.mark_encoding_succeeded(*fields.order_id);
  if (!encoding_marked) {
    latch_runtime_fault(std::move(encoding_marked).error());
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
  }
  fields.oms_state = oms::OutboundOrderState::PendingInitiation;
  fields.encoding = trace::SubmissionEncodingEvidence{
      encoded.value().invocation_ordinal(), encoded_order.byte_length(),
      model::calculate_sha256_digest(encoded_order.bytes())};
  if (!append_trace(trace::SubmissionTraceEventKind::Encoded, fields)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The fake slot copy is the only acceptance boundary; capture success/unknown latency exactly at
  // the returned outcome before later OMS and evidence finalization.
  auto initiated = initiator_.initiate(encoded_order, *measurement_clock_);
  if (!initiated) {
    latch_runtime_fault(std::move(initiated).error());
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
  }
  const auto initiation = initiated.value();
  const auto initiation_finished = initiation.accepted_slot_endpoint_nanoseconds();
  if (initiation.is_accepted() && !rollback.retain_after_acceptance(initiation)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, false,
                         &initiation_finished);
  }
  const auto outcome_requires_acceptance =
      initiation.outcome() != FakeInitiationOutcome::DefiniteFailureBeforeAcceptance;
  if (initiation.is_accepted() != outcome_requires_acceptance) {
    latch_runtime_fault(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidFakeState, "submission_coordinator.initiation_evidence"));
    return initiation.is_accepted()
               ? fail_internal(fields, &rollback, fields.order_id, binding, entry_started, false,
                               &initiation_finished)
               : fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
  }

  if (initiation.outcome() == FakeInitiationOutcome::DefiniteFailureBeforeAcceptance) {
    fields.initiation = trace::SubmissionInitiationEvidence{
        initiation.invocation_ordinal(), initiation.outcome(), initiation.write_ordinal()};
    auto marked = outbound_oms_.mark_initiation_definitely_failed(*fields.order_id);
    if (!marked) {
      latch_runtime_fault(std::move(marked).error());
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
    }
    fields.oms_state = oms::OutboundOrderState::LocallyFailed;
    if (!append_trace(trace::SubmissionTraceEventKind::InitiationDefinitelyFailed, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    if (!rollback.release_reservation(SubmissionStage::Initiation,
                                      SubmissionReason::InitiationDefinitelyFailed)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started);
    }
    fields.release_transition = trace::SubmissionReleaseTransition::Released;
    if (!append_trace(trace::SubmissionTraceEventKind::ReservationReleased, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true);
    }
    return complete_rejection(fields, SubmissionStage::Initiation,
                              SubmissionReason::InitiationDefinitelyFailed, fields.order_id,
                              std::nullopt, binding, entry_started);
  }

  fields.initiation = trace::SubmissionInitiationEvidence{
      initiation.invocation_ordinal(), initiation.outcome(), initiation.write_ordinal()};
  if (initiation.outcome() == FakeInitiationOutcome::AcceptedThenOutcomeLost) {
    auto marked = outbound_oms_.mark_submission_unknown(*fields.order_id);
    if (!marked) {
      latch_runtime_fault(std::move(marked).error());
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, false,
                           &initiation_finished);
    }
    if (private_order_reconciler_) {
      private_order_reconciler_->record_submission_uncertainty(*record);
    }
    fields.oms_state = oms::OutboundOrderState::SubmissionUnknown;
    fields.release_transition = trace::SubmissionReleaseTransition::Retained;
    if (!append_trace(trace::SubmissionTraceEventKind::SubmissionUnknown, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true,
                           &initiation_finished);
    }
    runtime::SubmissionDiagnosticFields diagnostic;
    diagnostic.attempt_id = attempt_id.value();
    diagnostic.owner_turn_ordinal = binding.owner_turn_ordinal;
    diagnostic.callback_ordinal = binding.callback_ordinal;
    diagnostic.order_id = fields.order_id;
    diagnostic.reservation_id = reservation_id;
    diagnostic.stage = SubmissionStage::Initiation;
    diagnostic.reason = SubmissionReason::InitiationOutcomeUnknown;
    static_cast<void>(diagnostics_.append_diagnostic(
        runtime::SubmissionDiagnosticKind::UnknownExposureRetained, std::move(diagnostic)));
    auto canonical =
        SubmitResult::create_submission_unknown_result(attempt_id.value(), *fields.order_id);
    fields.final_result = trace::SubmissionFinalResult::from_submit_result(canonical);
    if (!append_trace(trace::SubmissionTraceEventKind::SubmissionCompleted, fields)) {
      return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true,
                           &initiation_finished);
    }
    const auto duration =
        finish_measurement(entry_started, initiation_finished, fields, binding,
                           SubmissionStage::Initiation, SubmissionReason::InitiationOutcomeUnknown);
    return SubmitResult::create_submission_unknown_result(attempt_id.value(), *fields.order_id,
                                                          duration);
  }

  auto marked = outbound_oms_.mark_write_initiated(*fields.order_id);
  if (!marked) {
    latch_runtime_fault(std::move(marked).error());
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, false,
                         &initiation_finished);
  }
  fields.oms_state = oms::OutboundOrderState::WriteInitiated;
  fields.release_transition = trace::SubmissionReleaseTransition::Retained;
  if (!append_trace(trace::SubmissionTraceEventKind::WriteInitiated, fields)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true,
                         &initiation_finished);
  }
  auto canonical =
      SubmitResult::create_write_initiated_result(attempt_id.value(), *fields.order_id);
  fields.final_result = trace::SubmissionFinalResult::from_submit_result(canonical);
  if (!append_trace(trace::SubmissionTraceEventKind::SubmissionCompleted, fields)) {
    return fail_internal(fields, &rollback, fields.order_id, binding, entry_started, true,
                         &initiation_finished);
  }
  const auto duration = finish_measurement(entry_started, initiation_finished, fields, binding,
                                           SubmissionStage::Initiation, SubmissionReason::None);
  return SubmitResult::create_write_initiated_result(attempt_id.value(), *fields.order_id,
                                                     duration);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
