// Purpose: prove bounded identity preparation is transactional and repeat-safe while candidate
// mappings, canonical registries, OMS economics, and reservation ownership remain uncommitted.

#include "aegis/runtime/private_identity_preparation.hpp"
#include "aegis/runtime/private_order_reconciler.hpp"
#include "m4_private_event_fixture.hpp"
#include "m4_test_authority.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Keep the test's observation names explicit while preserving the production source-private types.
using PreparationClass = runtime::PrivateIdentityPreparationClassification;
using PreparationCapacity = runtime::PrivateIdentityPreparationCapacity;
using IdentityPlan = runtime::FirstSeenAuthoritativePrivateIdentityPlan;

static_assert(!std::is_copy_constructible_v<runtime::PrivateIdentityPreparationStore>);
static_assert(!std::is_move_constructible_v<runtime::PrivateIdentityPreparationStore>);

// ########################################################################

// --------------------------------------------------------
// Extract a validated fixture value or report a setup defect before exercising production logic.
template <typename Value> [[nodiscard]] Value extract_result_or_throw(model::Result<Value> result) {
  if (!result) {
    throw std::logic_error{"invalid private identity preparation fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------

// ########################################################################
// This fixture obtains every sealed plan from genuine production owner correlation after a real
// bot-context submission. The preparation store remains independent from canonical owner state.
class IdentityPreparationFixture final {
public:

  // --------------------------------------------------------
  // Install exact recovery authority before the first genuine local submission; the seed allows
  // independent fixtures to prove that a full candidate table cannot partially retain a fifth row.
  explicit IdentityPreparationFixture(std::uint8_t namespace_seed = 0x30U)
      : authority{test_support::create_m4_owner_test_authority_or_throw()},
        factory{
            extract_result_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
                authority.configuration, authority.m4_policy))} {
    test_support::install_recovery_bound_private_order_reconciler_or_throw(authority, 0x10U,
                                                                           namespace_seed);
    const auto submitted = test_support::submit_m4_order_or_throw(
        authority, test_support::create_m4_reference_order_request_or_throw());
    if (!submitted.order_id() ||
        submitted.disposition() != execution::SubmitDisposition::WriteInitiated) {
      throw std::logic_error{"missing genuine order in identity preparation fixture"};
    }
    order_id = *submitted.order_id();
  }

  // --------------------------------------------------------
  // Borrow the genuine retained row whose identity and provenance authorize every known plan.
  [[nodiscard]] const oms::OutboundOrderRecord& order_or_throw() const {
    const auto* row = authority.submission->outbound_oms().find_order(*order_id);
    if (row == nullptr) {
      throw std::logic_error{"missing retained row in identity preparation fixture"};
    }
    return *row;
  }

  // --------------------------------------------------------
  // Borrow the installed read-only planner after construction established its owner binding.
  [[nodiscard]] const runtime::PrivateOrderReconciler& reconciler() const noexcept {
    return *authority.submission->private_order_reconciler();
  }

  // --------------------------------------------------------
  // Create an ordinary timestamp-free event identity in the genuine order's account and venue.
  [[nodiscard]] oms::VenuePrivateIngressOrigin
  create_venue_origin_or_throw(std::uint8_t event_byte, std::uint64_t source_time = 100U,
                               std::uint8_t source_epoch_byte = 0x41U) const {
    const auto& provenance = order_or_throw().provenance();
    return oms::VenuePrivateIngressOrigin{
        oms::VenuePrivateEventKey{
            provenance.venue_id, provenance.logical_account_id,
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(
                source_epoch_byte),
            test_support::create_m4_opaque_identity_or_throw<oms::PrivateEventId>(event_byte)},
        model::SourceTimestamp{source_time}};
  }

  // --------------------------------------------------------
  // Derive a sealed acknowledgement with independently selectable raw ownership and exchange bytes.
  [[nodiscard]] IdentityPlan
  create_acknowledgement_plan_or_throw(std::uint8_t event_byte, std::uint8_t exchange_byte = 0x61U,
                                       bool include_local_order = true,
                                       std::uint64_t source_time = 100U) const {
    auto attempt = extract_result_or_throw(factory.create_venue_acknowledgement_attempt(
        create_venue_origin_or_throw(event_byte, source_time),
        test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(exchange_byte),
        include_local_order ? order_id : std::nullopt));
    return extract_result_or_throw(
        reconciler().derive_first_seen_authoritative_identity_plan(attempt.semantic_value()));
  }

  // --------------------------------------------------------
  // Derive one closed execution tuple; source epoch, price, source-side presence and raw exchange
  // identity vary independently so duplicate tests prove the accepted comparison boundaries.
  [[nodiscard]] IdentityPlan create_execution_plan_or_throw(
      std::uint8_t event_byte, std::uint8_t trade_byte = 0x71U, std::uint8_t exchange_byte = 0x61U,
      std::optional<execution::OrderSide> source_side = std::nullopt, std::int64_t price = 99,
      bool include_local_order = true, std::uint8_t source_epoch_byte = 0x41U) const {
    const auto& row = order_or_throw();
    auto locator = extract_result_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
        include_local_order ? order_id : std::nullopt,
        test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(exchange_byte)));
    auto attempt = extract_result_or_throw(factory.create_venue_execution_attempt(
        create_venue_origin_or_throw(event_byte, 100U, source_epoch_byte), std::move(locator),
        test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(trade_byte),
        row.provenance().instrument_id, row.provenance().metadata_revision,
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Price>(price), source_side));
    return extract_result_or_throw(
        reconciler().derive_first_seen_authoritative_identity_plan(attempt.semantic_value()));
  }

  // --------------------------------------------------------
  // Derive a matching reconciliation trade under a disjoint row identity and required source side.
  [[nodiscard]] IdentityPlan create_reconciliation_execution_plan_or_throw() const {
    const auto& row = order_or_throw();
    auto locator = extract_result_or_throw(
        oms::PrivateOrderLocator::create_private_order_locator(order_id, std::nullopt));
    auto attempt = extract_result_or_throw(factory.create_reconciliation_execution_attempt(
        oms::ReconciliationPrivateIngressOrigin{
            test_support::create_m4_reconciliation_epoch_or_throw(),
            test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
            test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(1U),
            model::SourceTimestamp{100U}},
        row.provenance().logical_account_id, row.provenance().venue_id, std::move(locator),
        test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U),
        row.provenance().instrument_id, row.provenance().metadata_revision,
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Quantity>(1),
        test_support::create_m4_decimal_or_throw<model::Price>(99), execution::OrderSide::Buy));
    return extract_result_or_throw(
        reconciler().derive_first_seen_authoritative_identity_plan(attempt.semantic_value()));
  }

  // --------------------------------------------------------
  // Retain real authority and producer normalization beside the one validated local identity.
  test_support::M4OwnerTestAuthority authority;
  runtime::PrivateOrderEventFactory factory;
  std::optional<model::OrderId> order_id;
};

// ########################################################################
// Snapshot every occupied immutable preparation, so rollback assertions inspect contents as well
// as counts and independently prove that no previously retained tuple was overwritten.
struct PreparationSnapshot {
  std::vector<runtime::PreparedPrivateEventRecord> events;
  std::vector<runtime::PreparedPrivateTradeRecord> trades;
  std::vector<runtime::PreparedPrivateMappingCandidate> candidates;

  // --------------------------------------------------------
  // Compare full insertion-ordered preparation histories.
  friend bool operator==(const PreparationSnapshot&, const PreparationSnapshot&) = default;

  // --------------------------------------------------------
};

// ########################################################################

// --------------------------------------------------------
// Copy each occupied table through its read-only inspection boundary.
[[nodiscard]] PreparationSnapshot
create_preparation_snapshot(const runtime::PrivateIdentityPreparationStore& store) {
  PreparationSnapshot snapshot;
  for (std::uint32_t index = 0U; index < store.event_record_count(); ++index) {
    snapshot.events.push_back(*store.event_record_at(index));
  }
  for (std::uint32_t index = 0U; index < store.trade_record_count(); ++index) {
    snapshot.trades.push_back(*store.trade_record_at(index));
  }
  for (std::uint32_t index = 0U; index < store.mapping_candidate_count(); ++index) {
    snapshot.candidates.push_back(*store.mapping_candidate_at(index));
  }
  return snapshot;
}

// --------------------------------------------------------
// Derive a valid sealed policy with independently authored event/trade and legal mapping bounds.
[[nodiscard]] runtime::M4Policy create_preparation_policy_or_throw(std::uint64_t events = 32U,
                                                                   std::uint64_t trades = 32U,
                                                                   std::uint64_t mappings = 32U) {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_event_identity_records = events;
  capacities.max_trade_identity_records = trades;
  capacities.max_exchange_order_mappings = mappings;
  return test_support::create_m4_test_authority_or_throw(capacities).m4_policy;
}

// --------------------------------------------------------
// First observations populate all required preparation tables without changing canonical owner
// identity, original reservation exposure, order state, or its economic projection.
TEST_CASE("M4 identity preparation retains a complete first observation atomically",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{fixture.authority.m4_policy};
  const auto plan = fixture.create_execution_plan_or_throw(1U);
  const auto projection_before = fixture.order_or_throw().private_projection();
  const auto admission_before = fixture.order_or_throw().admission();
  const auto held_before = fixture.authority.submission->reservations().held_reservation_count();
  CHECK(store.event_record_capacity() == 32U);
  CHECK(store.trade_record_capacity() == 32U);
  CHECK(store.mapping_candidate_capacity() == 32U);
  CHECK(store.event_record_at(0U) == nullptr);
  CHECK(store.trade_record_at(0U) == nullptr);
  CHECK(store.mapping_candidate_at(0U) == nullptr);

  const auto prepared = store.prepare_and_retain_identity(plan);
  CHECK(prepared.classification == PreparationClass::FirstObservation);
  CHECK_FALSE(prepared.capacity_exhaustion);
  CHECK_FALSE(prepared.safety_reason);
  CHECK(store.event_record_count() == 1U);
  CHECK(store.trade_record_count() == 1U);
  CHECK(store.mapping_candidate_count() == 1U);
  const auto* const event = store.find_prepared_event(plan.event_key());
  REQUIRE(event != nullptr);
  CHECK(event == store.event_record_at(0U));
  CHECK(event->ingress_semantic_value == plan.ingress_semantic_value());
  CHECK(event->resolution == prepared.resolution);
  CHECK(event->classification == PreparationClass::FirstObservation);
  const auto& trade = std::get<runtime::FirstSeenPrivateTradeIdentityPlan>(plan.trade_plan());
  CHECK(store.trade_record_at(0U)->key == trade.key);
  CHECK(store.trade_record_at(0U)->semantic_value == trade.semantic_value);
  CHECK(store.mapping_candidate_at(0U)->order_id == *fixture.order_id);
  CHECK(store.mapping_candidate_at(0U)->exchange_order_key.exchange_order_id ==
        test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U));
  CHECK(fixture.reconciler().event_identity_record_count() == 0U);
  CHECK(fixture.reconciler().trade_identity_record_count() == 0U);
  CHECK(fixture.reconciler().exchange_order_mapping_count() == 0U);
  CHECK(fixture.order_or_throw().private_projection() == projection_before);
  CHECK(fixture.order_or_throw().admission() == admission_before);
  CHECK(fixture.authority.submission->reservations().held_reservation_count() == held_before);
  CHECK(store.event_record_at(1U) == nullptr);
  CHECK(store.trade_record_at(1U) == nullptr);
  CHECK(store.mapping_candidate_at(1U) == nullptr);
}

// --------------------------------------------------------
// Event identity dominates capacity and independent trade checks; conflicts retain the original
// complete ingress and resolution instead of adopting any later caller's sealed plan.
TEST_CASE("M4 identity preparation preserves exact and conflicting event replays",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(1U, 1U)};
  const auto plan = fixture.create_execution_plan_or_throw(1U);
  REQUIRE_FALSE(store.prepare_and_retain_identity(plan).capacity_exhaustion);
  const auto before = create_preparation_snapshot(store);
  const auto* const original = store.find_prepared_event(plan.event_key());
  REQUIRE(original != nullptr);

  const auto repeated = store.prepare_and_retain_identity(plan);
  CHECK(repeated.classification == PreparationClass::RepeatedEvent);
  CHECK_FALSE(repeated.capacity_exhaustion);
  CHECK_FALSE(repeated.safety_reason);
  CHECK(repeated.resolution == original->resolution);
  CHECK(runtime::PrivateIdentityPreparationStore::classify_repeated_event(
            *original, plan.ingress_semantic_value()) == repeated);

  const auto contradictory =
      fixture.create_execution_plan_or_throw(1U, 0x71U, 0x61U, std::nullopt, 100);
  const auto conflict = store.prepare_and_retain_identity(contradictory);
  CHECK(conflict.classification == PreparationClass::EventConflict);
  CHECK_FALSE(conflict.capacity_exhaustion);
  CHECK(conflict.safety_reason == risk::AccountSafetyReason::EventIdentityConflict);
  CHECK(conflict.resolution == original->resolution);
  CHECK(create_preparation_snapshot(store) == before);
  CHECK(store.find_prepared_event(plan.event_key()) == original);
}

// --------------------------------------------------------
// Source time belongs to complete event ingress, while exact first-event comparison never adds a
// second diagnostic candidate claim for the same source identity.
TEST_CASE("M4 identity preparation compares source time in event identity reuse",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{fixture.authority.m4_policy};
  const auto original = fixture.create_acknowledgement_plan_or_throw(1U);
  REQUIRE_FALSE(store.prepare_and_retain_identity(original).capacity_exhaustion);
  const auto before = create_preparation_snapshot(store);
  const auto changed = fixture.create_acknowledgement_plan_or_throw(1U, 0x61U, true, 101U);
  CHECK(store.prepare_and_retain_identity(changed).classification ==
        PreparationClass::EventConflict);
  CHECK(create_preparation_snapshot(store) == before);
}

// --------------------------------------------------------
// Distinct event keys still deduplicate a complete trade tuple across source epochs, optional
// known source-side presence, and reconciliation origin; candidate changes never create ownership.
TEST_CASE("M4 identity preparation deduplicates trades across ordinary and reconciliation input",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(8U, 1U)};
  const auto original = fixture.create_execution_plan_or_throw(1U);
  REQUIRE_FALSE(store.prepare_and_retain_identity(original).capacity_exhaustion);
  const auto original_trade = *store.trade_record_at(0U);
  const auto original_candidate = *store.mapping_candidate_at(0U);
  const auto ordinary = fixture.create_execution_plan_or_throw(
      2U, 0x71U, 0x62U, execution::OrderSide::Buy, 99, true, 0x42U);
  const auto reconciliation = fixture.create_reconciliation_execution_plan_or_throw();
  for (const auto* plan : {&ordinary, &reconciliation}) {
    const auto result = store.prepare_and_retain_identity(*plan);
    CHECK(result.classification == PreparationClass::RepeatedTrade);
    CHECK_FALSE(result.capacity_exhaustion);
    CHECK_FALSE(result.safety_reason);
    CHECK(store.find_prepared_event(plan->event_key())->resolution == result.resolution);
  }
  CHECK(store.event_record_count() == 3U);
  CHECK(store.trade_record_count() == 1U);
  CHECK(store.mapping_candidate_count() == 1U);
  CHECK(*store.trade_record_at(0U) == original_trade);
  CHECK(*store.mapping_candidate_at(0U) == original_candidate);
  CHECK(store.event_record_at(2U)->key.origin() == oms::PrivateEventOrigin::Reconciliation);
}

// --------------------------------------------------------
// A changed included economic field conflicts with the retained trade even when its own table is
// full, preserving the first tuple and suppressing the later event's candidate claim.
TEST_CASE("M4 identity preparation preserves the first conflicting trade tuple",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(8U, 1U)};
  REQUIRE_FALSE(store.prepare_and_retain_identity(fixture.create_execution_plan_or_throw(1U))
                    .capacity_exhaustion);
  const auto original_trade = *store.trade_record_at(0U);
  const auto changed = fixture.create_execution_plan_or_throw(2U, 0x71U, 0x62U, std::nullopt, 100);
  const auto result = store.prepare_and_retain_identity(changed);
  CHECK(result.classification == PreparationClass::TradeConflict);
  CHECK(result.safety_reason == risk::AccountSafetyReason::TradeIdentityConflict);
  CHECK_FALSE(result.capacity_exhaustion);
  CHECK(store.event_record_count() == 2U);
  CHECK(store.trade_record_count() == 1U);
  CHECK(store.mapping_candidate_count() == 1U);
  CHECK(*store.trade_record_at(0U) == original_trade);
  CHECK(store.event_record_at(1U)->classification == PreparationClass::TradeConflict);
}

// --------------------------------------------------------
// The candidate table diagnoses two incompatible claims without becoming the permanent mapping
// authority used by correlation; exchange-only input remains unknown afterward.
TEST_CASE("M4 identity preparation keeps candidate conflicts outside ownership correlation",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  IdentityPreparationFixture second_owner{0x80U};
  runtime::PrivateIdentityPreparationStore store{fixture.authority.m4_policy};
  REQUIRE_FALSE(store.prepare_and_retain_identity(fixture.create_acknowledgement_plan_or_throw(1U))
                    .capacity_exhaustion);
  const auto original_candidate = *store.mapping_candidate_at(0U);
  const auto same_claim = fixture.create_acknowledgement_plan_or_throw(2U);
  CHECK(store.prepare_and_retain_identity(same_claim).classification ==
        PreparationClass::FirstObservation);

  const auto reverse_conflict = fixture.create_acknowledgement_plan_or_throw(3U, 0x62U);
  const auto direct_conflict = second_owner.create_acknowledgement_plan_or_throw(4U);
  for (const auto* plan : {&reverse_conflict, &direct_conflict}) {
    const auto result = store.prepare_and_retain_identity(*plan);
    CHECK(result.classification == PreparationClass::MappingConflict);
    CHECK_FALSE(result.capacity_exhaustion);
    CHECK_FALSE(result.safety_reason);
    CHECK(result.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Known);
  }
  const auto exchange_only = fixture.create_acknowledgement_plan_or_throw(5U, 0x61U, false);
  const auto unknown = store.prepare_and_retain_identity(exchange_only);
  CHECK(unknown.classification == PreparationClass::UnknownOrder);
  CHECK(unknown.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Unknown);
  CHECK(unknown.safety_reason == risk::AccountSafetyReason::UnknownOrder);
  CHECK(store.event_record_count() == 5U);
  CHECK(store.mapping_candidate_count() == 1U);
  CHECK(*store.mapping_candidate_at(0U) == original_candidate);
  CHECK(fixture.reconciler().exchange_order_mapping_count() == 0U);
}

// --------------------------------------------------------
// Known source-side contradiction stops before trade lookup and suppresses every candidate,
// preserving the sealed Known resolution and the authoritative contradiction reason.
TEST_CASE("M4 identity preparation stops source-side conflict before trade and candidate retention",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(8U, 1U)};
  REQUIRE_FALSE(store.prepare_and_retain_identity(fixture.create_execution_plan_or_throw(1U))
                    .capacity_exhaustion);
  const auto original_trade = *store.trade_record_at(0U);
  const auto mismatch =
      fixture.create_execution_plan_or_throw(2U, 0x71U, 0x62U, execution::OrderSide::Sell);
  const auto result = store.prepare_and_retain_identity(mismatch);
  CHECK(result.classification == PreparationClass::SourceSideConflict);
  CHECK(result.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  CHECK(result.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Known);
  CHECK_FALSE(result.capacity_exhaustion);
  CHECK(store.event_record_count() == 2U);
  CHECK(store.trade_record_count() == 1U);
  CHECK(store.mapping_candidate_count() == 1U);
  CHECK(*store.trade_record_at(0U) == original_trade);
}

// --------------------------------------------------------
// Unknown comparison remains immutable when a distinct event later supplies genuine local
// ownership for the same trade; a previous candidate never silently adopts the unknown fact.
TEST_CASE("M4 identity preparation conflicts on unknown to known trade resolution changes",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{fixture.authority.m4_policy};
  const auto unknown = fixture.create_execution_plan_or_throw(1U, 0x71U, 0x61U,
                                                              execution::OrderSide::Buy, 99, false);
  const auto first = store.prepare_and_retain_identity(unknown);
  CHECK(first.classification == PreparationClass::UnknownOrder);
  CHECK(first.safety_reason == risk::AccountSafetyReason::UnknownTrade);
  const auto original_trade = *store.trade_record_at(0U);
  const auto known = fixture.create_execution_plan_or_throw(2U);
  const auto conflict = store.prepare_and_retain_identity(known);
  CHECK(conflict.classification == PreparationClass::TradeConflict);
  CHECK(conflict.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Known);
  CHECK(store.mapping_candidate_count() == 0U);
  CHECK(*store.trade_record_at(0U) == original_trade);
  const auto replay = store.prepare_and_retain_identity(unknown);
  CHECK(replay.classification == PreparationClass::RepeatedEvent);
  CHECK(replay.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Unknown);
}

// --------------------------------------------------------
// Event capacity wins before a simultaneously needed trade slot, and failure does not publish the
// new candidate or overwrite any retained history. An exact replay still needs no headroom.
TEST_CASE("M4 identity preparation rolls back every table at event capacity",
          "[runtime][m4][identity-preparation][capacity]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(1U, 1U)};
  const auto first = fixture.create_execution_plan_or_throw(1U);
  REQUIRE_FALSE(store.prepare_and_retain_identity(first).capacity_exhaustion);
  const auto before = create_preparation_snapshot(store);
  const auto next = fixture.create_execution_plan_or_throw(2U, 0x72U);
  const auto result = store.prepare_and_retain_identity(next);
  CHECK(result.capacity_exhaustion == PreparationCapacity::EventRecords);
  CHECK(create_preparation_snapshot(store) == before);
  CHECK(store.find_prepared_event(next.event_key()) == nullptr);
  CHECK(store.prepare_and_retain_identity(first).classification == PreparationClass::RepeatedEvent);
  CHECK(create_preparation_snapshot(store) == before);
}

// --------------------------------------------------------
// Trade exhaustion cannot strand the new event key or its candidate. A later nonexecution can
// use the still-free event slot, proving that rollback is real and retry behavior is deterministic.
TEST_CASE("M4 identity preparation rolls back every table at trade capacity",
          "[runtime][m4][identity-preparation][capacity]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(3U, 1U)};
  REQUIRE_FALSE(store.prepare_and_retain_identity(fixture.create_execution_plan_or_throw(1U))
                    .capacity_exhaustion);
  const auto before = create_preparation_snapshot(store);
  const auto next = fixture.create_execution_plan_or_throw(2U, 0x72U);
  const auto result = store.prepare_and_retain_identity(next);
  CHECK(result.capacity_exhaustion == PreparationCapacity::TradeRecords);
  CHECK(store.prepare_and_retain_identity(next) == result);
  CHECK(create_preparation_snapshot(store) == before);
  CHECK(store.find_prepared_event(next.event_key()) == nullptr);
  CHECK_FALSE(store.prepare_and_retain_identity(fixture.create_acknowledgement_plan_or_throw(3U))
                  .capacity_exhaustion);
  CHECK(store.event_record_count() == 2U);
  CHECK(store.trade_record_count() == 1U);
  CHECK(store.mapping_candidate_count() == 1U);
}

// --------------------------------------------------------
// Four is the policy's minimum mapping capacity for the genuine four-slot OMS fixture. Separate
// genuine owners supply distinct sealed local identities so a fifth candidate can reach the full
// table without bypassing production plan construction or mutating a canonical registry.
TEST_CASE("M4 identity preparation rolls back every table at candidate capacity",
          "[runtime][m4][identity-preparation][capacity]") {
  runtime::PrivateIdentityPreparationStore store{create_preparation_policy_or_throw(8U, 8U, 4U)};
  for (std::uint8_t index = 0U; index < 4U; ++index) {
    IdentityPreparationFixture fixture{static_cast<std::uint8_t>(0x30U + index)};
    const auto plan = fixture.create_acknowledgement_plan_or_throw(
        static_cast<std::uint8_t>(index + 1U), static_cast<std::uint8_t>(0x61U + index));
    REQUIRE_FALSE(store.prepare_and_retain_identity(plan).capacity_exhaustion);
  }
  CHECK(store.mapping_candidate_count() == 4U);
  const auto before = create_preparation_snapshot(store);
  IdentityPreparationFixture fifth_owner{0x80U};
  const auto next = fifth_owner.create_execution_plan_or_throw(5U, 0x71U, 0x65U);
  const auto result = store.prepare_and_retain_identity(next);
  CHECK(result.capacity_exhaustion == PreparationCapacity::CandidateMappings);
  CHECK(store.prepare_and_retain_identity(next) == result);
  CHECK(create_preparation_snapshot(store) == before);
  CHECK(store.find_prepared_event(next.event_key()) == nullptr);
  CHECK(store.trade_record_count() == 0U);
}

// --------------------------------------------------------
// A provenance conflict retains its original sealed conflict resolution. A later same-key input
// from matching authority remains an event conflict and cannot replace that first resolution.
TEST_CASE("M4 identity preparation preserves provenance conflicts before trade classification",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_event_identity_records = 31U;
  const auto foreign = test_support::create_m4_test_authority_or_throw(capacities);
  const runtime::PrivateOrderEventFactory foreign_factory{
      extract_result_or_throw(runtime::M4ProvenanceResolver::create_m4_provenance_resolver(
          foreign.configuration, foreign.m4_policy))};
  const auto foreign_attempt =
      extract_result_or_throw(foreign_factory.create_venue_acknowledgement_attempt(
          fixture.create_venue_origin_or_throw(1U),
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U),
          fixture.order_id));
  const auto foreign_plan =
      extract_result_or_throw(fixture.reconciler().derive_first_seen_authoritative_identity_plan(
          foreign_attempt.semantic_value()));
  runtime::PrivateIdentityPreparationStore store{fixture.authority.m4_policy};
  const auto first = store.prepare_and_retain_identity(foreign_plan);
  CHECK(first.classification == PreparationClass::PreTradeConflict);
  CHECK(first.safety_reason == risk::AccountSafetyReason::ProvenanceMismatch);
  CHECK(first.resolution.resolution_kind() == oms::PrivateEventResolutionKind::Conflict);
  CHECK_FALSE(first.capacity_exhaustion);
  CHECK(store.trade_record_count() == 0U);
  CHECK(store.mapping_candidate_count() == 0U);
  const auto before = create_preparation_snapshot(store);

  const auto repeated = store.prepare_and_retain_identity(foreign_plan);
  CHECK(repeated.classification == PreparationClass::RepeatedEvent);
  CHECK(repeated.resolution == first.resolution);
  const auto now_known = fixture.create_acknowledgement_plan_or_throw(1U);
  const auto conflict = store.prepare_and_retain_identity(now_known);
  CHECK(conflict.classification == PreparationClass::EventConflict);
  CHECK(conflict.safety_reason == risk::AccountSafetyReason::EventIdentityConflict);
  CHECK(conflict.resolution == first.resolution);
  CHECK(create_preparation_snapshot(store) == before);
}

// --------------------------------------------------------
// Independent storage instances reproduce complete insertion-order evidence over the same sealed
// script, including conflict outcomes, without pointer or container-order dependence.
TEST_CASE("M4 identity preparation reproduces deterministic histories",
          "[runtime][m4][identity-preparation]") {
  IdentityPreparationFixture fixture;
  runtime::PrivateIdentityPreparationStore first{fixture.authority.m4_policy};
  runtime::PrivateIdentityPreparationStore second{fixture.authority.m4_policy};
  const std::vector<IdentityPlan> script{
      fixture.create_execution_plan_or_throw(1U),
      fixture.create_execution_plan_or_throw(1U),
      fixture.create_execution_plan_or_throw(2U),
      fixture.create_execution_plan_or_throw(3U, 0x71U, 0x61U, std::nullopt, 100),
      fixture.create_acknowledgement_plan_or_throw(4U, 0x62U),
      fixture.create_acknowledgement_plan_or_throw(5U, 0x63U, false),
  };
  for (const auto& plan : script) {
    CHECK(first.prepare_and_retain_identity(plan) == second.prepare_and_retain_identity(plan));
    CHECK(create_preparation_snapshot(first) == create_preparation_snapshot(second));
  }
}

// --------------------------------------------------------

} // namespace
