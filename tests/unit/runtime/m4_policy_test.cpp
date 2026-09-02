// Purpose: independently prove M4 policy validation, exact AEGISM4P bytes, fingerprint identity,
// and seven-field root provenance through the sole public sealed-authority factory.

#include "aegis/runtime/m4_policy.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "reference_configuration.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// ########################################################################
// One retained fixture owns the exact sealed M1-M3 authority chain used by public M4 construction.
struct M4PolicyAuthorityFixture {
  aegis::configuration::StartupConfiguration configuration;
  aegis::runtime::RuntimePolicy runtime_policy;
  std::unique_ptr<aegis::runtime::SubmissionCoordinator> submission;
};

// ########################################################################
// Test-owned field metadata drives only negative authoring cases and never production encoding.
struct M4PolicyCapacityCase {
  std::uint64_t aegis::runtime::M4PolicyCapacities::*member;
  std::string_view name;
};

// ########################################################################

// --------------------------------------------------------
// Enumerate all capacity members independently so omissions cannot hide scalar validation gaps.
constexpr std::array<M4PolicyCapacityCase, 26U> capacity_cases{{
    {&aegis::runtime::M4PolicyCapacities::max_private_admissions, "max_private_admissions"},
    {&aegis::runtime::M4PolicyCapacities::max_reconciliation_admissions,
     "max_reconciliation_admissions"},
    {&aegis::runtime::M4PolicyCapacities::max_account_safety_fences, "max_account_safety_fences"},
    {&aegis::runtime::M4PolicyCapacities::max_private_event_records, "max_private_event_records"},
    {&aegis::runtime::M4PolicyCapacities::max_event_identity_records, "max_event_identity_records"},
    {&aegis::runtime::M4PolicyCapacities::max_trade_identity_records, "max_trade_identity_records"},
    {&aegis::runtime::M4PolicyCapacities::max_exchange_order_mappings,
     "max_exchange_order_mappings"},
    {&aegis::runtime::M4PolicyCapacities::max_pending_fill_intervals_per_order,
     "max_pending_fill_intervals_per_order"},
    {&aegis::runtime::M4PolicyCapacities::max_cancel_attempts, "max_cancel_attempts"},
    {&aegis::runtime::M4PolicyCapacities::max_inventory_source_rows, "max_inventory_source_rows"},
    {&aegis::runtime::M4PolicyCapacities::max_inventory_aggregate_cells,
     "max_inventory_aggregate_cells"},
    {&aegis::runtime::M4PolicyCapacities::max_unattributed_exposure_rows,
     "max_unattributed_exposure_rows"},
    {&aegis::runtime::M4PolicyCapacities::max_account_safety_records, "max_account_safety_records"},
    {&aegis::runtime::M4PolicyCapacities::max_transition_effects_per_turn,
     "max_transition_effects_per_turn"},
    {&aegis::runtime::M4PolicyCapacities::max_order_callbacks_per_turn,
     "max_order_callbacks_per_turn"},
    {&aegis::runtime::M4PolicyCapacities::max_private_diagnostics, "max_private_diagnostics"},
    {&aegis::runtime::M4PolicyCapacities::max_private_audit_records, "max_private_audit_records"},
    {&aegis::runtime::M4PolicyCapacities::max_journal_records, "max_journal_records"},
    {&aegis::runtime::M4PolicyCapacities::max_snapshot_records, "max_snapshot_records"},
    {&aegis::runtime::M4PolicyCapacities::max_reconciliation_batches, "max_reconciliation_batches"},
    {&aegis::runtime::M4PolicyCapacities::max_reconciliation_rows_per_batch,
     "max_reconciliation_rows_per_batch"},
    {&aegis::runtime::M4PolicyCapacities::max_live_catchup_facts, "max_live_catchup_facts"},
    {&aegis::runtime::M4PolicyCapacities::max_recovery_epochs, "max_recovery_epochs"},
    {&aegis::runtime::M4PolicyCapacities::max_namespace_registrations,
     "max_namespace_registrations"},
    {&aegis::runtime::M4PolicyCapacities::max_recovery_notifications, "max_recovery_notifications"},
    {&aegis::runtime::M4PolicyCapacities::max_reference_intents, "max_reference_intents"},
}};

// --------------------------------------------------------
// Parse exact fixture identifiers and fail immediately for an authored test defect.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 policy fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Create the unchanged sealed M3 runtime policy or throw for an invalid test fixture.
[[nodiscard]] aegis::runtime::RuntimePolicy
create_runtime_policy_or_throw(const aegis::configuration::StartupConfiguration& configuration) {
  auto created = aegis::runtime::RuntimePolicy::create_runtime_policy(
      configuration,
      aegis::runtime::RuntimePolicyParams{
          aegis::runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U,
                                              32U, 100'000U},
          {{parse_identifier_or_throw<aegis::model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_identifier_or_throw<aegis::model::VenueId>("deribit"),
            parse_identifier_or_throw<aegis::model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_identifier_or_throw<aegis::model::VenueInstrumentId>("BTC-PERPETUAL"),
            aegis::model::InstrumentMetadataRevision::create_initial()}},
      });
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M4 policy fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create the deterministic M3 submission stack or throw before an invalid fixture can be used.
[[nodiscard]] std::unique_ptr<aegis::runtime::SubmissionCoordinator>
create_submission_coordinator_or_throw(
    const aegis::configuration::StartupConfiguration& configuration,
    const aegis::runtime::RuntimePolicy& policy) {
  constexpr std::uint64_t maximum_attempts = 10U;
  auto encoder = aegis::execution::FakeEncoderScript::create_fake_encoder_script(
      aegis::execution::FakeEncodingAction::Encode, maximum_attempts,
      {{1U, aegis::execution::FakeEncodingAction::Fail}});
  auto initiator = aegis::execution::FakeInitiatorScript::create_fake_initiator_script(
      aegis::execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts,
      {{1U, aegis::execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance},
       {2U, aegis::execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}});
  aegis::model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0x42U);
  auto order_ids =
      aegis::model::DeterministicOrderIdProvider::create_deterministic_order_id_provider(
          aegis::model::OrderNamespace{namespace_bytes});
  if (!encoder || !initiator || !order_ids) {
    throw std::logic_error{"invalid deterministic fake in M4 policy fixture"};
  }

  std::vector<std::optional<std::uint64_t>> clock_readings;
  clock_readings.reserve(static_cast<std::size_t>(maximum_attempts * 2U));
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    clock_readings.emplace_back(10'000U + index);
  }
  auto created = aegis::runtime::SubmissionCoordinator::create_submission_coordinator(
      configuration, policy,
      aegis::runtime::FakeSubmissionRuntimeParams{
          aegis::test_support::create_m3_reference_risk_policy_params_or_throw(configuration),
          aegis::execution::SubmissionPolicyCapacities{maximum_attempts, 4U, 4U, 1'024U, 2U, 110U,
                                                       8U},
          std::move(encoder).value(), std::move(initiator).value(),
          std::make_unique<aegis::execution::DeterministicSubmissionMeasurementClock>(
              std::move(clock_readings)),
          aegis::model::DeterministicOrderIdSource{std::move(order_ids).value()}});
  if (!created) {
    throw std::logic_error{"invalid submission policy in M4 policy fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create complete sealed authority or throw; the optional peer deliberately changes its hash.
[[nodiscard]] M4PolicyAuthorityFixture
create_policy_authority_or_throw(bool add_peer_subscription = false) {
  auto params = aegis::test_support::create_m3_enabled_two_firm_configuration_params_or_throw();
  if (add_peer_subscription) {
    params.subscriptions.push_back(aegis::market_data::Subscription{
        parse_identifier_or_throw<aegis::model::SubscriptionId>(
            "subscription.deribit-peer-btc-perpetual-book"),
        parse_identifier_or_throw<aegis::model::BotId>("bot.subsidiary-reference"),
        parse_identifier_or_throw<aegis::model::VenueId>("deribit"),
        parse_identifier_or_throw<aegis::model::InstrumentId>("BTC-USD-PERPETUAL"),
        aegis::market_data::SubscriptionChannel::OrderBook});
  }
  auto configured =
      aegis::configuration::StartupConfiguration::create_startup_configuration(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 policy fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = create_runtime_policy_or_throw(configuration);
  auto submission = create_submission_coordinator_or_throw(configuration, runtime);
  return M4PolicyAuthorityFixture{std::move(configuration), std::move(runtime),
                                  std::move(submission)};
}

// --------------------------------------------------------
// Values 101 through 125 plus one make field order visible while satisfying every relationship.
[[nodiscard]] aegis::runtime::M4PolicyCapacities create_golden_m4_policy_capacities() {
  return aegis::runtime::M4PolicyCapacities{
      101U, 102U, 103U, 104U, 105U, 106U, 107U, 108U, 109U, 110U, 111U, 112U, 113U,
      114U, 115U, 116U, 117U, 118U, 119U, 120U, 121U, 122U, 123U, 124U, 125U, 1U,
  };
}

// --------------------------------------------------------
// A coherent generic policy supports exhaustive mutation without selecting a reference fixture.
[[nodiscard]] aegis::runtime::M4PolicyCapacities create_ordinary_m4_policy_capacities() {
  return aegis::runtime::M4PolicyCapacities{
      32U, 32U, 32U, 32U, 32U, 32U, 32U, 4U,  32U, 32U, 32U, 32U, 32U,
      32U, 32U, 32U, 32U, 32U, 32U, 8U,  16U, 16U, 4U,  5U,  32U, 3U,
  };
}

// --------------------------------------------------------
// Create an M4 policy result through the sole public factory and retained M3 authority.
[[nodiscard]] aegis::model::Result<aegis::runtime::M4Policy>
create_m4_policy(const M4PolicyAuthorityFixture& sealed,
                 aegis::runtime::M4PolicyCapacities capacities) {
  return aegis::runtime::M4Policy::create_m4_policy(sealed.configuration, sealed.runtime_policy,
                                                    sealed.submission->reservations().policy(),
                                                    sealed.submission->policy(), capacities);
}

// --------------------------------------------------------
// Append one unsigned integer to test-owned bytes in literal big-endian order.
// Interesting syntax: the explicit zero check ends the unsigned countdown before subtraction
// could wrap from zero to the type's maximum value.
void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
  for (unsigned int shift = 56U;; shift -= 8U) {
    bytes.push_back(std::byte{static_cast<std::uint8_t>((value >> shift) & 0xffU)});
    if (shift == 0U) {
      break;
    }
  }
}

// --------------------------------------------------------
// Derive complete expected bytes without calling a production writer or capacity table.
[[nodiscard]] std::vector<std::byte>
derive_golden_m4_policy_bytes(const M4PolicyAuthorityFixture& sealed) {
  const auto capacities = create_golden_m4_policy_capacities();
  const auto& risk_policy = sealed.submission->reservations().policy();
  const auto& submission_policy = sealed.submission->policy();
  std::vector<std::byte> bytes;
  bytes.reserve(362U);
  for (const char character : std::string_view{"AEGISM4P"}) {
    bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  }
  bytes.push_back(std::byte{0x00});
  bytes.push_back(std::byte{0x01});
  const auto append_digest = [&](const aegis::model::Sha256Digest& digest) {
    bytes.insert(bytes.end(), digest.begin(), digest.end());
  };
  append_digest(sealed.configuration.fingerprint().bytes());
  append_u64(bytes, sealed.configuration.organization().revision().value());
  append_digest(sealed.runtime_policy.fingerprint().bytes());
  append_u64(bytes, risk_policy.revision().value());
  append_digest(risk_policy.fingerprint().bytes());
  append_digest(submission_policy.fingerprint().bytes());
  append_u64(bytes, capacities.max_private_admissions);
  append_u64(bytes, capacities.max_reconciliation_admissions);
  append_u64(bytes, capacities.max_account_safety_fences);
  append_u64(bytes, capacities.max_private_event_records);
  append_u64(bytes, capacities.max_event_identity_records);
  append_u64(bytes, capacities.max_trade_identity_records);
  append_u64(bytes, capacities.max_exchange_order_mappings);
  append_u64(bytes, capacities.max_pending_fill_intervals_per_order);
  append_u64(bytes, capacities.max_cancel_attempts);
  append_u64(bytes, capacities.max_inventory_source_rows);
  append_u64(bytes, capacities.max_inventory_aggregate_cells);
  append_u64(bytes, capacities.max_unattributed_exposure_rows);
  append_u64(bytes, capacities.max_account_safety_records);
  append_u64(bytes, capacities.max_transition_effects_per_turn);
  append_u64(bytes, capacities.max_order_callbacks_per_turn);
  append_u64(bytes, capacities.max_private_diagnostics);
  append_u64(bytes, capacities.max_private_audit_records);
  append_u64(bytes, capacities.max_journal_records);
  append_u64(bytes, capacities.max_snapshot_records);
  append_u64(bytes, capacities.max_reconciliation_batches);
  append_u64(bytes, capacities.max_reconciliation_rows_per_batch);
  append_u64(bytes, capacities.max_live_catchup_facts);
  append_u64(bytes, capacities.max_recovery_epochs);
  append_u64(bytes, capacities.max_namespace_registrations);
  append_u64(bytes, capacities.max_recovery_notifications);
  append_u64(bytes, capacities.max_reference_intents);
  return bytes;
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// The public factory and independent vector pin every field plus the exact accepted SHA-256.
TEST_CASE("M4 policy matches the complete AEGISM4P schema one golden") {
  STATIC_REQUIRE(aegis::runtime::canonical_m4_policy_schema_version == 1U);
  STATIC_REQUIRE(aegis::runtime::canonical_m4_policy_byte_size == 362U);
  STATIC_REQUIRE(!std::is_same_v<aegis::runtime::M4PolicyFingerprint,
                                 aegis::runtime::RuntimePolicyFingerprint>);

  const auto sealed = create_policy_authority_or_throw();
  const auto policy = create_m4_policy(sealed, create_golden_m4_policy_capacities());
  REQUIRE(policy);
  const auto expected = derive_golden_m4_policy_bytes(sealed);
  REQUIRE(expected.size() == 362U);
  REQUIRE(policy.value().canonical_bytes() == expected);
  REQUIRE(policy.value().fingerprint().to_hex() ==
          "8f8af768a590f036284ebd053cab41ef4a9f6a28039c730df225678cf878d591");

  const auto& root = policy.value().root_provenance();
  REQUIRE(root.configuration_fingerprint() == sealed.configuration.fingerprint().bytes());
  REQUIRE(root.organization_revision() == sealed.configuration.organization().revision());
  REQUIRE(root.runtime_policy_fingerprint() == sealed.runtime_policy.fingerprint().bytes());
  REQUIRE(root.risk_policy_revision() == sealed.submission->reservations().policy().revision());
  REQUIRE(root.risk_policy_fingerprint() ==
          sealed.submission->reservations().policy().fingerprint().bytes());
  REQUIRE(root.submission_policy_fingerprint() ==
          sealed.submission->policy().fingerprint().bytes());
  REQUIRE(root.m4_policy_fingerprint() == policy.value().fingerprint().bytes());
}

// --------------------------------------------------------
// Every named field independently rejects zero and a value one above the accepted u32 ceiling.
TEST_CASE("M4 policy validates every authored capacity before narrowing") {
  const auto sealed = create_policy_authority_or_throw();
  const auto over_bound =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U;

  for (const auto& capacity_case : capacity_cases) {
    auto zero = create_ordinary_m4_policy_capacities();
    zero.*(capacity_case.member) = 0U;
    const auto zero_result = create_m4_policy(sealed, zero);
    CAPTURE(capacity_case.name);
    REQUIRE_FALSE(zero_result);
    REQUIRE(zero_result.error().code == aegis::model::DomainErrorCode::InvalidM4Policy);
    REQUIRE(zero_result.error().context.field ==
            "m4_policy.capacities." + std::string{capacity_case.name});

    auto too_large = create_ordinary_m4_policy_capacities();
    too_large.*(capacity_case.member) = over_bound;
    const auto over_result = create_m4_policy(sealed, too_large);
    REQUIRE_FALSE(over_result);
    REQUIRE(over_result.error().code == aegis::model::DomainErrorCode::InvalidM4Policy);
    REQUIRE(over_result.error().context.field ==
            "m4_policy.capacities." + std::string{capacity_case.name});
  }
}

// --------------------------------------------------------
// Every derived lower bound rejects one below and all bounds accept together at exact equality.
TEST_CASE("M4 policy enforces fixed owner and atomic turn relationships") {
  const auto sealed = create_policy_authority_or_throw();
  auto capacities = create_ordinary_m4_policy_capacities();
  capacities.max_account_safety_fences = 1U;
  auto result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_account_safety_fences");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_exchange_order_mappings = 3U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_exchange_order_mappings");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_inventory_source_rows = 3U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_inventory_source_rows");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_inventory_aggregate_cells = 13U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_inventory_aggregate_cells");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_namespace_registrations = capacities.max_recovery_epochs;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_namespace_registrations");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_transition_effects_per_turn = capacities.max_pending_fill_intervals_per_order;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_transition_effects_per_turn");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_order_callbacks_per_turn = capacities.max_pending_fill_intervals_per_order;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_order_callbacks_per_turn");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_private_audit_records = capacities.max_pending_fill_intervals_per_order + 2U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_private_audit_records");

  auto exact = create_ordinary_m4_policy_capacities();
  exact.max_account_safety_fences = 2U;
  exact.max_exchange_order_mappings = 4U;
  exact.max_inventory_source_rows = 4U;
  exact.max_inventory_aggregate_cells = 14U;
  exact.max_namespace_registrations = exact.max_recovery_epochs + 1U;
  exact.max_transition_effects_per_turn = exact.max_pending_fill_intervals_per_order + 1U;
  exact.max_order_callbacks_per_turn = exact.max_pending_fill_intervals_per_order + 1U;
  exact.max_private_audit_records = exact.max_pending_fill_intervals_per_order + 3U;
  REQUIRE(create_m4_policy(sealed, exact));
}

// --------------------------------------------------------
// Generic policy accepts multiple intent slots; the later trusted reference driver requires one.
TEST_CASE("M4 generic policy does not impersonate reference fixture validation") {
  const auto sealed = create_policy_authority_or_throw();
  auto capacities = create_ordinary_m4_policy_capacities();
  capacities.max_reference_intents = 7U;
  const auto result = create_m4_policy(sealed, capacities);
  REQUIRE(result);
  REQUIRE(result.value().capacities().max_reference_intents == 7U);
}

// --------------------------------------------------------
// Audit backing products reject overflow in fixed precedence and accept exact u32 boundaries.
TEST_CASE("M4 policy checks topology-derived audit backing products") {
  const auto sealed = create_policy_authority_or_throw();
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());

  // ++++++++++++++++++++++++++++++++++++++++
  // Primary backing is checked first when both retained nested-storage products overflow.
  auto capacities = create_ordinary_m4_policy_capacities();
  capacities.max_private_audit_records = 65'536U;
  capacities.max_transition_effects_per_turn = 65'536U;
  capacities.max_order_callbacks_per_turn = 196'612U;
  REQUIRE(capacities.max_private_audit_records * capacities.max_transition_effects_per_turn ==
          maximum + 1U);
  REQUIRE((capacities.max_private_audit_records / 3U) * capacities.max_order_callbacks_per_turn >
          maximum);
  auto result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_transition_effects_per_turn");

  // ++++++++++++++++++++++++++++++++++++++++
  // Consecutive factors around 2^16 prove the exact accepted primary-backing boundary.
  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_private_audit_records = 65'535U;
  capacities.max_transition_effects_per_turn = 65'537U;
  REQUIRE(capacities.max_private_audit_records * capacities.max_transition_effects_per_turn ==
          maximum);
  REQUIRE(create_m4_policy(sealed, capacities));

  // ++++++++++++++++++++++++++++++++++++++++
  // Three slots reserved per callback span derive the exact Planned-buffer pool upper bound.
  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_private_audit_records = 196'608U;
  capacities.max_transition_effects_per_turn = 5U;
  capacities.max_order_callbacks_per_turn = 65'536U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_order_callbacks_per_turn");

  capacities.max_private_audit_records = 196'605U;
  capacities.max_order_callbacks_per_turn = 65'537U;
  REQUIRE((capacities.max_private_audit_records / 3U) * capacities.max_order_callbacks_per_turn ==
          maximum);
  REQUIRE(create_m4_policy(sealed, capacities));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Both checked reconciliation/recovery products reject overflow and accept exact u32 boundaries.
TEST_CASE("M4 policy checks every reconciliation and recovery product") {
  const auto sealed = create_policy_authority_or_throw();
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());

  auto capacities = create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_batches = maximum;
  capacities.max_reconciliation_rows_per_batch = 2U;
  auto result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_reconciliation_rows_per_batch");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_reconciliation_batches = maximum;
  capacities.max_reconciliation_rows_per_batch = 1U;
  REQUIRE(create_m4_policy(sealed, capacities));

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_recovery_epochs = (maximum / 2U) + 1U;
  capacities.max_namespace_registrations = capacities.max_recovery_epochs + 1U;
  capacities.max_recovery_notifications = 2U;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_recovery_notifications");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_recovery_epochs = 65'535U;
  capacities.max_namespace_registrations = 65'536U;
  capacities.max_recovery_notifications = 65'537U;
  REQUIRE(capacities.max_recovery_epochs * capacities.max_recovery_notifications == maximum);
  REQUIRE(create_m4_policy(sealed, capacities));
}

// --------------------------------------------------------
// Each disconnected M1-M3 authority fails before policy bytes or root provenance can exist.
TEST_CASE("M4 policy rejects every mismatched sealed authority") {
  const auto first = create_policy_authority_or_throw();
  const auto second = create_policy_authority_or_throw(true);

  auto result = aegis::runtime::M4Policy::create_m4_policy(
      second.configuration, first.runtime_policy, first.submission->reservations().policy(),
      first.submission->policy(), create_ordinary_m4_policy_capacities());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.runtime_policy_fingerprint");

  result = aegis::runtime::M4Policy::create_m4_policy(
      first.configuration, first.runtime_policy, second.submission->reservations().policy(),
      first.submission->policy(), create_ordinary_m4_policy_capacities());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.risk_policy_fingerprint");

  result = aegis::runtime::M4Policy::create_m4_policy(
      first.configuration, first.runtime_policy, first.submission->reservations().policy(),
      second.submission->policy(), create_ordinary_m4_policy_capacities());
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.submission_policy_fingerprint");
}

// --------------------------------------------------------
// Maximal drain and epoch values fail at their checked addition relationships before wrap.
TEST_CASE("M4 policy rejects unrepresentable drain and namespace relationships") {
  const auto sealed = create_policy_authority_or_throw();
  const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  auto capacities = create_ordinary_m4_policy_capacities();
  capacities.max_pending_fill_intervals_per_order = maximum;
  auto result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field ==
          "m4_policy.capacities.max_pending_fill_intervals_per_order");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_pending_fill_intervals_per_order = maximum - 2U;
  capacities.max_transition_effects_per_turn = maximum;
  capacities.max_order_callbacks_per_turn = maximum;
  capacities.max_private_audit_records = maximum;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_private_audit_records");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_pending_fill_intervals_per_order = maximum - 3U;
  capacities.max_transition_effects_per_turn = maximum - 2U;
  capacities.max_order_callbacks_per_turn = maximum - 2U;
  capacities.max_private_audit_records = maximum;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_transition_effects_per_turn");

  capacities = create_ordinary_m4_policy_capacities();
  capacities.max_recovery_epochs = maximum;
  capacities.max_namespace_registrations = maximum;
  result = create_m4_policy(sealed, capacities);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().context.field == "m4_policy.capacities.max_namespace_registrations");

  STATIC_REQUIRE(static_cast<std::uint16_t>(aegis::model::DomainErrorCode::InvalidM4Policy) ==
                 930U);
}

// --------------------------------------------------------
