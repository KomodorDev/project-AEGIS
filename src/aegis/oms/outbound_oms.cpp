// Purpose: implement collision-safe fixed-capacity outbound admission and initialize the complete
// M3-to-M4 OMS projection without releasing risk state or performing external work.

#include "aegis/oms/outbound_oms.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
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
  return model::DomainError::at_field(model::DomainErrorCode::InvalidOmsState, std::move(field));
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
model::Result<OutboundOms> OutboundOms::create(std::uint32_t capacity) {

  // ++++++++++++++++++++++++++++++++++++++++
  // A submission-capable policy must preallocate at least one retained OMS row.
  if (capacity == 0U) {
    return model::Result<OutboundOms>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Allocate every optional slot now so owner-local admission never grows the table; an unavailable
  // or unrepresentable allocation fails the submission-policy construction boundary closed.
  try {
    return model::Result<OutboundOms>::success(OutboundOms{capacity});
  } catch (const std::bad_alloc&) {
    return model::Result<OutboundOms>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  } catch (const std::length_error&) {
    return model::Result<OutboundOms>::failure(model::DomainError::at_field(
        model::DomainErrorCode::InvalidSubmissionPolicy, "submission_policy.oms_capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Materialize the complete fixed slot array and admission index before any row can be published.
OutboundOms::OutboundOms(std::uint32_t capacity) : capacity_{capacity}, slots_(capacity) {
  admission_order_.reserve(capacity);
}

// --------------------------------------------------------
// FNV-1a chooses a deterministic probe start, while later complete OrderId equality remains the
// only identity decision and therefore resolves every hash collision safely.
std::size_t OutboundOms::probe_start(const model::OrderId& order_id) const noexcept {
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
const OutboundOrderRecord* OutboundOms::find(const model::OrderId& order_id) const noexcept {
  std::size_t slot_index = probe_start(order_id);
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
OutboundOrderRecord* OutboundOms::find_mutable(const model::OrderId& order_id) noexcept {
  std::size_t slot_index = probe_start(order_id);
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
model::Result<OmsAdmissionResult> OutboundOms::admit(OutboundOrderAdmission admission) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Scan the full probe cluster for exact identity before consulting remaining capacity.
  std::size_t slot_index = probe_start(admission.order_id);
  std::optional<std::size_t> available_slot;
  for (std::uint32_t probes = 0U; probes < capacity_; ++probes) {
    const auto& slot = slots_[slot_index];
    if (!slot) {
      available_slot = slot_index;
      break;
    }
    if (slot->order_id() == admission.order_id) {
      return model::Result<OmsAdmissionResult>::success(
          OmsAdmissionResult{execution::SubmissionReason::DuplicateOrderIdentity});
    }
    slot_index = (slot_index + 1U == slots_.size()) ? 0U : slot_index + 1U;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A distinct order cannot create a row when the fixed table has no unused slot.
  if (!available_slot) {
    return model::Result<OmsAdmissionResult>::success(
        OmsAdmissionResult{execution::SubmissionReason::OmsCapacityExceeded});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Treat contradictory risk/identity evidence as an invariant failure, not ordinary non-admission.
  if (!is_consistent(admission)) {
    return model::Result<OmsAdmissionResult>::failure(
        create_invalid_oms_state_error("outbound_oms.admission"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Commit the complete row only after every fallible decision; optional storage was preallocated.
  auto& slot = slots_[*available_slot];
  auto record = OutboundOrderRecord{std::move(admission)};
  slot.emplace(std::move(record));
  admission_order_.push_back(static_cast<std::uint32_t>(*available_slot));
  ++size_;
  return model::Result<OmsAdmissionResult>::success(OmsAdmissionResult{*slot});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// One shared mutation gate makes every repeated, missing, terminal, or wrong-source operation fail
// before changing the retained record.
model::Result<void> OutboundOms::transition(const model::OrderId& order_id,
                                            OutboundOrderState expected,
                                            OutboundOrderState target) {
  auto* record = find_mutable(order_id);
  if (record == nullptr || record->state_ != expected) {
    return model::Result<void>::failure(create_invalid_oms_state_error("outbound_oms.state"));
  }
  record->state_ = target;
  return model::Result<void>::success();
}

// --------------------------------------------------------
// Successful exact encoding is the only transition into PendingInitiation.
model::Result<void> OutboundOms::mark_encoding_succeeded(const model::OrderId& order_id) {
  return transition(order_id, OutboundOrderState::PendingEncoding,
                    OutboundOrderState::PendingInitiation);
}

// --------------------------------------------------------
// Scripted encoding failure retains the row permanently in LocallyFailed.
model::Result<void> OutboundOms::mark_encoding_failed(const model::OrderId& order_id) {
  return transition(order_id, OutboundOrderState::PendingEncoding,
                    OutboundOrderState::LocallyFailed);
}

// --------------------------------------------------------
// A definite pre-copy initiation failure is terminal and remains locally known.
model::Result<void> OutboundOms::mark_initiation_definitely_failed(const model::OrderId& order_id) {
  return transition(order_id, OutboundOrderState::PendingInitiation,
                    OutboundOrderState::LocallyFailed);
}

// --------------------------------------------------------
// Accepted fake initiation retains the reservation-owning row as WriteInitiated.
model::Result<void> OutboundOms::mark_write_initiated(const model::OrderId& order_id) {
  return transition(order_id, OutboundOrderState::PendingInitiation,
                    OutboundOrderState::WriteInitiated);
}

// --------------------------------------------------------
// Lost post-copy certainty retains the reservation-owning row as SubmissionUnknown.
model::Result<void> OutboundOms::mark_submission_unknown(const model::OrderId& order_id) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Preserve the M3 state transition's exact failure behavior before adding its M4 uncertainty.
  auto result = transition(order_id, OutboundOrderState::PendingInitiation,
                           OutboundOrderState::SubmissionUnknown);
  if (!result) {
    return result;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The successful transition guarantees the retained row still exists at its stable address.
  find_mutable(order_id)->reconciliation_required_ = true;
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A post-acceptance internal fault can invalidate completed local evidence without undoing the
// accepted copy; conservatively require reconciliation while retaining the same order and risk.
model::Result<void>
OutboundOms::mark_submission_unknown_after_internal_fault(const model::OrderId& order_id) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Apply only the already-accepted conservative M3 downgrade before changing its M4 projection.
  auto result = transition(order_id, OutboundOrderState::WriteInitiated,
                           OutboundOrderState::SubmissionUnknown);
  if (!result) {
    return result;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // A post-acceptance internal fault also creates an explicit later reconciliation obligation.
  find_mutable(order_id)->reconciliation_required_ = true;
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::oms
