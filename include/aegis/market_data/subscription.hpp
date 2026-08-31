// Purpose: represent immutable market-data grants independently from execution permission.

#pragma once

#include "aegis/model/identifier.hpp"
#include "aegis/model/time.hpp"
#include "aegis/organization/organization.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace aegis::market_data {

// ########################################################################
// A subscription is an explicit bot/venue/instrument/channel observation grant. Its assigned
// channel vocabulary and dependency key cannot confer execution authority.
enum class SubscriptionChannel : std::uint8_t { OrderBook = 1 };

// ########################################################################
// Venue/instrument pairs form the canonical dependency key used during subscription validation.
using VenueInstrumentPair = std::pair<model::VenueId, model::InstrumentId>;

// ########################################################################
// A subscription owns the complete semantic key for one observation grant.
struct Subscription {
  model::SubscriptionId id;
  model::BotId bot_id;
  model::VenueId venue_id;
  model::InstrumentId instrument_id;
  SubscriptionChannel channel;

  // --------------------------------------------------------
  // Structural equality compares the complete observation grant.
  friend bool operator==(const Subscription&, const Subscription&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// The factory accepts an empty section, consumes the known instrument catalog for validation, and
// publishes ID- and semantic-key-unique grants in canonical subscription-ID order.
class SubscriptionConfiguration {
public:

  // --------------------------------------------------------
  // Validate dependencies and uniqueness before publishing canonical subscription-ID order.
  [[nodiscard]] static model::Result<SubscriptionConfiguration>
  create_subscription_configuration(model::SubscriptionRevision revision,
                                    std::vector<Subscription> subscriptions,
                                    const organization::Organization& organization,
                                    std::vector<VenueInstrumentPair> known_venue_instruments);

  // --------------------------------------------------------
  // Return the accepted subscription-section revision.
  [[nodiscard]] model::SubscriptionRevision revision() const noexcept { return revision_; }

  // --------------------------------------------------------
  // Borrow the canonical immutable subscription catalog.
  [[nodiscard]] const std::vector<Subscription>& subscriptions() const noexcept {
    return subscriptions_;
  }

  // --------------------------------------------------------
  // Find one configured observation grant by its nominal subscription identity.
  [[nodiscard]] const Subscription*
  find_subscription(const model::SubscriptionId& id) const noexcept;

  // --------------------------------------------------------
  // Structural equality compares the complete published subscription configuration.
  friend bool operator==(const SubscriptionConfiguration&,
                         const SubscriptionConfiguration&) = default;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Assemble only subscriptions that the public factory has validated and canonicalized.
  SubscriptionConfiguration(model::SubscriptionRevision revision,
                            std::vector<Subscription> subscriptions)
      : revision_{revision}, subscriptions_{std::move(subscriptions)} {}

  // --------------------------------------------------------
  model::SubscriptionRevision revision_;
  std::vector<Subscription> subscriptions_;
};

// ########################################################################
} // namespace aegis::market_data
