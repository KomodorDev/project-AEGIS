// Purpose: validate root/configuration agreement and derive source-normalization provenance without
// trusting caller-supplied organization, route, or metadata attribution.

#include "m4_provenance_resolver.hpp"

#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Report every provenance-authority mismatch through the stable private-event validation domain.
template <typename Value> [[nodiscard]] model::Result<Value> invalid(std::string field) {
  return model::Result<Value>::failure(
      model::DomainError::at_field(model::DomainErrorCode::InvalidPrivateEvent, std::move(field)));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Reject a configuration/root mismatch before any provenance value can be published.
model::Result<M4ProvenanceResolver>
M4ProvenanceResolver::create(const configuration::StartupConfiguration& configuration,
                             const M4Policy& policy) {

  // ++++++++++++++++++++++++++++++++++++++++
  // The root must identify this exact sealed configuration and organization authority.
  const auto& root = policy.root_provenance();
  if (root.configuration_fingerprint() != configuration.fingerprint().bytes()) {
    return invalid<M4ProvenanceResolver>("m4_provenance.configuration_fingerprint");
  }
  if (root.organization_revision() != configuration.organization().revision()) {
    return invalid<M4ProvenanceResolver>("m4_provenance.organization_revision");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Copy the sealed configuration into the resolver so later derivation has no borrowed lifetime.
  try {
    return model::Result<M4ProvenanceResolver>::success(M4ProvenanceResolver{configuration, root});
  } catch (const std::bad_alloc&) {
    return invalid<M4ProvenanceResolver>("m4_provenance.capacity_allocation");
  } catch (const std::length_error&) {
    return invalid<M4ProvenanceResolver>("m4_provenance.capacity_allocation");
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Produce the root-only lineage/runtime profile with typed subject absence.
model::M4Provenance M4ProvenanceResolver::root_only() const noexcept {
  return model::M4Provenance{root_};
}

// --------------------------------------------------------
// Require an exact configured account/venue binding and derive its owning firm.
model::Result<model::M4Provenance>
M4ProvenanceResolver::configured_account(const model::LogicalAccountId& logical_account_id,
                                         const model::VenueId& venue_id) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // A local account/source fact requires exact sealed ownership and venue binding.
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  if (binding == nullptr) {
    return invalid<model::M4Provenance>("m4_provenance.logical_account_id");
  }
  if (binding->venue_id != venue_id) {
    return invalid<model::M4Provenance>("m4_provenance.venue_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the closed account/venue/firm shape and no local-order attribution.
  auto subject =
      model::M4SubjectProvenance{logical_account_id, venue_id,     binding->firm_id, std::nullopt,
                                 std::nullopt,       std::nullopt, std::nullopt,     std::nullopt};
  return model::Result<model::M4Provenance>::success(
      model::M4Provenance{root_, std::move(subject)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Require an exact configured account/venue/instrument tuple and derive metadata revision.
model::Result<model::M4Provenance>
M4ProvenanceResolver::configured_instrument(const model::LogicalAccountId& logical_account_id,
                                            const model::VenueId& venue_id,
                                            const model::InstrumentId& instrument_id) const {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish exact account ownership before instrument support can be claimed.
  const auto* const binding = configuration_.find_logical_account(logical_account_id);
  if (binding == nullptr) {
    return invalid<model::M4Provenance>("m4_provenance.logical_account_id");
  }
  if (binding->venue_id != venue_id) {
    return invalid<model::M4Provenance>("m4_provenance.venue_id");
  }
  const auto* const metadata = configuration_.find_instrument_metadata(venue_id, instrument_id);
  if (metadata == nullptr) {
    return invalid<model::M4Provenance>("m4_provenance.instrument_id");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the configured account and supported-instrument shape as one closed value.
  auto subject = model::M4SubjectProvenance{
      logical_account_id, venue_id,
      binding->firm_id,   std::nullopt,
      std::nullopt,       std::nullopt,
      std::nullopt,       model::M4InstrumentSubject{instrument_id, metadata->revision()}};
  return model::Result<model::M4Provenance>::success(
      model::M4Provenance{root_, std::move(subject)});

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Retain the maximal source provenance independently proved for an authoritative raw subject.
model::M4Provenance M4ProvenanceResolver::authoritative_source(
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

} // namespace aegis::runtime
