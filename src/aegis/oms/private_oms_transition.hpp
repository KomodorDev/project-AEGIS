// Purpose: derive detached known-order M4 lifecycle proposals from validated snapshots without
// publishing private consumption, changing OMS or economics, or granting later mutation authority.

#pragma once

#include "aegis/execution/submission_route.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/risk/account_safety.hpp"
#include "aegis/risk/reservation_ledger.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace aegis::oms {

// ########################################################################
// A retained gap keeps its complete immutable normalized source fact; the planner validates the
// borrowed sequence as nonoverlapping intervals ordered by cumulative endpoint.
struct PendingPrivateExecution {
  NormalizedPrivateOrderInput execution;
};

// ########################################################################
// One retained cancel request keeps its first write fact and first causal rejection independently
// from its final status. Later distinct rejections belong to the future complete event registry and
// never overwrite this first evidence. Confirmed records a noncausal terminal close.
struct RetainedCancelAttempt {
  CancelAttemptId id;
  NormalizedPrivateOrderInput request;
  CancellationState state;
  std::optional<NormalizedPrivateOrderInput> write_outcome;
  std::optional<NormalizedPrivateOrderInput> causal_rejection;
};

// ########################################################################
// These are proposed lifecycle classifications, never canonical private-consumption outcomes.
enum class ProposedPrivateOmsClassification : std::uint8_t {
  Applied = 1,
  ProjectionOnly = 2,
  BufferedGap = 3,
  SafetyContained = 4,
  ForbiddenRejected = 5,
};

// ########################################################################
// Economics describe work a future joint reducer must preflight before any publication. Execution
// order is the incoming fact followed by the indicated contiguous prefix of retained gaps.
enum class ProposedPrivateEconomicsAction : std::uint8_t {
  None = 1,
  ApplyExecutions = 2,
  ReleaseResidual = 3,
  ApplyExecutionsThenReleaseResidual = 4,
};

// ########################################################################
// A cancel change is either one append at history.size() or one replacement at an existing index.
struct ProposedCancelHistoryChange {
  std::size_t index;
  RetainedCancelAttempt record;
};

// ########################################################################
// The synchronous view borrows a complete admission, detached projection, sealed route and policy,
// normalized first-seen known-order fact, and immutable side tables. Callers serialize their owner
// for the call and establish identity deduplication/correlation first. Authoring this view grants
// no authority over any retained row; global cancel count includes this order's history.
struct PrivateOmsTransitionInputs {
  const OutboundOrderAdmission& admission;
  const PrivateOrderProjection& before;
  const execution::InstalledSubmissionRoute& route;
  const runtime::M4Policy& policy;
  const recovery::RuntimeEpochId& runtime_epoch;
  const NormalizedPrivateOrderInput& input;
  std::span<const PendingPrivateExecution> pending;
  std::span<const RetainedCancelAttempt> cancels;
  std::uint32_t global_cancel_attempt_count;
};

// ########################################################################
// The complete value-only proposal contains no live references, scratch aliases, owner token, or
// commit operation. Counts state exact proposed per-input effects and callbacks; an execution
// drain contributes one effect/callback for the input and each drained gap. Forbidden local facts
// still need one audit effect but no callback. Capacity failure returns no partial proposal.
// Safety reasons include an open-order timeout as well as authoritative contradiction; absence
// never clears existing account safety. Rejection code is InvalidPrivateOmsState exactly for a
// forbidden local proposal. Closure cause appears only when the proposed economics close a hold.
struct PrivateOmsTransitionPlan {
  PrivateOrderProjection before;
  PrivateOrderProjection after;
  ProposedPrivateOmsClassification classification;
  ProposedPrivateEconomicsAction economics_action;
  std::optional<risk::ReservationClosureCause> reservation_closure_cause;
  std::optional<risk::AccountSafetyReason> safety_reason;
  std::optional<model::DomainErrorCode> rejection_code;
  std::optional<PendingPrivateExecution> gap_insertion;
  std::uint32_t drained_pending_prefix_count;
  std::optional<ProposedCancelHistoryChange> cancel_history_change;
  std::uint32_t transition_effect_count;
  std::uint32_t order_callback_count;
};

// ########################################################################

// --------------------------------------------------------
// Validate the complete detached snapshot and derive the ADR-0010 ordinary known-order partition
// without allocation on successful planning or any stored-state mutation. Invalid snapshots return
// InvalidPrivateOmsState; exhausted bounded side tables return PrivateEventCapacityExceeded.
// Authoritative contradictions propose containment; forbidden local transitions preserve the
// complete projection. Account/source fanout and complete-negative recovery authority are excluded.
[[nodiscard]] model::Result<PrivateOmsTransitionPlan>
derive_private_oms_transition(const PrivateOmsTransitionInputs& inputs);

// --------------------------------------------------------

} // namespace aegis::oms
