// Purpose: own the fixed-capacity M3 risk cells and held reservations, applying one atomic
// seven-scope check-and-reserve decision and exact-once release on the serialized owner.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submission_route.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/fixed_point.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/risk/exposure.hpp"
#include "aegis/risk/risk_policy.hpp"
#include "aegis/risk/risk_scope.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::risk {

// ########################################################################
// Reservation state is intentionally minimal until private events add lifecycle transitions in M4.
enum class ReservationState : std::uint8_t {
  Held = 1,
  Released = 2,
};

// ########################################################################

// ########################################################################
// ReservationEvidence retains the exact creating identity, side, and once-calculated economics.
struct ReservationEvidence {
  model::ReservationId reservation_id;
  ReservationState state;
  execution::OrderSide side;
  OrderExposure exposure;

  // --------------------------------------------------------
  friend bool operator==(const ReservationEvidence&, const ReservationEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// RiskScopeExposure is a read-only coherent view of the five mutable cells used by one complete
// scope/instrument/currency projection.
struct RiskScopeExposure {
  std::uint64_t open_order_count;
  model::Notional gross_reserved_quote_notional;
  model::Quantity reserved_buy_quantity;
  model::Quantity reserved_sell_quantity;
  model::Quantity worst_case_position_quantity;
  model::Notional reserved_buy_quote_notional;
  model::Notional reserved_sell_quote_notional;
  model::Notional instrument_worst_case_quote_notional;
  model::Notional worst_case_position_quote_notional;

  // --------------------------------------------------------
  friend bool operator==(const RiskScopeExposure&, const RiskScopeExposure&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// Canonical scope evidence owns the complete risk-policy key and its coherent mutable cell view so
// quiescent runtime inspection never depends on a live route or caller-authored subject spelling.
struct RiskScopeExposureEvidence {
  model::FirmId firm_id;
  RiskScopeKind scope;
  std::string scope_subject;
  model::InstrumentId instrument_id;
  std::string quote_currency;
  RiskScopeExposure exposure;

  // --------------------------------------------------------
  friend bool operator==(const RiskScopeExposureEvidence&,
                         const RiskScopeExposureEvidence&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// RiskCheckResult is either a committed held reservation or the first stable ordinary risk reason;
// policy and impossible state errors remain separate DomainError construction/invariant failures.
class RiskCheckResult final {
public:

  // --------------------------------------------------------
  // Construct an ordinary arithmetic or capacity rejection that has no limit evidence.
  [[nodiscard]] static RiskCheckResult rejected(execution::SubmissionReason reason) noexcept {
    return RiskCheckResult{reason, std::nullopt, std::nullopt, std::nullopt};
  }

  // --------------------------------------------------------
  // Construct the first exceeded fixed limit with its exact typed scope evidence.
  [[nodiscard]] static RiskCheckResult
  rejected(execution::SubmissionReason reason,
           execution::RiskLimitEvidence risk_evidence) noexcept {
    return RiskCheckResult{reason, std::nullopt, std::nullopt, std::move(risk_evidence)};
  }

  // --------------------------------------------------------
  [[nodiscard]] bool reserved() const noexcept { return reservation_id_.has_value(); }

  // --------------------------------------------------------
  [[nodiscard]] execution::SubmissionReason reason() const noexcept { return reason_; }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<model::ReservationId>& reservation_id() const noexcept {
    return reservation_id_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<OrderExposure>& exposure() const noexcept { return exposure_; }

  // --------------------------------------------------------
  [[nodiscard]] const std::optional<execution::RiskLimitEvidence>& risk_evidence() const noexcept {
    return risk_evidence_;
  }

  // --------------------------------------------------------
private:

  // ########################################################################
  // ReservationLedger alone may claim that all 35 cells and one slot committed atomically.
  friend class ReservationLedger;

  // ########################################################################

  // --------------------------------------------------------
  RiskCheckResult(execution::SubmissionReason reason,
                  std::optional<model::ReservationId> reservation_id,
                  std::optional<OrderExposure> exposure,
                  std::optional<execution::RiskLimitEvidence> risk_evidence) noexcept
      : reason_{reason}, reservation_id_{reservation_id}, exposure_{exposure},
        risk_evidence_{risk_evidence} {}

  // --------------------------------------------------------
  execution::SubmissionReason reason_;
  std::optional<model::ReservationId> reservation_id_;
  std::optional<OrderExposure> exposure_;
  std::optional<execution::RiskLimitEvidence> risk_evidence_;
};

// ########################################################################

// ########################################################################
// ReservationLedger preallocates every policy cell and reservation slot at construction. Its direct
// operations are deliberately owner-local and perform no locking, queueing, I/O, or heap
// allocation.
class ReservationLedger final {
public:

  // --------------------------------------------------------
  // Build all shared Count/Quantity/Notional/Directional cells and exactly capacity reusable slots.
  [[nodiscard]] static model::Result<ReservationLedger> create(RiskPolicySnapshot policy,
                                                               std::uint32_t capacity);

  // --------------------------------------------------------
  ReservationLedger(const ReservationLedger&) = delete;
  ReservationLedger& operator=(const ReservationLedger&) = delete;
  ReservationLedger(ReservationLedger&&) noexcept;
  ReservationLedger& operator=(ReservationLedger&&) noexcept;
  ~ReservationLedger();

  // --------------------------------------------------------
  // Calculate exposure, compute all 35 scratch candidates, apply canonical limit precedence, then
  // commit all cells and one ReservationId equal to the creating SubmissionAttemptId.
  [[nodiscard]] RiskCheckResult
  check_and_reserve(model::SubmissionAttemptId attempt_id,
                    const execution::InstalledSubmissionRoute& route,
                    const execution::CanonicalOrderEconomics& economics);

  // --------------------------------------------------------
  // Apply stored inverse deltas and transition one matching Held reservation exactly once.
  [[nodiscard]] model::Result<void> release(model::ReservationId reservation_id);

  // --------------------------------------------------------
  [[nodiscard]] const RiskPolicySnapshot& policy() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t capacity() const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t held_reservation_count() const noexcept;

  // --------------------------------------------------------
  // Borrow current evidence only while that exact identity still occupies its reusable slot.
  [[nodiscard]] const ReservationEvidence*
  find_reservation(model::ReservationId reservation_id) const noexcept;

  // --------------------------------------------------------
  // Borrow one current reusable-slot record in stable slot order; an empty or out-of-range slot
  // returns null and the caller may select only Held records for conservative evidence.
  [[nodiscard]] const ReservationEvidence*
  reservation_at(std::size_t stable_slot_index) const noexcept;

  // --------------------------------------------------------
  // Report the exact count of complete canonical risk-policy scope keys.
  [[nodiscard]] std::size_t scope_evidence_count() const noexcept;

  // --------------------------------------------------------
  // Copy one coherent scope projection in canonical risk-policy key order.
  [[nodiscard]] std::optional<RiskScopeExposureEvidence>
  scope_evidence_at(std::size_t canonical_index) const;

  // --------------------------------------------------------
  // Recompose one coherent scope projection from its shared mutable cells without mutation.
  [[nodiscard]] std::optional<RiskScopeExposure>
  scope_exposure(const model::FirmId& firm_id, RiskScopeKind scope, std::string_view scope_subject,
                 const model::InstrumentId& instrument_id,
                 std::string_view quote_currency) const noexcept;

private:

  // ########################################################################
  // Hide the fixed risk-cell and reservation-slot representation behind one stable owner handle.
  struct Impl;

  // ########################################################################

  // --------------------------------------------------------
  explicit ReservationLedger(std::unique_ptr<Impl> implementation) noexcept;

  // --------------------------------------------------------
  std::unique_ptr<Impl> implementation_;
};

// ########################################################################

} // namespace aegis::risk
