// Purpose: calculate partition-independent positive reservation-to-inventory allocations from
// cumulative fills without granting order, deduplication, or ledger mutation authority.

#pragma once

#include "aegis/risk/exposure.hpp"

#include <cstdint>

namespace aegis::risk {

// ########################################################################
// One checked cumulative projection conserves the original approved quantity and notional.
// Values remain nonnegative; the inventory owner applies Buy/Sell signs after validation.
struct CumulativeReservationConversion {
  OrderExposure remaining;
  OrderExposure cumulative_confirmed;
  OrderExposure newly_confirmed;

  // --------------------------------------------------------
  // Compare the complete residual, cumulative allocation, and incremental transfer.
  friend bool operator==(const CumulativeReservationConversion&,
                         const CumulativeReservationConversion&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Validate original once-rounded exposure and the previous cumulative allocation, then calculate
// the residual and allocation at the supplied cumulative quantity. Equal endpoints yield a zero
// transfer; the OMS owner separately decides whether a source execution is new and contiguous.
// Reject decreasing/overfilled quantities, incompatible metadata, and inconsistent allocations;
// preserve exact arithmetic/alignment error codes. A fill may be below the order-entry minimum.
[[nodiscard]] model::Result<CumulativeReservationConversion>
calculate_cumulative_reservation_conversion(const OrderExposure& original,
                                            const OrderExposure& previous_cumulative_confirmed,
                                            model::Quantity next_cumulative_quantity,
                                            const model::InstrumentMetadata& metadata,
                                            std::uint64_t notional_scale);

// --------------------------------------------------------

} // namespace aegis::risk
