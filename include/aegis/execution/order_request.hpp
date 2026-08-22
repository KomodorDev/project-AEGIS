// Purpose: define the small bounded-field M3 order request and its closed limit-only economics
// vocabulary without carrying caller-forgeable attribution, account, venue, or client identity.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"

#include <cstdint>

namespace aegis::execution {

// ########################################################################
// Both assigned sides are supported; every other underlying value fails canonical validation.
enum class OrderSide : std::uint8_t {
  Buy = 1,
  Sell = 2,
};

// ########################################################################

// ########################################################################
// M3 intentionally exposes only limit orders so risk never invents a market-order price collar.
enum class OrderType : std::uint8_t {
  Limit = 1,
};

// ########################################################################

// ########################################################################
// Good-til-cancelled is the sole deterministic M3 time-in-force value.
enum class TimeInForce : std::uint8_t {
  GoodTilCancelled = 1,
};

// ########################################################################

// ########################################################################
// A strategy supplies only route selection and exact economics; runtime-owned identity and
// attribution cannot be forged through this aggregate.
struct OrderRequest {
  model::RouteId route_id;
  model::InstrumentId instrument_id;
  OrderSide side;
  OrderType type;
  TimeInForce time_in_force;
  model::Price price;
  model::Quantity quantity;

  // --------------------------------------------------------
  // Structural equality supports deterministic request and benchmark-fixture proofs.
  friend bool operator==(const OrderRequest&, const OrderRequest&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Route and attribution are removed after authorization; risk and encoding receive only the exact
// economics that canonical validation approved.
struct CanonicalOrderEconomics {
  OrderSide side;
  OrderType type;
  TimeInForce time_in_force;
  model::Price price;
  model::Quantity quantity;

  // --------------------------------------------------------
  // Structural equality proves the encoder retained the exact risk-approved values.
  friend bool operator==(const CanonicalOrderEconomics&, const CanonicalOrderEconomics&) = default;

  // --------------------------------------------------------
};

// ########################################################################

} // namespace aegis::execution
