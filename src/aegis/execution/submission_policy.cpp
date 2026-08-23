// Purpose: validate M3 fake-only submission bounds and produce exact positional AEGISSUP bytes and
// their canonical SHA-256 identity.

#include "aegis/execution/submission_policy.hpp"

#include "aegis/model/domain_error.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::execution {
namespace {

using model::DomainError;
using model::DomainErrorCode;

// ########################################################################
// This private writer implements only the fixed-width positional AEGISSUP schema-one primitives.
class CanonicalSubmissionPolicyWriter final {
public:

  // --------------------------------------------------------
  // Append the fixed stream magic without a length prefix.
  [[nodiscard]] bool append_ascii_raw(std::string_view value) {
    if (!can_grow(value.size())) {
      return false;
    }
    for (const char character : value) {
      bytes_.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    return true;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool append_byte(std::uint8_t value) {
    if (!can_grow(1U)) {
      return false;
    }
    bytes_.push_back(std::byte{value});
    return true;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool append_u16(std::uint16_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  [[nodiscard]] bool append_u32(std::uint32_t value) {
    return append_byte(static_cast<std::uint8_t>((value >> 24U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 16U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>((value >> 8U) & 0xffU)) &&
           append_byte(static_cast<std::uint8_t>(value & 0xffU));
  }

  // --------------------------------------------------------
  [[nodiscard]] bool append_u64(std::uint64_t value) {
    // Interesting syntax: the explicit zero break avoids unsigned loop wrap after byte eight.
    for (unsigned int shift = 56U;; shift -= 8U) {
      if (!append_byte(static_cast<std::uint8_t>((value >> shift) & 0xffU))) {
        return false;
      }
      if (shift == 0U) {
        break;
      }
    }
    return true;
  }

  // --------------------------------------------------------
  [[nodiscard]] bool append_bytes(std::span<const std::byte> value) {
    if (!can_grow(value.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
  }

  // --------------------------------------------------------
  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  [[nodiscard]] bool can_grow(std::size_t additional) const noexcept {
    return additional <= std::numeric_limits<std::size_t>::max() - bytes_.size();
  }

  // --------------------------------------------------------
  std::vector<std::byte> bytes_;
};

// ########################################################################

// --------------------------------------------------------
// Return one stable fail-closed policy error without exposing authored values as text.
[[nodiscard]] model::Result<SubmissionPolicy> invalid(std::string_view field) {
  return model::Result<SubmissionPolicy>::failure(
      DomainError::at_field(DomainErrorCode::InvalidSubmissionPolicy, std::string{field}));
}

// --------------------------------------------------------
// Ensure all fake encoder actions remain on their assigned schema-one bytes.
[[nodiscard]] bool is_known(FakeEncodingAction action) noexcept {
  switch (action) {
  case FakeEncodingAction::Encode:
  case FakeEncodingAction::Fail:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Ensure all fake initiation outcomes remain on their assigned schema-one bytes.
[[nodiscard]] bool is_known(FakeInitiationOutcome outcome) noexcept {
  switch (outcome) {
  case FakeInitiationOutcome::DefiniteFailureBeforeAcceptance:
  case FakeInitiationOutcome::AcceptedAndInitiated:
  case FakeInitiationOutcome::AcceptedThenOutcomeLost:
    return true;
  default:
    return false;
  }
}

// --------------------------------------------------------
// Validate canonical scripts again at their cross-policy maximum boundary.
[[nodiscard]] bool is_valid(const FakeEncoderScript& script,
                            std::uint64_t maximum_attempts) noexcept {
  if (script.maximum_invocations() != maximum_attempts || !is_known(script.default_action()) ||
      script.overrides().size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  for (const auto& override : script.overrides()) {
    if (override.invocation_ordinal == 0U || override.invocation_ordinal > maximum_attempts ||
        !is_known(override.action)) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------
// Validate canonical initiator scripts at the same bounded outer-attempt authority.
[[nodiscard]] bool is_valid(const FakeInitiatorScript& script,
                            std::uint64_t maximum_attempts) noexcept {
  if (script.maximum_invocations() != maximum_attempts || !is_known(script.default_outcome()) ||
      script.overrides().size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  for (const auto& override : script.overrides()) {
    if (override.invocation_ordinal == 0U || override.invocation_ordinal > maximum_attempts ||
        !is_known(override.outcome)) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------
// Append the canonical default/count/ordered-override encoder script shape.
[[nodiscard]] bool append(CanonicalSubmissionPolicyWriter& writer,
                          const FakeEncoderScript& script) {
  if (!writer.append_byte(static_cast<std::uint8_t>(script.default_action())) ||
      !writer.append_u32(static_cast<std::uint32_t>(script.overrides().size()))) {
    return false;
  }
  for (const auto& override : script.overrides()) {
    if (!writer.append_u64(override.invocation_ordinal) ||
        !writer.append_byte(static_cast<std::uint8_t>(override.action))) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------
// Append the canonical default/count/ordered-override initiator script shape.
[[nodiscard]] bool append(CanonicalSubmissionPolicyWriter& writer,
                          const FakeInitiatorScript& script) {
  if (!writer.append_byte(static_cast<std::uint8_t>(script.default_outcome())) ||
      !writer.append_u32(static_cast<std::uint32_t>(script.overrides().size()))) {
    return false;
  }
  for (const auto& override : script.overrides()) {
    if (!writer.append_u64(override.invocation_ordinal) ||
        !writer.append_byte(static_cast<std::uint8_t>(override.outcome))) {
      return false;
    }
  }
  return true;
}

// --------------------------------------------------------
// Encode every AEGISSUP schema-one field in its assigned positional order.
[[nodiscard]] model::Result<std::vector<std::byte>> encode(const SubmissionPolicyParams& params) {
  CanonicalSubmissionPolicyWriter writer;
  const auto& capacities = params.capacities;
  if (!writer.append_ascii_raw("AEGISSUP") ||
      !writer.append_u16(canonical_submission_policy_schema_version) ||
      !writer.append_byte(static_cast<std::uint8_t>(params.capability)) ||
      !writer.append_bytes(params.configuration_fingerprint) ||
      !writer.append_bytes(params.runtime_policy_fingerprint) ||
      !writer.append_bytes(params.risk_policy_fingerprint) ||
      !writer.append_u64(params.risk_policy_revision.value()) ||
      !writer.append_u64(capacities.maximum_submission_attempts) ||
      !writer.append_u32(capacities.reservation_capacity) ||
      !writer.append_u32(capacities.oms_order_capacity) ||
      !writer.append_u16(capacities.encoded_byte_capacity) ||
      !writer.append_u32(capacities.accepted_write_capacity) ||
      !writer.append_u32(capacities.submission_trace_capacity) ||
      !writer.append_u32(capacities.submission_diagnostic_capacity) ||
      !append(writer, params.encoder_script) || !append(writer, params.initiator_script)) {
    return model::Result<std::vector<std::byte>>::failure(
        DomainError::at_field(DomainErrorCode::EncodingOverflow, "submission_policy"));
  }
  return model::Result<std::vector<std::byte>>::success(std::move(writer).take());
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Render the canonical digest as the repository-standard lowercase fixed-width hexadecimal form.
std::string SubmissionPolicyFingerprint::to_hex() const {
  const auto hexadecimal = model::sha256_hex(bytes_);
  return {hexadecimal.begin(), hexadecimal.end()};
}

// --------------------------------------------------------
// Validate all construction relationships before publishing canonical bytes or an identity.
model::Result<SubmissionPolicy> SubmissionPolicy::create(SubmissionPolicyParams params) {
  const auto& capacities = params.capacities;

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject any capability or bound that cannot create a usable deterministic fake-only runtime.
  if (params.capability != SubmissionCapability::DeterministicFakeOnly) {
    return invalid("submission_policy.capability");
  }
  if (capacities.maximum_submission_attempts == 0U ||
      capacities.maximum_submission_attempts > maximum_submission_attempts_supported) {
    return invalid("submission_policy.maximum_submission_attempts");
  }
  if (capacities.reservation_capacity == 0U || capacities.oms_order_capacity == 0U ||
      capacities.encoded_byte_capacity == 0U || capacities.accepted_write_capacity == 0U ||
      capacities.submission_trace_capacity == 0U ||
      capacities.submission_diagnostic_capacity == 0U) {
    return invalid("submission_policy.capacity");
  }
  if (static_cast<std::uint64_t>(capacities.accepted_write_capacity) >
          capacities.oms_order_capacity ||
      capacities.oms_order_capacity > capacities.reservation_capacity ||
      static_cast<std::uint64_t>(capacities.reservation_capacity) >
          capacities.maximum_submission_attempts) {
    return invalid("submission_policy.capacity_relationship");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // The independently derived exact AEGISFOE bound must fit both schema and selected capacity.
  if (params.required_encoded_order_bytes == 0U ||
      params.required_encoded_order_bytes > maximum_encoded_fake_order_bytes ||
      params.required_encoded_order_bytes > capacities.encoded_byte_capacity ||
      capacities.encoded_byte_capacity > maximum_encoded_fake_order_bytes) {
    return invalid("submission_policy.encoded_byte_capacity");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Script invocation domains must exactly equal the policy's outer-attempt ceiling.
  if (!is_valid(params.encoder_script, capacities.maximum_submission_attempts)) {
    return invalid("submission_policy.encoder_script");
  }
  if (!is_valid(params.initiator_script, capacities.maximum_submission_attempts)) {
    return invalid("submission_policy.initiator_script");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Encode only the accepted artifact and derive its one canonical fingerprint.
  auto canonical = encode(params);
  if (!canonical) {
    return model::Result<SubmissionPolicy>::failure(canonical.error());
  }
  auto canonical_bytes = std::move(canonical).value();
  SubmissionPolicyFingerprint fingerprint{model::sha256(canonical_bytes)};
  const auto required_encoded_order_bytes =
      static_cast<std::uint16_t>(params.required_encoded_order_bytes);
  return model::Result<SubmissionPolicy>::success(
      SubmissionPolicy{std::move(params), required_encoded_order_bytes, std::move(canonical_bytes),
                       std::move(fingerprint)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::execution
