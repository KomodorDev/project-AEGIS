// Purpose: represent immutable market-data grants independently from execution permission.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/time.hpp"
#include "aegis/organization/organization.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace aegis::market_data {

// A subscription is an explicit bot/venue/instrument/channel observation grant. Its assigned
// channel vocabulary and dependency key cannot confer execution authority.
enum class SubscriptionChannel : std::uint8_t { OrderBook = 1 };

using VenueInstrumentPair = std::pair<model::VenueId, model::InstrumentId>;

struct Subscription {
  model::SubscriptionId id;
  model::BotId bot_id;
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  SubscriptionChannel channel;

  friend bool operator==(const Subscription&, const Subscription&) = default;
};

// The factory accepts an empty section, consumes the known instrument catalog for validation, and
// publishes ID- and semantic-key-unique grants in canonical subscription-ID order.
class SubscriptionConfiguration {
public:
  [[nodiscard]] static model::Result<SubscriptionConfiguration>
  create(model::SubscriptionRevision revision, std::vector<Subscription> subscriptions,
         const organization::Organization& organization,
         std::vector<VenueInstrumentPair> known_venue_instruments);

  [[nodiscard]] model::SubscriptionRevision revision() const noexcept { return revision_; }
  [[nodiscard]] const std::vector<Subscription>& subscriptions() const noexcept {
    return subscriptions_;
  }
  [[nodiscard]] const Subscription* find(const model::SubscriptionId& id) const noexcept;

  friend bool operator==(const SubscriptionConfiguration&,
                         const SubscriptionConfiguration&) = default;

private:
  SubscriptionConfiguration(model::SubscriptionRevision revision,
                            std::vector<Subscription> subscriptions)
      : revision_{revision}, subscriptions_{std::move(subscriptions)} {}

  model::SubscriptionRevision revision_;
  std::vector<Subscription> subscriptions_;
};

} // namespace aegis::market_data
