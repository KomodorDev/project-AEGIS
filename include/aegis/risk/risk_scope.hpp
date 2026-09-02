// Purpose: assign the closed M3 risk-scope and limit vocabularies shared by policy, decisions,
// submission results, OMS evidence, and canonical traces.

#pragma once

#include <cstdint>

namespace aegis::risk {

// ########################################################################
// Scope order is decision order: the first exceeded limit in this sequence is canonical.
enum class RiskScopeKind : std::uint8_t {
  Bot = 1,
  Desk = 2,
  Firm = 3,
  Account = 4,
  Route = 5,
  Instrument = 6,
  Venue = 7,
};

// ########################################################################
// Limit order is stable inside each scope and matches the AEGISRSP schema-one encoding.
enum class RiskLimitKind : std::uint8_t {
  SingleOrderQuantity = 1,
  SingleOrderQuoteNotional = 2,
  OpenOrderCount = 3,
  GrossReservedQuoteNotional = 4,
  WorstCasePositionQuantity = 5,
  WorstCasePositionQuoteNotional = 6,
};

// ########################################################################

} // namespace aegis::risk
