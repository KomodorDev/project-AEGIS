// Purpose: retain genuine initial M4 orders beside their external recovery medium for transaction
// preparation tests; normalization uses the installed epoch and exposes no mutable owner seams.

#pragma once

#include "private_business_evidence_fixture.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace aegis::test_support {

// --------------------------------------------------------
// Derive exact authored capacities before installation, so no live storage is resized by tests.
[[nodiscard]] inline M4OwnerTestAuthority
create_transaction_authority_or_throw(runtime::M4PolicyCapacities capacities) {
  auto authority = create_m4_owner_test_authority_or_throw();
  authority.m4_policy = extract_evidence_value_or_throw(runtime::M4Policy::create_m4_policy(
      authority.configuration, authority.runtime_policy,
      authority.submission->reservations().policy(), authority.submission->policy(), capacities));
  return authority;
}

// --------------------------------------------------------

// ########################################################################
// Own one real initial order, its sole nonmoving coordinator, and the external fake handle. The
// copied factory remains sealed to that exact policy, and all order observations are quiescent.
class PrivateBusinessTransactionFixture final {
public:

  // --------------------------------------------------------
  // Acknowledge the namespace before installation and genuine Q=2/N=20 submission; keep the
  // external medium available to prove cold inspection remains fenced by detached preparations.
  explicit PrivateBusinessTransactionFixture(
      execution::OrderSide side = execution::OrderSide::Buy,
      runtime::M4PolicyCapacities capacities = create_ordinary_m4_policy_capacities())
      : authority{create_transaction_authority_or_throw(capacities)},
        factory{create_evidence_event_factory_or_throw(authority)} {
    auto recovery = create_m4_recovery_bootstrap_test_authority_or_throw(authority.m4_policy);
    medium = std::move(recovery.medium);
    const auto installed = authority.submission->install_recovery_bound_private_order_reconciler(
        authority.configuration, authority.m4_policy, std::move(recovery.bootstrap));
    if (!installed) {
      throw std::logic_error{"failed private business transaction owner installation"};
    }
    auto request = create_m4_reference_order_request_or_throw();
    request.side = side;
    const auto submitted = submit_m4_order_or_throw(authority, request);
    if (!submitted.order_id() ||
        submitted.disposition() != execution::SubmitDisposition::WriteInitiated) {
      throw std::logic_error{"missing initial private business transaction order"};
    }
    order_id_ = submitted.order_id();
  }

  // --------------------------------------------------------
  // Preserve the coordinator's stable address; individual tests may destroy its owning pointer.
  PrivateBusinessTransactionFixture(const PrivateBusinessTransactionFixture&) = delete;
  PrivateBusinessTransactionFixture& operator=(const PrivateBusinessTransactionFixture&) = delete;
  PrivateBusinessTransactionFixture(PrivateBusinessTransactionFixture&&) = delete;
  PrivateBusinessTransactionFixture& operator=(PrivateBusinessTransactionFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the genuine retained order only while this fixture still owns its coordinator.
  [[nodiscard]] const oms::OutboundOrderRecord& order() const {
    const auto* row = authority.submission->outbound_oms().find_order(*order_id_);
    if (row == nullptr) {
      throw std::logic_error{"missing private business transaction order"};
    }
    return *row;
  }

  // --------------------------------------------------------
  // Borrow the exact installed production implementation for immutable preparation and inspection.
  [[nodiscard]] const runtime::PrivateOrderReconciler& owner() const {
    const auto* reconciler = authority.submission->private_order_reconciler();
    if (reconciler == nullptr) {
      throw std::logic_error{"missing private business transaction owner"};
    }
    return *reconciler;
  }

  // --------------------------------------------------------
  // Supply one literal exchange identity independently of the uncommitted candidate mapping.
  [[nodiscard]] oms::ExchangeOrderId create_exchange_order_id_or_throw() const {
    return create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x51U);
  }

  // --------------------------------------------------------
  // Supply both exact locators, allowing first-seen correlation to propose a mapping.
  [[nodiscard]] oms::PrivateOrderLocator create_locator_or_throw() const {
    return extract_evidence_value_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
        order().order_id(), create_exchange_order_id_or_throw()));
  }

  // --------------------------------------------------------
  // Normalize each literal scenario with its original source facts. Reconciliation identity is
  // qualified by this installed runtime; it does not claim complete-batch admission authority.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_input_or_throw(BusinessEvidenceInput kind, bool reconciliation = false) const {
    const auto event = static_cast<std::uint8_t>(kind);
    const auto& provenance = order().provenance();
    const oms::VenuePrivateEventOrigin origin{
        oms::VenuePrivateEventKey{
            provenance.venue_id, provenance.logical_account_id,
            create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
            create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
    const oms::ReconciliationPrivateEventOrigin reconciliation_origin{
        extract_evidence_value_or_throw(
            recovery::ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
                owner().runtime_epoch_id(), 1U)),
        create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x41U),
        create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(event),
        model::SourceTimestamp{10U}, model::ReceiveTimestamp{20U}};
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
          reconciliation
              ? factory.normalize_reconciliation_rejection(
                    reconciliation_origin, provenance.logical_account_id, provenance.venue_id,
                    create_locator_or_throw(), oms::ExchangeRejectionCategory::InvalidOrder, {})
              : factory.normalize_venue_rejection(origin, create_locator_or_throw(),
                                                  oms::ExchangeRejectionCategory::InvalidOrder,
                                                  {}));
    }
    if (kind == BusinessEvidenceInput::CancellationAtZero) {
      const auto zero = create_m4_decimal_or_throw<model::Quantity>(0);
      return extract_evidence_value_or_throw(
          reconciliation
              ? factory.normalize_reconciliation_cancellation_result(
                    reconciliation_origin, provenance.logical_account_id, provenance.venue_id,
                    create_locator_or_throw(), oms::CancellationResult::Cancelled, zero)
              : factory.normalize_venue_cancellation_result(
                    origin, create_locator_or_throw(), oms::CancellationResult::Cancelled, zero));
    }
    const auto increment = create_m4_decimal_or_throw<model::Quantity>(
        kind == BusinessEvidenceInput::FullFill ? 2 : 1);
    const auto cumulative = create_m4_decimal_or_throw<model::Quantity>(
        kind == BusinessEvidenceInput::FullFill || kind == BusinessEvidenceInput::BufferedGap ? 2
                                                                                              : 1);
    const auto side =
        kind == BusinessEvidenceInput::ContradictorySide
            ? (order().economics().side == execution::OrderSide::Buy ? execution::OrderSide::Sell
                                                                     : execution::OrderSide::Buy)
            : order().economics().side;
    const auto trade = create_m4_opaque_identity_or_throw<oms::TradeId>(event);
    return extract_evidence_value_or_throw(
        reconciliation ? factory.normalize_reconciliation_execution(
                             reconciliation_origin, provenance.logical_account_id,
                             provenance.venue_id, create_locator_or_throw(), trade,
                             provenance.instrument_id, provenance.metadata_revision, increment,
                             cumulative, order().economics().price, side)
                       : factory.normalize_venue_execution(
                             origin, create_locator_or_throw(), trade, provenance.instrument_id,
                             provenance.metadata_revision, increment, cumulative,
                             order().economics().price, side));
  }

  // --------------------------------------------------------
  // Produce a receive-time-free configured or unknown-account source for genuine fence tests.
  [[nodiscard]] oms::PrivateOrderIngressAttempt
  create_acknowledgement_attempt_or_throw(std::uint8_t event, bool foreign_account = false) const {
    const auto& provenance = order().provenance();
    return extract_evidence_value_or_throw(factory.create_venue_acknowledgement_attempt(
        oms::VenuePrivateIngressOrigin{
            oms::VenuePrivateEventKey{
                provenance.venue_id,
                foreign_account
                    ? parse_m4_identifier_or_throw<model::LogicalAccountId>("account.foreign")
                    : provenance.logical_account_id,
                create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x31U),
                create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event)},
            model::SourceTimestamp{10U}},
        create_exchange_order_id_or_throw(),
        foreign_account ? std::optional<model::OrderId>{} : order_id_));
  }

  // --------------------------------------------------------
  // Keep cold fake media separately owned so tests can inspect its lease and destroy either owner.
  M4OwnerTestAuthority authority;
  runtime::PrivateOrderEventFactory factory;
  std::unique_ptr<recovery::DeterministicFakeRecoveryMedium> medium;

private:
  std::optional<model::OrderId> order_id_;
};

// ########################################################################

} // namespace aegis::test_support
