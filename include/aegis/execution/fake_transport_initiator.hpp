// Purpose: define the validated deterministic initiation script and fixed-capacity accepted-write
// slots that model transport uncertainty while remaining structurally incapable of communication.

#pragma once

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aegis::execution {

// ########################################################################
// Assigned outcomes name the exact before-copy, accepted, and accepted-but-uncertain boundaries.
enum class FakeInitiationOutcome : std::uint8_t {
  DefiniteFailureBeforeAcceptance = 1,
  AcceptedAndInitiated = 2,
  AcceptedThenOutcomeLost = 3,
};

// ########################################################################

// ########################################################################
// One override replaces the default outcome at an exact one-based initiator invocation.
struct FakeInitiationOverride {
  std::uint64_t invocation_ordinal;
  FakeInitiationOutcome outcome;

  // --------------------------------------------------------
  friend bool operator==(const FakeInitiationOverride&, const FakeInitiationOverride&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The validated script owns canonical ordinal-sorted unique overrides and a permanent default.
class FakeInitiatorScript final {
public:

  // --------------------------------------------------------
  // Validate assigned outcomes and bounded one-based ordinals, then canonicalize override order.
  [[nodiscard]] static model::Result<FakeInitiatorScript>
  create(FakeInitiationOutcome default_outcome, std::uint64_t maximum_invocations,
         std::vector<FakeInitiationOverride> overrides);

  // --------------------------------------------------------
  [[nodiscard]] FakeInitiationOutcome default_outcome() const noexcept { return default_outcome_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint64_t maximum_invocations() const noexcept { return maximum_invocations_; }

  // --------------------------------------------------------
  [[nodiscard]] const std::vector<FakeInitiationOverride>& overrides() const noexcept {
    return overrides_;
  }

  // --------------------------------------------------------
  // Select one exact override or the permanent default without state or allocation.
  [[nodiscard]] FakeInitiationOutcome
  outcome_for(model::InitiatorInvocationOrdinal ordinal) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only values accepted and sorted by create.
  FakeInitiatorScript(FakeInitiationOutcome default_outcome, std::uint64_t maximum_invocations,
                      std::vector<FakeInitiationOverride> overrides);

  // --------------------------------------------------------
  FakeInitiationOutcome default_outcome_;
  std::uint64_t maximum_invocations_;
  std::vector<FakeInitiationOverride> overrides_;
};

// ########################################################################

// ########################################################################
// A retained slot proves the exact bytes crossed the fake acceptance boundary and binds them to
// outer-attempt, initiator-invocation, encoder-invocation, and accepted-write identities.
class AcceptedFakeWrite final {
public:

  // --------------------------------------------------------
  [[nodiscard]] model::SubmissionAttemptId attempt_id() const noexcept { return attempt_id_; }

  // --------------------------------------------------------
  [[nodiscard]] model::EncoderInvocationOrdinal encoder_invocation_ordinal() const noexcept {
    return encoder_invocation_ordinal_;
  }

  // --------------------------------------------------------
  [[nodiscard]] model::InitiatorInvocationOrdinal initiator_invocation_ordinal() const noexcept {
    return initiator_invocation_ordinal_;
  }

  // --------------------------------------------------------
  [[nodiscard]] model::FakeWriteOrdinal write_ordinal() const noexcept { return write_ordinal_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint16_t byte_length() const noexcept { return byte_length_; }

  // --------------------------------------------------------
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {bytes_.data(), byte_length_};
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construction performs the exact accepted-slot copy before any accepted outcome is returned.
  AcceptedFakeWrite(const EncodedFakeOrder& encoded_order,
                    model::InitiatorInvocationOrdinal initiator_invocation_ordinal,
                    model::FakeWriteOrdinal write_ordinal) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  friend class DeterministicFakeWriteInitiator;

  // ########################################################################

  model::SubmissionAttemptId attempt_id_;
  model::EncoderInvocationOrdinal encoder_invocation_ordinal_;
  model::InitiatorInvocationOrdinal initiator_invocation_ordinal_;
  model::FakeWriteOrdinal write_ordinal_;
  std::array<std::byte, maximum_encoded_fake_order_bytes> bytes_{};
  std::uint16_t byte_length_;
};

// ########################################################################

// ########################################################################
// One reached initiator call reports its consumed invocation, effective outcome, accepted-write
// identity, and the immediate post-copy measurement reading at the exact accepted boundary.
class FakeInitiationResult final {
public:

  // --------------------------------------------------------
  [[nodiscard]] model::InitiatorInvocationOrdinal invocation_ordinal() const noexcept {
    return invocation_ordinal_;
  }

  // --------------------------------------------------------
  [[nodiscard]] FakeInitiationOutcome outcome() const noexcept { return outcome_; }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<model::FakeWriteOrdinal>& write_ordinal() const noexcept {
    return write_ordinal_;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool accepted() const noexcept { return write_ordinal_.has_value(); }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<std::uint64_t>&
  accepted_slot_endpoint_nanoseconds() const noexcept {
    return accepted_slot_endpoint_nanoseconds_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Pair the effective boundary outcome with its accepted identity and immediate clock reading.
  FakeInitiationResult(model::InitiatorInvocationOrdinal invocation_ordinal,
                       FakeInitiationOutcome outcome,
                       std::optional<model::FakeWriteOrdinal> write_ordinal,
                       std::optional<std::uint64_t> accepted_slot_endpoint_nanoseconds) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  friend class DeterministicFakeWriteInitiator;

  // ########################################################################

  model::InitiatorInvocationOrdinal invocation_ordinal_;
  FakeInitiationOutcome outcome_;
  std::optional<model::FakeWriteOrdinal> write_ordinal_;
  std::optional<std::uint64_t> accepted_slot_endpoint_nanoseconds_;
};

// ########################################################################

// ########################################################################
// This final in-memory type owns only a validated script and preallocated byte slots; it has no
// network endpoint, callback, credential, session, retry, or generic transport interface.
class DeterministicFakeWriteInitiator final {
public:

  // --------------------------------------------------------
  // Bind a validated script and allocate every positive accepted-write slot before submission.
  [[nodiscard]] static model::Result<DeterministicFakeWriteInitiator>
  create(FakeInitiatorScript script, std::uint32_t accepted_write_capacity);

  // --------------------------------------------------------
  // Consume one action, then copy accepted bytes and read its endpoint, or return pre-copy failure.
  [[nodiscard]] model::Result<FakeInitiationResult>
  initiate(const EncodedFakeOrder& encoded_order, SubmissionMeasurementClock& measurement_clock);

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint64_t invocations_consumed() const noexcept {
    return invocations_consumed_;
  }

  // --------------------------------------------------------
  [[nodiscard]] std::span<const AcceptedFakeWrite> accepted_writes() const noexcept {
    return accepted_writes_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const FakeInitiatorScript& script() const noexcept { return script_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construct only after create has rejected an unusable zero capacity.
  DeterministicFakeWriteInitiator(FakeInitiatorScript script,
                                  std::uint32_t accepted_write_capacity);

  // --------------------------------------------------------
  // Consume the next bounded one-based invocation before consulting or applying its action.
  [[nodiscard]] model::Result<model::InitiatorInvocationOrdinal> consume_invocation();

  // --------------------------------------------------------
  FakeInitiatorScript script_;
  std::uint32_t capacity_;
  std::uint64_t invocations_consumed_{0U};
  std::vector<AcceptedFakeWrite> accepted_writes_;
};

// ########################################################################

} // namespace aegis::execution
