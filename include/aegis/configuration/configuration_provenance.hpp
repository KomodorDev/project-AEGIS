// Purpose: carry stable configuration identity and exact section revisions into later evidence.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <string>
#include <utility>
#include <vector>

namespace aegis::configuration {
// ########################################################################
// The sealed startup snapshot is the only authority allowed to mint matching provenance.
class StartupConfiguration;
// ########################################################################
// This digest names the exact schema-versioned canonical configuration bytes; hexadecimal output is
// a display representation of the same identity rather than a second hash.
class ConfigurationFingerprint {
public:
  // --------------------------------------------------------
  // Own one already-computed fixed-width SHA-256 configuration identity.
  explicit ConfigurationFingerprint(model::Sha256Digest bytes) noexcept
      : bytes_{std::move(bytes)} {}
  // --------------------------------------------------------
  // Borrow the fixed-width binary configuration identity.
  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }
  // --------------------------------------------------------
  // Render the same identity in lowercase hexadecimal form.
  [[nodiscard]] std::string to_hex() const;
  // --------------------------------------------------------
  // Structural equality compares the complete fixed-width digest.
  friend bool operator==(const ConfigurationFingerprint&,
                         const ConfigurationFingerprint&) = default;
  // --------------------------------------------------------
private:
  model::Sha256Digest bytes_;
};
// ########################################################################
// Metadata revisions retain their venue/instrument key so later evidence can identify the precise
// reference-data version used for an instrument-specific decision.
struct InstrumentMetadataRevisionEntry {
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  model::InstrumentMetadataRevision revision;
  // --------------------------------------------------------
  // Structural equality compares the key and the exact metadata revision used.
  friend bool operator==(const InstrumentMetadataRevisionEntry&,
                         const InstrumentMetadataRevisionEntry&) = default;
  // --------------------------------------------------------
};
// ########################################################################
// Provenance binds one configuration fingerprint to every section revision needed to reproduce the
// accepted startup rulebook without copying the rulebook itself into each trace record.
class ConfigurationProvenance {
public:
  // --------------------------------------------------------
  // Borrow the exact accepted configuration identity.
  [[nodiscard]] const ConfigurationFingerprint& fingerprint() const noexcept {
    return fingerprint_;
  }
  // --------------------------------------------------------
  // Return the revision of the complete startup configuration.
  [[nodiscard]] model::ConfigurationRevision configuration_revision() const noexcept {
    return configuration_revision_;
  }
  // --------------------------------------------------------
  // Return the revision of the organization section.
  [[nodiscard]] model::OrganizationRevision organization_revision() const noexcept {
    return organization_revision_;
  }
  // --------------------------------------------------------
  // Return the revision of the strategy-settings section.
  [[nodiscard]] model::StrategyConfigurationRevision
  strategy_configuration_revision() const noexcept {
    return strategy_configuration_revision_;
  }
  // --------------------------------------------------------
  // Return the revision of the market-data subscription section.
  [[nodiscard]] model::SubscriptionRevision subscription_revision() const noexcept {
    return subscription_revision_;
  }
  // --------------------------------------------------------
  // Return the revision of the execution-route section.
  [[nodiscard]] model::RouteRevision route_revision() const noexcept { return route_revision_; }
  // --------------------------------------------------------
  // Borrow canonical instrument-specific revision entries.
  [[nodiscard]] const std::vector<InstrumentMetadataRevisionEntry>&
  instrument_metadata_revisions() const noexcept {
    return instrument_metadata_revisions_;
  }
  // --------------------------------------------------------
  // Resolve the instrument-specific revision for one canonical venue/instrument key.
  [[nodiscard]] const model::InstrumentMetadataRevision*
  find_instrument_metadata_revision(const model::VenueId& venue_id,
                                    const model::InstrumentId& instrument_id) const noexcept;
  // --------------------------------------------------------
  // Structural equality compares the complete reproducibility contract.
  friend bool operator==(const ConfigurationProvenance&, const ConfigurationProvenance&) = default;
  // --------------------------------------------------------
private:
  // --------------------------------------------------------
  // Interesting syntax: private construction plus the StartupConfiguration friend prevents callers
  // from minting provenance that was not derived from one atomically accepted snapshot.
  ConfigurationProvenance(ConfigurationFingerprint fingerprint,
                          model::ConfigurationRevision configuration_revision,
                          model::OrganizationRevision organization_revision,
                          model::StrategyConfigurationRevision strategy_configuration_revision,
                          model::SubscriptionRevision subscription_revision,
                          model::RouteRevision route_revision,
                          std::vector<InstrumentMetadataRevisionEntry> metadata_revisions)
      : fingerprint_{std::move(fingerprint)}, configuration_revision_{configuration_revision},
        organization_revision_{organization_revision},
        strategy_configuration_revision_{strategy_configuration_revision},
        subscription_revision_{subscription_revision}, route_revision_{route_revision},
        instrument_metadata_revisions_{std::move(metadata_revisions)} {}
  // --------------------------------------------------------
  ConfigurationFingerprint fingerprint_;
  model::ConfigurationRevision configuration_revision_;
  model::OrganizationRevision organization_revision_;
  model::StrategyConfigurationRevision strategy_configuration_revision_;
  model::SubscriptionRevision subscription_revision_;
  model::RouteRevision route_revision_;
  std::vector<InstrumentMetadataRevisionEntry> instrument_metadata_revisions_;

  // ########################################################################
  // Only the atomically sealed startup snapshot may mint matching provenance.
  friend class StartupConfiguration;
  // ########################################################################
};
// ########################################################################
} // namespace aegis::configuration
