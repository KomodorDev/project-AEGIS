// Purpose: define the validated deterministic fake-encoding script, exact fixed-capacity AEGISFOE
// result, and concrete offline encoder without any live transport capability.

#pragma once

#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aegis::oms {

// ########################################################################
// Keep the encoder header dependent only on the immutable OMS record contract's declaration.
class OutboundOrderRecord;

// ########################################################################

} // namespace aegis::oms

namespace aegis::execution {

// ########################################################################
// AEGISFOE schema one is bounded independently from the smaller policy-selected byte capacity.
inline constexpr std::uint16_t canonical_fake_order_schema_version = 1U;
inline constexpr std::size_t maximum_encoded_fake_order_bytes = 1'024U;
inline constexpr std::uint64_t maximum_submission_attempts_supported = 1'000'000U;

// ########################################################################
// Assigned script actions make exact encoding and an ordinary deterministic failure explicit.
enum class FakeEncodingAction : std::uint8_t {
  Encode = 1,
  Fail = 2,
};

// ########################################################################
// One override replaces the default action at an exact one-based encoder invocation.
struct FakeEncodingOverride {
  std::uint64_t invocation_ordinal;
  FakeEncodingAction action;

  // --------------------------------------------------------
  // Compare the complete scripted ordinal and action for deterministic fixture equality.
  friend bool operator==(const FakeEncodingOverride&, const FakeEncodingOverride&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The validated script owns canonical ordinal-sorted unique overrides and a non-exhausting default.
class FakeEncoderScript final {
public:

  // --------------------------------------------------------
  // Validate assigned actions and bounded one-based ordinals, then canonicalize override order.
  [[nodiscard]] static model::Result<FakeEncoderScript>
  create_fake_encoder_script(FakeEncodingAction default_action, std::uint64_t maximum_invocations,
                             std::vector<FakeEncodingOverride> overrides);

  // --------------------------------------------------------
  // Return the action used when no ordinal-specific override exists.
  [[nodiscard]] FakeEncodingAction default_action() const noexcept { return default_action_; }

  // --------------------------------------------------------
  // Return the maximum number of one-based invocations the script permits.
  [[nodiscard]] std::uint64_t maximum_invocations() const noexcept { return maximum_invocations_; }

  // --------------------------------------------------------
  // Borrow the canonical ordinal-sorted override sequence.
  [[nodiscard]] const std::vector<FakeEncodingOverride>& overrides() const noexcept {
    return overrides_;
  }

  // --------------------------------------------------------
  // Select one exact override or the permanent default without state or allocation.
  [[nodiscard]] FakeEncodingAction
  action_for(model::EncoderInvocationOrdinal ordinal) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only values accepted and sorted by create.
  FakeEncoderScript(FakeEncodingAction default_action, std::uint64_t maximum_invocations,
                    std::vector<FakeEncodingOverride> overrides);

  // --------------------------------------------------------
  FakeEncodingAction default_action_;
  std::uint64_t maximum_invocations_;
  std::vector<FakeEncodingOverride> overrides_;
};

// ########################################################################
// One successful local transform owns exact canonical bytes plus non-payload attempt/invocation
// provenance; no buffer can grow beyond the assigned AEGISFOE maximum.
class EncodedFakeOrder final {
public:

  // --------------------------------------------------------
  // Return the submission attempt represented by these immutable encoded bytes.
  [[nodiscard]] model::SubmissionAttemptId attempt_id() const noexcept { return attempt_id_; }

  // --------------------------------------------------------
  // Return the encoder invocation that produced these bytes.
  [[nodiscard]] model::EncoderInvocationOrdinal invocation_ordinal() const noexcept {
    return invocation_ordinal_;
  }

  // --------------------------------------------------------
  // Return the exact number of valid bytes in the fixed-capacity storage.
  [[nodiscard]] std::uint16_t byte_length() const noexcept { return byte_length_; }

  // --------------------------------------------------------
  // Borrow only the initialized encoded prefix, excluding unused fixed storage.
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {bytes_.data(), byte_length_};
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // The concrete encoder is the sole producer of valid exact-schema byte objects.
  EncodedFakeOrder(model::SubmissionAttemptId attempt_id,
                   model::EncoderInvocationOrdinal invocation_ordinal,
                   std::array<std::byte, maximum_encoded_fake_order_bytes> bytes,
                   std::uint16_t byte_length) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // Interesting syntax: friendship restricts successful byte construction to the validated fake
  // encoder path without exposing a public partial-state constructor.
  friend class DeterministicFakeOrderEncoder;

  // ########################################################################

  model::SubmissionAttemptId attempt_id_;
  model::EncoderInvocationOrdinal invocation_ordinal_;
  std::array<std::byte, maximum_encoded_fake_order_bytes> bytes_{};
  std::uint16_t byte_length_;
};

// ########################################################################
// One reached encoder call exposes its consumed invocation and either exact bytes or the scripted
// ordinary EncodingFailed action; invalid internal state remains a separate DomainError.
class FakeEncodingResult final {
public:

  // --------------------------------------------------------
  // Return the scripted action consumed for this invocation.
  [[nodiscard]] FakeEncodingAction action() const noexcept { return action_; }

  // --------------------------------------------------------
  // Return the one-based invocation ordinal consumed even when encoding failed.
  [[nodiscard]] model::EncoderInvocationOrdinal invocation_ordinal() const noexcept {
    return invocation_ordinal_;
  }

  // --------------------------------------------------------
  // Report whether this invocation produced a complete encoded order.
  [[nodiscard]] bool is_encoded() const noexcept { return encoded_order_.has_value(); }

  // --------------------------------------------------------
  // Borrow the complete encoded order, or return null when the scripted action failed.
  [[nodiscard]] const EncodedFakeOrder* encoded_order() const noexcept {
    return encoded_order_ ? &*encoded_order_ : nullptr;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Retain a scripted failure without manufacturing bytes.
  explicit FakeEncodingResult(model::EncoderInvocationOrdinal invocation_ordinal) noexcept;

  // --------------------------------------------------------
  // Retain the one exact encoded object from a successful invocation.
  explicit FakeEncodingResult(EncodedFakeOrder encoded_order) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // Interesting syntax: friendship centralizes construction of both result alternatives in the
  // deterministic encoder that owns invocation sequencing.
  friend class DeterministicFakeOrderEncoder;

  // ########################################################################

  FakeEncodingAction action_;
  model::EncoderInvocationOrdinal invocation_ordinal_;
  std::optional<EncodedFakeOrder> encoded_order_;
};

// ########################################################################
// This final concrete type performs only one bounded in-memory transform selected by its validated
// script; its API has no endpoint, session, credential, callback, file, or communication input.
class DeterministicFakeOrderEncoder final {
public:

  // --------------------------------------------------------
  // Bind one validated script and a positive policy-selected capacity no larger than 1,024 bytes.
  [[nodiscard]] static model::Result<DeterministicFakeOrderEncoder>
  create_deterministic_fake_order_encoder(FakeEncoderScript script, std::uint16_t byte_capacity);

  // --------------------------------------------------------
  // Consume one invocation/action, then either fail as scripted or preserve exact OMS economics.
  [[nodiscard]] model::Result<FakeEncodingResult>
  encode_order(const oms::OutboundOrderRecord& order);

  // --------------------------------------------------------
  // Return the fixed maximum encoded byte count accepted at construction.
  [[nodiscard]] std::uint16_t byte_capacity() const noexcept { return byte_capacity_; }

  // --------------------------------------------------------
  // Return how many invocation ordinals have been consumed, including scripted failures.
  [[nodiscard]] std::uint64_t invocations_consumed() const noexcept {
    return invocations_consumed_;
  }

  // --------------------------------------------------------
  // Borrow the immutable validated action script that governs future invocations.
  [[nodiscard]] const FakeEncoderScript& script() const noexcept { return script_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construct only after create has checked the fixed byte bound.
  DeterministicFakeOrderEncoder(FakeEncoderScript script, std::uint16_t byte_capacity);

  // --------------------------------------------------------
  // Consume the next bounded one-based invocation before consulting or applying its action.
  [[nodiscard]] model::Result<model::EncoderInvocationOrdinal> consume_invocation();

  // --------------------------------------------------------
  FakeEncoderScript script_;
  std::uint16_t byte_capacity_;
  std::uint64_t invocations_consumed_{0U};
};

// ########################################################################

} // namespace aegis::execution
