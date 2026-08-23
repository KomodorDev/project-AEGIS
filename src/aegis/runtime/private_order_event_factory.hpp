// Purpose: attach trusted source provenance to the closed M4 private-event vocabulary while
// keeping all firm, desk, bot, strategy, route, and metadata attribution caller-unforgeable.

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
// The factory derives provenance and closed scope from one validated resolver; its public inputs
// contain only source/local facts and never accept caller-authored organizational attribution.
class PrivateOrderEventFactory final {
public:

  // --------------------------------------------------------
  // Own one resolver so normalized source provenance never depends on a caller-managed lifetime.
  explicit PrivateOrderEventFactory(M4ProvenanceResolver resolver)
      : resolver_{std::move(resolver)} {}

  // --------------------------------------------------------
  // Normalize one ordinary venue acknowledgement without assuming the optional client locator is
  // locally owned.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  venue_acknowledgement(oms::VenuePrivateEventOrigin origin, oms::ExchangeOrderId exchange_order_id,
                        std::optional<model::OrderId> local_order_locator) const;

  // --------------------------------------------------------
  // Normalize the same authoritative acknowledgement shape from a validated reconciliation row.
  // The row translator supplies no instrument for KnownOrderLookup/Present and supplies the exact
  // raw instrument for OpenOrder; no other caller chooses this optional provenance field.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  reconciliation_acknowledgement(oms::ReconciliationPrivateEventOrigin origin,
                                 model::LogicalAccountId logical_account_id,
                                 model::VenueId venue_id, oms::ExchangeOrderId exchange_order_id,
                                 std::optional<model::OrderId> local_order_locator,
                                 std::optional<model::InstrumentId> source_instrument_id) const;

  // --------------------------------------------------------
  // Normalize a venue rejection with a nonempty locator and bounded opaque detail.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  venue_rejection(oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
                  oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const;

  // --------------------------------------------------------
  // Normalize the same definitive rejection shape from reconciliation authority.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> reconciliation_rejection(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator,
      oms::ExchangeRejectionCategory category, std::span<const std::byte> detail) const;

  // --------------------------------------------------------
  // Normalize a venue execution; source side is optional until correlation resolves ownership.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> venue_execution(
      oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
      model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
      model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
      model::Price execution_price, std::optional<execution::OrderSide> source_side) const;

  // --------------------------------------------------------
  // Normalize a reconciliation execution whose authoritative source row always supplies side.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> reconciliation_execution(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::TradeId trade_id,
      model::InstrumentId instrument_id, model::InstrumentMetadataRevision metadata_revision,
      model::Quantity incremental_quantity, model::Quantity cumulative_quantity,
      model::Price execution_price, execution::OrderSide source_side) const;

  // --------------------------------------------------------
  // Normalize an ordinary venue cancellation result with exact result-dependent cumulative shape.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  venue_cancellation_result(oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator locator,
                            oms::CancellationResult result,
                            std::optional<model::Quantity> terminal_cumulative_quantity) const;

  // --------------------------------------------------------
  // Normalize the same terminal/nonterminal cancellation result from reconciliation authority.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput> reconciliation_cancellation_result(
      oms::ReconciliationPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
      model::VenueId venue_id, oms::PrivateOrderLocator locator, oms::CancellationResult result,
      std::optional<model::Quantity> terminal_cumulative_quantity) const;

  // --------------------------------------------------------
  // Mint an account-scoped timeout only after exact configured account/venue validation.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  account_timeout(oms::LocalPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
                  model::VenueId venue_id) const;

  // --------------------------------------------------------
  // Mint a private-source disconnect for one configured account and exact affected source epoch.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  disconnect(oms::LocalPrivateEventOrigin origin, model::LogicalAccountId logical_account_id,
             model::VenueId venue_id, oms::PrivateSourceEpochId affected_source_epoch_id) const;

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Attach maximal independently proved source provenance to one venue/reconciliation payload.
  [[nodiscard]] model::Result<oms::NormalizedPrivateOrderInput>
  authoritative(oms::PrivateEventOriginValue origin, model::LogicalAccountId logical_account_id,
                model::VenueId venue_id, const std::optional<model::InstrumentId>& instrument_id,
                oms::PrivateOrderEventPayload payload) const;

  // --------------------------------------------------------
  // Retain the complete self-owned normalization authority.
  M4ProvenanceResolver resolver_;
};

// ########################################################################

} // namespace aegis::runtime
