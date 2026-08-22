// Purpose: validate authorized M3 order economics in one stable vocabulary/price/quantity/unit
// order and publish exact values without quantization or reinterpretation.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/instrument_metadata.hpp"

#include <optional>

namespace aegis::execution {

// ########################################################################
// Canonical validation either owns the exact approved economics or the first assigned reason.
struct CanonicalValidationDecision {
  std::optional<CanonicalOrderEconomics> economics;
  SubmissionReason reason;

  // --------------------------------------------------------
  [[nodiscard]] bool accepted() const noexcept { return economics.has_value(); }

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Validate without mutation, allocation, rounding, rescaling, or consulting risk state.
[[nodiscard]] CanonicalValidationDecision
validate_canonical_order(const OrderRequest& request,
                         const model::InstrumentMetadata& metadata) noexcept;

// --------------------------------------------------------

} // namespace aegis::execution
