// Purpose: validate and canonically order market-data subscriptions.

#include "aegis/market_data/subscription.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace aegis::market_data {
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

} // namespace

model::Result<SubscriptionConfiguration>
SubscriptionConfiguration::create(model::SubscriptionRevision revision,
                                  std::vector<Subscription> subscriptions,
                                  const organization::Organization& organization,
                                  std::vector<VenueInstrumentPair> known_venue_instruments) {
  const auto dependency_duplicates =
      sort_and_reject_duplicates(known_venue_instruments, "subscriptions.known_venue_instruments");
  if (!dependency_duplicates) {
    return model::Result<SubscriptionConfiguration>::failure(dependency_duplicates.error());
  }
  std::sort(subscriptions.begin(), subscriptions.end(),
            [](const Subscription& lhs, const Subscription& rhs) { return lhs.id < rhs.id; });

  for (std::size_t index = 1U; index < subscriptions.size(); ++index) {
    if (subscriptions[index - 1U].id == subscriptions[index].id) {
      return model::Result<SubscriptionConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DuplicateIdentifier, "subscriptions.id", index));
    }
  }

  using SemanticKey =
      std::tuple<model::BotId, model::VenueId, model::InstrumentId, SubscriptionChannel>;
  std::set<SemanticKey> semantic_keys;
  for (std::size_t index = 0U; index < subscriptions.size(); ++index) {
    const Subscription& subscription = subscriptions[index];
    if (organization.find_bot(subscription.bot_id) == nullptr) {
      return model::Result<SubscriptionConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::DanglingReference, "subscriptions.bot_id", index));
    }
    if (!contains_venue(known_venue_instruments, subscription.venue_id)) {
      return model::Result<SubscriptionConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "subscriptions.venue_id", index));
    }
    if (!contains_instrument(known_venue_instruments, subscription.instrument_id)) {
      return model::Result<SubscriptionConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::DanglingReference, "subscriptions.instrument_id", index));
    }
    if (!contains(known_venue_instruments,
                  VenueInstrumentPair{subscription.venue_id, subscription.instrument_id})) {
      return model::Result<SubscriptionConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::InvalidRelationship, "subscriptions.venue_instrument", index));
    }
    if (subscription.channel != SubscriptionChannel::OrderBook) {
      return model::Result<SubscriptionConfiguration>::failure(
          DomainError::at_index(DomainErrorCode::InvalidValue, "subscriptions.channel", index));
    }

    if (!semantic_keys
             .emplace(subscription.bot_id, subscription.venue_id, subscription.instrument_id,
                      subscription.channel)
             .second) {
      return model::Result<SubscriptionConfiguration>::failure(DomainError::at_index(
          DomainErrorCode::DuplicateIdentifier, "subscriptions.semantic_key", index));
    }
  }

  return model::Result<SubscriptionConfiguration>::success(
      SubscriptionConfiguration{revision, std::move(subscriptions)});
}

const Subscription*
SubscriptionConfiguration::find(const model::SubscriptionId& id) const noexcept {
  const auto found =
      std::lower_bound(subscriptions_.begin(), subscriptions_.end(), id,
                       [](const Subscription& subscription, const model::SubscriptionId& target) {
                         return subscription.id < target;
                       });
  if (found == subscriptions_.end() || found->id != id) {
    return nullptr;
  }
  return &*found;
}

} // namespace aegis::market_data
