// Purpose: validate deterministic fake-initiation scripts and implement the exact accepted-slot
// copy boundary without retries, callbacks, network endpoints, or any live communication
// capability.

#include "aegis/execution/fake_transport_initiator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace aegis::execution {
namespace {

// ########################################################################
// Preallocated accepted slots can cross the copy boundary without a throwing element relocation.
static_assert(std::is_nothrow_move_constructible_v<AcceptedFakeWrite>);

// ########################################################################

// --------------------------------------------------------
// Only the three ADR-assigned fake boundary outcomes are valid AEGISSUP values.
[[nodiscard]] bool is_assigned(FakeInitiationOutcome outcome) noexcept {
  return outcome == FakeInitiationOutcome::DefiniteFailureBeforeAcceptance ||
         outcome == FakeInitiationOutcome::AcceptedAndInitiated ||
         outcome == FakeInitiationOutcome::AcceptedThenOutcomeLost;
}

// --------------------------------------------------------
// Separate impossible local fake state from the ordinary definite capacity outcome.
[[nodiscard]] model::DomainError invalid_fake_state(std::string field) {
  return model::DomainError::at_field(model::DomainErrorCode::InvalidFakeState, std::move(field));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate outcomes and bounds before sorting authored overrides into canonical invocation order.
model::Result<FakeInitiatorScript>
FakeInitiatorScript::create(FakeInitiationOutcome default_outcome,
                            std::uint64_t maximum_invocations,
                            std::vector<FakeInitiationOverride> overrides) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Fail closed on an unassigned default or a maximum outside the AEGISSUP schema-one bound.
  if (!is_assigned(default_outcome)) {
    return model::Result<FakeInitiatorScript>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.initiator_script.default_outcome"));
  }
  if (maximum_invocations == 0U || maximum_invocations > maximum_submission_attempts_supported) {
    return model::Result<FakeInitiatorScript>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.maximum_submission_attempts"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical order makes authored input order irrelevant to selection and policy bytes.
  std::sort(overrides.begin(), overrides.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.invocation_ordinal < rhs.invocation_ordinal;
  });

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject every zero, out-of-range, duplicate, or unassigned override before publication.
  for (std::size_t index = 0U; index < overrides.size(); ++index) {
    const auto& override = overrides[index];
    if (override.invocation_ordinal == 0U || override.invocation_ordinal > maximum_invocations ||
        !is_assigned(override.outcome) ||
        (index != 0U && overrides[index - 1U].invocation_ordinal == override.invocation_ordinal)) {
      return model::Result<FakeInitiatorScript>::failure(
          model::DomainError::at_index(model::DomainErrorCode::InvalidSubmissionPolicy,
                                       "submission_policy.initiator_script.overrides", index));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the validated default plus canonical unique override list as one immutable value.
  return model::Result<FakeInitiatorScript>::success(
      FakeInitiatorScript{default_outcome, maximum_invocations, std::move(overrides)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the already-validated canonical script without another allocation or reorder.
FakeInitiatorScript::FakeInitiatorScript(FakeInitiationOutcome default_outcome,
                                         std::uint64_t maximum_invocations,
                                         std::vector<FakeInitiationOverride> overrides)
    : default_outcome_{default_outcome}, maximum_invocations_{maximum_invocations},
      overrides_{std::move(overrides)} {}

// --------------------------------------------------------
// Binary search the canonical unique list and otherwise return the non-exhausting default.
FakeInitiationOutcome
FakeInitiatorScript::outcome_for(model::InitiatorInvocationOrdinal ordinal) const noexcept {
  const auto found =
      std::lower_bound(overrides_.begin(), overrides_.end(), ordinal.value(),
                       [](const FakeInitiationOverride& override, std::uint64_t value) {
                         return override.invocation_ordinal < value;
                       });
  return found != overrides_.end() && found->invocation_ordinal == ordinal.value()
             ? found->outcome
             : default_outcome_;
}

// --------------------------------------------------------
// Copy every accepted byte and bind it to all local identities at the sole uncertainty boundary.
AcceptedFakeWrite::AcceptedFakeWrite(const EncodedFakeOrder& encoded_order,
                                     model::InitiatorInvocationOrdinal initiator_invocation_ordinal,
                                     model::FakeWriteOrdinal write_ordinal) noexcept
    : attempt_id_{encoded_order.attempt_id()},
      encoder_invocation_ordinal_{encoded_order.invocation_ordinal()},
      initiator_invocation_ordinal_{initiator_invocation_ordinal}, write_ordinal_{write_ordinal},
      byte_length_{encoded_order.byte_length()} {
  std::copy(encoded_order.bytes().begin(), encoded_order.bytes().end(), bytes_.begin());
}

// --------------------------------------------------------
// Retain the effective boundary outcome, accepted slot identity, and immediate post-copy endpoint.
FakeInitiationResult::FakeInitiationResult(
    model::InitiatorInvocationOrdinal invocation_ordinal, FakeInitiationOutcome outcome,
    std::optional<model::FakeWriteOrdinal> write_ordinal,
    std::optional<std::uint64_t> accepted_slot_endpoint_nanoseconds) noexcept
    : invocation_ordinal_{invocation_ordinal}, outcome_{outcome}, write_ordinal_{write_ordinal},
      accepted_slot_endpoint_nanoseconds_{accepted_slot_endpoint_nanoseconds} {}

// --------------------------------------------------------
// Reject zero capacity before allocating the complete accepted-write slot array.
model::Result<DeterministicFakeWriteInitiator>
DeterministicFakeWriteInitiator::create(FakeInitiatorScript script,
                                        std::uint32_t accepted_write_capacity) {
  if (accepted_write_capacity == 0U) {
    return model::Result<DeterministicFakeWriteInitiator>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.accepted_write_capacity"));
  }
  try {
    return model::Result<DeterministicFakeWriteInitiator>::success(
        DeterministicFakeWriteInitiator{std::move(script), accepted_write_capacity});
  } catch (const std::bad_alloc&) {
    return model::Result<DeterministicFakeWriteInitiator>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.accepted_write_capacity"));
  } catch (const std::length_error&) {
    return model::Result<DeterministicFakeWriteInitiator>::failure(
        model::DomainError::at_field(model::DomainErrorCode::InvalidSubmissionPolicy,
                                     "submission_policy.accepted_write_capacity"));
  }
}

// --------------------------------------------------------
// Reserve every fixed accepted slot before any fake initiation can cross the copy boundary.
DeterministicFakeWriteInitiator::DeterministicFakeWriteInitiator(
    FakeInitiatorScript script, std::uint32_t accepted_write_capacity)
    : script_{std::move(script)}, capacity_{accepted_write_capacity} {
  accepted_writes_.reserve(capacity_);
}

// --------------------------------------------------------
// Consume before selection so definite failures and accepted outcomes advance identically.
model::Result<model::InitiatorInvocationOrdinal>
DeterministicFakeWriteInitiator::consume_invocation() {
  if (invocations_consumed_ == script_.maximum_invocations()) {
    return model::Result<model::InitiatorInvocationOrdinal>::failure(
        invalid_fake_state("fake_write_initiator.invocation_ordinal"));
  }
  ++invocations_consumed_;
  return model::InitiatorInvocationOrdinal::from_value(invocations_consumed_);
}

// --------------------------------------------------------
// Capacity and scripted definite failure occur before copy; accepted outcomes are returned only
// after the exact bytes have been retained in their permanently inspectable slot.
model::Result<FakeInitiationResult>
DeterministicFakeWriteInitiator::initiate(const EncodedFakeOrder& encoded_order,
                                          SubmissionMeasurementClock& measurement_clock) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Every reached call first consumes its invocation and selected action exactly once.
  auto invocation = consume_invocation();
  if (!invocation) {
    return model::Result<FakeInitiationResult>::failure(invocation.error());
  }
  const auto selected_outcome = script_.outcome_for(invocation.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Both an explicit definite action and full slot capacity prove no accepted copy occurred.
  if (selected_outcome == FakeInitiationOutcome::DefiniteFailureBeforeAcceptance ||
      accepted_writes_.size() >= capacity_) {
    return model::Result<FakeInitiationResult>::success(FakeInitiationResult{
        invocation.value(), FakeInitiationOutcome::DefiniteFailureBeforeAcceptance, std::nullopt,
        std::nullopt});
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Assign an ordinal only as part of the accepted copy; the positive fixed capacity bounds it.
  auto write_ordinal = model::FakeWriteOrdinal::from_value(accepted_writes_.size() + 1U);
  if (!write_ordinal) {
    return model::Result<FakeInitiationResult>::failure(
        invalid_fake_state("fake_write_initiator.write_ordinal"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Emplacing the full byte copy is the exact boundary after which no definite failure is returned;
  // read the endpoint immediately, before constructing or returning any later result state.
  auto accepted_write = AcceptedFakeWrite{encoded_order, invocation.value(), write_ordinal.value()};
  accepted_writes_.push_back(std::move(accepted_write));
  const auto accepted_slot_endpoint = measurement_clock.now_nanoseconds();
  return model::Result<FakeInitiationResult>::success(
      FakeInitiationResult{invocation.value(), selected_outcome,
                           std::optional{write_ordinal.value()}, accepted_slot_endpoint});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::execution
