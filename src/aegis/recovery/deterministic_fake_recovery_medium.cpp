// Purpose: implement fixed-capacity typed fake-media validation, namespace publication and fake
// acknowledgement, and a sealed fail-closed bootstrap without canonical evidence bytes.

#include "aegis/recovery/deterministic_fake_recovery_medium.hpp"

#include "aegis/model/domain_error.hpp"
#include "aegis/runtime/m4_policy.hpp"

#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace aegis::recovery {
namespace {

// --------------------------------------------------------
// Construct one stable fake-recovery error without leaking implementation slot details.
[[nodiscard]] model::DomainError domain_error_from_fake_recovery_field(model::DomainErrorCode code,
                                                                       const char* field) {
  return model::DomainError::at_field(code, field);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return whether one namespace already appears in a fixed acknowledged registry prefix.
template <typename Accessor>
[[nodiscard]] bool does_prefix_contain_namespace(std::uint32_t count,
                                                 const model::OrderNamespace& candidate,
                                                 Accessor&& accessor) noexcept {
  for (std::uint32_t index = 0U; index < count; ++index) {
    const auto* const value = accessor(index);
    if (value != nullptr && *value == candidate) {
      return true;
    }
  }
  return false;
}

// --------------------------------------------------------

} // namespace

namespace detail {

// ########################################################################
// One externally serialized backing allocation owns every fixed slot and watermark independently of
// handle lifetime. Its acknowledged count never exceeds its published count, namespace rows match
// the acknowledged namespace-record projection, and all slots beyond active prefixes stay empty.
struct FakeRecoveryBacking {

  // --------------------------------------------------------
  // Allocate every bounded slot before the medium can publish any startup authority.
  FakeRecoveryBacking(RecoveryLineageId lineage, model::M4RootProvenance root,
                      std::uint32_t journal_record_capacity,
                      std::uint32_t registered_namespace_capacity)
      : lineage_id{lineage}, root_provenance{std::move(root)},
        journal_slots(static_cast<std::size_t>(journal_record_capacity)),
        namespace_slots(static_cast<std::size_t>(registered_namespace_capacity)) {}

  // --------------------------------------------------------
  // Retain only typed semantic rows and explicit prefix/lease state in the shared backing.
  RecoveryLineageId lineage_id;
  model::M4RootProvenance root_provenance;
  std::vector<std::optional<JournalRecord>> journal_slots;
  std::vector<std::optional<model::OrderNamespace>> namespace_slots;
  std::uint32_t published_journal_record_count{0U};
  std::uint32_t acknowledged_journal_record_count{0U};
  std::uint32_t registered_namespace_count{0U};
  bool lease_active{false};
  std::uint64_t lease_generation{0U};
};

// ########################################################################

// ########################################################################
// Shared startup-only control keeps backing alive and releases the exclusive writer at final use.
struct FakeJournalLeaseControl {

  // --------------------------------------------------------
  // Couple one immutable lease generation to the backing lifetime without copying owner state.
  FakeJournalLeaseControl(std::shared_ptr<FakeRecoveryBacking> backing_value,
                          std::uint64_t generation_value) noexcept
      : backing{std::move(backing_value)}, generation{generation_value} {}

  // --------------------------------------------------------
  // Release the incarnation flag only after the sealed bootstrap has disappeared.
  ~FakeJournalLeaseControl() noexcept {
    if (backing && backing->lease_active && backing->lease_generation == generation) {
      backing->lease_active = false;
    }
  }

  // --------------------------------------------------------
  // The generation prevents a stale handle from acting after a later incarnation acquires a lease.
  std::shared_ptr<FakeRecoveryBacking> backing;
  std::uint64_t generation;
};

// ########################################################################

} // namespace detail

namespace {

// --------------------------------------------------------
// Borrow a published record while keeping physical optional slots private to this translation unit.
[[nodiscard]] const JournalRecord*
published_journal_record_pointer_at(const detail::FakeRecoveryBacking& backing,
                                    std::uint32_t index) noexcept {
  if (index >= backing.published_journal_record_count) {
    return nullptr;
  }
  const auto& slot = backing.journal_slots[static_cast<std::size_t>(index)];
  return slot ? &*slot : nullptr;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Borrow one retained namespace without exposing mutable registry storage.
[[nodiscard]] const model::OrderNamespace*
registered_namespace_pointer_at(const detail::FakeRecoveryBacking& backing,
                                std::uint32_t index) noexcept {
  if (index >= backing.registered_namespace_count) {
    return nullptr;
  }
  const auto& slot = backing.namespace_slots[static_cast<std::size_t>(index)];
  return slot ? &*slot : nullptr;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Validate one record's causal envelope, presence profile, and complete typed equality projection.
[[nodiscard]] model::Result<void>
validate_fake_journal_record(const detail::FakeRecoveryBacking& backing,
                             const JournalRecord& record, std::uint32_t index) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate causal position, lineage, and owner-root authority before inspecting payload shape.
  const auto expected_sequence =
      JournalSequence::from_value(static_cast<std::uint64_t>(index) + 1U);
  if (!expected_sequence || record.sequence() != expected_sequence.value()) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.sequence"));
  }
  if (index == 0U) {
    if (record.predecessor()) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.predecessor"));
    }
  } else {
    const auto* const predecessor = published_journal_record_pointer_at(backing, index - 1U);
    if (predecessor == nullptr || !record.predecessor() ||
        *record.predecessor() != predecessor->sequence()) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.predecessor"));
    }
  }
  if (record.lineage_id() != backing.lineage_id) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.lineage_id"));
  }
  if (record.root_provenance() != backing.root_provenance) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::RecoveryProvenanceMismatch, "journal.root_provenance"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Namespace registration is lineage-scoped and cannot invent runtime or subject attribution.
  if (record.kind() == JournalRecordKind::NamespaceRegistered) {
    if (!std::holds_alternative<NamespaceRegisteredJournalPayload>(record.payload()) ||
        record.runtime_epoch_id() || record.subject_provenance() || record.replay_provenance() ||
        record.audit_link()) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.namespace_record"));
    }
    return model::Result<void>::success();
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Every non-namespace assignment remains unavailable until its complete owner slice exists.
  return model::Result<void>::failure(domain_error_from_fake_recovery_field(
      model::DomainErrorCode::InvalidJournalState, "journal.unsupported_record_kind"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Validate every retained prefix and cross-check the acknowledged namespace oracle without
// mutation.
[[nodiscard]] model::Result<void>
validate_cold_fake_recovery_backing(const detail::FakeRecoveryBacking& backing) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject impossible prefix and registry bounds before dereferencing any retained slot.
  if (backing.acknowledged_journal_record_count > backing.published_journal_record_count ||
      backing.published_journal_record_count > backing.journal_slots.size() ||
      backing.registered_namespace_count > backing.namespace_slots.size()) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.prefix_counts"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate every published causal row and its ordered namespace evidence without mutation.
  std::uint32_t observed_namespaces = 0U;
  for (std::uint32_t index = 0U; index < backing.published_journal_record_count; ++index) {
    const auto* const record = published_journal_record_pointer_at(backing, index);
    if (record == nullptr) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.published_gap"));
    }
    auto valid = validate_fake_journal_record(backing, *record, index);
    if (!valid) {
      return valid;
    }
    if (record->kind() == JournalRecordKind::NamespaceRegistered) {
      ++observed_namespaces;
      const auto& payload = std::get<NamespaceRegisteredJournalPayload>(record->payload());
      if (payload.registry_count_after_append != observed_namespaces) {
        return model::Result<void>::failure(domain_error_from_fake_recovery_field(
            model::DomainErrorCode::InvalidJournalState, "journal.namespace_count"));
      }
      for (std::uint32_t prior = 0U; prior < index; ++prior) {
        const auto* const prior_record = published_journal_record_pointer_at(backing, prior);
        if (prior_record != nullptr &&
            prior_record->kind() == JournalRecordKind::NamespaceRegistered &&
            std::get<NamespaceRegisteredJournalPayload>(prior_record->payload()).order_namespace ==
                payload.order_namespace) {
          return model::Result<void>::failure(domain_error_from_fake_recovery_field(
              model::DomainErrorCode::InvalidJournalState, "journal.duplicate_namespace"));
        }
      }
      if (index < backing.acknowledged_journal_record_count) {
        const auto* const retained =
            registered_namespace_pointer_at(backing, observed_namespaces - 1U);
        if (retained == nullptr || *retained != payload.order_namespace) {
          return model::Result<void>::failure(domain_error_from_fake_recovery_field(
              model::DomainErrorCode::InvalidJournalState, "journal.namespace_registry"));
        }
      }
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Cross-check the durable watermark against the independently retained namespace registry.
  std::uint32_t acknowledged_namespaces = 0U;
  for (std::uint32_t index = 0U; index < backing.acknowledged_journal_record_count; ++index) {
    const auto* const record = published_journal_record_pointer_at(backing, index);
    if (record != nullptr && record->kind() == JournalRecordKind::NamespaceRegistered) {
      ++acknowledged_namespaces;
    }
  }
  if (acknowledged_namespaces != backing.registered_namespace_count) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.namespace_registry"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Prove every slot outside each active prefix is empty before a later incarnation may reuse it.
  for (std::uint32_t index = backing.published_journal_record_count;
       index < static_cast<std::uint32_t>(backing.journal_slots.size()); ++index) {
    if (backing.journal_slots[static_cast<std::size_t>(index)]) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.unpublished_slot"));
    }
  }
  for (std::uint32_t index = backing.registered_namespace_count;
       index < static_cast<std::uint32_t>(backing.namespace_slots.size()); ++index) {
    if (backing.namespace_slots[static_cast<std::size_t>(index)]) {
      return model::Result<void>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.namespace_slot"));
    }
  }
  return model::Result<void>::success();

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Discard only the volatile published suffix after cold validation and outside a live lease.
void discard_volatile_journal_suffix(detail::FakeRecoveryBacking& backing) noexcept {
  for (std::uint32_t index = backing.acknowledged_journal_record_count;
       index < backing.published_journal_record_count; ++index) {
    backing.journal_slots[static_cast<std::size_t>(index)].reset();
  }
  backing.published_journal_record_count = backing.acknowledged_journal_record_count;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Keep every namespace-fenced startup authority together until later runtime composition consumes
// it.
RecoveryBootstrap::RecoveryBootstrap(RecoveryLineageId lineage_id, RuntimeEpochId runtime_epoch_id,
                                     model::OrderNamespace registered_order_namespace,
                                     model::M4RootProvenance root_provenance,
                                     std::shared_ptr<detail::FakeJournalLeaseControl> lease,
                                     model::DeterministicOrderIdProvider order_ids) noexcept
    : lineage_id_{lineage_id}, runtime_epoch_id_{runtime_epoch_id},
      registered_order_namespace_{registered_order_namespace},
      root_provenance_{std::move(root_provenance)}, lease_{std::move(lease)},
      order_ids_{std::move(order_ids)} {}

// --------------------------------------------------------

// --------------------------------------------------------
// Member destruction releases the lease after the sealed registered provider ceases use.
RecoveryBootstrap::~RecoveryBootstrap() = default;

// --------------------------------------------------------

// --------------------------------------------------------
// Allocate one stable backing under exact policy capacities; translate allocation/length failures
// into InvalidRecoveryPolicy without returning a partial medium.
model::Result<std::unique_ptr<DeterministicFakeRecoveryMedium>>
DeterministicFakeRecoveryMedium::create_deterministic_fake_recovery_medium_from_policy(
    RecoveryLineageId lineage_id, const runtime::M4Policy& policy) {
  try {
    const auto journal_record_capacity =
        static_cast<std::uint32_t>(policy.capacities().max_journal_records);
    const auto registered_namespace_capacity =
        static_cast<std::uint32_t>(policy.capacities().max_namespace_registrations);
    auto backing = std::make_shared<detail::FakeRecoveryBacking>(
        lineage_id, policy.root_provenance(), journal_record_capacity,
        registered_namespace_capacity);
    return model::Result<std::unique_ptr<DeterministicFakeRecoveryMedium>>::success(
        std::unique_ptr<DeterministicFakeRecoveryMedium>{
            new DeterministicFakeRecoveryMedium{std::move(backing)}});
  } catch (const std::bad_alloc&) {
    return model::Result<std::unique_ptr<DeterministicFakeRecoveryMedium>>::failure(
        domain_error_from_fake_recovery_field(model::DomainErrorCode::InvalidRecoveryPolicy,
                                              "fake_recovery_medium.capacity_allocation"));
  } catch (const std::length_error&) {
    return model::Result<std::unique_ptr<DeterministicFakeRecoveryMedium>>::failure(
        domain_error_from_fake_recovery_field(model::DomainErrorCode::InvalidRecoveryPolicy,
                                              "fake_recovery_medium.capacity_allocation"));
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Shared backing may outlive this external handle while a live bootstrap still owns its lease.
DeterministicFakeRecoveryMedium::~DeterministicFakeRecoveryMedium() = default;

// --------------------------------------------------------

// --------------------------------------------------------
// Validate a fully acknowledged cold prefix and every candidate resource before atomically
// publishing and fake-acknowledging one fresh namespace into sealed authority.
model::Result<RecoveryBootstrap> DeterministicFakeRecoveryMedium::bootstrap_recovery_from_namespace(
    const runtime::M4Policy& policy, model::OrderNamespace fresh_namespace) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Validate lease, policy, provenance, and the complete cold medium before any mutation.
  auto& backing = *backing_;
  if (backing.lease_active) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.append_lease"));
  }
  if (policy.root_provenance() != backing.root_provenance ||
      policy.capacities().max_journal_records != backing.journal_slots.size() ||
      policy.capacities().max_namespace_registrations != backing.namespace_slots.size()) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::RecoveryProvenanceMismatch, "fake_recovery_medium.policy"));
  }
  auto cold = validate_cold_fake_recovery_backing(backing);
  if (!cold) {
    return model::Result<RecoveryBootstrap>::failure(std::move(cold).error());
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Require the explicit crash-tail operation to discard any volatile suffix before bootstrap.
  // This keeps every reported bootstrap failure side-effect-free.
  if (backing.published_journal_record_count != backing.acknowledged_journal_record_count) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.unacknowledged_suffix"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Namespace-only acknowledged history needs no unavailable OMS or economic replay.
  for (std::uint32_t index = 0U; index < backing.acknowledged_journal_record_count; ++index) {
    const auto* const record = published_journal_record_pointer_at(backing, index);
    if (record == nullptr || record->kind() != JournalRecordKind::NamespaceRegistered) {
      return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.replay_required"));
    }
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Reject candidate identity and fixed-capacity exhaustion before constructing startup authority.
  if (does_prefix_contain_namespace(backing.registered_namespace_count, fresh_namespace,
                                    [&backing](std::uint32_t position) {
                                      return registered_namespace_pointer_at(backing, position);
                                    })) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.duplicate_namespace"));
  }
  if (backing.registered_namespace_count == backing.namespace_slots.size()) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::RecoveryCounterExhausted, "journal.namespace_capacity"));
  }
  if (backing.published_journal_record_count == backing.journal_slots.size()) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::JournalCapacityExceeded, "journal.capacity"));
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Construct every fallible runtime identity and startup allocation before journal publication.
  auto runtime_epoch = RuntimeEpochId::from_parts(fresh_namespace, 1U);
  auto order_ids = model::DeterministicOrderIdProvider::create(fresh_namespace);
  const auto next_sequence = JournalSequence::from_value(
      static_cast<std::uint64_t>(backing.published_journal_record_count) + 1U);
  if (!runtime_epoch) {
    return model::Result<RecoveryBootstrap>::failure(std::move(runtime_epoch).error());
  }
  if (!order_ids) {
    return model::Result<RecoveryBootstrap>::failure(std::move(order_ids).error());
  }
  if (!next_sequence) {
    return model::Result<RecoveryBootstrap>::failure(std::move(next_sequence).error());
  }
  if (backing.lease_generation == std::numeric_limits<std::uint64_t>::max()) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::RecoveryCounterExhausted, "journal.lease_generation"));
  }
  std::shared_ptr<detail::FakeJournalLeaseControl> lease;
  try {
    lease =
        std::make_shared<detail::FakeJournalLeaseControl>(backing_, backing.lease_generation + 1U);
  } catch (const std::bad_alloc&) {
    return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidRecoveryPolicy, "fake_recovery_medium.lease_allocation"));
  }

  std::optional<JournalSequence> predecessor;
  if (backing.published_journal_record_count != 0U) {
    const auto* const prior =
        published_journal_record_pointer_at(backing, backing.published_journal_record_count - 1U);
    if (prior == nullptr) {
      return model::Result<RecoveryBootstrap>::failure(domain_error_from_fake_recovery_field(
          model::DomainErrorCode::InvalidJournalState, "journal.predecessor"));
    }
    predecessor = prior->sequence();
  }
  JournalRecord namespace_record{
      backing.lineage_id,
      next_sequence.value(),
      predecessor,
      std::nullopt,
      JournalRecordKind::NamespaceRegistered,
      backing.root_provenance,
      std::nullopt,
      std::nullopt,
      JournalPayloadValue{NamespaceRegisteredJournalPayload{
          fresh_namespace, static_cast<std::uint32_t>(backing.registered_namespace_count + 1U)}},
      std::nullopt,
  };

  // ++++++++++++++++++++++++++++++++++++++++
  // Materialize the complete return value before the no-fail commit. The lease control remains
  // inert until the backing generation and active flag are published below.
  auto successful_bootstrap = model::Result<RecoveryBootstrap>::success(
      RecoveryBootstrap{backing.lineage_id, std::move(runtime_epoch).value(), fresh_namespace,
                        backing.root_provenance, std::move(lease), std::move(order_ids).value()});

  // ++++++++++++++++++++++++++++++++++++++++
  // Publish the record, fake acknowledgement, registry row, and lease in one no-fail commit.
  static_assert(std::is_nothrow_move_constructible_v<JournalRecord>);
  static_assert(std::is_nothrow_copy_constructible_v<model::OrderNamespace>);
  static_assert(std::is_nothrow_move_constructible_v<RecoveryBootstrap>);
  static_assert(std::is_nothrow_move_constructible_v<model::Result<RecoveryBootstrap>>);
  backing.journal_slots[static_cast<std::size_t>(backing.published_journal_record_count)].emplace(
      std::move(namespace_record));
  backing.namespace_slots[static_cast<std::size_t>(backing.registered_namespace_count)].emplace(
      fresh_namespace);
  ++backing.published_journal_record_count;
  ++backing.acknowledged_journal_record_count;
  ++backing.registered_namespace_count;

  // ++++++++++++++++++++++++++++++++++++++++
  // Activate the already materialized lease only after the namespace is acknowledged.
  ++backing.lease_generation;
  backing.lease_active = true;
  return successful_bootstrap;

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return the immutable lineage identity directly from the shared backing.
const RecoveryLineageId& DeterministicFakeRecoveryMedium::lineage_id() const noexcept {
  return backing_->lineage_id;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return the exact compatible M4 root used to validate every record.
const model::M4RootProvenance& DeterministicFakeRecoveryMedium::root_provenance() const noexcept {
  return backing_->root_provenance;
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return exact initialization-preallocated typed journal headroom.
std::uint32_t DeterministicFakeRecoveryMedium::journal_record_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->journal_slots.size());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return the published prefix during cold inspection; reject an active lease.
model::Result<std::uint32_t>
DeterministicFakeRecoveryMedium::published_journal_record_count() const {
  if (backing_->lease_active) {
    return model::Result<std::uint32_t>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.read_lease"));
  }
  return model::Result<std::uint32_t>::success(backing_->published_journal_record_count);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return the separately retained fake-acknowledged prefix during cold inspection.
model::Result<std::uint32_t>
DeterministicFakeRecoveryMedium::acknowledged_journal_record_count() const {
  if (backing_->lease_active) {
    return model::Result<std::uint32_t>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.read_lease"));
  }
  return model::Result<std::uint32_t>::success(backing_->acknowledged_journal_record_count);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return exact initialization-preallocated namespace-registry headroom.
std::uint32_t DeterministicFakeRecoveryMedium::registered_namespace_capacity() const noexcept {
  return static_cast<std::uint32_t>(backing_->namespace_slots.size());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Return the acknowledged namespace count during cold inspection; reject an active lease.
model::Result<std::uint32_t> DeterministicFakeRecoveryMedium::registered_namespace_count() const {
  if (backing_->lease_active) {
    return model::Result<std::uint32_t>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.read_lease"));
  }
  return model::Result<std::uint32_t>::success(backing_->registered_namespace_count);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Copy one published typed record by chronological index without exposing physical slots.
model::Result<JournalRecord> DeterministicFakeRecoveryMedium::published_journal_record_at(
    std::uint32_t chronological_index) const {
  if (backing_->lease_active) {
    return model::Result<JournalRecord>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.read_lease"));
  }
  const auto* const record = published_journal_record_pointer_at(*backing_, chronological_index);
  if (record == nullptr) {
    return model::Result<JournalRecord>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.record_index"));
  }
  return model::Result<JournalRecord>::success(*record);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Copy one acknowledged namespace by chronological index without exposing registry slots.
model::Result<model::OrderNamespace>
DeterministicFakeRecoveryMedium::registered_namespace_at(std::uint32_t chronological_index) const {
  if (backing_->lease_active) {
    return model::Result<model::OrderNamespace>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.read_lease"));
  }
  const auto* const value = registered_namespace_pointer_at(*backing_, chronological_index);
  if (value == nullptr) {
    return model::Result<model::OrderNamespace>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.namespace_index"));
  }
  return model::Result<model::OrderNamespace>::success(*value);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Model crash loss only between incarnations and only after complete cold validation; success
// preserves every acknowledged row and is a no-op when no volatile suffix exists.
model::Result<void> DeterministicFakeRecoveryMedium::discard_unacknowledged_journal_suffix() {
  if (backing_->lease_active) {
    return model::Result<void>::failure(domain_error_from_fake_recovery_field(
        model::DomainErrorCode::InvalidJournalState, "journal.append_lease"));
  }
  auto valid = validate_cold_fake_recovery_backing(*backing_);
  if (!valid) {
    return valid;
  }
  discard_volatile_journal_suffix(*backing_);
  return model::Result<void>::success();
}

// --------------------------------------------------------

} // namespace aegis::recovery
