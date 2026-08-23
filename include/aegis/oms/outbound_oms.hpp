// Purpose: retain fixed-capacity M3 outbound order identity, approved economics, provenance, and
// owner-local state without performing risk release, encoding, or transport work.

#pragma once

#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/model/sha256.hpp"
#include "aegis/model/time.hpp"
#include "aegis/risk/exposure.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace aegis::oms {

// ########################################################################
// Assigned states cover only local M3 admission, encoding, and fake-initiation outcomes.
enum class OutboundOrderState : std::uint8_t {
  PendingEncoding = 1,
  PendingInitiation = 2,
  WriteInitiated = 3,
  SubmissionUnknown = 4,
  LocallyFailed = 5,
};

// ########################################################################

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

// ########################################################################
// A record retains its complete admission permanently; only OutboundOms may advance the state.
class OutboundOrderRecord final {
public:

  // --------------------------------------------------------
  [[nodiscard]] const OutboundOrderAdmission& admission() const noexcept { return admission_; }

  // --------------------------------------------------------
  [[nodiscard]] model::SubmissionAttemptId attempt_id() const noexcept {
    return admission_.attempt_id;
  }

  // --------------------------------------------------------
  [[nodiscard]] const model::OrderId& order_id() const noexcept { return admission_.order_id; }

  // --------------------------------------------------------
  [[nodiscard]] model::ReservationId reservation_id() const noexcept {
    return admission_.reservation_id;
  }

  // --------------------------------------------------------
  [[nodiscard]] const execution::CanonicalOrderEconomics& economics() const noexcept {
    return admission_.economics;
  }

  // --------------------------------------------------------
  [[nodiscard]] const risk::OrderExposure& exposure() const noexcept { return admission_.exposure; }

  // --------------------------------------------------------
  [[nodiscard]] const OutboundOrderProvenance& provenance() const noexcept {
    return admission_.provenance;
  }

  // --------------------------------------------------------
  [[nodiscard]] OutboundOrderState state() const noexcept { return state_; }

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

  OutboundOrderAdmission admission_;
  OutboundOrderState state_{OutboundOrderState::PendingEncoding};
};

// ########################################################################

// ########################################################################
// Ordinary duplicate and capacity non-admission remain SubmitResult reasons, while impossible
// lower-level state is returned separately as a DomainError by OutboundOms::admit.
class OmsAdmissionResult final {
public:

  // --------------------------------------------------------
  [[nodiscard]] bool admitted() const noexcept { return record_ != nullptr; }

  // --------------------------------------------------------
  [[nodiscard]] const OutboundOrderRecord* record() const noexcept { return record_; }

  // --------------------------------------------------------
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

  const OutboundOrderRecord* record_{nullptr};
  std::optional<execution::SubmissionReason> reason_;
};

// ########################################################################

// ########################################################################
// The owner-local table uses deterministic open addressing and never erases rows, so complete
// OrderId equality resolves every hash collision and retained pointers stay stable.
class OutboundOms final {
public:

  // --------------------------------------------------------
  // Allocate the complete positive row capacity before owner-local submission begins.
  [[nodiscard]] static model::Result<OutboundOms> create(std::uint32_t capacity);

  // --------------------------------------------------------
  OutboundOms(const OutboundOms&) = delete;
  OutboundOms& operator=(const OutboundOms&) = delete;
  OutboundOms(OutboundOms&&) noexcept = default;
  OutboundOms& operator=(OutboundOms&&) noexcept = default;

  // --------------------------------------------------------
  // Check exact duplicate identity before capacity, then retain one complete admitted row.
  [[nodiscard]] model::Result<OmsAdmissionResult> admit(OutboundOrderAdmission admission);

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
  // Move only PendingInitiation to the ordinary terminal local fake WriteInitiated state.
  [[nodiscard]] model::Result<void> mark_write_initiated(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Move only PendingInitiation to terminal reconciliation-required SubmissionUnknown.
  [[nodiscard]] model::Result<void> mark_submission_unknown(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Conservatively downgrade only WriteInitiated after a latched post-acceptance internal fault;
  // ordinary M3 submission has no outgoing transition from WriteInitiated.
  [[nodiscard]] model::Result<void>
  mark_submission_unknown_after_internal_fault(const model::OrderId& order_id);

  // --------------------------------------------------------
  // Resolve an exact complete identity without changing table state.
  [[nodiscard]] const OutboundOrderRecord* find(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Borrow one retained row in canonical admission order; an out-of-range index returns null.
  [[nodiscard]] const OutboundOrderRecord* record_at(std::size_t admission_index) const noexcept;

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

  // --------------------------------------------------------
  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Construct only after create has rejected an unusable zero capacity.
  explicit OutboundOms(std::uint32_t capacity);

  // --------------------------------------------------------
  // Hash all 24 identity bytes only to choose a probe start; equality remains authoritative.
  [[nodiscard]] std::size_t probe_start(const model::OrderId& order_id) const noexcept;

  // --------------------------------------------------------
  // Resolve a mutable row for the closed transition implementation.
  [[nodiscard]] OutboundOrderRecord* find_mutable(const model::OrderId& order_id) noexcept;

  // --------------------------------------------------------
  // Apply one source-to-target transition or fail without mutation for missing/wrong state.
  [[nodiscard]] model::Result<void> transition(const model::OrderId& order_id,
                                               OutboundOrderState expected,
                                               OutboundOrderState target);

  // --------------------------------------------------------
  std::uint32_t capacity_;
  std::uint32_t size_{0U};
  std::vector<std::optional<OutboundOrderRecord>> slots_;
  std::vector<std::uint32_t> admission_order_;
};

// ########################################################################

} // namespace aegis::oms
