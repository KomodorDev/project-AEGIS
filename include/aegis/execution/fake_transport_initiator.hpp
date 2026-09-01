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
  // Compare the complete scripted ordinal and outcome for deterministic fixture equality.
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
  create_fake_initiator_script(FakeInitiationOutcome default_outcome,
                               std::uint64_t maximum_invocations,
                               std::vector<FakeInitiationOverride> overrides);

  // --------------------------------------------------------
  // Return the outcome used when no ordinal-specific override exists.
  [[nodiscard]] FakeInitiationOutcome default_outcome() const noexcept { return default_outcome_; }

  // --------------------------------------------------------
  // Return the maximum number of one-based invocations the script permits.
  [[nodiscard]] std::uint64_t maximum_invocations() const noexcept { return maximum_invocations_; }

  // --------------------------------------------------------
  // Borrow the canonical ordinal-sorted override sequence.
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
  // Return the submission attempt whose bytes occupy this accepted slot.
  [[nodiscard]] model::SubmissionAttemptId attempt_id() const noexcept { return attempt_id_; }

  // --------------------------------------------------------
  // Return the encoder invocation that produced the copied bytes.
  [[nodiscard]] model::EncoderInvocationOrdinal encoder_invocation_ordinal() const noexcept {
    return encoder_invocation_ordinal_;
  }

  // --------------------------------------------------------
  // Return the initiator invocation that accepted this write.
  [[nodiscard]] model::InitiatorInvocationOrdinal initiator_invocation_ordinal() const noexcept {
    return initiator_invocation_ordinal_;
  }

  // --------------------------------------------------------
  // Return the monotonic ordinal assigned to this accepted write.
  [[nodiscard]] model::FakeWriteOrdinal write_ordinal() const noexcept { return write_ordinal_; }

  // --------------------------------------------------------
  // Return the exact number of initialized bytes in the accepted slot.
  [[nodiscard]] std::uint16_t byte_length() const noexcept { return byte_length_; }

  // --------------------------------------------------------
  // Borrow only the initialized accepted prefix, excluding unused fixed storage.
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
  // Interesting syntax: friendship restricts accepted-slot construction to the initiator after
  // capacity and script checks have completed.
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
  // Return the one-based initiator invocation consumed for this result.
  [[nodiscard]] model::InitiatorInvocationOrdinal invocation_ordinal() const noexcept {
    return invocation_ordinal_;
  }

  // --------------------------------------------------------
  // Return the effective scripted transport outcome.
  [[nodiscard]] FakeInitiationOutcome outcome() const noexcept { return outcome_; }

  // --------------------------------------------------------
  // Return the accepted-write ordinal only when transport initiation copied the bytes.
  [[nodiscard]] const std::optional<model::FakeWriteOrdinal>& write_ordinal() const noexcept {
    return write_ordinal_;
  }

  // --------------------------------------------------------
  // Report whether the transport accepted and copied this invocation's bytes.
  [[nodiscard]] bool is_accepted() const noexcept { return write_ordinal_.has_value(); }

  // --------------------------------------------------------
  // Return the endpoint clock reading captured for an accepted invocation, when available.
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
  // Interesting syntax: friendship keeps result alternatives coupled to the initiator's consumed
  // ordinal, accepted-slot, and measurement-clock state.
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
  create_deterministic_fake_write_initiator(FakeInitiatorScript script,
                                            std::uint32_t accepted_write_capacity);

  // --------------------------------------------------------
  // Consume one action, then copy accepted bytes and read its endpoint, or return pre-copy failure.
  [[nodiscard]] model::Result<FakeInitiationResult>
  initiate(const EncodedFakeOrder& encoded_order, SubmissionMeasurementClock& measurement_clock);

  // --------------------------------------------------------
  // Return the number of accepted-write slots reserved during construction.
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Return how many invocation ordinals have been consumed, including failed outcomes.
  [[nodiscard]] std::uint64_t invocations_consumed() const noexcept {
    return invocations_consumed_;
  }

  // --------------------------------------------------------
  // Borrow the immutable accepted-write prefix in successful invocation order.
  [[nodiscard]] std::span<const AcceptedFakeWrite> accepted_writes() const noexcept {
    return accepted_writes_;
  }

  // --------------------------------------------------------
  // Borrow the immutable validated outcome script that governs future invocations.
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
