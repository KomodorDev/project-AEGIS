// Purpose: retain bounded typed journal history across deterministic M4 runtime incarnations and
// supply one move-only namespace-acknowledged bootstrap to the exact private runtime owner.

#pragma once

#include "aegis/model/m4_provenance.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/model/result.hpp"
#include "aegis/recovery/journal_record.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace aegis::runtime {

// ########################################################################
// The immutable M4 policy supplies the fake medium's exact capacities and root authority.
class M4Policy;

// ########################################################################
// The source-private reconciler alone may consume a completed bootstrap during owner installation.
class PrivateOrderReconciler;

// ########################################################################

} // namespace aegis::runtime

namespace aegis::recovery {
namespace detail {

// ########################################################################
// One shared backing allocation outlives either the external handle or a live runtime lease.
struct FakeRecoveryBacking;

// ########################################################################
// Shared lease control releases the single-incarnation flag after every installed authority that
// depends on the acknowledged namespace has been destroyed.
struct FakeJournalLeaseControl;

// ########################################################################

} // namespace detail

// ########################################################################
// A successful bootstrap proves one namespace record is published and fake-acknowledged, owns the
// exclusive live-incarnation lease, and keeps its deterministic client-identity provider sealed.
// It may be consumed only by the exact private-owner installation transaction after all fallible
// work succeeds. No public API exposes append, acknowledgement, provider extraction, replay, or
// business-state authority.
class RecoveryBootstrap final {
public:

  // --------------------------------------------------------
  // Copying would duplicate both the live journal lease and the registered order-ID stream.
  RecoveryBootstrap(const RecoveryBootstrap&) = delete;
  RecoveryBootstrap& operator=(const RecoveryBootstrap&) = delete;

  // --------------------------------------------------------
  // Interesting syntax: no-throw move operations transfer the sole lease and provider together;
  // the moved-from value owns no live authority.
  RecoveryBootstrap(RecoveryBootstrap&&) noexcept = default;
  RecoveryBootstrap& operator=(RecoveryBootstrap&&) noexcept = default;

  // --------------------------------------------------------
  // Release this bootstrap's lease share only after its sealed identity provider is destroyed.
  ~RecoveryBootstrap();

  // --------------------------------------------------------
  // Return the external fake-medium lineage shared by every retained namespace record.
  [[nodiscard]] const RecoveryLineageId& lineage_id() const noexcept { return lineage_id_; }

  // --------------------------------------------------------
  // Return the fresh runtime incarnation scoped by the acknowledged namespace.
  [[nodiscard]] const RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Return the namespace published and acknowledged before this capability was exposed for use.
  [[nodiscard]] const model::OrderNamespace& registered_order_namespace() const noexcept {
    return registered_order_namespace_;
  }

  // --------------------------------------------------------
  // Return the complete immutable owner root accepted during bootstrap.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept {
    return root_provenance_;
  }

  // --------------------------------------------------------
  // Only the medium can publish a namespace fence and assemble this complete authority.
private:

  // --------------------------------------------------------
  // Retain already validated identities, lease, and provider without exposing minting authority.
  RecoveryBootstrap(RecoveryLineageId lineage_id, RuntimeEpochId runtime_epoch_id,
                    model::OrderNamespace registered_order_namespace,
                    model::M4RootProvenance root_provenance,
                    std::shared_ptr<detail::FakeJournalLeaseControl> lease,
                    model::DeterministicOrderIdProvider order_ids) noexcept;

  // --------------------------------------------------------
  // Keep the complete namespace-fenced authority inseparable and inaccessible to ordinary callers.
  RecoveryLineageId lineage_id_;
  RuntimeEpochId runtime_epoch_id_;
  model::OrderNamespace registered_order_namespace_;
  model::M4RootProvenance root_provenance_;
  std::shared_ptr<detail::FakeJournalLeaseControl> lease_;
  model::DeterministicOrderIdProvider order_ids_;

  // ########################################################################
  // The external medium publishes and acknowledges the namespace before returning this authority.
  friend class DeterministicFakeRecoveryMedium;

  // ########################################################################
  // The exact source-private owner consumes lease and provider together after complete validation.
  friend class runtime::PrivateOrderReconciler;

  // ########################################################################
};

// ########################################################################
// The concrete fake medium owns one fixed-capacity in-memory typed journal and namespace registry.
// It models fake acknowledgement but makes no filesystem, process-crash, power-loss, AEGISJRN byte,
// digest, or real-durability claim. Every operation requires caller-provided external
// serialization; concurrent access is unsupported. At most one live incarnation lease exists across
// either its bootstrap or installed owners, cold queries require no live lease, and acknowledged
// namespace rows always form one contiguous journal prefix. The public surface has no path,
// endpoint, credential, callback, retry, thread, or background work.
class DeterministicFakeRecoveryMedium final {
public:

  // --------------------------------------------------------
  // Allocate every policy-sized slot before publishing a handle. Allocation or length failure
  // returns InvalidRecoveryPolicy at fake_recovery_medium.capacity_allocation with no partial
  // value.
  [[nodiscard]] static model::Result<std::unique_ptr<DeterministicFakeRecoveryMedium>>
  create_deterministic_fake_recovery_medium_from_policy(RecoveryLineageId lineage_id,
                                                        const runtime::M4Policy& policy);

  // --------------------------------------------------------
  // Keep one stable external owner address while a bootstrap or installed owner may share backing.
  DeterministicFakeRecoveryMedium(const DeterministicFakeRecoveryMedium&) = delete;
  DeterministicFakeRecoveryMedium& operator=(const DeterministicFakeRecoveryMedium&) = delete;
  DeterministicFakeRecoveryMedium(DeterministicFakeRecoveryMedium&&) = delete;
  DeterministicFakeRecoveryMedium& operator=(DeterministicFakeRecoveryMedium&&) = delete;

  // --------------------------------------------------------
  // Release the external handle; an active bootstrap keeps its shared backing alive until release.
  ~DeterministicFakeRecoveryMedium();

  // --------------------------------------------------------
  // Validate one cold, fully acknowledged namespace-only history and the exact policy before any
  // mutation, then atomically publish and fake-acknowledge one fresh namespace. Active lease,
  // incompatible policy, invalid history, an undiscarded volatile suffix, duplicate namespace,
  // exhausted capacity/counter, or allocation failure returns a DomainError without changing state.
  [[nodiscard]] model::Result<RecoveryBootstrap>
  bootstrap_recovery_from_namespace(const runtime::M4Policy& policy,
                                    model::OrderNamespace fresh_namespace);

  // --------------------------------------------------------
  // Return the immutable lineage supplied to the validated factory.
  [[nodiscard]] const RecoveryLineageId& lineage_id() const noexcept;

  // --------------------------------------------------------
  // Return the immutable root copied from the policy accepted by the validated factory.
  [[nodiscard]] const model::M4RootProvenance& root_provenance() const noexcept;

  // --------------------------------------------------------
  // Return the fixed number of initialization-preallocated typed journal slots.
  [[nodiscard]] std::uint32_t journal_record_capacity() const noexcept;

  // --------------------------------------------------------
  // Return the published prefix count during cold inspection; a live lease returns
  // InvalidJournalState at journal.read_lease.
  [[nodiscard]] model::Result<std::uint32_t> published_journal_record_count() const;

  // --------------------------------------------------------
  // Return the separately retained fake-acknowledged prefix count during cold inspection; a live
  // lease returns InvalidJournalState at journal.read_lease.
  [[nodiscard]] model::Result<std::uint32_t> acknowledged_journal_record_count() const;

  // --------------------------------------------------------
  // Return the fixed number of initialization-preallocated namespace-registry slots.
  [[nodiscard]] std::uint32_t registered_namespace_capacity() const noexcept;

  // --------------------------------------------------------
  // Return the acknowledged namespace count during cold inspection; a live lease returns
  // InvalidJournalState at journal.read_lease.
  [[nodiscard]] model::Result<std::uint32_t> registered_namespace_count() const;

  // --------------------------------------------------------
  // Copy one published row by zero-based chronological index during cold inspection. A live lease
  // or out-of-prefix index returns InvalidJournalState without exposing mutable storage.
  [[nodiscard]] model::Result<JournalRecord>
  published_journal_record_at(std::uint32_t chronological_index) const;

  // --------------------------------------------------------
  // Copy one acknowledged namespace by zero-based chronological index during cold inspection. A
  // live lease or out-of-prefix index returns InvalidJournalState.
  [[nodiscard]] model::Result<model::OrderNamespace>
  registered_namespace_at(std::uint32_t chronological_index) const;

  // --------------------------------------------------------
  // Explicitly model crash loss by removing only a validated unacknowledged suffix. A live lease or
  // invalid prefix returns an error without mutation; success preserves the acknowledged prefix.
  [[nodiscard]] model::Result<void> discard_unacknowledged_journal_suffix();

  // --------------------------------------------------------
  // Only the validated factory may install one shared backing allocation.
private:

  // --------------------------------------------------------
  // Install one already fully allocated shared backing without further fallible work.
  explicit DeterministicFakeRecoveryMedium(
      std::shared_ptr<detail::FakeRecoveryBacking> backing) noexcept
      : backing_{std::move(backing)} {}

  // --------------------------------------------------------
  // Keep the backing alive for cold inspection and for any outstanding bootstrap lease.
  std::shared_ptr<detail::FakeRecoveryBacking> backing_;
};

// ########################################################################

} // namespace aegis::recovery
