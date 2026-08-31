// Purpose: install sealed execution routes and exact instrument metadata into one canonical
// owner-local authorization catalog without depending on StartupConfiguration or subscriptions.

#pragma once

#include "aegis/configuration/configuration_provenance.hpp"
#include "aegis/execution/execution_route.hpp"
#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/instrument_metadata.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/organization/organization.hpp"

#include <utility>
#include <vector>

namespace aegis::execution {

// ########################################################################
// Runtime composition supplies this narrow already-validated projection instead of creating a
// reverse dependency from execution to StartupConfiguration.
struct SubmissionRouteInput {
  ExecutionRoute route;
  organization::BotAttribution attribution;
  model::InstrumentMetadata metadata;
};

// ########################################################################

// ########################################################################
// One installed route owns every immutable value needed by authorization, risk, OMS, encoding,
// and evidence after startup configuration is no longer consulted on the direct path.
class InstalledSubmissionRoute final {
public:

  // --------------------------------------------------------
  // Return the stable one-based ordinal assigned by canonical catalog ordering.
  [[nodiscard]] model::RouteOrdinal ordinal() const noexcept { return ordinal_; }

  // --------------------------------------------------------
  // Borrow the immutable execution route used for authorization and encoding.
  [[nodiscard]] const ExecutionRoute& route() const noexcept { return route_; }

  // --------------------------------------------------------
  // Borrow the exact bot ownership attribution sealed with the route.
  [[nodiscard]] const organization::BotAttribution& attribution() const noexcept {
    return attribution_;
  }

  // --------------------------------------------------------
  // Borrow the exact instrument economics sealed with the route.
  [[nodiscard]] const model::InstrumentMetadata& metadata() const noexcept { return metadata_; }

  // --------------------------------------------------------
  // Borrow the configuration digest under which this route projection was installed.
  [[nodiscard]] const configuration::ConfigurationFingerprint&
  configuration_fingerprint() const noexcept {
    return configuration_fingerprint_;
  }

  // --------------------------------------------------------
  // Return the configuration revision under which this route projection was installed.
  [[nodiscard]] model::ConfigurationRevision configuration_revision() const noexcept {
    return configuration_revision_;
  }

  // --------------------------------------------------------
  // Return the organization revision that authorized the retained attribution.
  [[nodiscard]] model::OrganizationRevision organization_revision() const noexcept {
    return organization_revision_;
  }

  // --------------------------------------------------------
  // Return the execution-route revision from which the catalog was built.
  [[nodiscard]] model::RouteRevision route_revision() const noexcept { return route_revision_; }

  // --------------------------------------------------------
private:

  // ########################################################################
  // Only canonical catalog construction may assign the stable route ordinal.
  friend class OwnerLocalRouteCatalog;

  // ########################################################################

  // --------------------------------------------------------
  // Capture one fully validated route projection at its canonical sorted position.
  InstalledSubmissionRoute(model::RouteOrdinal ordinal, SubmissionRouteInput input,
                           configuration::ConfigurationFingerprint configuration_fingerprint,
                           model::ConfigurationRevision configuration_revision,
                           model::OrganizationRevision organization_revision,
                           model::RouteRevision route_revision)
      : ordinal_{ordinal}, route_{std::move(input.route)},
        attribution_{std::move(input.attribution)}, metadata_{std::move(input.metadata)},
        configuration_fingerprint_{std::move(configuration_fingerprint)},
        configuration_revision_{configuration_revision},
        organization_revision_{organization_revision}, route_revision_{route_revision} {}

  // --------------------------------------------------------
  model::RouteOrdinal ordinal_;
  ExecutionRoute route_;
  organization::BotAttribution attribution_;
  model::InstrumentMetadata metadata_;
  configuration::ConfigurationFingerprint configuration_fingerprint_;
  model::ConfigurationRevision configuration_revision_;
  model::OrganizationRevision organization_revision_;
  model::RouteRevision route_revision_;
};

// ########################################################################

// ########################################################################
// Authorization returns either one borrowed immutable installed route or one stable first reason.
struct RouteAuthorizationDecision {
  const InstalledSubmissionRoute* installed_route;
  SubmissionReason reason;

  // --------------------------------------------------------
  // Report whether authorization selected an installed route rather than a rejection reason.
  [[nodiscard]] bool is_authorized() const noexcept { return installed_route != nullptr; }

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The catalog installs once, remains canonically RouteId-sorted, and performs no subscription or
// dynamic configuration lookup on submission.
class OwnerLocalRouteCatalog final {
public:

  // --------------------------------------------------------
  // Validate sealed route/attribution/metadata agreement and assign stable sorted ordinals.
  [[nodiscard]] static model::Result<OwnerLocalRouteCatalog> create_owner_local_route_catalog(
      configuration::ConfigurationFingerprint configuration_fingerprint,
      model::ConfigurationRevision configuration_revision,
      model::OrganizationRevision organization_revision, model::RouteRevision route_revision,
      std::vector<SubmissionRouteInput> inputs);

  // --------------------------------------------------------
  // Apply missing, ownership, enabled-state, then instrument precedence without mutation.
  [[nodiscard]] RouteAuthorizationDecision
  evaluate_route_authorization(const organization::BotAttribution& active_attribution,
                               const OrderRequest& request) const noexcept;

  // --------------------------------------------------------
  // Borrow every installed route in stable canonical order.
  [[nodiscard]] const std::vector<InstalledSubmissionRoute>& routes() const noexcept {
    return routes_;
  }

  // --------------------------------------------------------
  // Find an installed route by ID, or return null when the immutable catalog has no match.
  [[nodiscard]] const InstalledSubmissionRoute*
  find_route(const model::RouteId& route_id) const noexcept;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish only a completely validated immutable catalog.
  explicit OwnerLocalRouteCatalog(std::vector<InstalledSubmissionRoute> routes)
      : routes_{std::move(routes)} {}

  // --------------------------------------------------------
  std::vector<InstalledSubmissionRoute> routes_;
};

// ########################################################################

} // namespace aegis::execution
