// Purpose: validate and canonically order explicit execution-route grants.

#include "aegis/execution/execution_route.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::execution {
namespace {

using model::DomainError;
using model::DomainErrorCode;

template <typename Value>
[[nodiscard]] model::Result<void> sort_and_reject_duplicates(std::vector<Value>& values,
                                                             std::string_view field) {
  std::sort(values.begin(), values.end());
  for (std::size_t index = 1U; index < values.size(); ++index) {
    if (values[index - 1U] == values[index]) {
      return model::Result<void>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, std::string{field}, index));
    }
  }
  return model::Result<void>::success();
}

template <typename Value>
[[nodiscard]] bool contains(const std::vector<Value>& values, const Value& value) noexcept {
  return std::binary_search(values.begin(), values.end(), value);
}

[[nodiscard]] bool contains_venue(const std::vector<VenueInstrumentPair>& pairs,
                                  const model::VenueId& venue_id) noexcept {
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const VenueInstrumentPair& pair) { return pair.first == venue_id; });
}

[[nodiscard]] bool contains_instrument(const std::vector<VenueInstrumentPair>& pairs,
                                       const model::InstrumentId& instrument_id) noexcept {
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const VenueInstrumentPair& pair) { return pair.second == instrument_id; });
}

[[nodiscard]] bool contains_account(const std::vector<LogicalAccountVenueBinding>& bindings,
                                    const model::LogicalAccountId& logical_account_id) noexcept {
  return std::any_of(bindings.begin(), bindings.end(),
                     [&](const auto& binding) { return binding.first == logical_account_id; });
}

} // namespace

model::Result<ExecutionRouteConfiguration> ExecutionRouteConfiguration::create(
    model::RouteRevision revision, std::vector<ExecutionRoute> routes,
    const organization::Organization& organization,
    std::vector<VenueInstrumentPair> known_venue_instruments,
    std::vector<LogicalAccountVenueBinding> known_account_bindings) {
  const auto venue_instrument_duplicates =
      sort_and_reject_duplicates(known_venue_instruments, "routes.known_venue_instruments");
  if (!venue_instrument_duplicates) {
    return model::Result<ExecutionRouteConfiguration>::failure(venue_instrument_duplicates.error());
  }
  const auto account_binding_duplicates =
      sort_and_reject_duplicates(known_account_bindings, "routes.known_account_bindings");
  if (!account_binding_duplicates) {
    return model::Result<ExecutionRouteConfiguration>::failure(account_binding_duplicates.error());
  }
  std::sort(routes.begin(), routes.end(),
            [](const ExecutionRoute& lhs, const ExecutionRoute& rhs) { return lhs.id < rhs.id; });

  for (std::size_t index = 1U; index < routes.size(); ++index) {
    if (routes[index - 1U].id == routes[index].id) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, "routes.id", index));
    }
  }

  using SemanticKey =
      std::tuple<model::BotId, model::VenueId, model::LogicalAccountId, model::InstrumentId>;
  std::set<SemanticKey> semantic_keys;
  for (std::size_t index = 0U; index < routes.size(); ++index) {
    const ExecutionRoute& route = routes[index];
    if (organization.find_bot(route.bot_id) == nullptr) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.bot_id", index));
    }
    if (!contains_venue(known_venue_instruments, route.venue_id)) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.venue_id", index));
    }
    if (!contains_account(known_account_bindings, route.logical_account_id)) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "routes.logical_account_id", index));
    }
    if (!contains_instrument(known_venue_instruments, route.instrument_id)) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.instrument_id", index));
    }
    if (!contains(known_venue_instruments,
                  VenueInstrumentPair{route.venue_id, route.instrument_id})) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "routes.venue_instrument", index));
    }
    if (!contains(known_account_bindings,
                  LogicalAccountVenueBinding{route.logical_account_id, route.venue_id})) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "routes.account_venue", index));
    }
    if (route.state != ExecutionRouteState::Disabled &&
        route.state != ExecutionRouteState::Enabled) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::InvalidValue, "routes.state", index));
    }

    if (!semantic_keys
             .emplace(route.bot_id, route.venue_id, route.logical_account_id, route.instrument_id)
             .second) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "routes.semantic_key", index));
    }
  }

  return model::Result<ExecutionRouteConfiguration>::success(
      ExecutionRouteConfiguration{revision, std::move(routes)});
}

const ExecutionRoute* ExecutionRouteConfiguration::find(const model::RouteId& id) const noexcept {
  const auto found = std::lower_bound(
      routes_.begin(), routes_.end(), id,
      [](const ExecutionRoute& route, const model::RouteId& target) { return route.id < target; });
  if (found == routes_.end() || found->id != id) {
    return nullptr;
  }
  return &*found;
}

} // namespace aegis::execution
