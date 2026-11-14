// Purpose: derive one detached before/after OMS projection for each accepted execution proposed
// by a validated contiguous fill or pending-fill drain, without changing live order state.

#pragma once

#include "private_oms_transition.hpp"

#include <cstdint>

namespace aegis::oms {

// ########################################################################
// These two projections describe one execution's proposed contribution to the full OMS update.
// They carry no reference to a live record and grant no authority to modify it.
struct PrivateExecutionProjection {
  PrivateOrderProjection before;
  PrivateOrderProjection after;
};

// ########################################################################

// --------------------------------------------------------
// Derive and check one primary audit frame for an already qualified execution. Index zero takes
// the incoming execution and transition.before. Each later index takes the corresponding drained
// input and the preceding frame's after projection. The last projection must equal
// transition.after. Inconsistent chains and transitions that release a cancellation residual fail
// without any writes.
[[nodiscard]] model::Result<PrivateExecutionProjection> derive_private_execution_projection(
    const PrivateOmsTransitionPlan& transition, const NormalizedPrivateOrderInput& execution,
    const PrivateOrderProjection& preceding_projection, std::uint32_t effect_index);

// --------------------------------------------------------

} // namespace aegis::oms
