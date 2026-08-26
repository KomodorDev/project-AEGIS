// Purpose: derive source and retained-order M4 provenance only from an owned sealed configuration,
// a validated M4 root, and one genuine immutable OMS owner row.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/result.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <optional>
#include <utility>

namespace aegis::oms {

// ########################################################################
// A retained OMS row supplies immutable admission provenance without granting mutation authority.
class OutboundOrderRecord;

// ########################################################################

} // namespace aegis::oms

namespace aegis::runtime {

// ########################################################################
// The source factory privately mediates the resolver for the exact owner-bound reconciler.
class PrivateOrderEventFactory;

// ########################################################################

// ########################################################################
// The resolver is the single composition-root authority for subject presence. It owns one sealed
// configuration and copies the matching immutable seven-field root. Owning the configuration
// prevents a temporary or moved composition input from leaving a dangling authority reference.
class M4ProvenanceResolver final {
public:

  // --------------------------------------------------------
  // Publish a self-owned resolver only when configuration and root match; return
  // InvalidPrivateEvent for authority mismatch or translated allocation failure without publishing
  // a partial resolver.
  [[nodiscard]] static model::Result<M4ProvenanceResolver>
  create(const configuration::StartupConfiguration& configuration, const M4Policy& policy);

  // --------------------------------------------------------
  // Produce the root-only lineage/runtime profile with typed subject absence.
  [[nodiscard]] model::M4Provenance create_root_only_provenance() const noexcept;

  // --------------------------------------------------------
  // Require an exact configured account/venue binding and derive its owning firm.
  [[nodiscard]] model::Result<model::M4Provenance>
  create_configured_account_provenance(const model::LogicalAccountId& logical_account_id,
                                       const model::VenueId& venue_id) const;

  // --------------------------------------------------------
  // Require an exact configured account/venue/instrument tuple and derive metadata revision.
  [[nodiscard]] model::Result<model::M4Provenance>
  create_configured_instrument_provenance(const model::LogicalAccountId& logical_account_id,
                                          const model::VenueId& venue_id,
                                          const model::InstrumentId& instrument_id) const;

  // --------------------------------------------------------
  // Retain the maximal source provenance independently proved for an authoritative raw subject;
  // unknown/mismatched locators remain represented rather than becoming local ownership.
  [[nodiscard]] model::M4Provenance derive_authoritative_source_provenance(
      const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id,
      const std::optional<model::InstrumentId>& instrument_id) const noexcept;

  // --------------------------------------------------------
  // Expose the root copy used by every value from this resolver.
  [[nodiscard]] const model::M4RootProvenance& root() const noexcept { return root_; }

  // --------------------------------------------------------
  // Construction remains behind the root/configuration consistency check.
private:

  // --------------------------------------------------------
  // Return whether sealed configuration proves the exact logical-account and venue binding.
  [[nodiscard]] bool
  has_configured_account_venue_binding(const model::LogicalAccountId& logical_account_id,
                                       const model::VenueId& venue_id) const noexcept;

  // --------------------------------------------------------
  // Validate every retained admission authority against the sealed configuration and M4 root,
  // then copy the complete known-order subject; mismatch returns InvalidPrivateEvent.
  [[nodiscard]] model::Result<model::M4Provenance>
  derive_retained_order_provenance(const oms::OutboundOrderRecord& retained_order) const;

  // --------------------------------------------------------
  // Own the immutable configuration and copy the matching small root value.
  M4ProvenanceResolver(configuration::StartupConfiguration configuration,
                       model::M4RootProvenance root) noexcept
      : configuration_{std::move(configuration)}, root_{std::move(root)} {}

  // --------------------------------------------------------
  // Retain the sealed configuration authority and its exact matching root copy.
  configuration::StartupConfiguration configuration_;
  model::M4RootProvenance root_;

  // ########################################################################
  // Only the trusted source factory may delegate these owner-planning authority checks.
  friend class PrivateOrderEventFactory;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
