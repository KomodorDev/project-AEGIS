// Purpose: represent explicit execution grants without selecting or transmitting orders.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/time.hpp"
#include "aegis/organization/organization.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace aegis::execution {

enum class ExecutionRouteState : std::uint8_t { Disabled = 0, Enabled = 1 };

using VenueInstrumentPair = std::pair<model::VenueId, model::InstrumentId>;
using LogicalAccountVenueBinding = std::pair<model::LogicalAccountId, model::VenueId>;

struct ExecutionRoute {
  model::RouteId id;
  model::BotId bot_id;
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  model::InstrumentId instrument_id;
  ExecutionRouteState state;

  [[nodiscard]] bool is_enabled() const noexcept { return state == ExecutionRouteState::Enabled; }

  friend bool operator==(const ExecutionRoute&, const ExecutionRoute&) = default;
};

class ExecutionRouteConfiguration {
public:
  [[nodiscard]] static model::Result<ExecutionRouteConfiguration>
  create(model::RouteRevision revision, std::vector<ExecutionRoute> routes,
         const organization::Organization& organization,
         std::vector<VenueInstrumentPair> known_venue_instruments,
         std::vector<LogicalAccountVenueBinding> known_account_bindings);

  [[nodiscard]] model::RouteRevision revision() const noexcept { return revision_; }
  [[nodiscard]] const std::vector<ExecutionRoute>& routes() const noexcept { return routes_; }
  [[nodiscard]] const ExecutionRoute* find(const model::RouteId& id) const noexcept;

  friend bool operator==(const ExecutionRouteConfiguration&,
                         const ExecutionRouteConfiguration&) = default;

private:
  ExecutionRouteConfiguration(model::RouteRevision revision, std::vector<ExecutionRoute> routes)
      : revision_{revision}, routes_{std::move(routes)} {}

  model::RouteRevision revision_;
  std::vector<ExecutionRoute> routes_;
};

} // namespace aegis::execution
