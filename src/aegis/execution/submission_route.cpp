// Purpose: validate and install immutable M3 route projections, then authorize explicit RouteIds
// in the stable missing/ownership/enabled/instrument decision order.

#include "aegis/execution/submission_route.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace aegis::execution {
namespace {

// --------------------------------------------------------
// Build one stable construction failure without exposing authored values as free-form text.
[[nodiscard]] model::Result<OwnerLocalRouteCatalog> invalid_catalog(std::size_t index) {
  return model::Result<OwnerLocalRouteCatalog>::failure(model::DomainError::at_index(
      model::DomainErrorCode::InvalidRelationship, "submission_routes", index));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate every narrow projection before assigning canonical one-based route positions.
model::Result<OwnerLocalRouteCatalog>
OwnerLocalRouteCatalog::create(configuration::ConfigurationFingerprint configuration_fingerprint,
                               model::ConfigurationRevision configuration_revision,
                               model::OrganizationRevision organization_revision,
                               model::RouteRevision route_revision,
                               std::vector<SubmissionRouteInput> inputs) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical sorting makes input collection order irrelevant to route ordinals and evidence.
  std::sort(inputs.begin(), inputs.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.route.id < rhs.route.id; });

  // ++++++++++++++++++++++++++++++++++++++++
  // Reserve exact output size before validating so no allocation appears after publication.
  std::vector<InstalledSubmissionRoute> installed;
  installed.reserve(inputs.size());
  auto ordinal = model::RouteOrdinal::initial();
  for (std::size_t index = 0U; index < inputs.size(); ++index) {
    const auto& input = inputs[index];
    if ((index != 0U && inputs[index - 1U].route.id == input.route.id) ||
        input.route.bot_id != input.attribution.bot_id ||
        input.route.venue_id != input.metadata.venue_id() ||
        input.route.instrument_id != input.metadata.instrument_id()) {
      return invalid_catalog(index);
    }
    installed.push_back(InstalledSubmissionRoute{ordinal, input, configuration_fingerprint,
                                                 configuration_revision, organization_revision,
                                                 route_revision});
    if (index + 1U < inputs.size()) {
      auto next = ordinal.next();
      if (!next) {
        return model::Result<OwnerLocalRouteCatalog>::failure(next.error());
      }
      ordinal = next.value();
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish only the complete canonical catalog, including disabled routes.
  return model::Result<OwnerLocalRouteCatalog>::success(
      OwnerLocalRouteCatalog{std::move(installed)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolve one RouteId with deterministic binary search over immutable canonical storage.
const InstalledSubmissionRoute*
OwnerLocalRouteCatalog::find(const model::RouteId& route_id) const noexcept {
  const auto found =
      std::lower_bound(routes_.begin(), routes_.end(), route_id,
                       [](const InstalledSubmissionRoute& route, const model::RouteId& candidate) {
                         return route.route().id < candidate;
                       });
  if (found == routes_.end() || found->route().id != route_id) {
    return nullptr;
  }
  return &*found;
}

// --------------------------------------------------------
// Apply the closed authorization precedence without consulting market-data grants.
RouteAuthorizationDecision
OwnerLocalRouteCatalog::authorize(const organization::BotAttribution& active_attribution,
                                  const OrderRequest& request) const noexcept {
  const auto* const installed = find(request.route_id);
  if (installed == nullptr) {
    return RouteAuthorizationDecision{nullptr, SubmissionReason::RouteNotFound};
  }
  if (installed->attribution() != active_attribution) {
    return RouteAuthorizationDecision{nullptr, SubmissionReason::RouteNotOwned};
  }
  if (!installed->route().is_enabled()) {
    return RouteAuthorizationDecision{nullptr, SubmissionReason::RouteDisabled};
  }
  if (installed->route().instrument_id != request.instrument_id) {
    return RouteAuthorizationDecision{nullptr, SubmissionReason::RouteInstrumentMismatch};
  }
  return RouteAuthorizationDecision{installed, SubmissionReason::None};
}

// --------------------------------------------------------

} // namespace aegis::execution
