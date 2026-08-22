// Purpose: define the exact M3 order exposure approved by fixed risk and calculate inverse-contract
// quote face notional without binary floating point or caller-selected rounding.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/instrument_metadata.hpp"
#include "aegis/model/result.hpp"

#include <cstdint>

namespace aegis::risk {

// ########################################################################
// OrderExposure carries the original validated quantity and the once-rounded positive quote face
// notional reused unchanged by every applicable risk scope.
struct OrderExposure {
  model::Quantity quantity;
  model::Notional quote_notional;

  // --------------------------------------------------------
  // Return the exact canonical order quantity approved by risk.
  [[nodiscard]] model::Quantity order_quantity() const noexcept { return quantity; }

  // --------------------------------------------------------
  // Return the conservative quote-currency face notional rounded once for the whole decision.
  [[nodiscard]] model::Notional quote_face_notional() const noexcept { return quote_notional; }

  // --------------------------------------------------------
  // Structural equality proves repeated scopes and downstream OMS evidence use identical economics.
  friend bool operator==(const OrderExposure&, const OrderExposure&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Calculate supported inverse quote-multiplier exposure at the policy scale with AwayFromZero.
[[nodiscard]] model::Result<OrderExposure>
calculate_order_exposure(const execution::CanonicalOrderEconomics& economics,
                         const model::InstrumentMetadata& metadata, std::uint8_t notional_scale);

// --------------------------------------------------------

} // namespace aegis::risk
