// Purpose: prove M4 private and recovery identities are bounded, nominal, canonically encoded, and
// restart-safe without importing venue-native parsing or authenticated account identity.

#include "aegis/oms/private_order_identity.hpp"
#include "aegis/recovery/recovery_identity.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

namespace {

// ########################################################################
// Interesting syntax: requires-expressions prove authored counter APIs preserve source type instead
// of converting Boolean, character, or floating values through an accidental uint64 overload.
template <typename Value>
concept RuntimeEpochCounterInput =
    requires(aegis::model::OrderNamespace order_namespace, Value value) {
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(order_namespace, value);
    };

// ########################################################################

// --------------------------------------------------------
// Exercise the shared opaque profile independently for every component-owned nominal type.
template <typename Identity> void require_opaque_bounds(std::string_view field) {
  const std::array one_byte{std::byte{0x00}};
  const auto minimum = Identity::from_bytes(one_byte);
  REQUIRE(minimum);
  REQUIRE(minimum.value().bytes().size() == 1U);
  REQUIRE(minimum.value().bytes()[0] == std::byte{0x00});

  std::array<std::byte, Identity::maximum_byte_size> maximum{};
  maximum.front() = std::byte{0x7f};
  maximum.back() = std::byte{0x00};
  const auto accepted_maximum = Identity::from_bytes(maximum);
  REQUIRE(accepted_maximum);
  REQUIRE(accepted_maximum.value().bytes().size() == 128U);

  const std::span<const std::byte> empty;
  const auto absent = Identity::from_bytes(empty);
  REQUIRE_FALSE(absent);
  REQUIRE(absent.error().code == aegis::model::DomainErrorCode::InvalidPrivateIdentity);
  REQUIRE(absent.error().context.field == field);

  std::array<std::byte, Identity::maximum_byte_size + 1U> too_large{};
  const auto oversized = Identity::from_bytes(too_large);
  REQUIRE_FALSE(oversized);
  REQUIRE(oversized.error().code == aegis::model::DomainErrorCode::InvalidPrivateIdentity);
}

// --------------------------------------------------------
// Construct a visible deterministic namespace so byte-layout expectations stay test-authored.
[[nodiscard]] aegis::model::OrderNamespace create_order_namespace_from_seed(std::uint8_t seed) {
  aegis::model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  return aegis::model::OrderNamespace{bytes};
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Opaque adapter identities retain embedded zero bytes and reject both absent and over-bound input.
TEST_CASE("M4 opaque private identities enforce exact binary bounds") {
  require_opaque_bounds<aegis::oms::ExchangeOrderId>("exchange_order_id");
  require_opaque_bounds<aegis::oms::TradeId>("trade_id");
  require_opaque_bounds<aegis::oms::PrivateEventId>("private_event_id");
  require_opaque_bounds<aegis::oms::PrivateSourceEpochId>("private_source_epoch_id");
  require_opaque_bounds<aegis::oms::AuthoritativeCutId>("authoritative_cut_id");

  STATIC_REQUIRE_FALSE(std::is_same_v<aegis::oms::ExchangeOrderId, aegis::oms::TradeId>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<aegis::oms::ExchangeOrderId, aegis::oms::TradeId>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<aegis::oms::ExchangeOrderId>);
}

// --------------------------------------------------------
// Prefix and embedded-zero comparisons operate on active semantic bytes, never unused inline tail.
TEST_CASE("M4 opaque private identity comparison is deterministic") {
  const std::array prefix{std::byte{0x01}};
  const std::array longer{std::byte{0x01}, std::byte{0x00}};
  const std::array greater{std::byte{0x02}};

  const auto one = aegis::oms::TradeId::from_bytes(prefix).value();
  const auto one_again = aegis::oms::TradeId::from_bytes(prefix).value();
  const auto two_bytes = aegis::oms::TradeId::from_bytes(longer).value();
  const auto two = aegis::oms::TradeId::from_bytes(greater).value();

  REQUIRE(one == one_again);
  REQUIRE(one < two_bytes);
  REQUIRE(two_bytes < two);
}

// --------------------------------------------------------
// Namespace-counter identities expose their accepted 16-byte prefix and big-endian counter bytes.
TEST_CASE("M4 namespace counter identities use one canonical 24 byte profile") {
  const auto order_namespace = create_order_namespace_from_seed(0x10U);
  const auto runtime_epoch = aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(
      order_namespace, 0x0102U);
  REQUIRE(runtime_epoch);
  STATIC_REQUIRE(aegis::recovery::RuntimeEpochId::byte_size == 24U);

  for (std::size_t index = 0U; index < aegis::model::OrderNamespace::byte_size; ++index) {
    REQUIRE(runtime_epoch.value().bytes()[index] == order_namespace.bytes()[index]);
  }
  for (std::size_t index = 16U; index < 22U; ++index) {
    REQUIRE(runtime_epoch.value().bytes()[index] == 0U);
  }
  REQUIRE(runtime_epoch.value().bytes()[22] == 0x01U);
  REQUIRE(runtime_epoch.value().bytes()[23] == 0x02U);
  REQUIRE(runtime_epoch.value().order_namespace() == order_namespace);
  REQUIRE(runtime_epoch.value().counter() == 0x0102U);

  const auto zero =
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(order_namespace, 0U);
  REQUIRE_FALSE(zero);
  REQUIRE(zero.error().code == aegis::model::DomainErrorCode::InvalidRecoveryPolicy);
  REQUIRE(zero.error().context.field == "runtime_epoch_id");

  const auto negative =
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(order_namespace, -1);
  REQUIRE_FALSE(negative);
  REQUIRE(negative.error().code == aegis::model::DomainErrorCode::InvalidRecoveryPolicy);

  const auto local_zero =
      aegis::oms::LocalOrderEventId::identity_from_namespace_and_counter(order_namespace, 0U);
  const auto local_negative =
      aegis::oms::LocalOrderEventId::identity_from_namespace_and_counter(order_namespace, -1);
  REQUIRE_FALSE(local_zero);
  REQUIRE_FALSE(local_negative);
  REQUIRE(local_zero.error().code == aegis::model::DomainErrorCode::InvalidPrivateIdentity);

  const auto terminal = aegis::oms::LocalOrderEventId::identity_from_namespace_and_counter(
      order_namespace, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(terminal);
  REQUIRE(terminal.value().bytes().back() == 0xffU);

  const auto snapshot =
      aegis::recovery::RecoverySnapshotId::identity_from_namespace_and_counter(order_namespace, 3U);
  const auto reference =
      aegis::recovery::ReferenceIntentId::identity_from_namespace_and_counter(order_namespace, 4U);
  REQUIRE(snapshot);
  REQUIRE(reference);
  REQUIRE(snapshot.value().order_namespace() == order_namespace);
  REQUIRE(reference.value().counter() == 4U);

  STATIC_REQUIRE(aegis::oms::LocalOrderEventId::byte_size == 24U);
  STATIC_REQUIRE(aegis::recovery::RecoverySnapshotId::byte_size == 24U);
  STATIC_REQUIRE(aegis::recovery::ReferenceIntentId::byte_size == 24U);
  STATIC_REQUIRE_FALSE(
      std::is_same_v<aegis::recovery::RecoverySnapshotId, aegis::recovery::ReferenceIntentId>);

  STATIC_REQUIRE(RuntimeEpochCounterInput<int>);
  STATIC_REQUIRE(RuntimeEpochCounterInput<unsigned long long>);
  STATIC_REQUIRE_FALSE(RuntimeEpochCounterInput<bool>);
  STATIC_REQUIRE_FALSE(RuntimeEpochCounterInput<char>);
  STATIC_REQUIRE_FALSE(RuntimeEpochCounterInput<double>);
}

// --------------------------------------------------------
// Reconciliation and cancel identities append their counters without changing retained parents.
TEST_CASE("M4 composite identities preserve complete parent identities") {
  const auto first_namespace = create_order_namespace_from_seed(0x20U);
  const auto second_namespace = create_order_namespace_from_seed(0x40U);
  const auto first_epoch =
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(first_namespace, 1U)
          .value();
  const auto second_epoch =
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(second_namespace, 1U)
          .value();
  const auto reconciliation =
      aegis::recovery::ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
          first_epoch, 2U)
          .value();

  STATIC_REQUIRE(aegis::recovery::ReconciliationEpochId::byte_size == 32U);
  REQUIRE(std::equal(first_epoch.bytes().begin(), first_epoch.bytes().end(),
                     reconciliation.bytes().begin()));
  REQUIRE(reconciliation.bytes()[30] == 0U);
  REQUIRE(reconciliation.bytes()[31] == 2U);
  REQUIRE(reconciliation.runtime_epoch_id() == first_epoch);
  REQUIRE(reconciliation.counter() == 2U);

  const auto zero_reconciliation =
      aegis::recovery::ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
          first_epoch, 0U);
  const auto negative_reconciliation =
      aegis::recovery::ReconciliationEpochId::reconciliation_epoch_id_from_runtime_and_counter(
          first_epoch, -1);
  REQUIRE_FALSE(zero_reconciliation);
  REQUIRE_FALSE(negative_reconciliation);
  REQUIRE(zero_reconciliation.error().code == aegis::model::DomainErrorCode::InvalidRecoveryPolicy);

  auto order_provider =
      aegis::model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
          first_namespace)
          .value();
  const auto order_id = order_provider.generate_next_order_id().value();
  const auto first_cancel =
      aegis::oms::CancelAttemptId::cancel_attempt_id_from_components(first_epoch, order_id, 1U)
          .value();
  const auto restarted_cancel =
      aegis::oms::CancelAttemptId::cancel_attempt_id_from_components(second_epoch, order_id, 1U)
          .value();

  STATIC_REQUIRE(aegis::oms::CancelAttemptId::byte_size == 56U);
  REQUIRE(first_cancel != restarted_cancel);
  REQUIRE(std::equal(first_epoch.bytes().begin(), first_epoch.bytes().end(),
                     first_cancel.bytes().begin()));
  REQUIRE(std::equal(order_id.bytes().begin(), order_id.bytes().end(),
                     first_cancel.bytes().begin() +
                         static_cast<std::ptrdiff_t>(aegis::recovery::RuntimeEpochId::byte_size)));
  REQUIRE(first_cancel.bytes().back() == 1U);
  REQUIRE(first_cancel.runtime_epoch_id() == first_epoch);
  REQUIRE(first_cancel.order_id() == order_id);
  REQUIRE(first_cancel.ordinal() == 1U);

  const auto zero_cancel =
      aegis::oms::CancelAttemptId::cancel_attempt_id_from_components(first_epoch, order_id, 0U);
  REQUIRE_FALSE(zero_cancel);
  REQUIRE(zero_cancel.error().code == aegis::model::DomainErrorCode::InvalidPrivateIdentity);

  const auto negative_cancel =
      aegis::oms::CancelAttemptId::cancel_attempt_id_from_components(first_epoch, order_id, -1);
  REQUIRE_FALSE(negative_cancel);
  REQUIRE(negative_cancel.error().code == aegis::model::DomainErrorCode::InvalidPrivateIdentity);
}

// --------------------------------------------------------
// Move-only providers emit UINT64_MAX once, then preserve subsystem-specific sticky exhaustion.
TEST_CASE("M4 identity providers never wrap or duplicate terminal counters") {
  const auto order_namespace = create_order_namespace_from_seed(0x50U);
  auto runtime_result =
      aegis::recovery::RuntimeEpochIdProvider::create_namespace_counter_identity_provider(
          order_namespace, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(runtime_result);
  auto runtime_provider = std::move(runtime_result).value();
  const auto terminal_epoch = runtime_provider.generate_next_identity();
  REQUIRE(terminal_epoch);
  REQUIRE(terminal_epoch.value().counter() == std::numeric_limits<std::uint64_t>::max());
  const auto runtime_exhausted = runtime_provider.generate_next_identity();
  REQUIRE_FALSE(runtime_exhausted);
  REQUIRE(runtime_exhausted.error().code ==
          aegis::model::DomainErrorCode::RecoveryCounterExhausted);
  REQUIRE_FALSE(runtime_provider.generate_next_identity());

  auto local_result =
      aegis::oms::LocalOrderEventIdProvider::create_namespace_counter_identity_provider(
          order_namespace, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(local_result);
  auto local_provider = std::move(local_result).value();
  REQUIRE(local_provider.generate_next_identity());
  const auto local_exhausted = local_provider.generate_next_identity();
  REQUIRE_FALSE(local_exhausted);
  REQUIRE(local_exhausted.error().code == aegis::model::DomainErrorCode::PrivateCounterExhausted);

  auto snapshot_result =
      aegis::recovery::RecoverySnapshotIdProvider::create_namespace_counter_identity_provider(
          order_namespace, std::numeric_limits<std::uint64_t>::max());
  auto reference_result =
      aegis::recovery::ReferenceIntentIdProvider::create_namespace_counter_identity_provider(
          order_namespace, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(snapshot_result);
  REQUIRE(reference_result);
  auto snapshot_provider = std::move(snapshot_result).value();
  auto reference_provider = std::move(reference_result).value();
  REQUIRE(snapshot_provider.generate_next_identity());
  REQUIRE(reference_provider.generate_next_identity());
  REQUIRE_FALSE(snapshot_provider.generate_next_identity());
  REQUIRE_FALSE(reference_provider.generate_next_identity());

  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<aegis::recovery::RuntimeEpochIdProvider>);
  STATIC_REQUIRE(std::is_move_constructible_v<aegis::recovery::RuntimeEpochIdProvider>);
  STATIC_REQUIRE(std::is_final_v<aegis::recovery::RuntimeEpochIdProvider>);
}

// --------------------------------------------------------
// Cancel and reconciliation providers bind every emitted ordinal to their complete typed parents.
TEST_CASE("M4 composite identity providers retain parent scope and sticky exhaustion") {
  const auto order_namespace = create_order_namespace_from_seed(0x60U);
  const auto runtime_epoch =
      aegis::recovery::RuntimeEpochId::identity_from_namespace_and_counter(order_namespace, 1U)
          .value();
  auto order_provider =
      aegis::model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
          order_namespace)
          .value();
  const auto order_id = order_provider.generate_next_order_id().value();

  auto cancel_result = aegis::oms::CancelAttemptIdProvider::create_cancel_attempt_id_provider(
      runtime_epoch, order_id);
  REQUIRE(cancel_result);
  auto cancel_provider = std::move(cancel_result).value();
  const auto first = cancel_provider.generate_next_cancel_attempt_id();
  const auto second = cancel_provider.generate_next_cancel_attempt_id();
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first.value().ordinal() == 1U);
  REQUIRE(second.value().ordinal() == 2U);
  REQUIRE(first.value().runtime_epoch_id() == runtime_epoch);
  REQUIRE(first.value().order_id() == order_id);

  auto terminal_cancel_result =
      aegis::oms::CancelAttemptIdProvider::create_cancel_attempt_id_provider(
          runtime_epoch, order_id, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(terminal_cancel_result);
  auto terminal_cancel_provider = std::move(terminal_cancel_result).value();
  REQUIRE(terminal_cancel_provider.generate_next_cancel_attempt_id());
  const auto cancel_exhausted = terminal_cancel_provider.generate_next_cancel_attempt_id();
  REQUIRE_FALSE(cancel_exhausted);
  REQUIRE(cancel_exhausted.error().code == aegis::model::DomainErrorCode::PrivateCounterExhausted);
  REQUIRE_FALSE(terminal_cancel_provider.generate_next_cancel_attempt_id());

  auto reconciliation_result =
      aegis::recovery::ReconciliationEpochIdProvider::create_reconciliation_epoch_id_provider(
          runtime_epoch, std::numeric_limits<std::uint64_t>::max());
  REQUIRE(reconciliation_result);
  auto reconciliation_provider = std::move(reconciliation_result).value();
  REQUIRE(reconciliation_provider.generate_next_reconciliation_epoch_id());
  const auto exhausted = reconciliation_provider.generate_next_reconciliation_epoch_id();
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code == aegis::model::DomainErrorCode::RecoveryCounterExhausted);

  STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<aegis::oms::CancelAttemptIdProvider>);
  STATIC_REQUIRE(std::is_move_constructible_v<aegis::oms::CancelAttemptIdProvider>);
  STATIC_REQUIRE(std::is_final_v<aegis::oms::CancelAttemptIdProvider>);
}

// --------------------------------------------------------
// Recovery ordinals retain one-based semantics and report exact exhaustion without wrapping.
TEST_CASE("M4 recovery ordinals are nominal one based and non wrapping") {
  const auto first = aegis::recovery::JournalSequence::from_value(1U);
  REQUIRE(first);
  REQUIRE(first.value().value() == 1U);
  REQUIRE(first.value().derive_next_ordinal().value().value() == 2U);

  const auto zero = aegis::recovery::JournalSequence::from_value(0U);
  REQUIRE_FALSE(zero);
  REQUIRE(zero.error().code == aegis::model::DomainErrorCode::InvalidRecoveryPolicy);

  const auto terminal =
      aegis::recovery::AuditOrdinal::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(terminal);
  const auto exhausted = terminal.value().derive_next_ordinal();
  REQUIRE_FALSE(exhausted);
  REQUIRE(exhausted.error().code == aegis::model::DomainErrorCode::RecoveryCounterExhausted);
  REQUIRE(exhausted.error().context.field == "audit_ordinal");

  REQUIRE(aegis::recovery::SnapshotCommitOrdinal::from_value(1U));
  REQUIRE(aegis::recovery::DiagnosticOrdinal::from_value(1U));
  REQUIRE(aegis::recovery::ReconciliationRowOrdinal::from_value(1U));
  STATIC_REQUIRE_FALSE(
      std::is_same_v<aegis::recovery::JournalSequence, aegis::recovery::SnapshotCommitOrdinal>);
}

// --------------------------------------------------------
// The fixed lineage identity retains all 16 authored bytes and remains nominally distinct.
TEST_CASE("M4 recovery lineage identity has one exact fixed profile") {
  aegis::recovery::RecoveryLineageId::Bytes first_bytes{};
  aegis::recovery::RecoveryLineageId::Bytes second_bytes{};
  first_bytes.front() = 0x01U;
  second_bytes.front() = 0x02U;
  const aegis::recovery::RecoveryLineageId first{first_bytes};
  const aegis::recovery::RecoveryLineageId same{first_bytes};
  const aegis::recovery::RecoveryLineageId second{second_bytes};

  STATIC_REQUIRE(aegis::recovery::RecoveryLineageId::byte_size == 16U);
  REQUIRE(first.bytes() == first_bytes);
  REQUIRE(first == same);
  REQUIRE(first < second);
  STATIC_REQUIRE_FALSE(
      std::is_same_v<aegis::recovery::RecoveryLineageId, aegis::model::OrderNamespace>);
}

// --------------------------------------------------------
// Stable M4 errors append exactly to the existing machine-readable numeric contract.
TEST_CASE("M4 domain errors retain their accepted numeric assignments") {
  using aegis::model::DomainErrorCode;
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidPrivateIdentity) == 900U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidPrivateEvent) == 901U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::PrivateEventConflict) == 902U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::PrivateEventCapacityExceeded) == 903U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::PrivateCorrelationFailed) == 904U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidPrivateOmsState) == 905U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::PrivateCounterExhausted) == 906U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidInventoryState) == 910U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InventoryCapacityExceeded) == 911U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidReservationConversion) == 912U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::AccountNotSynchronized) == 913U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidAccountSafetyState) == 914U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::PrivateEvidenceExhausted) == 915U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidRecoveryPolicy) == 920U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidJournalState) == 921U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::JournalCapacityExceeded) == 922U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidRecoverySnapshot) == 923U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::SnapshotCapacityExceeded) == 924U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidReconciliation) == 925U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::ReconciliationIncomplete) == 926U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::RecoveryGap) == 927U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::RecoveryCounterExhausted) == 928U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::RecoveryProvenanceMismatch) == 929U);
  STATIC_REQUIRE(static_cast<std::uint16_t>(DomainErrorCode::InvalidM4Policy) == 930U);
}

// --------------------------------------------------------
