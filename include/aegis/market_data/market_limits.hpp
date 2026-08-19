// Purpose: define the shared compile-time storage ceilings for recorded and normalized M2 market
// data without coupling parser, policy, event, or book implementations.

#pragma once

#include <cstddef>

namespace aegis::market_data {

// ########################################################################
// AEGISMD schema one, normalized updates, and fixed-capacity books share these immutable ceilings.
// Changing one is a schema decision rather than a per-runtime policy update.
inline constexpr std::size_t maximum_recorded_frame_bytes = 4096U;
inline constexpr std::size_t maximum_changes_per_market_update = 64U;
inline constexpr std::size_t maximum_retained_book_depth = 64U;
inline constexpr std::size_t maximum_integrity_token_bytes = 64U;

// ########################################################################

} // namespace aegis::market_data
