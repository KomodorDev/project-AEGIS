// Purpose: create receive-time-free M4 private-order attempts, privately attach receipt
// observations, and mediate sealed provenance checks for one owner-bound read-only planner.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/identifier.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "m4_provenance_resolver.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// The owner-bound reconciler alone may use the factory's private provenance-planning delegates.
class PrivateOrderReconciler;

// ########################################################################

// ########################################################################
// The factory derives provenance and closed scope from one validated resolver; its public inputs
// contain only source/local facts and never accept caller-authored organizational attribution.
class PrivateOrderEventFactory final {
public:

  // --------------------------------------------------------
  // Own one resolver so normalized source provenance never depends on a caller-managed lifetime.
  explicit PrivateOrderEventFactory(M4ProvenanceResolver resolver)
      : resolver_{std::move(resolver)} {}

  // --------------------------------------------------------
  // Validate one ordinary acknowledgement into a receive-time-free attempt without inferring local
  // ownership from its optional client locator.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
  create_venue_acknowledgement_attempt(oms::VenuePrivateIngressOrigin origin,
                                       oms::ExchangeOrderId exchange_order_id,
                                       std::optional<model::OrderId> local_order_locator) const;

  // --------------------------------------------------------
  // Normalize one ordinary venue acknowledgement by attaching only the supplied receive time; the
  // returned value grants no executor admission or consumption authority.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_venue_acknowledgement(oms::VenuePrivateEventOrigin origin,
                                  oms::ExchangeOrderId exchange_order_id,
                                  std::optional<model::OrderId> local_order_locator) const;

  // --------------------------------------------------------
  // Normalize the same authoritative acknowledgement shape from a validated reconciliation row.
  // The row translator supplies no instrument for KnownOrderLookup/Present and supplies the exact
  // raw instrument for OpenOrder; no other caller chooses this optional provenance field.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_reconciliation_acknowledgement(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::ExchangeOrderId exchange_order_id,
      std::optional<model::OrderId> local_order_locator,
      std::optional<model::InstrumentId> source_instrument_id) const;

  // --------------------------------------------------------
  // Validate one ordinary venue rejection into a receive-time-free bounded source attempt.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt> create_venue_rejection_attempt(
      oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
      oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const;

  // --------------------------------------------------------
  // Normalize a venue rejection with a nonempty locator and bounded opaque detail by attaching only
  // the supplied receive observation.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_venue_rejection(oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
                            oms::ExchangeRejectionCategory category,
                            std::span<const std::byte> detail) const;

  // --------------------------------------------------------
  // Normalize the same definitive rejection shape from reconciliation authority.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> normalize_reconciliation_rejection(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator,
      oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const;

  // --------------------------------------------------------
  // Validate one ordinary execution attempt; source side remains optional until owner correlation.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt> create_venue_execution_attempt(
      oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
      oms::TradeId trade_id, model::InstrumentId instrument_id,
      model::InstrumentMetadataRevision metadata_revision, model::Quantity incremental_quantity,
      model::Quantity cumulative_quantity, model::Price execution_price,
      std::optional<execution::OrderSide> source_side) const;

  // --------------------------------------------------------
  // Normalize a venue execution by attaching only the supplied receive observation; source side
  // remains optional until owner correlation.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> normalize_venue_execution(
      oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
      model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
      model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
      model::Price execution_price, std::optional<execution::OrderSide> source_side) const;

  // --------------------------------------------------------
  // Normalize a reconciliation execution whose authoritative source row always supplies side.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> normalize_reconciliation_execution(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
      model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
      model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
      model::Price execution_price, execution::OrderSide source_side) const;

  // --------------------------------------------------------
  // Validate one ordinary cancellation-result attempt with its exact result-dependent cumulative
  // shape and no local receive observation.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
  create_venue_cancellation_result_attempt(
      oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
      oms::CancellationResult result,
      std::optional<model::Quantity> terminal_cumulative_quantity) const;

  // --------------------------------------------------------
  // Create a venue CancelRejected attempt with an exact causal CancelAttemptId retained by the
  // caller's request/response closure; this raw evidence grants no local-order authority.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
  create_venue_cancel_rejection_attempt_with_causal_id(
      oms::VenuePrivateIngressOrigin origin, oms::PrivateOrderLocator locator,
      oms::CancelAttemptId causal_cancel_attempt_id) const;

  // --------------------------------------------------------
  // Normalize an ordinary venue cancellation result with exact result-dependent cumulative shape.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> normalize_venue_cancellation_result(
      oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
      oms::CancellationResult result,
      std::optional<model::Quantity> terminal_cumulative_quantity) const;

  // --------------------------------------------------------
  // Attach receive time to one causally bound venue rejection without inferring order ownership.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_venue_cancel_rejection_with_causal_id(
      oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
      oms::CancelAttemptId causal_cancel_attempt_id) const;

  // --------------------------------------------------------
  // Normalize the same terminal/nonterminal cancellation result from reconciliation authority.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_reconciliation_cancellation_result(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::CancellationResult result,
      std::optional<model::Quantity> terminal_cumulative_quantity) const;

  // --------------------------------------------------------
  // Validate an account-scoped local timeout attempt under exact configured account authority.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
  create_account_timeout_attempt(oms::LocalPrivateIngressOrigin origin,
                                 model::LogicalAccountId logical_account_id,
                                 model::VenueId venue_id) const;

  // --------------------------------------------------------
  // Normalize an account-scoped timeout only after exact configured account/venue validation.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_account_timeout(oms::LocalPrivateEventOrigin origin,
                            model::LogicalAccountId logical_account_id,
                            model::VenueId venue_id) const;

  // --------------------------------------------------------
  // Validate a configured private-source disconnect attempt for one exact affected source epoch.
  [[nodiscard]] model::Result<oms::PrivateOrderIngressAttempt>
  create_disconnect_attempt(oms::LocalPrivateIngressOrigin origin,
                            model::LogicalAccountId logical_account_id, model::VenueId venue_id,
                            oms::PrivateSourceEpochId affected_source_epoch_id) const;

  // --------------------------------------------------------
  // Normalize a private-source disconnect for one configured account and affected source epoch.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  normalize_disconnect(oms::LocalPrivateEventOrigin origin,
                       model::LogicalAccountId logical_account_id, model::VenueId venue_id,
                       oms::PrivateSourceEpochId affected_source_epoch_id) const;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Return whether sealed configuration proves the exact logical-account and venue binding.
  [[nodiscard]] bool
  has_configured_account_venue_binding(const model::LogicalAccountId& logical_account_id,
                                       const model::VenueId& venue_id) const noexcept;

  // --------------------------------------------------------
  // Derive maximal correlation-independent source provenance from sealed configuration only.
  [[nodiscard]] model::M4Provenance derive_authoritative_source_provenance(
      const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id,
      const std::optional<model::InstrumentId>& instrument_id) const noexcept;

  // --------------------------------------------------------
  // Validate one genuine immutable OMS row and derive its complete known-order provenance.
  [[nodiscard]] model::Result<model::M4Provenance>
  derive_retained_order_provenance(const oms::OutboundOrderRecord& retained_order) const;

  // --------------------------------------------------------
  // Attach maximal independently proved source provenance to one receive-time-free attempt.
  [[nodiscard]] oms::PrivateOrderIngressAttempt create_authoritative_private_order_ingress_attempt(
      oms::PrivateIngressOriginValue origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, const std::optional<model::InstrumentId>& instrument_id,
      oms::PrivateOrderEventPayload payload) const noexcept;

  // --------------------------------------------------------
  // Attach one supplied observation time behind the private compatibility boundary.
  [[nodiscard]] oms::NormalizedPrivateOrderInput
  normalize_private_order_ingress_attempt(const oms::PrivateOrderIngressAttempt& attempt,
                                          model::ReceiveTimestamp received_at) const noexcept;

  // --------------------------------------------------------
  // Retain the complete self-owned normalization authority.
  M4ProvenanceResolver resolver_;

  // ########################################################################
  // Narrow friendship exposes only sealed provenance derivation to the bound read-only planner.
  friend class PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
