// Purpose: derive correlation-independent M4 source provenance only from an owned sealed
// configuration and validated M4 root without caller-authored organizational attribution.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/result.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <optional>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// The resolver is the single composition-root authority for subject presence. It owns one sealed
// configuration and copies the matching immutable seven-field root. Owning the configuration
// prevents a temporary or moved composition input from leaving a dangling authority reference.
class M4ProvenanceResolver final {
public:

  // --------------------------------------------------------
  // Reject a configuration/root mismatch before any provenance value can be published.
  [[nodiscard]] static model::Result<M4ProvenanceResolver>
  create(const configuration::StartupConfiguration& configuration, const M4Policy& policy);

  // --------------------------------------------------------
  // Produce the root-only lineage/runtime profile with typed subject absence.
  [[nodiscard]] model::M4Provenance root_only() const noexcept;

  // --------------------------------------------------------
  // Require an exact configured account/venue binding and derive its owning firm.
  [[nodiscard]] model::Result<model::M4Provenance>
  configured_account(const model::LogicalAccountId& logical_account_id,
                     const model::VenueId& venue_id) const;

  // --------------------------------------------------------
  // Require an exact configured account/venue/instrument tuple and derive metadata revision.
  [[nodiscard]] model::Result<model::M4Provenance>
  configured_instrument(const model::LogicalAccountId& logical_account_id,
                        const model::VenueId& venue_id,
                        const model::InstrumentId& instrument_id) const;

  // --------------------------------------------------------
  // Retain the maximal source provenance independently proved for an authoritative raw subject;
  // unknown/mismatched locators remain represented rather than becoming local ownership.
  [[nodiscard]] model::M4Provenance
  authoritative_source(const model::LogicalAccountId& logical_account_id,
                       const model::VenueId& venue_id,
                       const std::optional<model::InstrumentId>& instrument_id) const noexcept;

  // --------------------------------------------------------
  // Expose the root copy used by every value from this resolver.
  [[nodiscard]] const model::M4RootProvenance& root() const noexcept { return root_; }

  // --------------------------------------------------------
  // Construction remains behind the root/configuration consistency check.
private:

  // --------------------------------------------------------
  // Own the immutable configuration and copy the matching small root value.
  M4ProvenanceResolver(configuration::StartupConfiguration configuration,
                       model::M4RootProvenance root) noexcept
      : configuration_{std::move(configuration)}, root_{std::move(root)} {}

  // --------------------------------------------------------
  configuration::StartupConfiguration configuration_;
  model::M4RootProvenance root_;
};

// ########################################################################

} // namespace aegis::runtime
