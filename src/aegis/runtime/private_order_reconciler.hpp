// Purpose: define dormant fixed-capacity private-identity storage bound to one pristine submission
// owner and expose only immutable policy, capacity, and empty-count inspection.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/oms/private_order_event.hpp"
#include "aegis/oms/private_order_identity.hpp"
#include "aegis/oms/private_order_resolution.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "private_order_event_factory.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace aegis::runtime {

// ########################################################################
// The pristine coordinator owns and destroys the dormant child while remaining at one stable
// address.
class SubmissionCoordinator;

// ########################################################################

// ########################################################################
// One event-identity slot schema pairs timestamp-free ingress semantics with an immutable
// resolution and original disposition. This slice allocates slots but never populates them.
struct PrivateEventIdentityRecord {
  oms::PrivateEventRegistryKey key;
  oms::PrivateEventIngressSemanticValue ingress_semantic_value;
  oms::PrivateEventResolution resolution;
  oms::PrivateEventDisposition original_disposition;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares every slot field without publishing
  // a separate comparison capability.
  friend bool operator==(const PrivateEventIdentityRecord&,
                         const PrivateEventIdentityRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One trade-identity slot schema pairs a complete semantic tuple with its original and current
// dispositions. This slice allocates slots but never populates or changes them.
struct PrivateTradeIdentityRecord {
  oms::TradeKey key;
  oms::PrivateTradeSemanticValue semantic_value;
  oms::PrivateEventDisposition original_disposition;
  oms::PrivateEventDisposition current_disposition;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares the complete inert slot value while
  // keeping equality discoverable only through the type itself.
  friend bool operator==(const PrivateTradeIdentityRecord&,
                         const PrivateTradeIdentityRecord&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// One exchange-mapping slot schema binds a venue/account-scoped exchange key to one local order.
// This slice allocates slots but never populates them.
struct PrivateExchangeOrderMapping {
  oms::ExchangeOrderKey exchange_order_key;
  model::OrderId order_id;

  // --------------------------------------------------------
  // Interesting syntax: the defaulted hidden friend compares the complete nominal mapping without
  // introducing a generic public comparison helper.
  friend bool operator==(const PrivateExchangeOrderMapping&,
                         const PrivateExchangeOrderMapping&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// ########################################################################
// The dormant pre-activation child is permanently bound to one nonmoving coordinator and owns fully
// preallocated empty identity tables. Its invariant is that every slot stays empty and every count
// stays zero; it exposes no activation, planning, insertion, recovery, consumption, or
// event-application operation.
class PrivateOrderReconciler final {
public:

  // --------------------------------------------------------
  // Preserve the owner pointer and fixed storage addresses for the child's complete lifetime.
  PrivateOrderReconciler(const PrivateOrderReconciler&) = delete;
  PrivateOrderReconciler& operator=(const PrivateOrderReconciler&) = delete;
  PrivateOrderReconciler(PrivateOrderReconciler&&) = delete;
  PrivateOrderReconciler& operator=(PrivateOrderReconciler&&) = delete;

  // --------------------------------------------------------
  // Return the fixed number of preallocated event-identity slots.
  [[nodiscard]] std::uint32_t event_identity_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(event_identity_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of preallocated trade-identity slots.
  [[nodiscard]] std::uint32_t trade_identity_record_capacity() const noexcept {
    return static_cast<std::uint32_t>(trade_identity_records_.size());
  }

  // --------------------------------------------------------
  // Return the fixed number of preallocated exchange-order mapping slots.
  [[nodiscard]] std::uint32_t exchange_order_mapping_capacity() const noexcept {
    return static_cast<std::uint32_t>(exchange_order_mappings_.size());
  }

  // --------------------------------------------------------
  // Return zero because this dormant boundary cannot populate an event-identity slot.
  [[nodiscard]] std::uint32_t event_identity_record_count() const noexcept {
    return event_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return zero because this dormant boundary cannot populate a trade-identity slot.
  [[nodiscard]] std::uint32_t trade_identity_record_count() const noexcept {
    return trade_identity_record_count_;
  }

  // --------------------------------------------------------
  // Return zero because this dormant boundary cannot populate an exchange-order mapping slot.
  [[nodiscard]] std::uint32_t exchange_order_mapping_count() const noexcept {
    return exchange_order_mapping_count_;
  }

  // --------------------------------------------------------
  // Borrow the immutable policy copy that authorized and sized this child.
  [[nodiscard]] const M4Policy& m4_policy() const noexcept { return m4_policy_; }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Validate exact owner/configuration/policy agreement and allocate every empty slot before
  // returning a child; InvalidM4Policy or allocation failure returns an error without publication.
  [[nodiscard]] static model::Result<std::unique_ptr<PrivateOrderReconciler>>
  create_dormant_private_order_reconciler(const SubmissionCoordinator& owner,
                                          const configuration::StartupConfiguration& configuration,
                                          const M4Policy& policy);

  // --------------------------------------------------------
  // Retain only a const view of the already stable owner and fully allocated empty tables; the
  // owner must outlive this child, which unique ownership and member destruction order guarantee.
  PrivateOrderReconciler(
      const SubmissionCoordinator& owner, M4Policy m4_policy,
      PrivateOrderEventFactory event_factory,
      std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records,
      std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records,
      std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings) noexcept;

  // --------------------------------------------------------
  // Retain dormant authority and fixed storage; no operation in this slice changes these members.
  const SubmissionCoordinator* owner_;
  M4Policy m4_policy_;
  PrivateOrderEventFactory event_factory_;
  std::vector<std::optional<PrivateEventIdentityRecord>> event_identity_records_;
  std::vector<std::optional<PrivateTradeIdentityRecord>> trade_identity_records_;
  std::vector<std::optional<PrivateExchangeOrderMapping>> exchange_order_mappings_;
  std::uint32_t event_identity_record_count_{0U};
  std::uint32_t trade_identity_record_count_{0U};
  std::uint32_t exchange_order_mapping_count_{0U};

  // ########################################################################
  // Interesting syntax: friendship lets only the owning coordinator invoke the private validating
  // factory; it publishes no construction operation to other callers.
  friend class SubmissionCoordinator;

  // ########################################################################
};

// ########################################################################

} // namespace aegis::runtime
