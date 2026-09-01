// Purpose: provide one deterministic sealed M4 source-normalization fixture, raw order locator,
// and typed private/recovery identities for independent unit tests.

#pragma once

#include "aegis/model/fixed_point.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/oms/outbound_oms.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/recovery/recovery_identity.hpp"
#include "aegis/runtime/m4_provenance_resolver.hpp"
#include "aegis/runtime/private_order_event_factory.hpp"
#include "m4_test_authority.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace aegis::test_support {

// --------------------------------------------------------
// Parse one nominal identifier and fail immediately for an invalid test literal.
template <typename Identifier>
[[nodiscard]] Identifier parse_m4_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 private-event fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build one exact decimal value without binary floating-point input.
template <typename Decimal>
[[nodiscard]] Decimal create_m4_decimal_or_throw(std::int64_t coefficient,
                                                 std::uint8_t scale = 0U) {
  auto created = Decimal::from_scaled(coefficient, scale);
  if (!created) {
    throw std::logic_error{"invalid decimal in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Build one exact positive ordinal or revision.
template <typename Identity>
[[nodiscard]] Identity create_m4_ordinal_or_throw(std::uint64_t value) {
  auto created = Identity::from_value(value);
  if (!created) {
    throw std::logic_error{"invalid ordinal in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Build one bounded opaque private identity from a distinguishable single byte.
template <typename Identity>
[[nodiscard]] Identity create_m4_opaque_identity_or_throw(std::uint8_t value) {
  const std::array bytes{std::byte{value}};
  auto created = Identity::from_bytes(std::span<const std::byte>{bytes});
  if (!created) {
    throw std::logic_error{"invalid opaque identity in M4 private-event fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Reuse one deterministic namespace while varying counters to produce exact local order IDs.
[[nodiscard]] inline model::OrderId create_m4_order_id_or_throw(std::uint64_t counter) {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(0x20U + index);
  }
  auto provider = model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
      model::OrderNamespace{bytes}, counter);
  if (!provider) {
    throw std::logic_error{"invalid order provider in M4 private-event fixture"};
  }
  auto identity = provider.value().generate_next_order_id();
  if (!identity) {
    throw std::logic_error{"exhausted order provider in M4 private-event fixture"};
  }
  return identity.value();
}

// --------------------------------------------------------
// Use one distinct deterministic restart namespace for local event/recovery identity families.
[[nodiscard]] inline model::OrderNamespace create_m4_restart_namespace() noexcept {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(0xa0U + index);
  }
  return model::OrderNamespace{bytes};
}

// --------------------------------------------------------
// Construct one local-order-event ID at an exact nonzero counter.
[[nodiscard]] inline oms::LocalOrderEventId
create_m4_local_event_id_or_throw(std::uint64_t counter) {
  auto created = oms::LocalOrderEventId::identity_from_namespace_and_counter(
      create_m4_restart_namespace(), counter);
  if (!created) {
    throw std::logic_error{"invalid local event ID in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Construct one runtime epoch under the same deterministic restart namespace.
[[nodiscard]] inline recovery::RuntimeEpochId
create_m4_runtime_epoch_or_throw(std::uint64_t counter = 1U) {
  auto created = recovery::RuntimeEpochId::identity_from_namespace_and_counter(
      create_m4_restart_namespace(), counter);
  if (!created) {
    throw std::logic_error{"invalid runtime epoch in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Construct one reconciliation epoch under the deterministic runtime epoch.
[[nodiscard]] inline recovery::ReconciliationEpochId
create_m4_reconciliation_epoch_or_throw(std::uint64_t counter = 1U) {
  auto created = recovery::ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
      create_m4_runtime_epoch_or_throw(), counter);
  if (!created) {
    throw std::logic_error{"invalid reconciliation epoch in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------

// ########################################################################
// The fixture owns one sealed authority, two self-contained normalization values, and a fixed raw
// order locator/economics row; that row grants no local-event authority.
class M4PrivateEventFixture final {
public:

  // --------------------------------------------------------
  // Build the real M4 authority and admit one consistent baseline order; throw std::logic_error for
  // any setup defect before a fixture can become observable.
  M4PrivateEventFixture()
      : authority_{create_m4_test_authority_or_throw()},
        resolver_{
            create_m4_provenance_resolver_or_throw(authority_.configuration, authority_.m4_policy)},
        factory_{resolver_}, outbound_{create_outbound_oms_or_throw()} {
    auto admitted = outbound_.admit_outbound_order(create_outbound_order_admission_or_throw());
    if (!admitted || !admitted.value().is_admitted() || admitted.value().record() == nullptr) {
      throw std::logic_error{"failed to admit M4 private-event fixture order"};
    }
    record_ = admitted.value().record();
  }

  // --------------------------------------------------------
  // Keep the fixture at a stable address because record_ points into outbound_; copying or moving
  // would invalidate or misbind that retained-row view.
  M4PrivateEventFixture(const M4PrivateEventFixture&) = delete;
  M4PrivateEventFixture& operator=(const M4PrivateEventFixture&) = delete;
  M4PrivateEventFixture(M4PrivateEventFixture&&) = delete;
  M4PrivateEventFixture& operator=(M4PrivateEventFixture&&) = delete;

  // --------------------------------------------------------
  // Borrow the sealed configuration and M4 policy that authorize every fixture value.
  [[nodiscard]] const M4TestAuthority& test_authority() const noexcept { return authority_; }

  // --------------------------------------------------------
  // Borrow the resolver copied into the factory when the fixture was constructed.
  [[nodiscard]] const runtime::M4ProvenanceResolver& provenance_resolver() const noexcept {
    return resolver_;
  }

  // --------------------------------------------------------
  // Borrow the source-normalization factory that owns its own resolver copy.
  [[nodiscard]] const runtime::PrivateOrderEventFactory& private_event_factory() const noexcept {
    return factory_;
  }

  // --------------------------------------------------------
  // Borrow the baseline OMS row whose existence is established by construction.
  [[nodiscard]] const oms::OutboundOrderRecord& outbound_order_record() const noexcept {
    return *record_;
  }

  // --------------------------------------------------------
  // Return the logical account named by the retained baseline row.
  [[nodiscard]] model::LogicalAccountId account_id() const {
    return outbound_order_record().provenance().logical_account_id;
  }

  // --------------------------------------------------------
  // Return the venue named by the retained baseline row.
  [[nodiscard]] model::VenueId venue_id() const {
    return outbound_order_record().provenance().venue_id;
  }

  // --------------------------------------------------------
  // Return the instrument named by the retained baseline row.
  [[nodiscard]] model::InstrumentId instrument_id() const {
    return outbound_order_record().provenance().instrument_id;
  }

  // --------------------------------------------------------
  // Create a local origin at the authored times, or throw when its event counter is invalid.
  [[nodiscard]] oms::LocalPrivateEventOrigin
  create_local_private_event_origin_or_throw(std::uint64_t event_counter = 1U,
                                             std::uint64_t source_time = 100U,
                                             std::uint64_t receive_time = 200U) const {
    return oms::LocalPrivateEventOrigin{create_m4_local_event_id_or_throw(event_counter),
                                        model::SourceTimestamp{source_time},
                                        model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  // Create a venue origin under the retained account and venue, or throw for an invalid identity.
  [[nodiscard]] oms::VenuePrivateEventOrigin
  create_venue_private_event_origin_or_throw(std::uint8_t event_byte = 1U,
                                             std::uint64_t source_time = 100U,
                                             std::uint64_t receive_time = 200U) const {
    return oms::VenuePrivateEventOrigin{
        oms::VenuePrivateEventKey{
            venue_id(), account_id(),
            create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U),
            create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event_byte)},
        model::SourceTimestamp{source_time}, model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  // Create a reconciliation origin at the authored cut, or throw for an invalid row identity.
  [[nodiscard]] oms::ReconciliationPrivateEventOrigin
  create_reconciliation_private_event_origin_or_throw(std::uint64_t row = 1U,
                                                      std::uint64_t cut_time = 100U,
                                                      std::uint64_t receive_time = 200U) const {
    return oms::ReconciliationPrivateEventOrigin{
        create_m4_reconciliation_epoch_or_throw(),
        create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
        create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(row),
        model::SourceTimestamp{cut_time}, model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  // Create an attempt for the retained order, or throw when the requested ordinal is invalid.
  [[nodiscard]] oms::CancelAttemptId
  create_cancel_attempt_id_or_throw(std::uint64_t ordinal = 1U) const {
    auto created = oms::CancelAttemptId::cancel_attempt_id_from_components(
        create_m4_runtime_epoch_or_throw(), outbound_order_record().order_id(), ordinal);
    if (!created) {
      throw std::logic_error{"invalid cancel attempt in M4 private-event fixture"};
    }
    return created.value();
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Extract a validated resolver value without exposing a test-only construction bypass.
  [[nodiscard]] static runtime::M4ProvenanceResolver
  create_m4_provenance_resolver_or_throw(const configuration::StartupConfiguration& configuration,
                                         const runtime::M4Policy& policy) {
    auto created =
        runtime::M4ProvenanceResolver::create_m4_provenance_resolver(configuration, policy);
    if (!created) {
      throw std::logic_error{"invalid M4 resolver in private-event fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Build one small fixed OMS table for the baseline retained row.
  [[nodiscard]] static oms::OutboundOms create_outbound_oms_or_throw() {
    auto created = oms::OutboundOms::create_outbound_oms(4U);
    if (!created) {
      throw std::logic_error{"invalid OMS in M4 private-event fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Derive complete M3 retained provenance from the same sealed M4 authority.
  [[nodiscard]] oms::OutboundOrderAdmission create_outbound_order_admission_or_throw() const {
    const auto route_id =
        parse_m4_identifier_or_throw<model::RouteId>("route.deribit-testnet-btc-perpetual");
    const auto* const route = authority_.configuration.routes().find_route(route_id);
    if (route == nullptr) {
      throw std::logic_error{"missing route in M4 private-event fixture"};
    }
    const auto* const attribution = authority_.configuration.organization().find_bot(route->bot_id);
    const auto* const metadata =
        authority_.configuration.find_instrument_metadata(route->venue_id, route->instrument_id);
    if (attribution == nullptr || metadata == nullptr) {
      throw std::logic_error{"missing attribution in M4 private-event fixture"};
    }
    const auto& root = authority_.m4_policy.root_provenance();
    const auto quantity = create_m4_decimal_or_throw<model::Quantity>(3);
    return oms::OutboundOrderAdmission{
        create_m4_ordinal_or_throw<model::SubmissionAttemptId>(1U),
        create_m4_order_id_or_throw(1U),
        create_m4_ordinal_or_throw<model::ReservationId>(1U),
        execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                           execution::TimeInForce::GoodTilCancelled,
                                           create_m4_decimal_or_throw<model::Price>(10), quantity},
        risk::OrderExposure{quantity, create_m4_decimal_or_throw<model::Notional>(30)},
        oms::OutboundOrderProvenance{
            route->id,
            route->venue_id,
            route->logical_account_id,
            route->instrument_id,
            metadata->venue_instrument_id(),
            attribution->firm_id,
            attribution->desk_id,
            attribution->bot_id,
            attribution->strategy_id,
            root.configuration_fingerprint(),
            authority_.configuration.revision(),
            root.organization_revision(),
            authority_.configuration.routes().revision(),
            metadata->revision(),
            root.runtime_policy_fingerprint(),
            root.risk_policy_fingerprint(),
            root.risk_policy_revision(),
            root.submission_policy_fingerprint(),
        }};
  }

  // --------------------------------------------------------
  // Retain the complete sealed authority, copied factories, stable OMS storage, and row view.
  M4TestAuthority authority_;
  runtime::M4ProvenanceResolver resolver_;
  runtime::PrivateOrderEventFactory factory_;
  oms::OutboundOms outbound_;
  const oms::OutboundOrderRecord* record_{nullptr};
};

// ########################################################################

} // namespace aegis::test_support
