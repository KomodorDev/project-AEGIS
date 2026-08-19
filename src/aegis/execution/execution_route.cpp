// Purpose: validate, firm-scope, and canonically order explicit execution-route grants.

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

// --------------------------------------------------------
// Canonicalize caller-supplied dependency catalogs before binary search so duplicate permissions
// cannot make membership ambiguous and their reported indices are input-order independent.
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
// --------------------------------------------------------
// Test membership in a dependency catalog that its caller has already canonicalized.
template <typename Value>
[[nodiscard]] bool contains(const std::vector<Value>& values, const Value& value) noexcept {
  return std::binary_search(values.begin(), values.end(), value);
}
// --------------------------------------------------------
// Staged existence checks distinguish an unknown venue or instrument from a known but forbidden
// venue/instrument relationship, preserving precise deterministic errors.
[[nodiscard]] bool contains_venue(const std::vector<VenueInstrumentPair>& pairs,
                                  const model::VenueId& venue_id) noexcept {
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const VenueInstrumentPair& pair) { return pair.first == venue_id; });
}
// --------------------------------------------------------
// Check instrument existence independently of its venue pairing for precise failure reporting.
[[nodiscard]] bool contains_instrument(const std::vector<VenueInstrumentPair>& pairs,
                                       const model::InstrumentId& instrument_id) noexcept {
  return std::any_of(pairs.begin(), pairs.end(),
                     [&](const VenueInstrumentPair& pair) { return pair.second == instrument_id; });
}
// --------------------------------------------------------
// LogicalAccountId is authoritative: conflicting owner or venue records are duplicates too. Sorting
// the full tuple first gives every such conflict a deterministic collection index.
[[nodiscard]] model::Result<void>
sort_and_reject_duplicate_account_ids(std::vector<LogicalAccountVenueBinding>& bindings) {
  std::sort(bindings.begin(), bindings.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.logical_account_id, lhs.firm_id, lhs.venue_id) <
           std::tie(rhs.logical_account_id, rhs.firm_id, rhs.venue_id);
  });
  for (std::size_t index = 1U; index < bindings.size(); ++index) {
    if (bindings[index - 1U].logical_account_id == bindings[index].logical_account_id) {
      return model::Result<void>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "routes.known_account_bindings", index));
    }
  }
  return model::Result<void>::success();
}
// --------------------------------------------------------
// The preceding unique-ID check makes this canonical lookup resolve exactly one owner and venue.
[[nodiscard]] const LogicalAccountVenueBinding*
find_account(const std::vector<LogicalAccountVenueBinding>& bindings,
             const model::LogicalAccountId& logical_account_id) noexcept {
  const auto found = std::lower_bound(
      bindings.begin(), bindings.end(), logical_account_id,
      [](const LogicalAccountVenueBinding& binding, const model::LogicalAccountId& target) {
        return binding.logical_account_id < target;
      });
  return found != bindings.end() && found->logical_account_id == logical_account_id ? &*found
                                                                                    : nullptr;
}
// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate all route dependencies and publish an immutable, firm-scoped canonical grant set.
model::Result<ExecutionRouteConfiguration> ExecutionRouteConfiguration::create(
    model::RouteRevision revision, std::vector<ExecutionRoute> routes,
    const organization::Organization& organization,
    std::vector<VenueInstrumentPair> known_venue_instruments,
    std::vector<LogicalAccountVenueBinding> known_account_bindings) {
  // ++++++++++++++++++++++++++++++++++++++++
  // Catalog defects take precedence because every route validation depends on an unambiguous
  // source.
  const auto venue_instrument_duplicates =
      sort_and_reject_duplicates(known_venue_instruments, "routes.known_venue_instruments");
  if (!venue_instrument_duplicates) {
    return model::Result<ExecutionRouteConfiguration>::failure(venue_instrument_duplicates.error());
  }
  const auto account_binding_duplicates =
      sort_and_reject_duplicate_account_ids(known_account_bindings);
  if (!account_binding_duplicates) {
    return model::Result<ExecutionRouteConfiguration>::failure(account_binding_duplicates.error());
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical route order makes duplicate positions and the published snapshot input-order neutral.
  std::sort(routes.begin(), routes.end(),
            [](const ExecutionRoute& lhs, const ExecutionRoute& rhs) { return lhs.id < rhs.id; });

  for (std::size_t index = 1U; index < routes.size(); ++index) {
    if (routes[index - 1U].id == routes[index].id) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, "routes.id", index));
    }
  }
  // ++++++++++++++++++++++++++++++++++++++++
  // Validate every dependency and assigned state before rejecting duplicate authorization meaning.
  using SemanticKey =
      std::tuple<model::BotId, model::VenueId, model::LogicalAccountId, model::InstrumentId>;
  std::set<SemanticKey> semantic_keys;
  for (std::size_t index = 0U; index < routes.size(); ++index) {
    const ExecutionRoute& route = routes[index];
    const auto* const bot = organization.find_bot(route.bot_id);
    if (bot == nullptr) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.bot_id", index));
    }
    if (!contains_venue(known_venue_instruments, route.venue_id)) {
      return model::Result<ExecutionRouteConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "routes.venue_id", index));
    }
    const auto* const account = find_account(known_account_bindings, route.logical_account_id);
    if (account == nullptr) {
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
    if (account->venue_id != route.venue_id) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "routes.account_venue", index));
    }
    // Venue compatibility is insufficient: the bot and logical account must share one peer firm.
    if (account->firm_id != bot->firm_id) {
      return model::Result<ExecutionRouteConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "routes.account_firm", index));
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
  // ++++++++++++++++++++++++++++++++++++++++
}
// --------------------------------------------------------
// Factory canonicalization makes lower_bound sufficient without a mutable secondary index.
const ExecutionRoute* ExecutionRouteConfiguration::find(const model::RouteId& id) const noexcept {
  const auto found = std::lower_bound(
      routes_.begin(), routes_.end(), id,
      [](const ExecutionRoute& route, const model::RouteId& target) { return route.id < target; });
  if (found == routes_.end() || found->id != id) {
    return nullptr;
  }
  return &*found;
}
// --------------------------------------------------------

} // namespace aegis::execution
