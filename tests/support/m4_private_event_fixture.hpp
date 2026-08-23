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
template <typename Identifier> [[nodiscard]] Identifier m4_id(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 private-event fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build one exact decimal value without binary floating-point input.
template <typename Decimal>
[[nodiscard]] Decimal m4_decimal(std::int64_t coefficient, std::uint8_t scale = 0U) {
  auto created = Decimal::from_scaled(coefficient, scale);
  if (!created) {
    throw std::logic_error{"invalid decimal in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Build one exact positive ordinal or revision.
template <typename Identity> [[nodiscard]] Identity m4_ordinal(std::uint64_t value) {
  auto created = Identity::from_value(value);
  if (!created) {
    throw std::logic_error{"invalid ordinal in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Build one bounded opaque private identity from a distinguishable single byte.
template <typename Identity> [[nodiscard]] Identity m4_opaque(std::uint8_t value) {
  const std::array bytes{std::byte{value}};
  auto created = Identity::from_bytes(std::span<const std::byte>{bytes});
  if (!created) {
    throw std::logic_error{"invalid opaque identity in M4 private-event fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Reuse one deterministic namespace while varying counters to produce exact local order IDs.
[[nodiscard]] inline model::OrderId m4_order_id(std::uint64_t counter) {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(0x20U + index);
  }
  auto provider =
      model::DeterministicOrderIdProvider::create(model::OrderNamespace{bytes}, counter);
  if (!provider) {
    throw std::logic_error{"invalid order provider in M4 private-event fixture"};
  }
  auto identity = provider.value().next();
  if (!identity) {
    throw std::logic_error{"exhausted order provider in M4 private-event fixture"};
  }
  return identity.value();
}

// --------------------------------------------------------
// Use one distinct deterministic restart namespace for local event/recovery identity families.
[[nodiscard]] inline model::OrderNamespace m4_restart_namespace() noexcept {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(0xa0U + index);
  }
  return model::OrderNamespace{bytes};
}

// --------------------------------------------------------
// Construct one local-order-event ID at an exact nonzero counter.
[[nodiscard]] inline oms::LocalOrderEventId m4_local_event_id(std::uint64_t counter) {
  auto created = oms::LocalOrderEventId::from_parts(m4_restart_namespace(), counter);
  if (!created) {
    throw std::logic_error{"invalid local event ID in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Construct one runtime epoch under the same deterministic restart namespace.
[[nodiscard]] inline recovery::RuntimeEpochId m4_runtime_epoch(std::uint64_t counter = 1U) {
  auto created = recovery::RuntimeEpochId::from_parts(m4_restart_namespace(), counter);
  if (!created) {
    throw std::logic_error{"invalid runtime epoch in M4 private-event fixture"};
  }
  return created.value();
}

// --------------------------------------------------------
// Construct one reconciliation epoch under the deterministic runtime epoch.
[[nodiscard]] inline recovery::ReconciliationEpochId
m4_reconciliation_epoch(std::uint64_t counter = 1U) {
  auto created = recovery::ReconciliationEpochId::from_parts(m4_runtime_epoch(), counter);
  if (!created) {
    throw std::logic_error{"invalid reconciliation epoch in M4 private-event fixture"};
  }
  return created.value();
}

// ########################################################################
// The fixture owns one sealed authority, two self-contained normalization values, and a fixed raw
// order locator/economics row; that row grants no local-event authority.
class M4PrivateEventFixture final {
public:

  // --------------------------------------------------------
  // Build the real M4 authority and admit one internally consistent baseline order.
  M4PrivateEventFixture()
      : authority_{m4_test_authority()},
        resolver_{resolver(authority_.configuration, authority_.m4_policy)}, factory_{resolver_},
        outbound_{outbound()} {
    auto admitted = outbound_.admit(admission());
    if (!admitted || !admitted.value().admitted() || admitted.value().record() == nullptr) {
      throw std::logic_error{"failed to admit M4 private-event fixture order"};
    }
    record_ = admitted.value().record();
  }

  // --------------------------------------------------------
  M4PrivateEventFixture(const M4PrivateEventFixture&) = delete;
  M4PrivateEventFixture& operator=(const M4PrivateEventFixture&) = delete;
  M4PrivateEventFixture(M4PrivateEventFixture&&) = delete;
  M4PrivateEventFixture& operator=(M4PrivateEventFixture&&) = delete;

  // --------------------------------------------------------
  [[nodiscard]] const M4TestAuthority& authority() const noexcept { return authority_; }

  // --------------------------------------------------------
  [[nodiscard]] const runtime::M4ProvenanceResolver& resolver() const noexcept { return resolver_; }

  // --------------------------------------------------------
  [[nodiscard]] const runtime::PrivateOrderEventFactory& factory() const noexcept {
    return factory_;
  }

  // --------------------------------------------------------
  [[nodiscard]] const oms::OutboundOrderRecord& record() const noexcept { return *record_; }

  // --------------------------------------------------------
  [[nodiscard]] model::LogicalAccountId account_id() const {
    return record().provenance().logical_account_id;
  }

  // --------------------------------------------------------
  [[nodiscard]] model::VenueId venue_id() const { return record().provenance().venue_id; }

  // --------------------------------------------------------
  [[nodiscard]] model::InstrumentId instrument_id() const {
    return record().provenance().instrument_id;
  }

  // --------------------------------------------------------
  [[nodiscard]] oms::LocalPrivateEventOrigin local_origin(std::uint64_t event_counter = 1U,
                                                          std::uint64_t source_time = 100U,
                                                          std::uint64_t receive_time = 200U) const {
    return oms::LocalPrivateEventOrigin{m4_local_event_id(event_counter),
                                        model::SourceTimestamp{source_time},
                                        model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  [[nodiscard]] oms::VenuePrivateEventOrigin venue_origin(std::uint8_t event_byte = 1U,
                                                          std::uint64_t source_time = 100U,
                                                          std::uint64_t receive_time = 200U) const {
    return oms::VenuePrivateEventOrigin{
        oms::VenuePrivateEventKey{venue_id(), account_id(),
                                  m4_opaque<oms::PrivateSourceEpochId>(0x41U),
                                  m4_opaque<oms::PrivateEventId>(event_byte)},
        model::SourceTimestamp{source_time}, model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  [[nodiscard]] oms::ReconciliationPrivateEventOrigin
  reconciliation_origin(std::uint64_t row = 1U, std::uint64_t cut_time = 100U,
                        std::uint64_t receive_time = 200U) const {
    return oms::ReconciliationPrivateEventOrigin{
        m4_reconciliation_epoch(), m4_opaque<oms::AuthoritativeCutId>(0x51U),
        m4_ordinal<recovery::ReconciliationRowOrdinal>(row), model::SourceTimestamp{cut_time},
        model::ReceiveTimestamp{receive_time}};
  }

  // --------------------------------------------------------
  [[nodiscard]] oms::CancelAttemptId cancel_attempt(std::uint64_t ordinal = 1U) const {
    auto created =
        oms::CancelAttemptId::from_parts(m4_runtime_epoch(), record().order_id(), ordinal);
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
  resolver(const configuration::StartupConfiguration& configuration,
           const runtime::M4Policy& policy) {
    auto created = runtime::M4ProvenanceResolver::create(configuration, policy);
    if (!created) {
      throw std::logic_error{"invalid M4 resolver in private-event fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Build one small fixed OMS table for the baseline retained row.
  [[nodiscard]] static oms::OutboundOms outbound() {
    auto created = oms::OutboundOms::create(4U);
    if (!created) {
      throw std::logic_error{"invalid OMS in M4 private-event fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Derive complete M3 retained provenance from the same sealed M4 authority.
  [[nodiscard]] oms::OutboundOrderAdmission admission() const {
    const auto route_id = m4_id<model::RouteId>("route.deribit-testnet-btc-perpetual");
    const auto* const route = authority_.configuration.routes().find(route_id);
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
    const auto quantity = m4_decimal<model::Quantity>(3);
    return oms::OutboundOrderAdmission{
        m4_ordinal<model::SubmissionAttemptId>(1U),
        m4_order_id(1U),
        m4_ordinal<model::ReservationId>(1U),
        execution::CanonicalOrderEconomics{execution::OrderSide::Buy, execution::OrderType::Limit,
                                           execution::TimeInForce::GoodTilCancelled,
                                           m4_decimal<model::Price>(10), quantity},
        risk::OrderExposure{quantity, m4_decimal<model::Notional>(30)},
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
  M4TestAuthority authority_;
  runtime::M4ProvenanceResolver resolver_;
  runtime::PrivateOrderEventFactory factory_;
  oms::OutboundOms outbound_;
  const oms::OutboundOrderRecord* record_{nullptr};
};

// ########################################################################

} // namespace aegis::test_support
