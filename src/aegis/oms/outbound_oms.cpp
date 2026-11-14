// Purpose: retain collision-safe outbound admissions and apply validated private execution gaps
// to bounded OMS storage without changing risk or performing external work.

#include "aegis/oms/outbound_oms.hpp"

#include "private_oms_transition.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aegis::oms {
namespace {

// --------------------------------------------------------
// Construct the representation-independent zero used before any execution has been applied.
[[nodiscard]] model::Quantity create_zero_quantity() {
  return model::Quantity::from_scaled(0, 0).value();
}

// --------------------------------------------------------
// Report every impossible admission or transition through the one stable OMS invariant code.
[[nodiscard]] model::DomainError create_invalid_oms_state_error(std::string field) {
  return model::DomainError::create_at_field(model::DomainErrorCode::InvalidOmsState,
                                             std::move(field));
}

// --------------------------------------------------------
// Risk creates a reservation from the same attempt and approves the unchanged order quantity;
// rejecting either mismatch prevents the OMS from retaining internally contradictory evidence.
[[nodiscard]] bool is_consistent(const OutboundOrderAdmission& admission) noexcept {
  return admission.reservation_id.value() == admission.attempt_id.value() &&
         admission.exposure.order_quantity() == admission.economics.quantity;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// New rows retain the admitted economics and start with the exact pristine pre-execution M4
// projection; the known zero construction cannot fail for its fixed literal.
OutboundOrderRecord::OutboundOrderRecord(OutboundOrderAdmission admission)
    : admission_{std::move(admission)}, cumulative_filled_quantity_{create_zero_quantity()} {}

// --------------------------------------------------------

// --------------------------------------------------------
// Accepted admission points at the permanent table row and carries no rejection reason.
OmsAdmissionResult::OmsAdmissionResult(const OutboundOrderRecord& record) noexcept
    : record_{&record} {}

// --------------------------------------------------------
// Ordinary non-admission retains its stable reason without manufacturing a row.
OmsAdmissionResult::OmsAdmissionResult(execution::SubmissionReason reason) noexcept
    : reason_{reason} {}

// --------------------------------------------------------

// --------------------------------------------------------
// Reject an unusable zero-capacity table at the startup-policy boundary.
model::Result<OutboundOms> OutboundOms::create_outbound_oms(std::uint32_t capacity) {

  // ++++++++++++++++++++++++++++++++++++++++
  // A submission-capable policy must preallocate at least one retained OMS row.
  if (capacity == 0U) {
    return model::Result<OutboundOms>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate every optional slot now so owner-local admission never grows the table; an unavailable
  // or unrepresentable allocation fails the submission-policy construction boundary closed.
  try {
    return model::Result<OutboundOms>::create_success(OutboundOms{capacity});
  } catch (const std::bad_alloc&) {
    return model::Result<OutboundOms>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  } catch (const std::length_error&) {
    return model::Result<OutboundOms>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Materialize the complete fixed slot array and admission index before any row can be published.
OutboundOms::OutboundOms(std::uint32_t capacity)
    : capacity_{capacity}, slots_(capacity),
      storage_incarnation_{std::make_shared<StorageIncarnation>()} {
  admission_order_.reserve(capacity);
}

// --------------------------------------------------------
// FNV-1a chooses a deterministic probe start, while later complete OrderId equality remains the
// only identity decision and therefore resolves every hash collision safely.
std::size_t
OutboundOms::calculate_probe_start_index(const model::OrderId& order_id) const noexcept {
  constexpr std::uint64_t offset_basis = 14'695'981'039'346'656'037ULL;
  constexpr std::uint64_t prime = 1'099'511'628'211ULL;

  std::uint64_t hash = offset_basis;
  for (const auto byte : order_id.bytes()) {
    hash ^= static_cast<std::uint64_t>(byte);
    hash *= prime;
  }
  return static_cast<std::size_t>(hash % static_cast<std::uint64_t>(capacity_));
}

// --------------------------------------------------------
// Probe until exact identity or the first never-used slot; rows are never erased, so an empty slot
// proves the identity cannot occur later in the probe sequence.
std::optional<std::size_t>
OutboundOms::find_retained_slot_index(const model::OrderId& order_id) const noexcept {
  if (!storage_incarnation_) {
    return std::nullopt;
  }
  std::size_t slot_index = calculate_probe_start_index(order_id);
  for (std::uint32_t probes = 0U; probes < capacity_; ++probes) {
    const auto& slot = slots_[slot_index];
    if (!slot) {
      return std::nullopt;
    }
    if (slot->order_id() == order_id) {
      return slot_index;
    }
    slot_index = (slot_index + 1U == slots_.size()) ? 0U : slot_index + 1U;
  }
  return std::nullopt;
}

// --------------------------------------------------------
// Probe until exact identity or the first never-used slot; rows are never erased, so an empty slot
// proves the identity cannot occur later in the probe sequence.
const OutboundOrderRecord* OutboundOms::find_order(const model::OrderId& order_id) const noexcept {
  std::size_t slot_index = calculate_probe_start_index(order_id);
  for (std::uint32_t probes = 0U; probes < capacity_; ++probes) {
    const auto& slot = slots_[slot_index];
    if (!slot) {
      return nullptr;
    }
    if (slot->order_id() == order_id) {
      return &*slot;
    }
    slot_index = (slot_index + 1U == slots_.size()) ? 0U : slot_index + 1U;
  }
  return nullptr;
}

// --------------------------------------------------------
// Resolve one immutable retained row through its stable canonical admission position.
const OutboundOrderRecord* OutboundOms::record_at(std::size_t admission_index) const noexcept {
  if (admission_index >= admission_order_.size()) {
    return nullptr;
  }
  const auto slot_index = static_cast<std::size_t>(admission_order_[admission_index]);
  return slot_index < slots_.size() && slots_[slot_index] ? &*slots_[slot_index] : nullptr;
}

// --------------------------------------------------------
// Mutable resolution uses the identical collision-safe probe contract as public inspection.
OutboundOrderRecord* OutboundOms::find_mutable_order(const model::OrderId& order_id) noexcept {
  std::size_t slot_index = calculate_probe_start_index(order_id);
  for (std::uint32_t probes = 0U; probes < capacity_; ++probes) {
    auto& slot = slots_[slot_index];
    if (!slot) {
      return nullptr;
    }
    if (slot->order_id() == order_id) {
      return &*slot;
    }
    slot_index = (slot_index + 1U == slots_.size()) ? 0U : slot_index + 1U;
  }
  return nullptr;
}

// --------------------------------------------------------
// Duplicate identity wins even when every slot is occupied; only a distinct identity may observe
// capacity non-admission, and neither ordinary outcome mutates the table.
model::Result<OmsAdmissionResult>
OutboundOms::admit_outbound_order(OutboundOrderAdmission admission) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Scan the full probe cluster for exact identity before consulting remaining capacity.
  std::size_t slot_index = calculate_probe_start_index(admission.order_id);
  std::optional<std::size_t> available_slot;
  for (std::uint32_t probes = 0U; probes < capacity_; ++probes) {
    const auto& slot = slots_[slot_index];
    if (!slot) {
      available_slot = slot_index;
      break;
    }
    if (slot->order_id() == admission.order_id) {
      return model::Result<OmsAdmissionResult>::create_success(
          OmsAdmissionResult{execution::SubmissionReason::DuplicateOrderIdentity});
    }
    slot_index = (slot_index + 1U == slots_.size()) ? 0U : slot_index + 1U;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A distinct order cannot create a row when the fixed table has no unused slot.
  if (!available_slot) {
    return model::Result<OmsAdmissionResult>::create_success(
        OmsAdmissionResult{execution::SubmissionReason::OmsCapacityExceeded});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Treat contradictory risk/identity evidence as an invariant failure, not ordinary non-admission.
  if (!is_consistent(admission)) {
    return model::Result<OmsAdmissionResult>::create_failure(
        create_invalid_oms_state_error("outbound_oms.admission"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Commit the complete row only after every fallible decision; optional storage was preallocated.
  auto& slot = slots_[*available_slot];
  auto record = OutboundOrderRecord{std::move(admission)};
  slot.emplace(std::move(record));
  admission_order_.push_back(static_cast<std::uint32_t>(*available_slot));
  ++size_;
  return model::Result<OmsAdmissionResult>::create_success(OmsAdmissionResult{*slot});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// One shared mutation gate makes every repeated, missing, terminal, or wrong-source operation fail
// before changing the retained record.
model::Result<void> OutboundOms::transition_order_state(const model::OrderId& order_id,
                                                        OutboundOrderState expected,
                                                        OutboundOrderState target) {
  auto* record = find_mutable_order(order_id);
  if (record == nullptr || record->state_ != expected) {
    return model::Result<void>::create_failure(
        create_invalid_oms_state_error("outbound_oms.state"));
  }
  record->state_ = target;
  return model::Result<void>::create_success();
}

// --------------------------------------------------------
// Successful exact encoding is the only transition into PendingInitiation.
model::Result<void> OutboundOms::mark_encoding_succeeded(const model::OrderId& order_id) {
  return transition_order_state(order_id, OutboundOrderState::PendingEncoding,
                                OutboundOrderState::PendingInitiation);
}

// --------------------------------------------------------
// Scripted encoding failure retains the row permanently in LocallyFailed.
model::Result<void> OutboundOms::mark_encoding_failed(const model::OrderId& order_id) {
  return transition_order_state(order_id, OutboundOrderState::PendingEncoding,
                                OutboundOrderState::LocallyFailed);
}

// --------------------------------------------------------
// A definite pre-copy initiation failure is terminal and remains locally known.
model::Result<void> OutboundOms::mark_initiation_definitely_failed(const model::OrderId& order_id) {
  return transition_order_state(order_id, OutboundOrderState::PendingInitiation,
                                OutboundOrderState::LocallyFailed);
}

// --------------------------------------------------------
// Accepted fake initiation retains the reservation-owning row as WriteInitiated.
model::Result<void> OutboundOms::mark_write_initiated(const model::OrderId& order_id) {
  return transition_order_state(order_id, OutboundOrderState::PendingInitiation,
                                OutboundOrderState::WriteInitiated);
}

// --------------------------------------------------------
// Lost post-copy certainty retains the reservation-owning row as SubmissionUnknown.
model::Result<void> OutboundOms::mark_submission_unknown(const model::OrderId& order_id) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the M3 state transition's exact failure behavior before adding its M4 uncertainty.
  auto result = transition_order_state(order_id, OutboundOrderState::PendingInitiation,
                                       OutboundOrderState::SubmissionUnknown);
  if (!result) {
    return result;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The successful transition guarantees the retained row still exists at its stable address.
  find_mutable_order(order_id)->reconciliation_required_ = true;
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A post-acceptance internal fault can invalidate completed local evidence without undoing the
// accepted copy; conservatively require reconciliation while retaining the same order and risk.
model::Result<void>
OutboundOms::mark_submission_unknown_after_internal_fault(const model::OrderId& order_id) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Apply only the already-accepted conservative M3 downgrade before changing its M4 projection.
  auto result = transition_order_state(order_id, OutboundOrderState::WriteInitiated,
                                       OutboundOrderState::SubmissionUnknown);
  if (!result) {
    return result;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A post-acceptance internal fault also creates an explicit later reconciliation obligation.
  find_mutable_order(order_id)->reconciliation_required_ = true;
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Compare addresses only after proving this facade still owns its original live storage.
bool OutboundOms::has_retained_order_record(const OutboundOrderRecord& record) const noexcept {
  if (!storage_incarnation_) {
    return false;
  }
  for (const auto slot_index : admission_order_) {
    if (&slots_[slot_index].value() == &record) {
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------
// Reserve the exact per-order bound at cold install; a failed allocation cannot alter the OMS.
model::Result<OutboundOms::PendingPrivateExecutionStorage>
OutboundOms::prepare_pending_private_execution_storage(std::uint64_t capacity_per_order) const {
  using StorageResult = model::Result<PendingPrivateExecutionStorage>;
  if (capacity_per_order == 0U || !storage_incarnation_ || size_ != 0U ||
      !pending_private_executions_by_slot_.empty() ||
      capacity_per_order > std::numeric_limits<std::uint32_t>::max() / capacity_ ||
      capacity_per_order >
          std::numeric_limits<std::size_t>::max() / sizeof(PendingPrivateExecution) / capacity_) {
    return StorageResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_oms.pending_storage_capacity"));
  }
  try {
    PendingPrivateExecutionStorage storage(slots_.size());
    for (auto& pending : storage) {
      pending.reserve(static_cast<std::size_t>(capacity_per_order));
    }
    return StorageResult::create_success(std::move(storage));
  } catch (const std::bad_alloc&) {
    return StorageResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_oms.pending_storage_allocation"));
  } catch (const std::length_error&) {
    return StorageResult::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidM4Policy, "private_oms.pending_storage_allocation"));
  }
}

// --------------------------------------------------------
// Publish the complete preallocation while the coordinator is still pristine.
void OutboundOms::install_prepared_pending_private_execution_storage(
    PendingPrivateExecutionStorage storage, std::uint32_t capacity_per_order) noexcept {
  static_assert(std::is_nothrow_move_assignable_v<PendingPrivateExecutionStorage>);
  pending_private_executions_by_slot_ = std::move(storage);
  pending_private_execution_capacity_ = capacity_per_order;
}

// --------------------------------------------------------
// Borrow the canonical ordered prefix, or an empty sequence when the row or M4 storage is absent.
std::span<const PendingPrivateExecution>
OutboundOms::pending_private_executions(const model::OrderId& order_id) const noexcept {
  const auto index = find_retained_slot_index(order_id);
  return index && *index < pending_private_executions_by_slot_.size()
             ? std::span<const PendingPrivateExecution>{pending_private_executions_by_slot_[*index]}
             : std::span<const PendingPrivateExecution>{};
}

// --------------------------------------------------------
// Confirm exact OMS baseline, closed effect shape, and both logical and physical insertion room.
model::Result<void>
OutboundOms::validate_buffered_private_execution(const model::OrderId& order_id,
                                                 const PrivateOmsTransitionPlan& transition) const {
  const auto index = find_retained_slot_index(order_id);
  if (!index || *index >= pending_private_executions_by_slot_.size() ||
      slots_[*index]->private_projection() != transition.before ||
      transition.classification != ProposedPrivateOmsClassification::BufferedGap ||
      transition.economics_action != ProposedPrivateEconomicsAction::None ||
      transition.reservation_closure_cause || transition.safety_reason ||
      transition.rejection_code || !transition.gap_insertion || transition.cancel_history_change ||
      transition.drained_pending_prefix_count != 0U || transition.transition_effect_count != 1U ||
      transition.order_callback_count != 0U) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateOmsState, "private_oms.buffered_execution_plan"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Bounded logical capacity still wins if the allocator provided excess reserved storage.
  const auto& pending = pending_private_executions_by_slot_[*index];
  if (pending.size() >= pending_private_execution_capacity_ ||
      pending.size() >= pending.capacity()) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::PrivateEventCapacityExceeded, "private_oms.pending_fill_capacity"));
  }
  const auto& input = transition.gap_insertion->execution;
  const auto* execution = std::get_if<ExecutionPayload>(&input.payload());
  if (execution == nullptr || pending.size() != transition.before.pending_fill_count) {
    return model::Result<void>::create_failure(model::DomainError::create_at_field(
        model::DomainErrorCode::InvalidPrivateOmsState, "private_oms.buffered_execution_fact"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject every projection field outside the closed gap shape, even for an otherwise valid row.
  auto expected = transition.before;
  expected.execution_evidence_observed = true;
  ++expected.pending_fill_count;
  if (!expected.exchange_order_id && execution->locator.exchange_order_id()) {
    expected.exchange_order_id = execution->locator.exchange_order_id();
    expected.exchange_mapping_established_by_execution = true;
  }
  if (transition.after != expected) {
    return model::Result<void>::create_failure(
        model::DomainError::create_at_field(model::DomainErrorCode::InvalidPrivateOmsState,
                                            "private_oms.buffered_execution_projection"));
  }
  return model::Result<void>::create_success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain one complete validated interval in ascending endpoint order, then publish its OMS cache.
void OutboundOms::apply_validated_buffered_private_execution(
    const model::OrderId& order_id, const PrivateOmsTransitionPlan& transition) noexcept {
  static_assert(std::is_nothrow_copy_constructible_v<PendingPrivateExecution>);
  static_assert(std::is_nothrow_copy_assignable_v<PendingPrivateExecution>);
  static_assert(std::is_nothrow_move_constructible_v<PendingPrivateExecution>);
  static_assert(std::is_nothrow_move_assignable_v<PendingPrivateExecution>);
  const auto index = *find_retained_slot_index(order_id);
  auto& pending = pending_private_executions_by_slot_[index];
  const auto& incoming = *transition.gap_insertion;
  const auto cumulative_end =
      std::get_if<ExecutionPayload>(&incoming.execution.payload())->cumulative_quantity;
  const auto insertion = std::lower_bound(
      pending.begin(), pending.end(), cumulative_end,
      [](const PendingPrivateExecution& retained, model::Quantity value) noexcept {
        return std::get_if<ExecutionPayload>(&retained.execution.payload())->cumulative_quantity <
               value;
      });
  pending.insert(insertion, incoming);
  auto& row = *slots_[index];
  row.exchange_order_id_ = transition.after.exchange_order_id;
  row.execution_evidence_observed_ = transition.after.execution_evidence_observed;
  row.exchange_mapping_established_by_execution_ =
      transition.after.exchange_mapping_established_by_execution;
  row.pending_fill_count_ = transition.after.pending_fill_count;
}

// --------------------------------------------------------

} // namespace aegis::oms
