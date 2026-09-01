// Purpose: validate root/configuration agreement and derive source or retained-order provenance
// without trusting caller-supplied organization, route, or metadata attribution.

#include "m4_provenance_resolver.hpp"

#include "aegis/oms/outbound_oms.hpp"

#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Report every provenance-authority mismatch through the stable private-event validation domain.
template <typename Value>
[[nodiscard]] model::Result<Value>
create_provenance_resolution_failure_from_field(std::string field) {
  return model::Result<Value>::create_failure(model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidPrivateEvent, std::move(field)));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Publish a self-owned resolver only after authority validation and complete configuration copying;
// return InvalidPrivateEvent for mismatch/allocation failure without publishing partial state.
model::Result<M4ProvenanceResolver> M4ProvenanceResolver::create_m4_provenance_resolver(
    const configuration::StartupConfiguration& configuration, const M4Policy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // The root must identify this exact sealed configuration and organization authority.
  const auto& root = policy.root_provenance();
  if (root.configuration_fingerprint() != configuration.fingerprint().bytes()) {
    return create_provenance_resolution_failure_from_field<M4ProvenanceResolver>(
        "m4_provenance.configuration_fingerprint");
  }
  if (root.organization_revision() != configuration.organization().revision()) {
    return create_provenance_resolution_failure_from_field<M4ProvenanceResolver>(
        "m4_provenance.organization_revision");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the sealed configuration into the resolver so later derivation has no borrowed lifetime.
  try {
    return model::Result<M4ProvenanceResolver>::create_success(
        M4ProvenanceResolver{configuration, root});
  } catch (const std::bad_alloc&) {
    return create_provenance_resolution_failure_from_field<M4ProvenanceResolver>(
        "m4_provenance.capacity_allocation");
  } catch (const std::length_error&) {
    return create_provenance_resolution_failure_from_field<M4ProvenanceResolver>(
        "m4_provenance.capacity_allocation");
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Produce the root-only lineage/runtime profile with typed subject absence.
model::M4Provenance M4ProvenanceResolver::create_root_only_provenance() const noexcept {
  return model::M4Provenance{root_};
}

// --------------------------------------------------------
// Require an exact configured account/venue binding and derive its owning firm.
model::Result<model::M4Provenance> M4ProvenanceResolver::create_configured_account_provenance(
    const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // A local account/source fact requires exact sealed ownership and venue binding.
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  if (binding == nullptr) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.logical_account_id");
  }
  if (binding->venue_id != venue_id) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.venue_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the closed account/venue/firm shape and no local-order attribution.
  auto subject =
      model::M4SubjectProvenance{logical_account_id, venue_id,     binding->firm_id, std::nullopt,
                                 std::nullopt,       std::nullopt, std::nullopt,     std::nullopt};
  return model::Result<model::M4Provenance>::create_success(
      model::M4Provenance{root_, std::move(subject)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Require an exact configured account/venue/instrument tuple and derive metadata revision.
model::Result<model::M4Provenance> M4ProvenanceResolver::create_configured_instrument_provenance(
    const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id,
    const model::InstrumentId& instrument_id) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish exact account ownership before instrument support can be claimed.
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  if (binding == nullptr) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.logical_account_id");
  }
  if (binding->venue_id != venue_id) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.venue_id");
  }
  const auto* const metadata = configuration_.find_instrument_metadata(venue_id, instrument_id);
  if (metadata == nullptr) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.instrument_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the configured account and supported-instrument shape as one closed value.
  auto subject = model::M4SubjectProvenance{
      logical_account_id, venue_id,
      binding->firm_id,   std::nullopt,
      std::nullopt,       std::nullopt,
      std::nullopt,       model::M4InstrumentSubject{instrument_id, metadata->revision()}};
  return model::Result<model::M4Provenance>::create_success(
      model::M4Provenance{root_, std::move(subject)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the maximal source provenance independently proved for an authoritative raw subject.
model::M4Provenance M4ProvenanceResolver::derive_authoritative_source_provenance(
    const model::LogicalAccountId& logical_account_id, const model::VenueId& venue_id,
    const std::optional<model::InstrumentId>& instrument_id) const noexcept {

  // ++++++++++++++++++++++++++++++++++++++++
  // Firm ownership is independently provable from a known account even when its claimed venue is
  // contradictory; exact venue compatibility is classified later by the private-event owner.
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  std::optional<model::FirmId> firm_id;
  if (binding != nullptr) {
    firm_id = binding->firm_id;
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Instrument support requires the account's exact configured venue plus configured metadata.
  std::optional<model::M4InstrumentSubject> instrument_subject;
  if (binding != nullptr && binding->venue_id == venue_id && instrument_id.has_value()) {
    const auto* const metadata = configuration_.find_instrument_metadata(venue_id, *instrument_id);
    if (metadata != nullptr) {
      instrument_subject = model::M4InstrumentSubject{*instrument_id, metadata->revision()};
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Unknown authoritative provenance never receives local desk/bot/strategy/route ownership.
  auto subject = model::M4SubjectProvenance{
      logical_account_id, venue_id,     std::move(firm_id), std::nullopt,
      std::nullopt,       std::nullopt, std::nullopt,       std::move(instrument_subject)};
  return model::M4Provenance{root_, std::move(subject)};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Return whether sealed configuration proves the exact logical-account and venue binding.
bool M4ProvenanceResolver::has_configured_account_venue_binding(
    const model::LogicalAccountId& logical_account_id,
    const model::VenueId& venue_id) const noexcept {
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  return binding != nullptr && binding->venue_id == venue_id;
}

// --------------------------------------------------------
// Validate the complete immutable admission authority, then copy its full known-order provenance.
model::Result<model::M4Provenance> M4ProvenanceResolver::derive_retained_order_provenance(
    const oms::OutboundOrderRecord& retained_order) const {
  const auto& provenance = retained_order.provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Require every retained root projection to agree with this exact sealed configuration and M4
  // policy root before any subject lookup can enrich the row.
  if (provenance.configuration_fingerprint != configuration_.fingerprint().bytes() ||
      provenance.configuration_revision != configuration_.revision() ||
      provenance.organization_revision != configuration_.organization().revision() ||
      provenance.runtime_policy_fingerprint != root_.runtime_policy_fingerprint() ||
      provenance.risk_policy_fingerprint != root_.risk_policy_fingerprint() ||
      provenance.risk_policy_revision != root_.risk_policy_revision() ||
      provenance.submission_policy_fingerprint != root_.submission_policy_fingerprint()) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.retained_order_root");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the retained account only from sealed configuration and require its exact owner and
  // venue fields to match the immutable row.
  const auto* const binding = configuration_.find_logical_account(provenance.logical_account_id);
  if (binding == nullptr || binding->venue_id != provenance.venue_id ||
      binding->firm_id != provenance.firm_id) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.retained_order_account");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Resolve the exact route under its sealed revision and require every route-owned subject field
  // to agree with the retained row.
  const auto* const route = configuration_.routes().find_route(provenance.route_id);
  if (configuration_.routes().revision() != provenance.route_revision || route == nullptr ||
      route->logical_account_id != provenance.logical_account_id ||
      route->venue_id != provenance.venue_id || route->instrument_id != provenance.instrument_id ||
      route->bot_id != provenance.bot_id) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.retained_order_route");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Re-derive the complete bot attribution instead of treating the retained desk, firm, or
  // strategy fields as construction authority.
  const auto* const attribution = configuration_.organization().find_bot(provenance.bot_id);
  if (attribution == nullptr || attribution->firm_id != provenance.firm_id ||
      attribution->desk_id != provenance.desk_id ||
      attribution->strategy_id != provenance.strategy_id) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.retained_order_attribution");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Re-derive the venue-native instrument and revision from sealed metadata before publishing the
  // known-order instrument subject.
  const auto* const metadata =
      configuration_.find_instrument_metadata(provenance.venue_id, provenance.instrument_id);
  if (metadata == nullptr || metadata->venue_instrument_id() != provenance.venue_instrument_id ||
      metadata->revision() != provenance.metadata_revision) {
    return create_provenance_resolution_failure_from_field<model::M4Provenance>(
        "m4_provenance.retained_order_instrument");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish all and only the complete owner-derived subject fields paired with this resolver's
  // independently validated seven-field root.
  auto subject = model::M4SubjectProvenance{
      provenance.logical_account_id,
      provenance.venue_id,
      provenance.firm_id,
      provenance.desk_id,
      provenance.bot_id,
      provenance.strategy_id,
      model::M4RouteSubject{provenance.route_id, provenance.route_revision},
      model::M4InstrumentSubject{provenance.instrument_id, provenance.metadata_revision}};
  return model::Result<model::M4Provenance>::create_success(
      model::M4Provenance{root_, std::move(subject)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace aegis::runtime
