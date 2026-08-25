// Purpose: carry the exact shared M4 root authority needed by OMS, inventory, recovery, runtime,
// and evidence without making those lower subsystems depend on a composition-root policy type.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <optional>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// Forward declaration preserves model-layer independence while restricting root construction.
class M4Policy;

// ########################################################################

// ########################################################################
// The composition-root resolver is the sole subject-provenance construction authority.
class M4ProvenanceResolver;

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

// ########################################################################
// Route identity and its sealed revision are one atomic provenance group; neither may appear alone.
struct M4RouteSubject {
  RouteId route_id;
  RouteRevision route_revision;

  // --------------------------------------------------------
  // Structural equality keeps the route grant inseparable from its revision.
  friend bool operator==(const M4RouteSubject&, const M4RouteSubject&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Instrument identity and metadata revision are one atomic supported-subject group.
struct M4InstrumentSubject {
  InstrumentId instrument_id;
  InstrumentMetadataRevision metadata_revision;

  // --------------------------------------------------------
  // Structural equality keeps the instrument inseparable from its metadata authority.
  friend bool operator==(const M4InstrumentSubject&, const M4InstrumentSubject&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One optional M4 subject retains only attribution proved by sealed configuration or a complete
// retained local-order projection. Private construction makes every allowed presence shape closed.
class M4SubjectProvenance final {
public:

  // --------------------------------------------------------
  // Every subject identifies the exact logical account named by its source or local order.
  [[nodiscard]] const LogicalAccountId& logical_account_id() const noexcept {
    return logical_account_id_;
  }

  // --------------------------------------------------------
  // Every subject identifies the exact venue named by its source or local order.
  [[nodiscard]] const VenueId& venue_id() const noexcept { return venue_id_; }

  // --------------------------------------------------------
  // Firm is present only when configuration independently proves account ownership.
  [[nodiscard]] const std::optional<FirmId>& firm_id() const noexcept { return firm_id_; }

  // --------------------------------------------------------
  // Desk appears only in the complete retained known-order shape.
  [[nodiscard]] const std::optional<DeskId>& desk_id() const noexcept { return desk_id_; }

  // --------------------------------------------------------
  // Bot appears only in the complete retained known-order shape.
  [[nodiscard]] const std::optional<BotId>& bot_id() const noexcept { return bot_id_; }

  // --------------------------------------------------------
  // Strategy appears only in the complete retained known-order shape.
  [[nodiscard]] const std::optional<StrategyId>& strategy_id() const noexcept {
    return strategy_id_;
  }

  // --------------------------------------------------------
  // Route and revision appear atomically only in the complete retained known-order shape.
  [[nodiscard]] const std::optional<M4RouteSubject>& route() const noexcept { return route_; }

  // --------------------------------------------------------
  // Supported instrument and metadata revision always appear or remain absent together.
  [[nodiscard]] const std::optional<M4InstrumentSubject>& instrument() const noexcept {
    return instrument_;
  }

  // --------------------------------------------------------
  // Structural equality includes every presence bit and every proved subject value.
  friend bool operator==(const M4SubjectProvenance&, const M4SubjectProvenance&) = default;

  // --------------------------------------------------------
  // Raw callers cannot select a provenance presence shape.
private:

  // --------------------------------------------------------
  // Publish one resolver-validated subject without empty or sentinel identifiers.
  M4SubjectProvenance(LogicalAccountId logical_account_id, VenueId venue_id,
                      std::optional<FirmId> firm_id, std::optional<DeskId> desk_id,
                      std::optional<BotId> bot_id, std::optional<StrategyId> strategy_id,
                      std::optional<M4RouteSubject> route,
                      std::optional<M4InstrumentSubject> instrument) noexcept
      : logical_account_id_{std::move(logical_account_id)}, venue_id_{std::move(venue_id)},
        firm_id_{std::move(firm_id)}, desk_id_{std::move(desk_id)}, bot_id_{std::move(bot_id)},
        strategy_id_{std::move(strategy_id)}, route_{std::move(route)},
        instrument_{std::move(instrument)} {}

  // --------------------------------------------------------
  // Store the closed optional groups without a caller-visible shape discriminator.
  LogicalAccountId logical_account_id_;
  VenueId venue_id_;
  std::optional<FirmId> firm_id_;
  std::optional<DeskId> desk_id_;
  std::optional<BotId> bot_id_;
  std::optional<StrategyId> strategy_id_;
  std::optional<M4RouteSubject> route_;
  std::optional<M4InstrumentSubject> instrument_;

  // ########################################################################
  // The trusted resolver derives every presence bit from sealed authority.
  friend class runtime::M4ProvenanceResolver;

  // ########################################################################
};

// ########################################################################

// ########################################################################
// The complete provenance value combines the mandatory seven-field root with either no subject or
// one resolver-minted subject; it is shared unchanged by OMS, inventory, recovery, and evidence.
class M4Provenance final {
public:

  // --------------------------------------------------------
  // Borrow the complete policy/configuration authority common to every M4 record.
  [[nodiscard]] const M4RootProvenance& root() const noexcept { return root_; }

  // --------------------------------------------------------
  // Absence is the typed lineage/runtime state; no sentinel subject identifiers exist.
  [[nodiscard]] const std::optional<M4SubjectProvenance>& subject() const noexcept {
    return subject_;
  }

  // --------------------------------------------------------
  // Structural equality includes root authority plus the complete optional subject shape.
  friend bool operator==(const M4Provenance&, const M4Provenance&) = default;

  // --------------------------------------------------------
  // Only the trusted resolver may combine root and subject authority.
private:

  // --------------------------------------------------------
  // Construct root-only lineage provenance.
  explicit M4Provenance(M4RootProvenance root) noexcept : root_{std::move(root)} {}

  // --------------------------------------------------------
  // Construct one subject-scoped provenance value.
  M4Provenance(M4RootProvenance root, M4SubjectProvenance subject) noexcept
      : root_{std::move(root)}, subject_{std::move(subject)} {}

  // --------------------------------------------------------
  // Retain the mandatory root and optional closed subject as one immutable provenance value.
  M4RootProvenance root_;
  std::optional<M4SubjectProvenance> subject_;

  // ########################################################################
  // The trusted resolver is the only authority allowed to attach a subject to a root.
  friend class runtime::M4ProvenanceResolver;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::model
