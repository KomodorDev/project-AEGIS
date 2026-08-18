// Purpose: validate and seal the complete startup rulebook with deterministic provenance.

#pragma once

#include "aegis/configuration/configuration_provenance.hpp"
#include "aegis/execution/execution_route.hpp"
#include "aegis/market_data/subscription.hpp"
#include "aegis/model/instrument_metadata.hpp"
#include "aegis/organization/organization.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace aegis::configuration {

// The schema version is part of the canonical byte prefix; changing it is a compatibility decision,
// not an implementation detail.
inline constexpr std::uint16_t canonical_configuration_schema_version = 1U;

// M1 deliberately supports only the credential-free reference environment. Adding another
// assigned value changes the configuration semantics and therefore requires an explicit decision.
enum class VenueEnvironment : std::uint8_t {
  Testnet = 1,
};

// Venue definitions deliberately contain no endpoints or credentials in the M1 rulebook.
struct VenueDefinition {
  model::VenueId id;
  VenueEnvironment environment;

  friend bool operator==(const VenueDefinition&, const VenueDefinition&) = default;
};

// A logical account is owned by one peer firm at one venue; it is not a venue credential container.
struct LogicalAccountVenueBinding {
  model::LogicalAccountId logical_account_id;
  model::FirmId firm_id;
  model::VenueId venue_id;

  friend bool operator==(const LogicalAccountVenueBinding&,
                         const LogicalAccountVenueBinding&) = default;
};

// ObserveOnly is configuration, not an execution capability. Later milestones may add explicitly
// assigned modes without changing the fact that execution requires a separate enabled route.
enum class StrategyMode : std::uint8_t {
  ObserveOnly = 1,
};

// Settings must agree with immutable bot registration, while ObserveOnly remains non-executing.
struct BotStrategySettings {
  model::BotId bot_id;
  model::StrategyId strategy_id;
  StrategyMode mode;

  friend bool operator==(const BotStrategySettings&, const BotStrategySettings&) = default;
};

// These are authoring parameters, not a published partially validated configuration. Every
// collection is copied into a canonically sorted immutable value only if the whole snapshot passes.
struct StartupConfigurationParams {
  model::ConfigurationRevision revision;

  model::OrganizationRevision organization_revision;
  std::vector<organization::Firm> firms;
  std::vector<organization::Desk> desks;
  std::vector<organization::BotRegistration> bots;

  model::StrategyConfigurationRevision strategy_configuration_revision;
  std::vector<BotStrategySettings> strategy_settings;

  std::vector<VenueDefinition> venues;
  std::vector<LogicalAccountVenueBinding> logical_accounts;
  std::vector<model::InstrumentMetadataParams> instrument_metadata;

  model::SubscriptionRevision subscription_revision;
  std::vector<market_data::Subscription> subscriptions;

  model::RouteRevision route_revision;
  std::vector<execution::ExecutionRoute> routes;
};

// Successful creation publishes one immutable unit whose validated sections, canonical bytes,
// fingerprint, and revision provenance cannot be observed in a partially updated combination.
class StartupConfiguration {
public:
  [[nodiscard]] static model::Result<StartupConfiguration>
  create(StartupConfigurationParams params);

  [[nodiscard]] model::ConfigurationRevision revision() const noexcept { return revision_; }
  [[nodiscard]] const organization::Organization& organization() const noexcept {
    return organization_;
  }
  [[nodiscard]] model::StrategyConfigurationRevision
  strategy_configuration_revision() const noexcept {
    return strategy_configuration_revision_;
  }
  [[nodiscard]] const std::vector<BotStrategySettings>& strategy_settings() const noexcept {
    return strategy_settings_;
  }
  [[nodiscard]] const std::vector<VenueDefinition>& venues() const noexcept { return venues_; }
  [[nodiscard]] const std::vector<LogicalAccountVenueBinding>& logical_accounts() const noexcept {
    return logical_accounts_;
  }
  [[nodiscard]] const std::vector<model::InstrumentMetadata>& instrument_metadata() const noexcept {
    return instrument_metadata_;
  }
  [[nodiscard]] const market_data::SubscriptionConfiguration& subscriptions() const noexcept {
    return subscriptions_;
  }
  [[nodiscard]] const execution::ExecutionRouteConfiguration& routes() const noexcept {
    return routes_;
  }
  [[nodiscard]] const std::vector<std::byte>& canonical_bytes() const noexcept {
    return canonical_bytes_;
  }
  [[nodiscard]] const ConfigurationFingerprint& fingerprint() const noexcept {
    return fingerprint_;
  }
  [[nodiscard]] const ConfigurationProvenance& provenance() const noexcept { return provenance_; }

  [[nodiscard]] const VenueDefinition* find_venue(const model::VenueId& venue_id) const noexcept;
  [[nodiscard]] const LogicalAccountVenueBinding*
  find_logical_account(const model::LogicalAccountId& logical_account_id) const noexcept;
  [[nodiscard]] const BotStrategySettings*
  find_strategy_settings(const model::BotId& bot_id) const noexcept;
  [[nodiscard]] const model::InstrumentMetadata*
  find_instrument_metadata(const model::VenueId& venue_id,
                           const model::InstrumentId& instrument_id) const noexcept;

private:
  // Only create may assemble the sealed snapshot, keeping every derived identity synchronized.
  StartupConfiguration(model::ConfigurationRevision revision,
                       organization::Organization organization,
                       model::StrategyConfigurationRevision strategy_configuration_revision,
                       std::vector<BotStrategySettings> strategy_settings,
                       std::vector<VenueDefinition> venues,
                       std::vector<LogicalAccountVenueBinding> logical_accounts,
                       std::vector<model::InstrumentMetadata> instrument_metadata,
                       market_data::SubscriptionConfiguration subscriptions,
                       execution::ExecutionRouteConfiguration routes,
                       std::vector<std::byte> canonical_bytes, ConfigurationFingerprint fingerprint,
                       ConfigurationProvenance provenance)
      : revision_{revision}, organization_{std::move(organization)},
        strategy_configuration_revision_{strategy_configuration_revision},
        strategy_settings_{std::move(strategy_settings)}, venues_{std::move(venues)},
        logical_accounts_{std::move(logical_accounts)},
        instrument_metadata_{std::move(instrument_metadata)},
        subscriptions_{std::move(subscriptions)}, routes_{std::move(routes)},
        canonical_bytes_{std::move(canonical_bytes)}, fingerprint_{std::move(fingerprint)},
        provenance_{std::move(provenance)} {}

  model::ConfigurationRevision revision_;
  organization::Organization organization_;
  model::StrategyConfigurationRevision strategy_configuration_revision_;
  std::vector<BotStrategySettings> strategy_settings_;
  std::vector<VenueDefinition> venues_;
  std::vector<LogicalAccountVenueBinding> logical_accounts_;
  std::vector<model::InstrumentMetadata> instrument_metadata_;
  market_data::SubscriptionConfiguration subscriptions_;
  execution::ExecutionRouteConfiguration routes_;
  std::vector<std::byte> canonical_bytes_;
  ConfigurationFingerprint fingerprint_;
  ConfigurationProvenance provenance_;
};

} // namespace aegis::configuration
