// Purpose: own genuine initial M4 orders and trusted normalization sources for business evidence
// qualification; expose immutable observations without canonical consumption or mutable test seams.

#pragma once

#include "aegis/risk/inventory_ledger.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace aegis::test_support {

// ########################################################################
// Each literal scenario begins with the same genuine M3 order and a distinct authoritative fact;
// the integer expectations are authored separately from the production planning algorithms.
enum class BusinessEvidenceInput : std::uint8_t {
  Acknowledgement = 1,
  PartialFill = 2,
  FullFill = 3,
  ExchangeRejection = 4,
  CancellationAtZero = 5,
  BufferedGap = 6,
  ContradictorySide = 7,
};

// ########################################################################

// --------------------------------------------------------
// Stop fixture setup at the precise invalid value rather than exposing partial test authority.
template <typename Value>
[[nodiscard]] Value extract_evidence_value_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid private business fixture: " + result.error().context.field};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Install the production M4 owner before the first genuine submission acquires callback authority.
[[nodiscard]] inline test_support::M4OwnerTestAuthority create_evidence_authority_or_throw(
    runtime::M4PolicyCapacities capacities = create_ordinary_m4_policy_capacities()) {
  auto authority = test_support::create_m4_owner_test_authority_or_throw();
  authority.m4_policy = extract_evidence_value_or_throw(runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities));
  test_support::install_recovery_bound_private_order_reconciler_or_throw(authority);
  return authority;
}

// --------------------------------------------------------
// Bind authoritative normalization to the same complete policy root as the owning runtime.
[[nodiscard]] inline runtime::PrivateOrderEventFactory
create_evidence_event_factory_or_throw(const test_support::M4OwnerTestAuthority& authority) {
  return runtime::PrivateOrderEventFactory{
      extract_evidence_value_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
          authority.configuration, authority.m4_policy))};
}

// --------------------------------------------------------
// Copy every canonical risk cell so read-only planning cannot conceal an unrelated partial update.
[[nodiscard]] inline std::vector<risk::RiskScopeExposureEvidence>
collect_evidence_scope_rows_or_throw(const risk::ReservationLedger& reservations) {
  std::vector<risk::RiskScopeExposureEvidence> result;
  for (std::size_t index = 0U; index < reservations.scope_evidence_count(); ++index) {
    const auto row = reservations.scope_evidence_at(index);
    if (!row) {
      throw std::logic_error{"missing private business scope evidence"};
    }
    result.push_back(*row);
  }
  return result;
}

// --------------------------------------------------------

// ########################################################################
// Own one real admitted reference order and its exact sealed normalization source. The coordinator
// outlives all borrowed rows; tests obtain only const observations and detached proposals from it.
class BusinessEvidenceFixture final {
public:

  // --------------------------------------------------------
  // Submit an explicit integer quantity at limit100 and multiplier10; default Q=2 gives literal
  // N=20.
  explicit BusinessEvidenceFixture(
      execution::OrderSide side = execution::OrderSide::Buy,
      runtime::M4PolicyCapacities capacities = create_ordinary_m4_policy_capacities(),
      std::int64_t original_quantity = 2)
      : authority{create_evidence_authority_or_throw(capacities)},
        factory{create_evidence_event_factory_or_throw(authority)} {
    auto request = test_support::create_m4_reference_order_request_or_throw();
    request.side = side;
    request.quantity = create_m4_decimal_or_throw<model::Quantity>(original_quantity);
    const auto submitted = test_support::submit_m4_order_or_throw(authority, request);
    if (!submitted.order_id()) {
      throw std::logic_error{"private business fixture failed to submit its order"};
    }
    order_ = authority.submission->outbound_oms().find_order(*submitted.order_id());
    owner_ = authority.submission->private_order_reconciler();
    if (order_ == nullptr || owner_ == nullptr ||
        order_->state() != oms::OutboundOrderState::WriteInitiated) {
      throw std::logic_error{"private business fixture lacks its genuine initial owner row"};
    }
  }

  // --------------------------------------------------------
  // Keep the coordinator and every borrowed observation at stable addresses for the fixture life.
  BusinessEvidenceFixture(const BusinessEvidenceFixture&) = delete;
  BusinessEvidenceFixture& operator=(const BusinessEvidenceFixture&) = delete;
  BusinessEvidenceFixture(BusinessEvidenceFixture&&) = delete;
  BusinessEvidenceFixture& operator=(BusinessEvidenceFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the exact retained order established by the real submission path.
  [[nodiscard]] const oms::OutboundOrderRecord& order() const noexcept { return *order_; }

  // --------------------------------------------------------
  // Borrow the private owner solely for its const planning and completion-observation interfaces.
  [[nodiscard]] const runtime::PrivateOrderReconciler& owner() const noexcept { return *owner_; }

  // --------------------------------------------------------
  // Author one fixed exchange identity, independent from any candidate mapping in a proposal.
  [[nodiscard]] oms::ExchangeOrderId create_exchange_order_id_or_throw() const {
    return test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x51U);
  }

  // --------------------------------------------------------
  // Preserve the genuine local locator and a possible first exchange mapping without publishing it.
  [[nodiscard]] oms::PrivateOrderLocator create_business_locator_or_throw() const {
    return extract_evidence_value_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
        order().order_id(), create_exchange_order_id_or_throw()));
  }

  // --------------------------------------------------------
  // Bind ordinary source identity and times to the genuine retained account and venue.
  [[nodiscard]] oms::VenuePrivateEventOrigin
  create_venue_origin_or_throw(std::uint8_t event = 1U) const {
    return oms::VenuePrivateEventOrigin{
        oms::VenuePrivateEventKey{
            order().provenance().venue_id, order().provenance().logical_account_id,
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
  }

  // --------------------------------------------------------
  // Keep reconciliation origin nominally distinct while preserving equal economic source facts.
  [[nodiscard]] oms::ReconciliationPrivateEventOrigin
  create_reconciliation_origin_or_throw(std::uint8_t event = 1U) const {
    return oms::ReconciliationPrivateEventOrigin{
        test_support::create_m4_reconciliation_epoch_or_throw(),
        test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
        test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(event),
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
  }

  // --------------------------------------------------------
  // Normalize literal reference inputs through trusted factories; none advances live lifecycle.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_business_input_or_throw(BusinessEvidenceInput kind, bool reconciliation = false) const {
    const auto event = static_cast<std::uint8_t>(kind);
    const auto origin = create_venue_origin_or_throw(event);
    const auto reconciliation_origin = create_reconciliation_origin_or_throw(event);
    const auto& provenance = order().provenance();
    if (kind == BusinessEvidenceInput::Acknowledgement) {
      return extract_evidence_value_or_throw(
          reconciliation ? factory.normalize_reconciliation_acknowledgement(
                               reconciliation_origin, provenance.logical_account_id,
                               provenance.venue_id, create_exchange_order_id_or_throw(),
                               order().order_id(), provenance.instrument_id)
                         : factory.normalize_venue_acknowledgement(
                               origin, create_exchange_order_id_or_throw(), order().order_id()));
    }
    if (kind == BusinessEvidenceInput::ExchangeRejection) {
      return extract_evidence_value_or_throw(
          reconciliation ? factory.normalize_reconciliation_rejection(
                               reconciliation_origin, provenance.logical_account_id,
                               provenance.venue_id, create_business_locator_or_throw(),
                               oms::ExchangeRejectionCategory::InvalidOrder, {})
                         : factory.normalize_venue_rejection(
                               origin, create_business_locator_or_throw(),
                               oms::ExchangeRejectionCategory::InvalidOrder, {}));
    }
    if (kind == BusinessEvidenceInput::CancellationAtZero) {
      const auto zero = test_support::create_m4_decimal_or_throw<model::Quantity>(0);
      return extract_evidence_value_or_throw(
          reconciliation
              ? factory.normalize_reconciliation_cancellation_result(
                    reconciliation_origin, provenance.logical_account_id, provenance.venue_id,
                    create_business_locator_or_throw(), oms::CancellationResult::Cancelled, zero)
              : factory.normalize_venue_cancellation_result(
                    origin, create_business_locator_or_throw(), oms::CancellationResult::Cancelled,
                    zero));
    }
    const auto increment = test_support::create_m4_decimal_or_throw<model::Quantity>(
        kind == BusinessEvidenceInput::FullFill ? 2 : 1);
    const auto cumulative = test_support::create_m4_decimal_or_throw<model::Quantity>(
        kind == BusinessEvidenceInput::FullFill || kind == BusinessEvidenceInput::BufferedGap ? 2
                                                                                              : 1);
    const auto side =
        kind == BusinessEvidenceInput::ContradictorySide
            ? (order().economics().side == execution::OrderSide::Buy ? execution::OrderSide::Sell
                                                                     : execution::OrderSide::Buy)
            : order().economics().side;
    const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(event);
    return extract_evidence_value_or_throw(
        reconciliation ? factory.normalize_reconciliation_execution(
                             reconciliation_origin, provenance.logical_account_id,
                             provenance.venue_id, create_business_locator_or_throw(), trade,
                             provenance.instrument_id, provenance.metadata_revision, increment,
                             cumulative, order().economics().price, side)
                       : factory.normalize_venue_execution(
                             origin, create_business_locator_or_throw(), trade,
                             provenance.instrument_id, provenance.metadata_revision, increment,
                             cumulative, order().economics().price, side));
  }

  // --------------------------------------------------------
  // Public fixture values permit ordinary source normalization and cold owner destruction only;
  // the coordinator still exposes no mutable reservation or inventory seam to these tests.
  test_support::M4OwnerTestAuthority authority;
  runtime::PrivateOrderEventFactory factory;

private:
  const oms::OutboundOrderRecord* order_{nullptr};
  const runtime::PrivateOrderReconciler* owner_{nullptr};
};

// ########################################################################

} // namespace aegis::test_support
