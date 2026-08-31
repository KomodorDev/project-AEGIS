// Purpose: define the immutable schema-one M3 submission policy that binds deterministic fake
// scripts, bounded capacities, exact encoded-size admission, and canonical provenance digests.

#pragma once

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegis::execution {

// The AEGISSUP version is independent from configuration, runtime, risk, fake-order, and trace
// schemas; changing its ordered positional fields requires a new assigned value.
inline constexpr std::uint16_t canonical_submission_policy_schema_version = 1U;

// ########################################################################
// Schema one permits only deterministic credential-free local fakes and cannot name a live mode.
enum class SubmissionCapability : std::uint8_t {
  DeterministicFakeOnly = 1,
};

// ########################################################################

// ########################################################################
// All mutable owner storage limits are sealed together so no direct-path component can silently
// allocate or select an independent capacity.
struct SubmissionPolicyCapacities {
  std::uint64_t maximum_submission_attempts;
  std::uint32_t reservation_capacity;
  std::uint32_t oms_order_capacity;
  std::uint16_t encoded_byte_capacity;
  std::uint32_t accepted_write_capacity;
  std::uint32_t submission_trace_capacity;
  std::uint32_t submission_diagnostic_capacity;

  // --------------------------------------------------------
  // Structural equality compares the complete bounded-runtime contract.
  friend bool operator==(const SubmissionPolicyCapacities&,
                         const SubmissionPolicyCapacities&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Construction receives raw canonical identities and the independently derived longest AEGISFOE
// size while validated fake-script objects retain their canonical sorted override order.
struct SubmissionPolicyParams {
  SubmissionCapability capability;
  model::Sha256Digest configuration_fingerprint;
  model::Sha256Digest runtime_policy_fingerprint;
  model::Sha256Digest risk_policy_fingerprint;
  model::RiskPolicyRevision risk_policy_revision;
  SubmissionPolicyCapacities capacities;
  std::uint64_t required_encoded_order_bytes;
  FakeEncoderScript encoder_script;
  FakeInitiatorScript initiator_script;
};

// ########################################################################

// ########################################################################
// This digest names the exact AEGISSUP positional bytes; hexadecimal rendering is not a second
// hash.
class SubmissionPolicyFingerprint final {
public:

  // --------------------------------------------------------
  // Wrap the already computed digest without altering its canonical bytes.
  explicit SubmissionPolicyFingerprint(model::Sha256Digest bytes) noexcept
      : bytes_{std::move(bytes)} {}

  // --------------------------------------------------------
  // Borrow the exact 32-byte digest identifying the canonical policy.
  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render the digest as lowercase hexadecimal without recomputing it.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Compare fingerprints by their complete digest bytes.
  friend bool operator==(const SubmissionPolicyFingerprint&,
                         const SubmissionPolicyFingerprint&) = default;

  // --------------------------------------------------------
private:
  model::Sha256Digest bytes_;
};

// ########################################################################

// ########################################################################
// Successful creation publishes one coherent fake-only policy after validating all capacity,
// invocation-bound, and exact-byte relationships before canonical encoding.
class SubmissionPolicy final {
public:

  // --------------------------------------------------------
  // Validate, encode, and fingerprint one complete AEGISSUP artifact atomically.
  [[nodiscard]] static model::Result<SubmissionPolicy>
  create_submission_policy(SubmissionPolicyParams params);

  // --------------------------------------------------------
  // Return the sealed capability mode; schema one permits only the deterministic fake path.
  [[nodiscard]] constexpr SubmissionCapability capability() const noexcept { return capability_; }

  // --------------------------------------------------------
  // Borrow the configuration digest to which this policy is bound.
  [[nodiscard]] const model::Sha256Digest& configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow the runtime-policy digest to which this policy is bound.
  [[nodiscard]] const model::Sha256Digest& runtime_policy_fingerprint() const noexcept {
    return runtime_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Borrow the risk-policy digest validated during policy construction.
  [[nodiscard]] const model::Sha256Digest& risk_policy_fingerprint() const noexcept {
    return risk_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Return the exact risk-policy revision paired with the retained digest.
  [[nodiscard]] model::RiskPolicyRevision risk_policy_revision() const noexcept {
    return risk_policy_revision_;
  }

  // --------------------------------------------------------
  // Borrow the coherent set of prevalidated owner-storage limits.
  [[nodiscard]] const SubmissionPolicyCapacities& capacities() const noexcept {
    return capacities_;
  }

  // --------------------------------------------------------
  // Return the route/attribution-derived bound proven to fit the encoded byte capacity.
  [[nodiscard]] constexpr std::uint16_t required_encoded_order_bytes() const noexcept {
    return required_encoded_order_bytes_;
  }

  // --------------------------------------------------------
  // Borrow the validated deterministic encoder script.
  [[nodiscard]] const FakeEncoderScript& encoder_script() const noexcept { return encoder_script_; }

  // --------------------------------------------------------
  // Borrow the validated deterministic transport-initiation script.
  [[nodiscard]] const FakeInitiatorScript& initiator_script() const noexcept {
    return initiator_script_;
  }

  // --------------------------------------------------------
  // Borrow the exact canonical bytes whose digest is exposed by fingerprint().
  [[nodiscard]] const std::vector<std::byte>& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

  // --------------------------------------------------------
  // Borrow the digest derived from the retained canonical bytes.
  [[nodiscard]] const SubmissionPolicyFingerprint& fingerprint() const noexcept {
    return fingerprint_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only fields that the factory has validated and derived from the same canonical bytes.
  SubmissionPolicy(SubmissionPolicyParams params, std::uint16_t required_encoded_order_bytes,
                   std::vector<std::byte> canonical_bytes, SubmissionPolicyFingerprint fingerprint)
      : capability_{params.capability},
        configuration_fingerprint_{std::move(params.configuration_fingerprint)},
        runtime_policy_fingerprint_{std::move(params.runtime_policy_fingerprint)},
        risk_policy_fingerprint_{std::move(params.risk_policy_fingerprint)},
        risk_policy_revision_{params.risk_policy_revision}, capacities_{params.capacities},
        required_encoded_order_bytes_{required_encoded_order_bytes},
        encoder_script_{std::move(params.encoder_script)},
        initiator_script_{std::move(params.initiator_script)},
        canonical_bytes_{std::move(canonical_bytes)}, fingerprint_{std::move(fingerprint)} {}

  // --------------------------------------------------------
  SubmissionCapability capability_;
  model::Sha256Digest configuration_fingerprint_;
  model::Sha256Digest runtime_policy_fingerprint_;
  model::Sha256Digest risk_policy_fingerprint_;
  model::RiskPolicyRevision risk_policy_revision_;
  SubmissionPolicyCapacities capacities_;
  std::uint16_t required_encoded_order_bytes_;
  FakeEncoderScript encoder_script_;
  FakeInitiatorScript initiator_script_;
  std::vector<std::byte> canonical_bytes_;
  SubmissionPolicyFingerprint fingerprint_;
};

// ########################################################################

} // namespace aegis::execution
