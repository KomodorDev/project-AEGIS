// Purpose: define the nominal runtime, journal, snapshot, reconciliation, audit, and reference
// identities used by M4 fake-backed recovery without introducing media or coordinator behavior.

#pragma once

#include "aegis/model/bounded_identity.hpp"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace aegis::recovery {
namespace detail {

// ########################################################################
// Recovery lineage bytes identify one externally retained fake-media history.
struct RecoveryLineageIdTag;

// ########################################################################
// Runtime epochs use a fresh registered namespace and recovery-domain counter errors.
struct RuntimeEpochIdTag {
  static constexpr std::string_view field = "runtime_epoch_id";
  static constexpr model::DomainErrorCode invalid_code =
      model::DomainErrorCode::InvalidRecoveryPolicy;
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::RecoveryCounterExhausted;
};

// ########################################################################
// Snapshot IDs are namespace-scoped labels distinct from commit ordering.
struct RecoverySnapshotIdTag {
  static constexpr std::string_view field = "recovery_snapshot_id";
  static constexpr model::DomainErrorCode invalid_code =
      model::DomainErrorCode::InvalidRecoveryPolicy;
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::RecoveryCounterExhausted;
};

// ########################################################################
// Reference intent IDs retain one-shot driver identity independently of order identity.
struct ReferenceIntentIdTag {
  static constexpr std::string_view field = "reference_intent_id";
  static constexpr model::DomainErrorCode invalid_code =
      model::DomainErrorCode::InvalidRecoveryPolicy;
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::RecoveryCounterExhausted;
};

// ########################################################################
// Interesting syntax: this local macro gives every one-based recovery ordinal the same validation
// kernel while preserving a distinct tag, field, and exhaustion profile for each public type.
#define AEGIS_RECOVERY_ORDINAL_TAG(TagName, FieldName)                                             \
  struct TagName {                                                                                 \
    static constexpr std::string_view field = FieldName;                                           \
    static constexpr model::DomainErrorCode invalid_code =                                         \
        model::DomainErrorCode::InvalidRecoveryPolicy;                                             \
    static constexpr model::DomainErrorCode exhaustion_code =                                      \
        model::DomainErrorCode::RecoveryCounterExhausted;                                          \
  }

// ########################################################################
// Journal sequence names the acknowledged lineage prefix order.
AEGIS_RECOVERY_ORDINAL_TAG(JournalSequenceTag, "journal_sequence");

// ########################################################################
// Snapshot commit order is independent of namespace-scoped snapshot identity.
AEGIS_RECOVERY_ORDINAL_TAG(SnapshotCommitOrdinalTag, "snapshot_commit_ordinal");

// ########################################################################
// Audit ordinal fixes canonical audit order within one runtime epoch.
AEGIS_RECOVERY_ORDINAL_TAG(AuditOrdinalTag, "audit_ordinal");

// ########################################################################
// Diagnostic ordinal fixes bounded recovery diagnostic order.
AEGIS_RECOVERY_ORDINAL_TAG(DiagnosticOrdinalTag, "diagnostic_ordinal");

// ########################################################################
// Reconciliation row ordinal identifies one canonical row within an authoritative cut.
AEGIS_RECOVERY_ORDINAL_TAG(ReconciliationRowOrdinalTag, "reconciliation_row_ordinal");

// ########################################################################
// Close the local macro so later code cannot add an unreviewed recovery ordinal implicitly.
#undef AEGIS_RECOVERY_ORDINAL_TAG

} // namespace detail

// ########################################################################
// These recovery aliases assign lineage, epoch, snapshot, intent, and ordinal semantics to the
// shared fixed-storage kernels without moving recovery behavior into the model component.
using RecoveryLineageId = model::FixedOpaqueIdentity<detail::RecoveryLineageIdTag, 16U>;
using RuntimeEpochId = model::NamespaceCounterIdentity<detail::RuntimeEpochIdTag>;
using RecoverySnapshotId = model::NamespaceCounterIdentity<detail::RecoverySnapshotIdTag>;
using ReferenceIntentId = model::NamespaceCounterIdentity<detail::ReferenceIntentIdTag>;
using RuntimeEpochIdProvider = model::NamespaceCounterIdentityProvider<RuntimeEpochId>;
using RecoverySnapshotIdProvider = model::NamespaceCounterIdentityProvider<RecoverySnapshotId>;
using ReferenceIntentIdProvider = model::NamespaceCounterIdentityProvider<ReferenceIntentId>;
using JournalSequence = model::OneBasedComponentOrdinal<detail::JournalSequenceTag>;
using SnapshotCommitOrdinal = model::OneBasedComponentOrdinal<detail::SnapshotCommitOrdinalTag>;
using AuditOrdinal = model::OneBasedComponentOrdinal<detail::AuditOrdinalTag>;
using DiagnosticOrdinal = model::OneBasedComponentOrdinal<detail::DiagnosticOrdinalTag>;
using ReconciliationRowOrdinal =
    model::OneBasedComponentOrdinal<detail::ReconciliationRowOrdinalTag>;

// ########################################################################

// ########################################################################
// Reconciliation epochs extend one runtime epoch with a nonzero local counter.
class ReconciliationEpochId final {
public:
  static constexpr std::size_t byte_size = RuntimeEpochId::byte_size + sizeof(std::uint64_t);
  using Bytes = std::array<std::uint8_t, byte_size>;
  static constexpr std::string_view field = "reconciliation_epoch_id";
  static constexpr model::DomainErrorCode exhaustion_code =
      model::DomainErrorCode::RecoveryCounterExhausted;

  // --------------------------------------------------------
  // Validate the local ordinal before appending it to the complete runtime epoch.
  // Interesting syntax: the constrained integer parameter rejects floating-point and unsupported
  // caller types before the explicit range and nonzero checks run.
  template <model::detail::CheckedIntegerInput Counter>
  [[nodiscard]] static model::Result<ReconciliationEpochId>
  reconciliation_epoch_id_from_runtime_and_counter(const RuntimeEpochId& runtime_epoch_id,
                                                   Counter counter) {
    if (!std::in_range<std::uint64_t>(counter) || counter == 0) {
      return model::Result<ReconciliationEpochId>::create_failure(
          model::DomainError::create_at_field(model::DomainErrorCode::InvalidRecoveryPolicy,
                                              "reconciliation_epoch_id"));
    }
    const auto validated_counter = static_cast<std::uint64_t>(counter);
    Bytes bytes{};
    model::detail::append_identity_counter<RuntimeEpochId::byte_size>(
        bytes, runtime_epoch_id.bytes(), validated_counter);
    return model::Result<ReconciliationEpochId>::create_success(
        ReconciliationEpochId{runtime_epoch_id, validated_counter, bytes});
  }

  // --------------------------------------------------------
  // Expose the fixed 32-byte projection used by replay and canonical evidence.
  [[nodiscard]] constexpr const Bytes& bytes() const noexcept { return bytes_; }

  // --------------------------------------------------------
  // Preserve the complete typed runtime epoch for active-epoch validation.
  [[nodiscard]] constexpr const RuntimeEpochId& runtime_epoch_id() const noexcept {
    return runtime_epoch_id_;
  }

  // --------------------------------------------------------
  // Expose the nonzero local counter for high-water and exhaustion checks.
  [[nodiscard]] constexpr std::uint64_t counter() const noexcept { return counter_; }

  // --------------------------------------------------------
  // Equality and ordering include both the parent epoch and local ordinal.
  friend constexpr bool operator==(const ReconciliationEpochId&,
                                   const ReconciliationEpochId&) = default;
  friend constexpr auto operator<=>(const ReconciliationEpochId&,
                                    const ReconciliationEpochId&) = default;

  // --------------------------------------------------------
  // Prevent construction without validation and canonical projection.
private:

  // --------------------------------------------------------
  // Retain typed components beside their exact encoded bytes.
  explicit constexpr ReconciliationEpochId(RuntimeEpochId runtime_epoch_id, std::uint64_t counter,
                                           Bytes bytes) noexcept
      : runtime_epoch_id_{runtime_epoch_id}, counter_{counter}, bytes_{bytes} {}

  // --------------------------------------------------------
  // Components remain directly inspectable so consumers never decode byte offsets.
  RuntimeEpochId runtime_epoch_id_;
  std::uint64_t counter_;
  Bytes bytes_;
};

// ########################################################################

// ########################################################################
// One runtime owns one reconciliation-epoch counter stream with sticky non-wrapping exhaustion.
class ReconciliationEpochIdProvider final {
public:

  // --------------------------------------------------------
  // Start an active runtime's reconciliation epoch stream at one.
  [[nodiscard]] static model::Result<ReconciliationEpochIdProvider>
  create_reconciliation_epoch_id_provider(RuntimeEpochId runtime_epoch_id) {
    return create_reconciliation_epoch_id_provider(runtime_epoch_id, 1U);
  }

  // --------------------------------------------------------
  // Restore a checked next ordinal without permitting implicit negative conversion.
  // Interesting syntax: the constrained integer parameter keeps signed and unsigned inputs
  // available for explicit validation while excluding unsupported caller types at compile time.
  template <model::detail::CheckedIntegerInput Counter>
  [[nodiscard]] static model::Result<ReconciliationEpochIdProvider>
  create_reconciliation_epoch_id_provider(RuntimeEpochId runtime_epoch_id,
                                          Counter initial_counter) {
    const auto validation = ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
        runtime_epoch_id, initial_counter);
    if (!validation) {
      return model::Result<ReconciliationEpochIdProvider>::create_failure(validation.error());
    }
    return model::Result<ReconciliationEpochIdProvider>::create_success(
        ReconciliationEpochIdProvider{runtime_epoch_id,
                                      static_cast<std::uint64_t>(initial_counter)});
  }

  // --------------------------------------------------------
  // Copying is forbidden because it would duplicate active reconciliation epochs.
  ReconciliationEpochIdProvider(const ReconciliationEpochIdProvider&) = delete;
  ReconciliationEpochIdProvider& operator=(const ReconciliationEpochIdProvider&) = delete;

  // --------------------------------------------------------
  // Transfer stream ownership and poison the moved-from provider.
  // Interesting syntax: the explicit move operations copy the small counter state, then mark the
  // source exhausted so moving cannot duplicate live identity authority.
  ReconciliationEpochIdProvider(ReconciliationEpochIdProvider&& other) noexcept
      : runtime_epoch_id_{other.runtime_epoch_id_}, next_counter_{other.next_counter_},
        exhausted_{other.exhausted_} {
    other.exhausted_ = true;
  }

  // --------------------------------------------------------
  // Replace stream ownership while preserving sticky source exhaustion.
  ReconciliationEpochIdProvider& operator=(ReconciliationEpochIdProvider&& other) noexcept {
    if (this != &other) {
      runtime_epoch_id_ = other.runtime_epoch_id_;
      next_counter_ = other.next_counter_;
      exhausted_ = other.exhausted_;
      other.exhausted_ = true;
    }
    return *this;
  }

  // --------------------------------------------------------
  // Emit one epoch identity, then advance or enter non-wrapping terminal exhaustion.
  [[nodiscard]] model::Result<ReconciliationEpochId> generate_next_reconciliation_epoch_id() {
    if (exhausted_) {
      return model::Result<ReconciliationEpochId>::create_failure(
          model::DomainError::create_at_field(ReconciliationEpochId::exhaustion_code,
                                              std::string{ReconciliationEpochId::field}));
    }
    auto identity = ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
        runtime_epoch_id_, next_counter_);
    if (next_counter_ == std::numeric_limits<std::uint64_t>::max()) {
      exhausted_ = true;
    } else {
      ++next_counter_;
    }
    return identity;
  }

  // --------------------------------------------------------
  // Only a checked factory may establish the next reconciliation ordinal.
private:

  // --------------------------------------------------------
  // Retain the parent runtime and first not-yet-issued local counter.
  ReconciliationEpochIdProvider(RuntimeEpochId runtime_epoch_id,
                                std::uint64_t initial_counter) noexcept
      : runtime_epoch_id_{runtime_epoch_id}, next_counter_{initial_counter} {}

  // --------------------------------------------------------
  // The latch distinguishes a published terminal identity from a usable counter.
  RuntimeEpochId runtime_epoch_id_;
  std::uint64_t next_counter_;
  bool exhausted_{false};
};

// ########################################################################

} // namespace aegis::recovery
