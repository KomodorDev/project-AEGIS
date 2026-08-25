// Purpose: carry the exact shared M4 root authority needed by OMS, inventory, recovery, runtime,
// and evidence without making those lower subsystems depend on a composition-root policy type.

#pragma once

#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

namespace aegis::runtime {

// ########################################################################
// Forward declaration preserves model-layer independence while restricting root construction.
class M4Policy;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::model {

// ########################################################################
// Only a validated M4 policy may mint the complete seven-field root authority. Every field is
// mandatory; absence and sentinel fingerprints are not part of this contract.
class M4RootProvenance final {
public:

  // --------------------------------------------------------
  // Identify the complete sealed startup configuration without copying configuration state.
  [[nodiscard]] const Sha256Digest& configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Retain the organization authority revision used for all runtime-derived attribution.
  [[nodiscard]] OrganizationRevision organization_revision() const noexcept {
    return organization_revision_;
  }

  // --------------------------------------------------------
  // Identify the exact immutable runtime policy that supplied bounded owner behavior.
  [[nodiscard]] const Sha256Digest& runtime_policy_fingerprint() const noexcept {
    return runtime_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Retain the fixed-risk revision paired with its independently sealed fingerprint.
  [[nodiscard]] RiskPolicyRevision risk_policy_revision() const noexcept {
    return risk_policy_revision_;
  }

  // --------------------------------------------------------
  // Identify the complete seven-scope risk rulebook used for every M4 economic decision.
  [[nodiscard]] const Sha256Digest& risk_policy_fingerprint() const noexcept {
    return risk_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Identify the unchanged M3 fake-only submission policy and its direct-path bounds.
  [[nodiscard]] const Sha256Digest& submission_policy_fingerprint() const noexcept {
    return submission_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Identify the M4 capacity policy that closes every private and recovery owner bound.
  [[nodiscard]] const Sha256Digest& m4_policy_fingerprint() const noexcept {
    return m4_policy_fingerprint_;
  }

  // --------------------------------------------------------
  // Structural equality requires all seven authority fields to match exactly.
  friend bool operator==(const M4RootProvenance&, const M4RootProvenance&) = default;

  // --------------------------------------------------------
  // Raw callers cannot construct provenance independently of validated policy creation.
private:

  // --------------------------------------------------------
  // Publish one complete root only after the policy has validated and hashed every authority.
  M4RootProvenance(Sha256Digest configuration_fingerprint,
                   OrganizationRevision organization_revision,
                   Sha256Digest runtime_policy_fingerprint, RiskPolicyRevision risk_policy_revision,
                   Sha256Digest risk_policy_fingerprint, Sha256Digest submission_policy_fingerprint,
                   Sha256Digest m4_policy_fingerprint) noexcept
      : configuration_fingerprint_{configuration_fingerprint},
        organization_revision_{organization_revision},
        runtime_policy_fingerprint_{runtime_policy_fingerprint},
        risk_policy_revision_{risk_policy_revision},
        risk_policy_fingerprint_{risk_policy_fingerprint},
        submission_policy_fingerprint_{submission_policy_fingerprint},
        m4_policy_fingerprint_{m4_policy_fingerprint} {}

  // --------------------------------------------------------
  // Store the complete seven-field root without optional or sentinel states.
  Sha256Digest configuration_fingerprint_;
  OrganizationRevision organization_revision_;
  Sha256Digest runtime_policy_fingerprint_;
  RiskPolicyRevision risk_policy_revision_;
  Sha256Digest risk_policy_fingerprint_;
  Sha256Digest submission_policy_fingerprint_;
  Sha256Digest m4_policy_fingerprint_;

  // ########################################################################
  // The validated immutable M4 policy is the only root-provenance minting authority.
  friend class runtime::M4Policy;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::model
