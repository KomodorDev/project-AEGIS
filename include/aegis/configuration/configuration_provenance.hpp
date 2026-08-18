// Purpose: carry stable configuration identity and exact section revisions into later evidence.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"

#include <string>
#include <utility>
#include <vector>

namespace aegis::configuration {

class StartupConfiguration;

class ConfigurationFingerprint {
public:
  explicit ConfigurationFingerprint(model::Sha256Digest bytes) noexcept
      : bytes_{std::move(bytes)} {}

  [[nodiscard]] const model::Sha256Digest& bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::string to_hex() const;

  friend bool operator==(const ConfigurationFingerprint&,
                         const ConfigurationFingerprint&) = default;

private:
  model::Sha256Digest bytes_;
};

struct InstrumentMetadataRevisionEntry {
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  model::InstrumentMetadataRevision revision;

  friend bool operator==(const InstrumentMetadataRevisionEntry&,
                         const InstrumentMetadataRevisionEntry&) = default;
};

class ConfigurationProvenance {
public:
  [[nodiscard]] const ConfigurationFingerprint& fingerprint() const noexcept {
    return fingerprint_;
  }
  [[nodiscard]] model::ConfigurationRevision configuration_revision() const noexcept {
    return configuration_revision_;
  }
  [[nodiscard]] model::OrganizationRevision organization_revision() const noexcept {
    return organization_revision_;
  }
  [[nodiscard]] model::StrategyConfigurationRevision
  strategy_configuration_revision() const noexcept {
    return strategy_configuration_revision_;
  }
  [[nodiscard]] model::SubscriptionRevision subscription_revision() const noexcept {
    return subscription_revision_;
  }
  [[nodiscard]] model::RouteRevision route_revision() const noexcept { return route_revision_; }
  [[nodiscard]] const std::vector<InstrumentMetadataRevisionEntry>&
  instrument_metadata_revisions() const noexcept {
    return instrument_metadata_revisions_;
  }
  [[nodiscard]] const model::InstrumentMetadataRevision*
  find_instrument_metadata_revision(const model::VenueId& venue_id,
                                    const model::InstrumentId& instrument_id) const noexcept;

  friend bool operator==(const ConfigurationProvenance&, const ConfigurationProvenance&) = default;

private:
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

  ConfigurationFingerprint fingerprint_;
  model::ConfigurationRevision configuration_revision_;
  model::OrganizationRevision organization_revision_;
  model::StrategyConfigurationRevision strategy_configuration_revision_;
  model::SubscriptionRevision subscription_revision_;
  model::RouteRevision route_revision_;
  std::vector<InstrumentMetadataRevisionEntry> instrument_metadata_revisions_;

  friend class StartupConfiguration;
};

} // namespace aegis::configuration
