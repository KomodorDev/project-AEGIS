// Purpose: retain fixed-capacity outbound order identity, approved economics, provenance, and the
// M3-to-M4 owner-local OMS projection without performing risk, encoding, or transport work.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/risk/exposure.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aegis::oms {

// ########################################################################
// Values 1-5 preserve the local M3 admission, encoding, and fake-initiation assignments; values
// 6-11 append the M4 venue and reconciliation lifecycle outcomes without renumbering M3 evidence.
enum class OutboundOrderState : std::uint8_t {
  PendingEncoding = 1,
  PendingInitiation = 2,
  WriteInitiated = 3,
  SubmissionUnknown = 4,
  LocallyFailed = 5,
  Working = 6,
  PartiallyFilled = 7,
  Filled = 8,
  ExchangeRejected = 9,
  Cancelled = 10,
  ReconciledAbsent = 11,
};

// ########################################################################
// Cancellation is orthogonal to the primary OMS state, so local write uncertainty cannot become a
// false venue-terminal order fact.
enum class CancellationState : std::uint8_t {
  Unassigned = 0,
  None = 1,
  Requested = 2,
  WriteInitiated = 3,
  OutcomeUnknown = 4,
  DefinitelyFailed = 5,
  Rejected = 6,
  Confirmed = 7,
};

// ########################################################################
// A complete detached copy of one row's mutable M4 projection supports recovery and atomic tests
// without exposing a mutable record alias. The execution-evidence and execution-established-mapping
// facts are monotonic; the latter implies the former and a retained exchange identity. Values
// returned by OutboundOrderRecord are coherent snapshots, and a caller-authored or modified copy
// grants no authority over the retained row.
struct PrivateOrderProjection {
  OutboundOrderState state;
  bool exchange_acknowledged;
  std::optional<ExchangeOrderId> exchange_order_id;
  model::Quantity cumulative_filled_quantity;
  std::optional<model::Quantity> authoritative_terminal_cumulative_quantity;
  CancellationState cancellation_state;
  bool reconciliation_required;
  bool execution_evidence_observed;
  bool exchange_mapping_established_by_execution;
  std::uint32_t pending_fill_count;
  std::uint32_t cancel_attempt_count;

  // --------------------------------------------------------
  // Structural equality lets a forbidden transition prove every mutable OMS field stayed fixed.
  friend bool operator==(const PrivateOrderProjection&, const PrivateOrderProjection&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// One admitted order owns the complete immutable identity and provenance projection needed by the
// offline encoder, canonical evidence, and deterministic inspection.
struct OutboundOrderProvenance {
  model::RouteId route_id;
  model::VenueId venue_id;
  model::LogicalAccountId logical_account_id;
  model::InstrumentId instrument_id;
  model::VenueInstrumentId venue_instrument_id;
  model::FirmId firm_id;
  model::DeskId desk_id;
  model::BotId bot_id;
  model::StrategyId strategy_id;
  model::Sha256Digest configuration_fingerprint;
  model::ConfigurationRevision configuration_revision;
  model::OrganizationRevision organization_revision;
  model::RouteRevision route_revision;
  model::InstrumentMetadataRevision metadata_revision;
  model::Sha256Digest runtime_policy_fingerprint;
  model::Sha256Digest risk_policy_fingerprint;
  model::RiskPolicyRevision risk_policy_revision;
  model::Sha256Digest submission_policy_fingerprint;

  // --------------------------------------------------------
  // Structural equality proves that OMS retention did not alter any startup authority.
  friend bool operator==(const OutboundOrderProvenance&, const OutboundOrderProvenance&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// Admission transfers the already-authorized, validated, and risk-reserved order into the OMS;
// callers cannot omit its attempt, local identity, reservation, exposure, or provenance.
struct OutboundOrderAdmission {
  model::SubmissionAttemptId attempt_id;
  model::OrderId order_id;
  model::ReservationId reservation_id;
  execution::CanonicalOrderEconomics economics;
  risk::OrderExposure exposure;
  OutboundOrderProvenance provenance;

  // --------------------------------------------------------
  // Structural equality supports exact retained-record and replay assertions.
  friend bool operator==(const OutboundOrderAdmission&, const OutboundOrderAdmission&) = default;

  // --------------------------------------------------------
};

// ########################################################################
// A record retains its complete admission permanently and exposes only detached or const
// inspection. OutboundOms owns every current transition; no M4 reconciler or caller receives
// mutation authority in this prerequisite slice.
class OutboundOrderRecord final {
public:

  // --------------------------------------------------------
  // Borrow the complete immutable admission retained by this row.
  [[nodiscard]] const OutboundOrderAdmission& admission() const noexcept { return admission_; }

  // --------------------------------------------------------
  // Return the submission attempt that created this row.
  [[nodiscard]] model::SubmissionAttemptId attempt_id() const noexcept {
    return admission_.attempt_id;
  }

  // --------------------------------------------------------
  // Borrow the permanent local order identity retained by this row.
  [[nodiscard]] const model::OrderId& order_id() const noexcept { return admission_.order_id; }

  // --------------------------------------------------------
  // Return the exact held reservation identity paired with this order.
  [[nodiscard]] model::ReservationId reservation_id() const noexcept {
    return admission_.reservation_id;
  }

  // --------------------------------------------------------
  // Borrow the canonical order economics approved before OMS admission.
  [[nodiscard]] const execution::CanonicalOrderEconomics& economics() const noexcept {
    return admission_.economics;
  }

  // --------------------------------------------------------
  // Borrow the once-calculated risk exposure approved for this order.
  [[nodiscard]] const risk::OrderExposure& exposure() const noexcept { return admission_.exposure; }

  // --------------------------------------------------------
  // Borrow the complete startup and policy provenance retained at admission.
  [[nodiscard]] const OutboundOrderProvenance& provenance() const noexcept {
    return admission_.provenance;
  }

  // --------------------------------------------------------
  // Return the row's current primary local, venue, or reconciliation lifecycle state.
  [[nodiscard]] OutboundOrderState state() const noexcept { return state_; }

  // --------------------------------------------------------
  // Copy every M4 scalar and bounded side-table count as one coherent inspection value.
  [[nodiscard]] PrivateOrderProjection private_projection() const noexcept {
    return PrivateOrderProjection{state_,
                                  exchange_acknowledged_,
                                  exchange_order_id_,
                                  cumulative_filled_quantity_,
                                  authoritative_terminal_cumulative_quantity_,
                                  cancellation_state_,
                                  reconciliation_required_,
                                  execution_evidence_observed_,
                                  exchange_mapping_established_by_execution_,
                                  pending_fill_count_,
                                  cancel_attempt_count_};
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Publish a new row only in PendingEncoding after all ordinary admission checks have passed.
  explicit OutboundOrderRecord(OutboundOrderAdmission admission);

  // --------------------------------------------------------

  // ########################################################################
  // Only the table owner may create rows or apply the closed transition table.
  friend class OutboundOms;

  // ########################################################################

  // Retain the immutable admission and the complete owner-local M4 projection at one stable row
  // address; bounded counters describe side tables that later slices will preallocate separately.
  OutboundOrderAdmission admission_;
  OutboundOrderState state_{OutboundOrderState::PendingEncoding};
  bool exchange_acknowledged_{false};
  std::optional<ExchangeOrderId> exchange_order_id_;
  model::Quantity cumulative_filled_quantity_;
  std::optional<model::Quantity> authoritative_terminal_cumulative_quantity_;
  CancellationState cancellation_state_{CancellationState::None};
  bool reconciliation_required_{false};
  bool execution_evidence_observed_{false};
  bool exchange_mapping_established_by_execution_{false};
  std::uint32_t pending_fill_count_{0U};
  std::uint32_t cancel_attempt_count_{0U};
};

// ########################################################################
// Ordinary duplicate and capacity non-admission remain SubmitResult reasons, while impossible
// lower-level state is returned separately as a DomainError by OutboundOms::admit_outbound_order.
// Every
// constructed result contains either one stable row pointer or one ordinary non-admission reason,
// never both.
class OmsAdmissionResult final {
public:

  // --------------------------------------------------------
  // Return whether admission retained one permanent OMS row.
  [[nodiscard]] bool is_admitted() const noexcept { return record_ != nullptr; }

  // --------------------------------------------------------
  // Borrow the retained row on admission, or return null after ordinary non-admission.
  [[nodiscard]] const OutboundOrderRecord* record() const noexcept { return record_; }

  // --------------------------------------------------------
  // Return the stable duplicate or capacity reason only for ordinary non-admission.
  [[nodiscard]] const std::optional<execution::SubmissionReason>& reason() const noexcept {
    return reason_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Accepted admission carries exactly one stable table record and no rejection reason.
  explicit OmsAdmissionResult(const OutboundOrderRecord& record) noexcept;

  // --------------------------------------------------------
  // Rejected admission carries no table record and exactly one OMS-stage reason.
  explicit OmsAdmissionResult(execution::SubmissionReason reason) noexcept;

  // --------------------------------------------------------

  // ########################################################################
  // Only the OMS can manufacture a result paired with its retained table state.
  friend class OutboundOms;

  // ########################################################################

  // Exactly one of the stable row pointer or ordinary non-admission reason is present.
  const OutboundOrderRecord* record_{nullptr};
  std::optional<execution::SubmissionReason> reason_;
};

// ########################################################################
// The owner-local table uses deterministic open addressing and never erases rows, so complete
// OrderId equality resolves every hash collision and retained pointers stay stable. Every fallible
// transition validates its exact source state before mutation and otherwise returns InvalidOmsState
// with the complete table unchanged.
class OutboundOms final {
public:

  // --------------------------------------------------------
  // Allocate the complete positive row capacity before owner-local submission begins; zero or an
  // unavailable allocation returns InvalidSubmissionPolicy without publishing a table.
  [[nodiscard]] static model::Result<OutboundOms> create_outbound_oms(std::uint32_t capacity);

  // --------------------------------------------------------
  // Preserve unique table ownership: copying is forbidden and moves transfer all preallocated
  // state.
  OutboundOms(const OutboundOms&) = delete;
  OutboundOms& operator=(const OutboundOms&) = delete;
  OutboundOms(OutboundOms&&) noexcept = default;
  OutboundOms& operator=(OutboundOms&&) noexcept = default;

  // --------------------------------------------------------
  // Check exact duplicate identity before capacity, then retain one complete admitted row;
  // contradictory admission evidence returns InvalidOmsState without modifying the table.
  [[nodiscard]] model::Result<OmsAdmissionResult>
  admit_outbound_order(OutboundOrderAdmission admission);

  // --------------------------------------------------------
  // Move only PendingEncoding to PendingInitiation after exact encoding succeeds.
  [[nodiscard]] model::Result<void> mark_encoding_succeeded(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Move only PendingEncoding to retained terminal LocallyFailed after scripted encoding failure.
  [[nodiscard]] model::Result<void> mark_encoding_failed(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Move only PendingInitiation to retained terminal LocallyFailed before fake acceptance.
  [[nodiscard]] model::Result<void>
  mark_initiation_definitely_failed(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Move only PendingInitiation to the ordinary M3 handoff state WriteInitiated; the retained order
  // remains OpenVenueRisk for M4 processing.
  [[nodiscard]] model::Result<void> mark_write_initiated(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Move only PendingInitiation to the M3 handoff state SubmissionUnknown and require later
  // reconciliation; the retained order remains OpenVenueRisk for M4 processing.
  [[nodiscard]] model::Result<void> mark_submission_unknown(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Conservatively downgrade only WriteInitiated after a latched post-acceptance internal fault and
  // require later reconciliation; ordinary M3 submission has no outgoing transition from
  // WriteInitiated.
  [[nodiscard]] model::Result<void>
  mark_submission_unknown_after_internal_fault(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Resolve an exact complete identity without changing table state.
  [[nodiscard]] const OutboundOrderRecord*
  find_order(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Borrow one retained row in canonical admission order; an out-of-range index returns null.
  [[nodiscard]] const OutboundOrderRecord* record_at(std::size_t admission_index) const noexcept;

  // --------------------------------------------------------
  // Return the fixed number of row slots preallocated at construction.
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  // Return the exact number of permanently retained admission rows.
  [[nodiscard]] std::uint32_t order_count() const noexcept { return size_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construct only after create has rejected an unusable zero capacity.
  explicit OutboundOms(std::uint32_t capacity);

  // --------------------------------------------------------
  // Hash all 24 identity bytes only to choose a probe start; equality remains authoritative.
  [[nodiscard]] std::size_t
  calculate_probe_start_index(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Resolve a mutable row for the closed transition implementation.
  [[nodiscard]] OutboundOrderRecord* find_mutable_order(const model::OrderId& order_id) noexcept;

  // --------------------------------------------------------
  // Apply one source-to-target transition or fail without mutation for missing/wrong state.
  [[nodiscard]] model::Result<void> transition_order_state(const model::OrderId& order_id,
                                                           OutboundOrderState expected,
                                                           OutboundOrderState target);

  // --------------------------------------------------------
  // Retain the fixed slot table and canonical admission index without post-construction growth.
  std::uint32_t capacity_;
  std::uint32_t size_{0U};
  std::vector<std::optional<OutboundOrderRecord>> slots_;
  std::vector<std::uint32_t> admission_order_;

  // --------------------------------------------------------
};

// ########################################################################

} // namespace aegis::oms
