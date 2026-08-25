// Purpose: define the stable M4 account-correctness states and reasons without implementing
// safety transitions, submission gating, or recovery decisions.

#pragma once

#include <cstdint>

namespace aegis::risk {

// ########################################################################
// Account correctness later gates exposure-increasing submissions without importing M5 risk modes.
enum class AccountSafetyState : std::uint8_t {
  Synchronized = 1,
  ReconciliationRequired = 2,
  Quarantined = 3,
};

// ########################################################################

// ########################################################################
// Stable reasons let later owners retain the first correctness loss and each unique escalation.
enum class AccountSafetyReason : std::uint8_t {
  SubmissionUnknown = 1,
  TimeoutObserved = 2,
  DisconnectObserved = 3,
  IncompleteReconciliation = 4,
  RecoveryGap = 5,
  UnknownOrder = 6,
  UnknownTrade = 7,
  UnexplainedPosition = 8,
  PermissionMismatch = 9,
  MarginModeMismatch = 10,
  OutOfScopeInstrument = 11,
  CorrelationConflict = 12,
  EventIdentityConflict = 13,
  TradeIdentityConflict = 14,
  AuthoritativeContradiction = 15,
  CriticalAdmissionLoss = 16,
  EvidenceCapacityExhausted = 17,
  ArithmeticOrStateCapacityFailure = 18,
  ProvenanceMismatch = 19,
};

// ########################################################################

} // namespace aegis::risk
