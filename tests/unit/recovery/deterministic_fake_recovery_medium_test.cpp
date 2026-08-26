// Purpose: prove deterministic fake recovery publishes and fake-acknowledges every client
// namespace, keeps startup authority sealed, and defines typed records without inventing ADR-0014
// bytes.

#include "aegis/recovery/deterministic_fake_recovery_medium.hpp"
#include "m4_test_authority.hpp"

#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace aegis;

// ########################################################################
// Interesting syntax: requires-expression concepts prove forbidden public capabilities remain
// absent without needing a callable test backdoor. The fake stays concrete, single-owner, and
// offline.
template <typename Value>
concept HasPublicEndpoint = requires(Value& value) { value.endpoint(); };

template <typename Value>
concept HasPublicPath = requires(Value& value) { value.path(); };

template <typename Value>
concept HasPublicConnectOperation = requires(Value& value) { value.connect(); };

template <typename Value>
concept HasPublicRetryOperation = requires(Value& value) { value.retry(); };

template <typename Value>
concept HasPublicJournalAccessor = requires(Value& value) { value.journal(); };

template <typename Value>
concept HasPublicJournalAcknowledgementOperation =
    requires(Value& value) { value.acknowledge_all_published(); };

template <typename Value>
concept HasPublicOrderIdProviderExtraction =
    requires(Value& value) { value.take_order_id_provider(); };

static_assert(std::is_final_v<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!std::is_copy_constructible_v<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!std::is_move_constructible_v<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!std::is_copy_constructible_v<recovery::RecoveryBootstrap>);
static_assert(std::is_move_constructible_v<recovery::RecoveryBootstrap>);
static_assert(!HasPublicEndpoint<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!HasPublicPath<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!HasPublicConnectOperation<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!HasPublicRetryOperation<recovery::DeterministicFakeRecoveryMedium>);
static_assert(!HasPublicJournalAccessor<recovery::RecoveryBootstrap>);
static_assert(!HasPublicJournalAcknowledgementOperation<recovery::RecoveryBootstrap>);
static_assert(!HasPublicOrderIdProviderExtraction<recovery::RecoveryBootstrap>);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::NamespaceRegistered) == 1U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::SubmissionProjection) == 2U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::PrivateEventInput) == 3U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::ReconciliationInput) == 4U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::AccountSafetyFence) == 5U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::ReferenceIntentState) == 6U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::IdentityHighWater) == 7U);
static_assert(static_cast<std::uint8_t>(recovery::JournalRecordKind::RecoveryDecision) == 8U);
static_assert(
    static_cast<std::uint8_t>(recovery::JournalRecordKind::RecoveryNotificationDecision) == 9U);

// ########################################################################

// --------------------------------------------------------
// Build a distinguishable fixed recovery lineage with no string or ambient entropy dependency.
[[nodiscard]] recovery::RecoveryLineageId
construct_recovery_lineage(std::uint8_t seed = 0x10U) noexcept {
  recovery::RecoveryLineageId::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return recovery::RecoveryLineageId{bytes};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build a distinct restart namespace whose complete 16-byte value is easy to inspect.
[[nodiscard]] model::OrderNamespace construct_order_namespace(std::uint8_t seed) noexcept {
  model::OrderNamespace::Bytes bytes{};
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return model::OrderNamespace{bytes};
}

// --------------------------------------------------------

// --------------------------------------------------------
// Extract one expected successful Result or fail the fixture before the behavior assertion.
template <typename Value>
[[nodiscard]] Value take_recovery_result_value_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid deterministic fake-recovery fixture value: " +
                           std::to_string(static_cast<std::uint16_t>(result.error().code)) + "/" +
                           result.error().context.field};
  }
  return std::move(result).value();
}

// --------------------------------------------------------

// --------------------------------------------------------
// Create one exact policy-sized external medium and fail immediately on fixture construction error.
template <typename Authority>
[[nodiscard]] std::unique_ptr<recovery::DeterministicFakeRecoveryMedium>
create_deterministic_recovery_medium_from_authority_or_throw(const Authority& authority) {
  return take_recovery_result_value_or_throw(
      recovery::DeterministicFakeRecoveryMedium::
          create_deterministic_fake_recovery_medium_from_policy(construct_recovery_lineage(),
                                                                authority.m4_policy));
}

// --------------------------------------------------------

// --------------------------------------------------------
// Namespace bootstrap publishes and fake-acknowledges sequence one before returning sealed
// authority.
TEST_CASE("M4 fake recovery bootstrap acknowledges the first typed namespace record",
          "[recovery][m4][journal][bootstrap]") {
  auto authority = test_support::create_m4_test_authority_or_throw();
  auto fake = create_deterministic_recovery_medium_from_authority_or_throw(authority);
  const auto first_namespace = construct_order_namespace(0x20U);

  CHECK(fake->lineage_id() == construct_recovery_lineage());
  CHECK(fake->root_provenance() == authority.m4_policy.root_provenance());
  CHECK(fake->journal_record_capacity() == authority.m4_policy.capacities().max_journal_records);
  CHECK(fake->registered_namespace_capacity() ==
        authority.m4_policy.capacities().max_namespace_registrations);

  // ++++++++++++++++++++++++++++++++++++++++
  // The live bootstrap exposes identity facts but no journal, acknowledgement, or minting method.
  {
    auto bootstrap = take_recovery_result_value_or_throw(
        fake->bootstrap_recovery_from_namespace(authority.m4_policy, first_namespace));
    CHECK(bootstrap.lineage_id() == construct_recovery_lineage());
    CHECK(bootstrap.registered_order_namespace() == first_namespace);
    CHECK(bootstrap.runtime_epoch_id().order_namespace() == first_namespace);
    CHECK(bootstrap.runtime_epoch_id().counter() == 1U);
    CHECK(bootstrap.root_provenance() == authority.m4_policy.root_provenance());
    const auto live_read = fake->published_journal_record_count();
    REQUIRE_FALSE(live_read);
    CHECK(live_read.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(live_read.error().context.field == "journal.read_lease");

    const auto live_acknowledged = fake->acknowledged_journal_record_count();
    REQUIRE_FALSE(live_acknowledged);
    CHECK(live_acknowledged.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(live_acknowledged.error().context.field == "journal.read_lease");

    const auto live_namespace_count = fake->registered_namespace_count();
    REQUIRE_FALSE(live_namespace_count);
    CHECK(live_namespace_count.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(live_namespace_count.error().context.field == "journal.read_lease");

    const auto live_record = fake->published_journal_record_at(0U);
    REQUIRE_FALSE(live_record);
    CHECK(live_record.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(live_record.error().context.field == "journal.read_lease");

    const auto live_namespace = fake->registered_namespace_at(0U);
    REQUIRE_FALSE(live_namespace);
    CHECK(live_namespace.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(live_namespace.error().context.field == "journal.read_lease");
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold inspection sees the namespace record only after the live incarnation releases its lease.
  CHECK(take_recovery_result_value_or_throw(fake->published_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_at(0U)) == first_namespace);

  const auto record = take_recovery_result_value_or_throw(fake->published_journal_record_at(0U));
  CHECK(record.lineage_id() == construct_recovery_lineage());
  CHECK(record.sequence().value() == 1U);
  CHECK_FALSE(record.predecessor());
  CHECK_FALSE(record.runtime_epoch_id());
  CHECK(record.kind() == recovery::JournalRecordKind::NamespaceRegistered);
  CHECK(record.root_provenance() == authority.m4_policy.root_provenance());
  CHECK_FALSE(record.subject_provenance());
  CHECK_FALSE(record.replay_provenance());
  CHECK_FALSE(record.audit_link());
  const auto* const payload =
      std::get_if<recovery::NamespaceRegisteredJournalPayload>(&record.payload());
  REQUIRE(payload != nullptr);
  CHECK(payload->order_namespace == first_namespace);
  CHECK(payload->registry_count_after_append == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Cold invalid indexes fail exactly, and explicit crash-tail discard is a no-op on this fully
  // acknowledged prefix.
  const auto missing_record = fake->published_journal_record_at(1U);
  REQUIRE_FALSE(missing_record);
  CHECK(missing_record.error().code == model::DomainErrorCode::InvalidJournalState);
  CHECK(missing_record.error().context.field == "journal.record_index");

  const auto missing_namespace = fake->registered_namespace_at(1U);
  REQUIRE_FALSE(missing_namespace);
  CHECK(missing_namespace.error().code == model::DomainErrorCode::InvalidJournalState);
  CHECK(missing_namespace.error().context.field == "journal.namespace_index");

  REQUIRE(fake->discard_unacknowledged_journal_suffix());
  CHECK(take_recovery_result_value_or_throw(fake->published_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count()) == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// One live lease excludes a second runtime, and acknowledged namespace identity is never reusable.
TEST_CASE("M4 fake recovery medium grants one lease and rejects namespace reuse",
          "[recovery][m4][journal][bootstrap]") {
  auto authority = test_support::create_m4_test_authority_or_throw();
  auto fake = create_deterministic_recovery_medium_from_authority_or_throw(authority);
  const auto first_namespace = construct_order_namespace(0x30U);

  {
    auto bootstrap = take_recovery_result_value_or_throw(
        fake->bootstrap_recovery_from_namespace(authority.m4_policy, first_namespace));
    const auto second = fake->bootstrap_recovery_from_namespace(authority.m4_policy,
                                                                construct_order_namespace(0x40U));
    REQUIRE_FALSE(second);
    CHECK(second.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(second.error().context.field == "journal.append_lease");
    const auto discard = fake->discard_unacknowledged_journal_suffix();
    REQUIRE_FALSE(discard);
    CHECK(discard.error().code == model::DomainErrorCode::InvalidJournalState);
    CHECK(discard.error().context.field == "journal.append_lease");
    CHECK(bootstrap.registered_order_namespace() == first_namespace);
  }

  const auto reused = fake->bootstrap_recovery_from_namespace(authority.m4_policy, first_namespace);
  REQUIRE_FALSE(reused);
  CHECK(reused.error().code == model::DomainErrorCode::InvalidJournalState);
  CHECK(reused.error().context.field == "journal.duplicate_namespace");
  CHECK(take_recovery_result_value_or_throw(fake->published_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_count()) == 1U);

  {
    auto second = take_recovery_result_value_or_throw(fake->bootstrap_recovery_from_namespace(
        authority.m4_policy, construct_order_namespace(0x40U)));
    CHECK(second.registered_order_namespace() == construct_order_namespace(0x40U));
  }
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_count()) == 2U);
  const auto record = take_recovery_result_value_or_throw(fake->published_journal_record_at(1U));
  CHECK(record.sequence().value() == 2U);
  REQUIRE(record.predecessor());
  CHECK(record.predecessor()->value() == 1U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Policy/root mismatch and exact journal capacity fail before changing the retained medium.
TEST_CASE("M4 fake recovery rejects incompatible policy and full journal without mutation",
          "[recovery][m4][journal][capacity]") {
  auto authority = test_support::create_m4_test_authority_or_throw();
  auto fake = create_deterministic_recovery_medium_from_authority_or_throw(authority);

  auto foreign_capacities = test_support::create_ordinary_m4_policy_capacities();
  ++foreign_capacities.max_private_admissions;
  auto foreign = test_support::create_m4_test_authority_or_throw(foreign_capacities);
  const auto mismatch =
      fake->bootstrap_recovery_from_namespace(foreign.m4_policy, construct_order_namespace(0x80U));
  REQUIRE_FALSE(mismatch);
  CHECK(mismatch.error().code == model::DomainErrorCode::RecoveryProvenanceMismatch);
  CHECK(mismatch.error().context.field == "fake_recovery_medium.policy");
  CHECK(take_recovery_result_value_or_throw(fake->published_journal_record_count()) == 0U);
  CHECK(take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count()) == 0U);
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_count()) == 0U);

  auto one_record_capacities = test_support::create_ordinary_m4_policy_capacities();
  one_record_capacities.max_journal_records = 1U;
  auto one_record_authority =
      test_support::create_m4_test_authority_or_throw(one_record_capacities);
  auto full = create_deterministic_recovery_medium_from_authority_or_throw(one_record_authority);
  {
    auto bootstrap = take_recovery_result_value_or_throw(full->bootstrap_recovery_from_namespace(
        one_record_authority.m4_policy, construct_order_namespace(0x81U)));
    CHECK(bootstrap.registered_order_namespace() == construct_order_namespace(0x81U));
  }
  const auto rejected = full->bootstrap_recovery_from_namespace(one_record_authority.m4_policy,
                                                                construct_order_namespace(0x82U));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::JournalCapacityExceeded);
  CHECK(rejected.error().context.field == "journal.capacity");
  CHECK(take_recovery_result_value_or_throw(full->published_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(full->acknowledged_journal_record_count()) == 1U);
  CHECK(take_recovery_result_value_or_throw(full->registered_namespace_count()) == 1U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Namespace registration has independent headroom beyond the recovery-epoch budget, then rejects
// the first request beyond the exact namespace and journal bounds atomically.
TEST_CASE("M4 fake recovery enforces namespace registry capacity",
          "[recovery][m4][journal][capacity]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_recovery_epochs = 1U;
  capacities.max_namespace_registrations = 3U;
  capacities.max_journal_records = 3U;
  auto authority = test_support::create_m4_test_authority_or_throw(capacities);
  auto fake = create_deterministic_recovery_medium_from_authority_or_throw(authority);

  {
    auto first = take_recovery_result_value_or_throw(fake->bootstrap_recovery_from_namespace(
        authority.m4_policy, construct_order_namespace(0x90U)));
    CHECK(first.registered_order_namespace() == construct_order_namespace(0x90U));
  }
  {
    auto second = take_recovery_result_value_or_throw(fake->bootstrap_recovery_from_namespace(
        authority.m4_policy, construct_order_namespace(0xa0U)));
    CHECK(second.registered_order_namespace() == construct_order_namespace(0xa0U));
  }
  {
    auto third = take_recovery_result_value_or_throw(fake->bootstrap_recovery_from_namespace(
        authority.m4_policy, construct_order_namespace(0xb0U)));
    CHECK(third.registered_order_namespace() == construct_order_namespace(0xb0U));
  }
  const auto before_published =
      take_recovery_result_value_or_throw(fake->published_journal_record_count());
  const auto before_acknowledged =
      take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count());
  const auto exhausted = fake->bootstrap_recovery_from_namespace(authority.m4_policy,
                                                                 construct_order_namespace(0xc0U));
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::RecoveryCounterExhausted);
  CHECK(exhausted.error().context.field == "journal.namespace_capacity");
  CHECK(take_recovery_result_value_or_throw(fake->published_journal_record_count()) ==
        before_published);
  CHECK(take_recovery_result_value_or_throw(fake->acknowledged_journal_record_count()) ==
        before_acknowledged);
  CHECK(take_recovery_result_value_or_throw(fake->registered_namespace_count()) == 3U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Shared opaque backing prevents a live bootstrap from dangling if its external handle is released.
TEST_CASE("M4 fake recovery bootstrap keeps backing alive independently of external handle",
          "[recovery][m4][journal][lifetime]") {
  auto authority = test_support::create_m4_test_authority_or_throw();
  auto fake = create_deterministic_recovery_medium_from_authority_or_throw(authority);
  auto bootstrap = take_recovery_result_value_or_throw(fake->bootstrap_recovery_from_namespace(
      authority.m4_policy, construct_order_namespace(0xc0U)));
  fake.reset();
  CHECK(bootstrap.lineage_id() == construct_recovery_lineage());
  CHECK(bootstrap.registered_order_namespace() == construct_order_namespace(0xc0U));
  CHECK(bootstrap.runtime_epoch_id().order_namespace() == construct_order_namespace(0xc0U));
  CHECK(bootstrap.root_provenance() == authority.m4_policy.root_provenance());
}

// --------------------------------------------------------

} // namespace
