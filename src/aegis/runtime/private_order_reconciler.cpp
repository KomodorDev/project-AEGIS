// Purpose: validate one pristine submission-owner/M4-policy relationship and preallocate its
// dormant empty private-identity storage without activation, recovery, planning, or mutation.

#include "private_order_reconciler.hpp"

#include "aegis/model/domain_error.hpp"
#include "submission_coordinator.hpp"

#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace aegis::runtime {
namespace {

// --------------------------------------------------------
// Return an install-time failure through the stable M4 policy domain without publishing a child.
template <typename Value>
[[nodiscard]] model::Result<Value> private_order_reconciler_failure_from_field(std::string field) {
  return model::Result<Value>::failure(
      model::DomainError::at_field(model::DomainErrorCode::InvalidM4Policy, std::move(field)));
}

// --------------------------------------------------------
// Return whether all three dormant table capacities remain exactly representable by their u32
// inspection surface, even if a future policy construction path regresses its accepted bound.
[[nodiscard]] bool are_private_identity_storage_capacities_implementable(
    const M4PolicyCapacities& capacities) noexcept {
  return std::in_range<std::uint32_t>(capacities.max_event_identity_records) &&
         std::in_range<std::uint32_t>(capacities.max_trade_identity_records) &&
         std::in_range<std::uint32_t>(capacities.max_exchange_order_mappings);
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Validate every inherited authority and allocate all empty identity slots before owner
// publication; any mismatch or allocation failure returns InvalidM4Policy and leaves the owner
// unchanged.
model::Result<std::unique_ptr<PrivateOrderReconciler>>
PrivateOrderReconciler::create_dormant_private_order_reconciler(
    const SubmissionCoordinator& owner, const configuration::StartupConfiguration& configuration,
    const M4Policy& policy) try {
  const auto& root = policy.root_provenance();

  // ++++++++++++++++++++++++++++++++++++++++
  // Bind to exact risk, submission, runtime, and configuration identities using immutable public
  // owner views; this dormant child receives no direct mutation friendship from the coordinator.
  if (root.risk_policy_fingerprint() != owner.reservations().policy().fingerprint().bytes() ||
      root.risk_policy_revision() != owner.reservations().policy().revision() ||
      root.submission_policy_fingerprint() != owner.policy().fingerprint().bytes() ||
      root.runtime_policy_fingerprint() != owner.policy().runtime_policy_fingerprint() ||
      root.configuration_fingerprint() != owner.policy().configuration_fingerprint()) {
    return private_order_reconciler_failure_from_field<std::unique_ptr<PrivateOrderReconciler>>(
        "private_order_reconciler.owner_provenance");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the trusted resolver to prove configuration/organization agreement before copying its
  // dormant source-normalization authority into the child. Preserve allocation as the earlier
  // capacity failure class instead of misreporting it as provenance disagreement.
  auto resolver = M4ProvenanceResolver::create(configuration, policy);
  if (!resolver) {
    const auto* const failure_field =
        resolver.error().context.field == "m4_provenance.capacity_allocation"
            ? "private_order_reconciler.capacity_allocation"
            : "private_order_reconciler.configuration_provenance";
    return private_order_reconciler_failure_from_field<std::unique_ptr<PrivateOrderReconciler>>(
        failure_field);
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Recheck the implementation widths and the exact owner's permanent mapping lower bound before
  // any fixed storage is allocated or installed.
  const auto& capacities = policy.capacities();
  if (!are_private_identity_storage_capacities_implementable(capacities) ||
      capacities.max_exchange_order_mappings < owner.outbound_oms().capacity()) {
    return private_order_reconciler_failure_from_field<std::unique_ptr<PrivateOrderReconciler>>(
        "private_order_reconciler.capacities");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize every slot as empty local state; this boundary provides no operation that can
  // populate an event, trade, or mapping record.
  auto event_identity_records = std::vector<std::optional<PrivateEventIdentityRecord>>(
      static_cast<std::size_t>(capacities.max_event_identity_records));
  auto trade_identity_records = std::vector<std::optional<PrivateTradeIdentityRecord>>(
      static_cast<std::size_t>(capacities.max_trade_identity_records));
  auto exchange_order_mappings = std::vector<std::optional<PrivateExchangeOrderMapping>>(
      static_cast<std::size_t>(capacities.max_exchange_order_mappings));
  auto event_factory = PrivateOrderEventFactory{std::move(resolver).value()};
  auto owned_m4_policy = policy;

  // ++++++++++++++++++++++++++++++++++++++++
  // Interesting syntax: these traits prove every already-allocated member transfers without
  // throwing; only the final `new` allocation remains fallible and is translated by the function
  // try block. The coordinator still decides whether to publish the temporary result.
  static_assert(std::is_nothrow_move_constructible_v<M4Policy>);
  static_assert(std::is_nothrow_move_constructible_v<PrivateOrderEventFactory>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(event_identity_records)>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(trade_identity_records)>);
  static_assert(std::is_nothrow_move_constructible_v<decltype(exchange_order_mappings)>);
  static_assert(std::is_nothrow_destructible_v<PrivateOrderReconciler>);
  auto reconciler = std::unique_ptr<PrivateOrderReconciler>{new PrivateOrderReconciler{
      owner, std::move(owned_m4_policy), std::move(event_factory),
      std::move(event_identity_records), std::move(trade_identity_records),
      std::move(exchange_order_mappings)}};
  return model::Result<std::unique_ptr<PrivateOrderReconciler>>::success(std::move(reconciler));

  // ++++++++++++++++++++++++++++++++++++++++
} catch (const std::bad_alloc&) {
  return private_order_reconciler_failure_from_field<std::unique_ptr<PrivateOrderReconciler>>(
      "private_order_reconciler.capacity_allocation");
} catch (const std::length_error&) {
  return private_order_reconciler_failure_from_field<std::unique_ptr<PrivateOrderReconciler>>(
      "private_order_reconciler.capacity_allocation");
}

// --------------------------------------------------------
// Retain the stable owner pointer and fully allocated empty tables; unique ownership guarantees the
// child is destroyed before the coordinator address or any inspected M3 component becomes invalid.
PrivateOrderReconciler::PrivateOrderReconciler(
    const SubmissionCoordinator& owner, M4Policy m4_policy, PrivateOrderEventFactory event_factory,
    std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
    std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
    std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings) noexcept
    : owner_{&owner}, m4_policy_{std::move(m4_policy)}, event_factory_{std::move(event_factory)},
      event_identity_records_{std::move(event_identity_records)},
      trade_identity_records_{std::move(trade_identity_records)},
      exchange_order_mappings_{std::move(exchange_order_mappings)} {}

// --------------------------------------------------------

} // namespace aegis::runtime
