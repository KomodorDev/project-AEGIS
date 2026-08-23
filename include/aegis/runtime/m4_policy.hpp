// Purpose: define the immutable 26-capacity AEGISM4P contract and derive its complete shared root
// provenance from already validated M1-M3 configuration, runtime, risk, and submission policies.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/execution/submission_policy.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/risk/risk_policy.hpp"
#include "aegis/runtime/runtime_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegis::runtime {

inline constexpr std::uint16_t canonical_m4_policy_schema_version = 1U;
inline constexpr std::size_t canonical_m4_policy_byte_size = 362U;

// ########################################################################
// Every M4 owner capacity is authored explicitly at unsigned 64-bit width so over-bound values can
// be rejected before narrowing to the implementation's fixed u32 storage limits.
struct M4PolicyCapacities {
  std::uint64_t max_private_admissions;
  std::uint64_t max_reconciliation_admissions;
  std::uint64_t max_account_safety_fences;
  std::uint64_t max_private_event_records;
  std::uint64_t max_event_identity_records;
  std::uint64_t max_trade_identity_records;
  std::uint64_t max_exchange_order_mappings;
  std::uint64_t max_pending_fill_intervals_per_order;
  std::uint64_t max_cancel_attempts;
  std::uint64_t max_inventory_source_rows;
  std::uint64_t max_inventory_aggregate_cells;
  std::uint64_t max_unattributed_exposure_rows;
  std::uint64_t max_account_safety_records;
  std::uint64_t max_transition_effects_per_turn;
  std::uint64_t max_order_callbacks_per_turn;
  std::uint64_t max_private_diagnostics;
  std::uint64_t max_private_audit_records;
  std::uint64_t max_journal_records;
  std::uint64_t max_snapshot_records;
  std::uint64_t max_reconciliation_batches;
  std::uint64_t max_reconciliation_rows_per_batch;
  std::uint64_t max_live_catchup_facts;
  std::uint64_t max_recovery_epochs;
  std::uint64_t max_namespace_registrations;
  std::uint64_t max_recovery_notifications;
  std::uint64_t max_reference_intents;

  // --------------------------------------------------------
  // Structural equality ensures every one of the 26 authored capacities participates.
  friend bool operator==(const M4PolicyCapacities&, const M4PolicyCapacities&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The digest names exact schema-one AEGISM4P bytes and cannot be confused with earlier policies.
class M4PolicyFingerprint final {
public:

  // --------------------------------------------------------
  // Wrap an already-computed digest without treating display text as policy identity.
  explicit M4PolicyFingerprint(model::Sha256Digest bytes) noexcept : bytes_{bytes} {}

  // --------------------------------------------------------
  // Expose the exact digest bytes carried by shared root provenance.
  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Render lowercase hexadecimal for diagnostics without rehashing.
  [[nodiscard]] std::string to_hex() const;

  // --------------------------------------------------------
  // Fingerprint equality compares the complete SHA-256 value.
  friend bool operator==(const M4PolicyFingerprint&, const M4PolicyFingerprint&) = default;

  // --------------------------------------------------------
  // Keep digest storage immutable after construction.
private:
  // The exact SHA-256 bytes are the complete fingerprint state.
  model::Sha256Digest bytes_;
};

// ########################################################################

// ########################################################################
// Successful creation cross-validates the complete M1-M3 authority chain, validates all M4 bounds,
// and publishes bytes, fingerprint, capacities, and root provenance atomically.
class M4Policy final {
public:

  // --------------------------------------------------------
  // Derive M4 authority only from one mutually consistent set of sealed M1-M3 policies.
  [[nodiscard]] static model::Result<M4Policy>
  create(const configuration::StartupConfiguration& configuration,
         const RuntimePolicy& runtime_policy, const risk::RiskPolicySnapshot& risk_policy,
         const execution::SubmissionPolicy& submission_policy, M4PolicyCapacities capacities);

  // --------------------------------------------------------
  // Expose the complete immutable owner-capacity contract.
  [[nodiscard]] const M4PolicyCapacities& capacities() const noexcept { return capacities_; }

  // --------------------------------------------------------
  // Expose the sole shared provenance root minted during validated construction.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Retain the exact positional schema bytes for evidence and compatibility checks.
  [[nodiscard]] const std::vector<std::byte>& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }

  // --------------------------------------------------------
  // Expose the hash of the complete canonical bytes without recomputation.
  [[nodiscard]] const M4PolicyFingerprint& fingerprint() const noexcept { return fingerprint_; }

  // --------------------------------------------------------
  // Structural equality includes capacities, root, bytes, and fingerprint.
  friend bool operator==(const M4Policy&, const M4Policy&) = default;

  // --------------------------------------------------------
  // Only the public authority-validating factory may publish a policy.
private:

  // --------------------------------------------------------
  // Assemble only fully validated values whose bytes and digest already agree.
  M4Policy(M4PolicyCapacities capacities, model::M4RootProvenance root_provenance,
           std::vector<std::byte> canonical_bytes, M4PolicyFingerprint fingerprint)
      : capacities_{capacities}, root_provenance_{std::move(root_provenance)},
        canonical_bytes_{std::move(canonical_bytes)}, fingerprint_{std::move(fingerprint)} {}

  // --------------------------------------------------------
  // Store every published artifact together so no field can be observed independently.
  M4PolicyCapacities capacities_;
  model::M4RootProvenance root_provenance_;
  std::vector<std::byte> canonical_bytes_;
  M4PolicyFingerprint fingerprint_;
};

// ########################################################################

} // namespace aegis::runtime
