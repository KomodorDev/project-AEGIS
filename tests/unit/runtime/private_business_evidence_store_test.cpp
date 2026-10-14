// Purpose: prove bounded typed business evidence preparation validates every borrowed value before
// leasing cold backing, retaining complete immutable proposals without publication or ordinal use.

#include "aegis/runtime/private_business_evidence_store.hpp"
#include "private_business_evidence_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;
using test_support::BusinessEvidenceFixture;
using test_support::BusinessEvidenceInput;
using test_support::extract_evidence_value_or_throw;

// ########################################################################
// Opaque construction and const access prevent component callers from granting publication rights.
template <typename Store>
concept HasBusinessEvidencePublication =
    requires(Store& store, runtime::PreparedPrivateBusinessEvidence&& plan) {
      store.publish_private_business_evidence(std::move(plan));
    };

static_assert(!std::is_copy_constructible_v<runtime::PreparedPrivateBusinessEvidence>);
static_assert(std::is_nothrow_move_constructible_v<runtime::PreparedPrivateBusinessEvidence>);
static_assert(!std::is_default_constructible_v<runtime::PreparedPrivateBusinessEvidence>);
static_assert(!std::is_copy_constructible_v<runtime::PrivateBusinessEvidenceStore>);
static_assert(!std::is_move_constructible_v<runtime::PrivateBusinessEvidenceStore>);
static_assert(!HasBusinessEvidencePublication<runtime::PrivateBusinessEvidenceStore>);
static_assert(std::same_as<decltype(std::declval<const runtime::PreparedPrivateBusinessEvidence&>()
                                        .primary_audit_record_at(0U)),
                           const trace::OrderAuditRecord*>);
static_assert(!std::is_default_constructible_v<trace::PrivateEventEvidence>);
static_assert(!std::is_default_constructible_v<trace::OrderAuditRecord>);
static_assert(
    !std::is_constructible_v<recovery::PrivateEventInputJournalPayload,
                             oms::NormalizedPrivateOrderInput, oms::PrivateEventResolution>);

// ########################################################################

// --------------------------------------------------------
// Author a literal executor position without generating or acknowledging an admission turn.
[[nodiscard]] recovery::JournalReplayProvenance create_store_replay_provenance_or_throw() {
  return recovery::JournalReplayProvenance{
      test_support::create_m4_ordinal_or_throw<model::AdmissionOrdinal>(7U),
      test_support::create_m4_ordinal_or_throw<model::ReceiveSequence>(9U)};
}

// --------------------------------------------------------
// Check the store's canonical prefix and ordinal independently of any detached scratch contents.
void check_store_prefix_empty(const runtime::PrivateBusinessEvidenceStore& store) {
  CHECK(store.accepted_private_event_record_count() == 0U);
  CHECK(store.accepted_private_audit_record_count() == 0U);
  CHECK(store.next_audit_ordinal().value() == 1U);
}

// --------------------------------------------------------

// ########################################################################
// Own one genuine sealed known resolution and separately authored acknowledgement effect/callback
// rows. Changing authored arrays exercises generic validation without replacing canonical state.
class AcknowledgementEvidenceStoreFixture final {
public:

  // --------------------------------------------------------
  // Copy exact live before projections and author the literal acknowledgement-only after tuple.
  explicit AcknowledgementEvidenceStoreFixture(
      runtime::M4PolicyCapacities capacities = test_support::create_ordinary_m4_policy_capacities())
      : owner{execution::OrderSide::Buy, capacities},
        input{owner.normalize_business_input_or_throw(BusinessEvidenceInput::Acknowledgement)},
        identity{extract_evidence_value_or_throw(
            owner.owner().derive_first_seen_authoritative_identity_plan(
                oms::PrivateEventIngressSemanticValue::from_normalized_input(input)))},
        resolution{
            std::get<runtime::KnownFirstSeenPrivateCorrelationPlan>(identity.correlation_plan())
                .resolution},
        effects{create_acknowledgement_effect_or_throw()},
        callbacks{trace::OrderCallbackAuditValue{
            owner.order().order_id(), resolution.known_resolution()->provenance,
            trace::originating_event_identity_from_normalized_input(input), std::nullopt,
            std::nullopt}},
        store{extract_evidence_value_or_throw(
            runtime::PrivateBusinessEvidenceStore::create_private_business_evidence_store(
                owner.authority.m4_policy, owner.owner().recovery_lineage_id(),
                owner.owner().runtime_epoch_id()))} {}

  // --------------------------------------------------------
  // Author one exact known-order acknowledgement, whose economics and safety remain unchanged.
  [[nodiscard]] trace::PrivateOrderAuditEffect create_acknowledgement_effect_or_throw() const {
    const auto before = owner.order().private_projection();
    auto after = before;
    after.state = oms::OutboundOrderState::Working;
    after.exchange_acknowledged = true;
    after.exchange_order_id = owner.create_exchange_order_id_or_throw();
    const auto* reservation =
        owner.authority.submission->reservations().find_reservation(owner.order().reservation_id());
    if (reservation == nullptr || resolution.known_resolution() == nullptr) {
      throw std::logic_error{"missing genuine acknowledgement fixture authority"};
    }
    return trace::PrivateOrderAuditEffect{owner.order().order_id(),
                                          resolution.known_resolution()->provenance,
                                          before,
                                          after,
                                          *reservation,
                                          *reservation,
                                          std::nullopt,
                                          std::nullopt};
  }

  // --------------------------------------------------------
  // Borrow one row's complete source and effect frame only during generic store validation.
  [[nodiscard]] runtime::PrimaryPrivateAuditInput create_primary_input() const {
    return runtime::PrimaryPrivateAuditInput{input,        resolution,
                                             std::nullopt, oms::PrivateEventDisposition::Applied,
                                             effects,      trace::CallbackDecision::None,
                                             std::nullopt};
  }

  // --------------------------------------------------------
  // Prepare authored acknowledgement rows with the complete single-callback prospective range.
  [[nodiscard]] model::Result<runtime::PreparedPrivateBusinessEvidence>
  prepare_acknowledgement_evidence() {
    const auto primary = create_primary_input();
    const auto range =
        extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
            test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 1U));
    return store->prepare_known_private_business_evidence(
        input, resolution, std::nullopt, create_store_replay_provenance_or_throw(),
        std::span{&primary, 1U}, range, callbacks);
  }

  // --------------------------------------------------------
  BusinessEvidenceFixture owner;
  oms::NormalizedPrivateOrderInput input;
  runtime::FirstSeenAuthoritativePrivateIdentityPlan identity;
  oms::PrivateEventResolution resolution;
  std::array<trace::PrivateOrderAuditEffect, 1U> effects;
  std::array<trace::OrderCallbackAuditValue, 1U> callbacks;
  std::unique_ptr<runtime::PrivateBusinessEvidenceStore> store;
};

// ########################################################################

// --------------------------------------------------------
// Author the full coherent seven-scope exposure tuple from integer residual and confirmed units;
// these literals are independent from production reservation conversion and replacement algorithms.
[[nodiscard]] risk::RiskScopeExposure create_drain_scope_or_throw(std::int64_t remaining,
                                                                  std::int64_t confirmed) {
  const auto create_quantity_or_throw = [](std::int64_t value) {
    return test_support::create_m4_decimal_or_throw<model::Quantity>(value);
  };
  const auto create_notional_or_throw = [](std::int64_t value) {
    return test_support::create_m4_decimal_or_throw<model::Notional>(value);
  };
  return risk::RiskScopeExposure{remaining > 0 ? 1U : 0U,
                                 create_notional_or_throw(remaining * 10),
                                 create_quantity_or_throw(remaining),
                                 create_quantity_or_throw(0),
                                 create_quantity_or_throw(remaining + confirmed),
                                 create_notional_or_throw(remaining * 10),
                                 create_notional_or_throw(0),
                                 create_notional_or_throw((remaining + confirmed) * 10),
                                 create_notional_or_throw((remaining + confirmed) * 10),
                                 create_quantity_or_throw(confirmed),
                                 create_notional_or_throw(confirmed * 10)};
}

// --------------------------------------------------------

// ########################################################################
// The generic component fixture authors an exact five-execution drain over a genuine Q=5 local
// subject. Its detached OMS/economic prefixes describe a valid prospective chain; no canonical row
// or pending registry is fabricated or committed, and only the initial owner supplies attribution.
class ExecutionDrainEvidenceFixture final {
public:

  // --------------------------------------------------------
  // Build all fixed source facts and literal per-execution prefixes before generic preparation.
  ExecutionDrainEvidenceFixture()
      : owner{execution::OrderSide::Buy, create_drain_capacities(), 5},
        store{extract_evidence_value_or_throw(
            runtime::PrivateBusinessEvidenceStore::create_private_business_evidence_store(
                owner.authority.m4_policy, owner.owner().recovery_lineage_id(),
                owner.owner().runtime_epoch_id()))} {
    inputs.reserve(5U);
    identities.reserve(5U);
    effects.reserve(5U);
    callbacks.reserve(5U);
    primaries.reserve(5U);
    const auto* held =
        owner.authority.submission->reservations().find_reservation(owner.order().reservation_id());
    if (held == nullptr) {
      throw std::logic_error{"missing genuine drain reservation"};
    }
    auto projection = owner.order().private_projection();
    projection.pending_fill_count = 4U;
    projection.execution_evidence_observed = true;
    auto reservation = *held;
    for (std::uint8_t index = 0U; index < 5U; ++index) {
      const auto increment = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
      const auto cumulative = test_support::create_m4_decimal_or_throw<model::Quantity>(index + 1);
      inputs.push_back(extract_evidence_value_or_throw(owner.factory.normalize_venue_execution(
          owner.create_venue_origin_or_throw(static_cast<std::uint8_t>(0x31U + index)),
          owner.create_business_locator_or_throw(),
          test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(
              static_cast<std::uint8_t>(0x61U + index)),
          owner.order().provenance().instrument_id, owner.order().provenance().metadata_revision,
          increment, cumulative, owner.order().economics().price, execution::OrderSide::Buy)));
      identities.push_back(extract_evidence_value_or_throw(
          owner.owner().derive_first_seen_authoritative_identity_plan(
              oms::PrivateEventIngressSemanticValue::from_normalized_input(inputs.back()))));
      const auto& resolution = std::get<runtime::KnownFirstSeenPrivateCorrelationPlan>(
                                   identities.back().correlation_plan())
                                   .resolution;
      auto after_projection = projection;
      after_projection.state =
          index == 4U ? oms::OutboundOrderState::Filled : oms::OutboundOrderState::PartiallyFilled;
      after_projection.exchange_order_id = owner.create_exchange_order_id_or_throw();
      after_projection.exchange_mapping_established_by_execution = true;
      after_projection.execution_evidence_observed = true;
      after_projection.cumulative_filled_quantity = cumulative;
      if (index != 0U) {
        --after_projection.pending_fill_count;
      }
      auto after_reservation = reservation;
      after_reservation.remaining_exposure.quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(4 - index);
      after_reservation.remaining_exposure.quote_notional =
          test_support::create_m4_decimal_or_throw<model::Notional>((4 - index) * 10);
      after_reservation.cumulative_confirmed_exposure.quantity = cumulative;
      after_reservation.cumulative_confirmed_exposure.quote_notional =
          test_support::create_m4_decimal_or_throw<model::Notional>((index + 1) * 10);
      if (index == 4U) {
        after_reservation.state = risk::ReservationState::ConsumedByFill;
        after_reservation.closure_cause = risk::ReservationClosureCause::FullFill;
      }
      effects.push_back(std::array{trace::PrivateOrderAuditEffect{
          owner.order().order_id(), resolution.known_resolution()->provenance, projection,
          after_projection, reservation, after_reservation,
          create_inventory_effects_or_throw(index), std::nullopt}});
      callbacks.push_back(trace::OrderCallbackAuditValue{
          owner.order().order_id(), resolution.known_resolution()->provenance,
          trace::originating_event_identity_from_normalized_input(inputs.back()),
          std::get<oms::ExecutionPayload>(inputs.back().payload()).trade_id, cumulative});
      projection = after_projection;
      reservation = after_reservation;
    }
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
      const auto& resolution = std::get<runtime::KnownFirstSeenPrivateCorrelationPlan>(
                                   identities[index].correlation_plan())
                                   .resolution;
      const auto& trade =
          std::get<runtime::FirstSeenPrivateTradeIdentityPlan>(identities[index].trade_plan())
              .semantic_value;
      primaries.push_back(runtime::PrimaryPrivateAuditInput{
          inputs[index], resolution, trade,
          index == 0U ? oms::PrivateEventDisposition::Applied
                      : oms::PrivateEventDisposition::AppliedFromBuffer,
          effects[index], trace::CallbackDecision::None, std::nullopt});
    }
  }

  // --------------------------------------------------------
  // Use the exact accepted drain width and audit span at their smallest coherent topology bounds.
  [[nodiscard]] static runtime::M4PolicyCapacities create_drain_capacities() noexcept {
    auto capacities = test_support::create_ordinary_m4_policy_capacities();
    capacities.max_private_event_records = 1U;
    capacities.max_transition_effects_per_turn = 5U;
    capacities.max_order_callbacks_per_turn = 5U;
    capacities.max_private_audit_records = 7U;
    return capacities;
  }

  // --------------------------------------------------------
  // Author each canonical scope key and its literal prefix, preserving exact before/delta/after.
  [[nodiscard]] std::array<trace::InventoryScopeAuditEffect, 7U>
  create_inventory_effects_or_throw(std::uint8_t index) const {
    const auto& provenance = owner.order().provenance();
    const auto currency =
        extract_evidence_value_or_throw(trace::QuoteCurrency::parse_quote_currency("USD"));
    const auto before = create_drain_scope_or_throw(5 - index, index);
    const auto after = create_drain_scope_or_throw(4 - index, index + 1);
    const auto delta_quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
    const auto delta_notional = test_support::create_m4_decimal_or_throw<model::Notional>(10);
    const std::array<trace::RiskScopeAuditSubject, 7U> subjects{
        provenance.bot_id,   provenance.desk_id,
        provenance.firm_id,  provenance.logical_account_id,
        provenance.route_id, provenance.instrument_id,
        provenance.venue_id};
    return {
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[0U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[1U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[2U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[3U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[4U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[5U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional},
        trace::InventoryScopeAuditEffect{provenance.firm_id, subjects[6U], provenance.instrument_id,
                                         currency, before, after, delta_quantity, delta_notional}};
  }

  // --------------------------------------------------------
  // Prepare every actual source execution, corresponding primary, and prospective callback in
  // order.
  [[nodiscard]] model::Result<runtime::PreparedPrivateBusinessEvidence> prepare_execution_drain() {
    const auto range =
        extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
            test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 5U));
    return store->prepare_known_private_business_evidence(
        inputs[0U], primaries[0U].first_admission_resolution, primaries[0U].trade_semantic_value,
        create_store_replay_provenance_or_throw(), primaries, range, callbacks);
  }

  // --------------------------------------------------------
  BusinessEvidenceFixture owner;
  std::unique_ptr<runtime::PrivateBusinessEvidenceStore> store;
  std::vector<oms::NormalizedPrivateOrderInput> inputs;
  std::vector<runtime::FirstSeenAuthoritativePrivateIdentityPlan> identities;
  std::vector<std::array<trace::PrivateOrderAuditEffect, 1U>> effects;
  std::vector<trace::OrderCallbackAuditValue> callbacks;
  std::vector<runtime::PrimaryPrivateAuditInput> primaries;
};

// ########################################################################

// --------------------------------------------------------
// Every possible audit primary owns full fixed effect backing, and the Planned pool uses exactly
// floor(audit/3); its absent terminal slot is counted in headroom without inventing a terminal row.
TEST_CASE("business evidence cold capacities exactly match audit topology products",
          "[private-business-evidence-store]") {
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  capacities.max_private_event_records = 1U;
  capacities.max_private_audit_records = 8U;
  capacities.max_transition_effects_per_turn = 5U;
  capacities.max_order_callbacks_per_turn = 5U;
  AcknowledgementEvidenceStoreFixture fixture{capacities};
  const auto& store = *fixture.store;
  CHECK(store.root_provenance() == fixture.owner.authority.m4_policy.root_provenance());
  CHECK(store.lineage_id() == fixture.owner.owner().recovery_lineage_id());
  CHECK(store.runtime_epoch_id() == fixture.owner.owner().runtime_epoch_id());
  CHECK(store.private_event_record_capacity() == 1U);
  CHECK(store.private_audit_record_capacity() == 8U);
  CHECK(store.primary_effect_buffer_count() == 8U);
  CHECK(store.primary_effect_buffer_capacity() == 5U);
  CHECK(store.planned_callback_buffer_count() == 2U);
  CHECK(store.planned_callback_buffer_capacity() == 5U);
  CHECK_FALSE(store.has_outstanding_preparation());
  check_store_prefix_empty(store);
  {
    const auto prepared = fixture.prepare_acknowledgement_evidence();
    REQUIRE(prepared);
    CHECK(store.has_outstanding_preparation());
    CHECK(prepared.value().primary_audit_record_count() == 1U);
    CHECK(prepared.value().audit_span()->audit_record_count() == 3U);
    CHECK(prepared.value().planned_callback_record()->audit_ordinal().value() == 2U);
    CHECK(prepared.value().reserved_terminal_audit_ordinal()->value() == 3U);
    CHECK(prepared.value().primary_audit_record_at(1U) == nullptr);
    check_store_prefix_empty(store);
  }
  CHECK_FALSE(store.has_outstanding_preparation());
  check_store_prefix_empty(store);
}

// --------------------------------------------------------
// An opaque plan exclusively owns scratch; moves revoke every source accessor, and releasing the
// final lease enables a deterministic retry at the same unconsumed audit ordinal.
TEST_CASE("business evidence lease moves and abandonment preserve empty canonical prefixes",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  std::optional<trace::PrivateEventEvidence> event;
  {
    auto first = fixture.prepare_acknowledgement_evidence();
    REQUIRE(first);
    event = *first.value().private_event_evidence();
    auto moved = std::move(first).value();
    CHECK_FALSE(first.value().has_preparation());
    CHECK(first.value().private_event_evidence() == nullptr);
    CHECK(first.value().journal_payload() == nullptr);
    CHECK(first.value().audit_span() == nullptr);
    CHECK(first.value().primary_audit_record_count() == 0U);
    CHECK(first.value().primary_audit_record_at(0U) == nullptr);
    CHECK(first.value().planned_callback_record() == nullptr);
    CHECK_FALSE(first.value().reserved_terminal_audit_ordinal());
    CHECK(first.value().order_callback_count() == 0U);
    const auto busy = fixture.prepare_acknowledgement_evidence();
    REQUIRE_FALSE(busy);
    CHECK(busy.error().code == model::DomainErrorCode::PrivateEvidenceExhausted);
    CHECK(moved.private_event_evidence()->input() == fixture.input);
    CHECK(moved.primary_audit_record_at(0U)->effect_at(0U) != nullptr);
    check_store_prefix_empty(*fixture.store);
  }
  CHECK_FALSE(fixture.store->has_outstanding_preparation());
  const auto repeated = fixture.prepare_acknowledgement_evidence();
  REQUIRE(repeated);
  CHECK(*repeated.value().private_event_evidence() == *event);
  CHECK(repeated.value().audit_span()->first_audit_ordinal().value() == 1U);
}

// --------------------------------------------------------
// Complete immutable nested rows remain inspectable after both the scratch store and its source
// owner disappear; no borrowed input/effect/callback span escapes successful preparation.
TEST_CASE("business evidence prepared values outlive every borrowed input and owner",
          "[private-business-evidence-store]") {
  auto fixture = std::make_unique<AcknowledgementEvidenceStoreFixture>();
  auto result = fixture->prepare_acknowledgement_evidence();
  REQUIRE(result);
  auto prepared = std::move(result).value();
  const auto input = fixture->input;
  const auto effect = fixture->effects[0U];
  const auto callback = fixture->callbacks[0U];
  fixture.reset();
  REQUIRE(prepared.has_preparation());
  CHECK(prepared.private_event_evidence()->input() == input);
  CHECK(prepared.journal_payload()->input() == input);
  CHECK(*prepared.primary_audit_record_at(0U)->effect_at(0U) == effect);
  CHECK(*prepared.planned_callback_record()->callback_at(0U) == callback);
  CHECK(prepared.audit_span()->audit_record_count() == 3U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// A malformed known subject, OMS/reservation pairing, or callback fact cannot acquire a lease or
// retain a partial prefix; every failed attempt permits immediate reuse by the unchanged valid row.
TEST_CASE("business evidence rejects inconsistent effect and callback fields before retaining rows",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto original_effect = fixture.effects[0U];
  const auto original_callback = fixture.callbacks[0U];
  const auto foreign = test_support::create_m4_order_id_or_throw(99U);
  for (std::uint32_t defect = 0U; defect < 12U; ++defect) {
    CAPTURE(defect);
    fixture.effects[0U] = original_effect;
    fixture.callbacks[0U] = original_callback;
    switch (defect) {
    case 0U:
      fixture.effects[0U].order_id = foreign;
      break;
    case 1U:
      fixture.effects[0U].reservation_after.side = execution::OrderSide::Sell;
      break;
    case 2U:
      fixture.effects[0U].reservation_after.state = risk::ReservationState::Released;
      break;
    case 3U:
      fixture.effects[0U].oms_after.cumulative_filled_quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(1);
      break;
    case 4U:
      fixture.callbacks[0U].order_id = foreign;
      break;
    case 5U:
      fixture.callbacks[0U].originating_event =
          trace::OriginatingEventIdentity::create_without_originating_event();
      break;
    case 6U:
      fixture.callbacks[0U].trade_id =
          test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);
      break;
    case 7U:
      fixture.callbacks[0U].applied_cumulative_quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(1);
      break;
    case 8U:
      fixture.effects[0U].oms_after.exchange_order_id =
          test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x7FU);
      break;
    case 9U:
      fixture.effects[0U].oms_after.pending_fill_count = 5U;
      break;
    case 10U:
      fixture.effects[0U].oms_after.cancel_attempt_count = 33U;
      break;
    default:
      fixture.effects[0U].oms_after.cancellation_state = oms::CancellationState::Unassigned;
      break;
    }
    const auto rejected = fixture.prepare_acknowledgement_evidence();
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(fixture.store->has_outstanding_preparation());
    check_store_prefix_empty(*fixture.store);
    fixture.effects[0U] = original_effect;
    fixture.callbacks[0U] = original_callback;
    const auto repaired = fixture.prepare_acknowledgement_evidence();
    REQUIRE(repaired);
    CHECK(*repaired.value().primary_audit_record_at(0U)->effect_at(0U) == original_effect);
  }
}

// --------------------------------------------------------
// Every nested span has its own exact bound and shape; empty/oversized input, a mismatched primary,
// or an aggregate callback profile on a primary cannot bypass fixed backing preflight.
TEST_CASE("business evidence preflights individual bounds and callback topology atomically",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto replay = create_store_replay_provenance_or_throw();
  const auto range =
      extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
          test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 1U));
  const auto two_callbacks =
      extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
          test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 2U));
  const auto original_primary = fixture.create_primary_input();
  std::vector<runtime::PrimaryPrivateAuditInput> too_many_primary(6U, original_primary);
  std::vector<trace::PrivateOrderAuditEffect> too_many_effects(33U, fixture.effects[0U]);
  std::vector<trace::OrderCallbackAuditValue> too_many_callbacks(33U, fixture.callbacks[0U]);
  for (std::uint32_t defect = 0U; defect < 10U; ++defect) {
    CAPTURE(defect);
    auto primary = original_primary;
    std::span<const runtime::PrimaryPrivateAuditInput> primaries{&primary, 1U};
    std::span<const trace::OrderCallbackAuditValue> callbacks{fixture.callbacks};
    std::optional<trace::CallbackOrdinalRange> callback_range{range};
    switch (defect) {
    case 0U:
      primaries = {};
      break;
    case 1U:
      primaries = too_many_primary;
      break;
    case 2U:
      primary.effects = {};
      break;
    case 3U:
      primary.effects = too_many_effects;
      break;
    case 4U:
      callbacks = too_many_callbacks;
      break;
    case 5U:
      callback_range.reset();
      break;
    case 6U:
      callbacks = {};
      break;
    case 7U:
      callback_range = two_callbacks;
      break;
    case 8U:
      primary.callback_decision = trace::CallbackDecision::Planned;
      break;
    default:
      primary.disposition = static_cast<oms::PrivateEventDisposition>(0U);
      break;
    }
    const auto rejected = fixture.store->prepare_known_private_business_evidence(
        fixture.input, fixture.resolution, std::nullopt, replay, primaries, callback_range,
        callbacks);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(fixture.store->has_outstanding_preparation());
    check_store_prefix_empty(*fixture.store);
    const auto repaired = fixture.prepare_acknowledgement_evidence();
    REQUIRE(repaired);
    CHECK(repaired.value().audit_span()->first_audit_ordinal().value() == 1U);
  }
}

// --------------------------------------------------------
// Complete policy identity is compared before retention; a sealed resolution from a different
// capacity-policy root is still foreign authority, even when logical source identifiers match.
TEST_CASE("business evidence rejects foreign sealed root and conflicting primary sources",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  auto capacities = test_support::create_ordinary_m4_policy_capacities();
  ++capacities.max_private_event_records;
  const AcknowledgementEvidenceStoreFixture foreign{capacities};
  const auto original_primary = fixture.create_primary_input();
  const auto range =
      extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
          test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 1U));
  const auto foreign_root = fixture.store->prepare_known_private_business_evidence(
      foreign.input, foreign.resolution, std::nullopt, create_store_replay_provenance_or_throw(),
      std::span{&original_primary, 1U}, range, fixture.callbacks);
  REQUIRE_FALSE(foreign_root);
  const auto different_input =
      fixture.owner.normalize_business_input_or_throw(BusinessEvidenceInput::PartialFill);
  const runtime::PrimaryPrivateAuditInput mismatched_primary{
      different_input, fixture.resolution,
      std::nullopt,    oms::PrivateEventDisposition::Applied,
      fixture.effects, trace::CallbackDecision::None,
      std::nullopt};
  const auto conflicting_source = fixture.store->prepare_known_private_business_evidence(
      fixture.input, fixture.resolution, std::nullopt, create_store_replay_provenance_or_throw(),
      std::span{&mismatched_primary, 1U}, range, fixture.callbacks);
  REQUIRE_FALSE(conflicting_source);
  CHECK_FALSE(fixture.store->has_outstanding_preparation());
  check_store_prefix_empty(*fixture.store);
  const auto valid = fixture.prepare_acknowledgement_evidence();
  REQUIRE(valid);
}

// --------------------------------------------------------

// --------------------------------------------------------
// The exact accepted drain boundary retains every execution identity, economic prefix and callback;
// a five-primary span occupies seven audit slots while the terminal row remains absent.
TEST_CASE(
    "business evidence prepares maximum drain with independent complete per-execution prefixes",
    "[private-business-evidence-store]") {
  ExecutionDrainEvidenceFixture fixture;
  const auto canonical_before = fixture.owner.order().private_projection();
  const auto canonical_reservation =
      *fixture.owner.authority.submission->reservations().find_reservation(
          fixture.owner.order().reservation_id());
  auto result = fixture.prepare_execution_drain();
  REQUIRE(result);
  const auto& prepared = result.value();
  CHECK(prepared.primary_audit_record_count() == 5U);
  CHECK(prepared.audit_span()->audit_record_count() == 7U);
  CHECK(prepared.audit_span()->last_audit_ordinal().value() == 7U);
  CHECK(prepared.reserved_terminal_audit_ordinal()->value() == 7U);
  const auto* planned = prepared.planned_callback_record();
  REQUIRE(planned != nullptr);
  CHECK(planned->audit_ordinal().value() == 6U);
  CHECK(planned->callback_count() == 5U);
  CHECK(planned->callback_range()->first_callback_ordinal().value() == 11U);
  CHECK(planned->callback_range()->last_callback_ordinal().value() == 15U);
  for (std::uint32_t index = 0U; index < 5U; ++index) {
    const auto* row = prepared.primary_audit_record_at(index);
    REQUIRE(row != nullptr);
    CHECK(row->audit_ordinal().value() == index + 1U);
    CHECK(row->source_input() == fixture.inputs[index]);
    CHECK(row->disposition() == (index == 0U ? oms::PrivateEventDisposition::Applied
                                             : oms::PrivateEventDisposition::AppliedFromBuffer));
    CHECK(row->effect_count() == 1U);
    CHECK(*row->effect_at(0U) == fixture.effects[index][0U]);
    CHECK(*planned->callback_at(index) == fixture.callbacks[index]);
    const auto* effect = row->effect_at(0U);
    REQUIRE(effect->inventory_effects);
    CHECK(effect->reservation_before.remaining_exposure.quantity ==
          test_support::create_m4_decimal_or_throw<model::Quantity>(5 - index));
    CHECK(effect->reservation_after.remaining_exposure.quantity ==
          test_support::create_m4_decimal_or_throw<model::Quantity>(4 - index));
    CHECK(effect->reservation_after.cumulative_confirmed_exposure.quantity ==
          test_support::create_m4_decimal_or_throw<model::Quantity>(index + 1U));
    for (const auto& inventory : *effect->inventory_effects) {
      CHECK(inventory.before == create_drain_scope_or_throw(5 - index, index));
      CHECK(inventory.after == create_drain_scope_or_throw(4 - index, index + 1U));
      CHECK(inventory.signed_quantity_delta ==
            test_support::create_m4_decimal_or_throw<model::Quantity>(1));
      CHECK(inventory.signed_notional_delta ==
            test_support::create_m4_decimal_or_throw<model::Notional>(10));
    }
  }
  CHECK(prepared.primary_audit_record_at(5U) == nullptr);
  CHECK(planned->callback_at(5U) == nullptr);
  check_store_prefix_empty(*fixture.store);
  CHECK(fixture.owner.order().private_projection() == canonical_before);
  CHECK(*fixture.owner.authority.submission->reservations().find_reservation(
            fixture.owner.order().reservation_id()) == canonical_reservation);
  CHECK(fixture.owner.owner().event_identity_record_count() == 0U);
}

// --------------------------------------------------------
// Fully authored chains reject a changed scope prefix, broken ordering, duplicate event/trade,
// source mapping conflict, or complete numeric inconsistency before any scratch becomes leased.
TEST_CASE("business evidence validates each complete drain prefix and immutable source identity",
          "[private-business-evidence-store]") {
  ExecutionDrainEvidenceFixture fixture;
  const auto original_effects = fixture.effects;
  const auto original_inputs = fixture.inputs;
  const auto original_callbacks = fixture.callbacks;
  for (std::uint32_t defect = 0U; defect < 14U; ++defect) {
    CAPTURE(defect);
    for (std::size_t index = 0U; index < fixture.inputs.size(); ++index) {
      fixture.inputs[index] = original_inputs[index];
      fixture.effects[index] = original_effects[index];
      fixture.callbacks[index] = original_callbacks[index];
    }
    auto& effect = fixture.effects[1U][0U];
    auto& scope = (*effect.inventory_effects)[0U];
    switch (defect) {
    case 0U:
      scope.after.gross_reserved_quote_notional =
          test_support::create_m4_decimal_or_throw<model::Notional>(999);
      break;
    case 1U:
      scope.after.reserved_buy_quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(999);
      break;
    case 2U:
      scope.after.reserved_sell_quote_notional =
          test_support::create_m4_decimal_or_throw<model::Notional>(1);
      break;
    case 3U:
      scope.after.worst_case_position_quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(999);
      break;
    case 4U:
      scope.after.worst_case_position_quote_notional =
          test_support::create_m4_decimal_or_throw<model::Notional>(999);
      break;
    case 5U:
      std::swap((*effect.inventory_effects)[0U], (*effect.inventory_effects)[1U]);
      break;
    case 6U:
      effect.oms_before.pending_fill_count = 1U;
      break;
    case 7U: {
      auto before = create_drain_scope_or_throw(4, 2);
      auto after = create_drain_scope_or_throw(3, 3);
      scope.before = before;
      scope.after = after;
      break;
    }
    case 11U:
      fixture.effects[0U][0U].oms_after.execution_evidence_observed = false;
      fixture.effects[0U][0U].oms_after.exchange_mapping_established_by_execution = false;
      fixture.effects[1U][0U].oms_before = fixture.effects[0U][0U].oms_after;
      break;
    case 13U:
      fixture.effects[0U][0U].oms_before.execution_evidence_observed = false;
      break;
    case 12U:
      fixture.effects[1U][0U].oms_after.pending_fill_count = 4U;
      fixture.effects[2U][0U].oms_before.pending_fill_count = 4U;
      break;
    default: {
      const auto& original = std::get<oms::ExecutionPayload>(original_inputs[1U].payload());
      const auto event = defect == 8U ? 0x31U : 0x32U;
      const auto trade = defect == 9U ? 0x61U : 0x62U;
      auto locator = original.locator;
      if (defect == 10U) {
        locator =
            extract_evidence_value_or_throw(oms::PrivateOrderLocator::create_private_order_locator(
                fixture.owner.order().order_id(),
                test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x7FU)));
      }
      fixture.inputs[1U] =
          extract_evidence_value_or_throw(fixture.owner.factory.normalize_venue_execution(
              fixture.owner.create_venue_origin_or_throw(static_cast<std::uint8_t>(event)), locator,
              test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(
                  static_cast<std::uint8_t>(trade)),
              original.instrument_id, original.metadata_revision, original.incremental_quantity,
              original.cumulative_quantity, original.execution_price, original.source_side));
      fixture.callbacks[1U].originating_event =
          trace::originating_event_identity_from_normalized_input(fixture.inputs[1U]);
      fixture.callbacks[1U].trade_id =
          std::get<oms::ExecutionPayload>(fixture.inputs[1U].payload()).trade_id;
      break;
    }
    }
    const auto rejected = fixture.prepare_execution_drain();
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(fixture.store->has_outstanding_preparation());
    check_store_prefix_empty(*fixture.store);
  }
  for (std::size_t index = 0U; index < fixture.inputs.size(); ++index) {
    fixture.inputs[index] = original_inputs[index];
    fixture.effects[index] = original_effects[index];
    fixture.callbacks[index] = original_callbacks[index];
  }
  const auto repaired = fixture.prepare_execution_drain();
  REQUIRE(repaired);
  CHECK(repaired.value().primary_audit_record_count() == 5U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Replay suppression and exact duplicate observations retain complete rows without inventing
// callbacks, terminal ordinals, repeated OMS mutations, or economic application.
TEST_CASE("business evidence callback-free profiles preserve suppression and one-row spans",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  for (const auto disposition :
       {oms::PrivateEventDisposition::Applied, oms::PrivateEventDisposition::ExactEventDuplicate}) {
    CAPTURE(disposition);
    auto primary = fixture.create_primary_input();
    auto unchanged_effect = fixture.effects[0U];
    if (disposition == oms::PrivateEventDisposition::Applied) {
      primary.callback_decision = trace::CallbackDecision::SuppressedReplay;
    } else {
      unchanged_effect.oms_after = unchanged_effect.oms_before;
      primary.effects = std::span{&unchanged_effect, 1U};
      primary.callback_decision = trace::CallbackDecision::SuppressedDuplicate;
      primary.disposition = disposition;
    }
    const auto prepared = fixture.store->prepare_known_private_business_evidence(
        fixture.input, fixture.resolution, std::nullopt, create_store_replay_provenance_or_throw(),
        std::span{&primary, 1U}, std::nullopt, {});
    REQUIRE(prepared);
    CHECK(prepared.value().private_event_evidence()->disposition() == disposition);
    CHECK(prepared.value().primary_audit_record_at(0U)->callback_decision() ==
          primary.callback_decision);
    CHECK(prepared.value().audit_span()->audit_record_count() == 1U);
    CHECK(prepared.value().order_callback_count() == 0U);
    CHECK(prepared.value().planned_callback_record() == nullptr);
    CHECK_FALSE(prepared.value().reserved_terminal_audit_ordinal());
    check_store_prefix_empty(*fixture.store);
  }
}

// --------------------------------------------------------
// Inline textual values retain their whole semantic spelling and exact error position; malformed
// currency or an overlong/unassigned error is rejected rather than truncated or sanitized.
TEST_CASE("business evidence inline currency and error bounds preserve exact complete fields",
          "[private-business-evidence-store]") {
  for (const auto currency : {"USD", "USD123456789"}) {
    const auto parsed = trace::QuoteCurrency::parse_quote_currency(currency);
    REQUIRE(parsed);
    CHECK(parsed.value().value() == currency);
  }
  for (const auto invalid : {"", "US", "usd", "1USD", "USD-", "USD1234567890"}) {
    CHECK_FALSE(trace::QuoteCurrency::parse_quote_currency(invalid));
  }
  const std::string embedded_zero{"a\0b", 3U};
  const auto exact_error = model::DomainError::create_at_index(
      model::DomainErrorCode::InvalidPrivateEvent, embedded_zero, 4U);
  const auto bounded = trace::BoundedAuditDomainError::from_domain_error(exact_error);
  REQUIRE(bounded);
  CHECK(bounded.value().code() == model::DomainErrorCode::InvalidPrivateEvent);
  CHECK(bounded.value().field() == std::string_view{embedded_zero});
  CHECK(bounded.value().collection_index() == 4U);
  const auto maximum =
      trace::BoundedAuditDomainError::from_domain_error(model::DomainError::create_at_field(
          model::DomainErrorCode::InvalidPrivateEvent, std::string(256U, 'x')));
  REQUIRE(maximum);
  CHECK(maximum.value().field() == std::string(256U, 'x'));
  CHECK_FALSE(trace::BoundedAuditDomainError::from_domain_error(model::DomainError::create_at_field(
      model::DomainErrorCode::InvalidPrivateEvent, std::string(257U, 'x'))));
  CHECK_FALSE(trace::BoundedAuditDomainError::from_domain_error(
      model::DomainError::create_at_field(static_cast<model::DomainErrorCode>(0U), "invalid")));
}

// --------------------------------------------------------

// --------------------------------------------------------
// Exact trade duplicates keep the already-applied cumulative prefix unchanged while retaining the
// complete separate trade comparison and suppressing all callback and economic repeat effects.
TEST_CASE("business evidence exact trade duplicate retains its closed trade tuple without effects",
          "[private-business-evidence-store]") {
  ExecutionDrainEvidenceFixture fixture;
  auto unchanged_effect = fixture.effects[1U][0U];
  unchanged_effect.oms_after = unchanged_effect.oms_before;
  unchanged_effect.reservation_after = unchanged_effect.reservation_before;
  unchanged_effect.inventory_effects.reset();
  const auto& source = fixture.primaries[0U];
  const runtime::PrimaryPrivateAuditInput primary{source.input,
                                                  source.first_admission_resolution,
                                                  source.trade_semantic_value,
                                                  oms::PrivateEventDisposition::ExactTradeDuplicate,
                                                  std::span{&unchanged_effect, 1U},
                                                  trace::CallbackDecision::SuppressedDuplicate,
                                                  std::nullopt};
  const auto result = fixture.store->prepare_known_private_business_evidence(
      source.input, source.first_admission_resolution, source.trade_semantic_value,
      create_store_replay_provenance_or_throw(), std::span{&primary, 1U}, std::nullopt, {});
  REQUIRE(result);
  CHECK(result.value().private_event_evidence()->trade_semantic_value() ==
        source.trade_semantic_value);
  CHECK(result.value().private_event_evidence()->disposition() ==
        oms::PrivateEventDisposition::ExactTradeDuplicate);
  CHECK(result.value().primary_audit_record_at(0U)->callback_decision() ==
        trace::CallbackDecision::SuppressedDuplicate);
  CHECK(result.value().primary_audit_record_at(0U)->effect_at(0U)->oms_before ==
        result.value().primary_audit_record_at(0U)->effect_at(0U)->oms_after);
  CHECK(result.value().audit_span()->audit_record_count() == 1U);
  CHECK(result.value().planned_callback_record() == nullptr);
  check_store_prefix_empty(*fixture.store);
}

// --------------------------------------------------------

// --------------------------------------------------------
// A syntactically valid source local locator confers no substitute ownership: even a copied sealed
// genuine resolution must reject a different explicit local identifier before preparing any row.
TEST_CASE("business evidence rejects explicit source locator inconsistent with sealed known order",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto changed =
      extract_evidence_value_or_throw(fixture.owner.factory.normalize_venue_acknowledgement(
          fixture.owner.create_venue_origin_or_throw(),
          fixture.owner.create_exchange_order_id_or_throw(),
          test_support::create_m4_order_id_or_throw(99U)));
  const runtime::PrimaryPrivateAuditInput primary{
      changed,         fixture.resolution,
      std::nullopt,    oms::PrivateEventDisposition::Applied,
      fixture.effects, trace::CallbackDecision::None,
      std::nullopt};
  auto callback = fixture.callbacks[0U];
  callback.originating_event = trace::originating_event_identity_from_normalized_input(changed);
  const auto range =
      extract_evidence_value_or_throw(trace::CallbackOrdinalRange::create_callback_ordinal_range(
          test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U), 1U));
  const auto rejected = fixture.store->prepare_known_private_business_evidence(
      changed, fixture.resolution, std::nullopt, create_store_replay_provenance_or_throw(),
      std::span{&primary, 1U}, range, std::span{&callback, 1U});
  REQUIRE_FALSE(rejected);
  CHECK_FALSE(fixture.store->has_outstanding_preparation());
  check_store_prefix_empty(*fixture.store);
  const auto repaired = fixture.prepare_acknowledgement_evidence();
  REQUIRE(repaired);
}

// --------------------------------------------------------

// --------------------------------------------------------
// A coherent standalone economic frame does not prove its source caused it: acknowledgement input
// must neither invent execution economics nor borrow a definitive rejection's residual release.
TEST_CASE("business evidence rejects coherent economics incompatible with the nonexecution source",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto replay = create_store_replay_provenance_or_throw();
  const auto callback_ordinal =
      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  const auto range = extract_evidence_value_or_throw(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(callback_ordinal, 1U));
  for (const auto fabricated_cause :
       {BusinessEvidenceInput::FullFill, BusinessEvidenceInput::ExchangeRejection}) {
    CAPTURE(fabricated_cause);
    const auto source = fixture.owner.normalize_business_input_or_throw(fabricated_cause);
    auto genuine = fixture.owner.owner().prepare_initial_known_authoritative_business_evidence(
        source, replay, callback_ordinal);
    REQUIRE(genuine);
    const auto copied_effect = *genuine.value().primary_audit_record_at(0U)->effect_at(0U);
    REQUIRE(trace::validate_private_order_audit_effect(copied_effect,
                                                       fixture.store->root_provenance()));
    const runtime::PrimaryPrivateAuditInput falsely_attributed{
        fixture.input,
        fixture.resolution,
        std::nullopt,
        oms::PrivateEventDisposition::Applied,
        std::span{&copied_effect, 1U},
        trace::CallbackDecision::None,
        std::nullopt};
    const auto rejected = fixture.store->prepare_known_private_business_evidence(
        fixture.input, fixture.resolution, std::nullopt, replay, std::span{&falsely_attributed, 1U},
        range, fixture.callbacks);
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(fixture.store->has_outstanding_preparation());
    check_store_prefix_empty(*fixture.store);
  }
  const auto repaired = fixture.prepare_acknowledgement_evidence();
  REQUIRE(repaired);
}

// --------------------------------------------------------

// --------------------------------------------------------
// An exact event replay preserves the original pre-trade side-conflict resolution and absence of a
// trade tuple. It emits only duplicate evidence; an exact trade replay cannot bypass that boundary.
TEST_CASE(
    "business evidence event duplicates preserve pretrade side conflict without trade authority",
    "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto input =
      fixture.owner.normalize_business_input_or_throw(BusinessEvidenceInput::ContradictorySide);
  const auto replay = create_store_replay_provenance_or_throw();
  const auto callback_ordinal =
      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  std::optional<oms::PrivateEventResolution> resolution;
  std::optional<trace::PrivateOrderAuditEffect> original_effect;
  std::optional<trace::OrderCallbackAuditValue> original_callback;
  {
    auto genuine = fixture.owner.owner().prepare_initial_known_authoritative_business_evidence(
        input, replay, callback_ordinal);
    REQUIRE(genuine);
    CHECK(genuine.value().private_event_evidence()->disposition() ==
          oms::PrivateEventDisposition::SafetyContained);
    CHECK_FALSE(genuine.value().private_event_evidence()->trade_semantic_value());
    resolution = genuine.value().private_event_evidence()->first_admission_resolution();
    original_effect = *genuine.value().primary_audit_record_at(0U)->effect_at(0U);
    original_callback = *genuine.value().planned_callback_record()->callback_at(0U);
  }
  REQUIRE(resolution);
  REQUIRE(original_effect);
  REQUIRE(original_callback);
  REQUIRE(original_effect->account_safety_transition);
  CHECK(original_effect->oms_after.execution_evidence_observed);
  CHECK_FALSE(fixture.owner.owner().business_evidence_store().has_outstanding_preparation());
  {
    const runtime::PrimaryPrivateAuditInput contained{input,
                                                      *resolution,
                                                      std::nullopt,
                                                      oms::PrivateEventDisposition::SafetyContained,
                                                      std::span{&*original_effect, 1U},
                                                      trace::CallbackDecision::None,
                                                      std::nullopt};
    const auto range = extract_evidence_value_or_throw(
        trace::CallbackOrdinalRange::create_callback_ordinal_range(callback_ordinal, 1U));
    const auto prepared = fixture.store->prepare_known_private_business_evidence(
        input, *resolution, std::nullopt, replay, std::span{&contained, 1U}, range,
        std::span{&*original_callback, 1U});
    REQUIRE(prepared);
    CHECK_FALSE(prepared.value().private_event_evidence()->trade_semantic_value());
    check_store_prefix_empty(*fixture.store);
  }
  auto unchanged = *original_effect;
  unchanged.oms_before = unchanged.oms_after;
  unchanged.account_safety_transition.reset();
  {
    const runtime::PrimaryPrivateAuditInput duplicate{
        input,
        *resolution,
        std::nullopt,
        oms::PrivateEventDisposition::ExactEventDuplicate,
        std::span{&unchanged, 1U},
        trace::CallbackDecision::SuppressedDuplicate,
        std::nullopt};
    const auto prepared = fixture.store->prepare_known_private_business_evidence(
        input, *resolution, std::nullopt, replay, std::span{&duplicate, 1U}, std::nullopt, {});
    REQUIRE(prepared);
    CHECK(prepared.value().private_event_evidence()->first_admission_resolution() == *resolution);
    CHECK_FALSE(prepared.value().private_event_evidence()->trade_semantic_value());
    CHECK(prepared.value().private_event_evidence()->disposition() ==
          oms::PrivateEventDisposition::ExactEventDuplicate);
    CHECK(*prepared.value().primary_audit_record_at(0U)->effect_at(0U) == unchanged);
    CHECK(prepared.value().primary_audit_record_at(0U)->callback_decision() ==
          trace::CallbackDecision::SuppressedDuplicate);
    CHECK(prepared.value().audit_span()->audit_record_count() == 1U);
    CHECK(prepared.value().planned_callback_record() == nullptr);
    CHECK(prepared.value().order_callback_count() == 0U);
    check_store_prefix_empty(*fixture.store);
  }
  auto missing_execution_cache = unchanged;
  missing_execution_cache.oms_before.execution_evidence_observed = false;
  missing_execution_cache.oms_after.execution_evidence_observed = false;
  const runtime::PrimaryPrivateAuditInput invalid_execution_duplicate{
      input,
      *resolution,
      std::nullopt,
      oms::PrivateEventDisposition::ExactEventDuplicate,
      std::span{&missing_execution_cache, 1U},
      trace::CallbackDecision::SuppressedDuplicate,
      std::nullopt};
  const auto missing_cache = fixture.store->prepare_known_private_business_evidence(
      input, *resolution, std::nullopt, replay, std::span{&invalid_execution_duplicate, 1U},
      std::nullopt, {});
  REQUIRE_FALSE(missing_cache);
  CHECK_FALSE(fixture.store->has_outstanding_preparation());
  check_store_prefix_empty(*fixture.store);
  const runtime::PrimaryPrivateAuditInput invalid_trade_duplicate{
      input,
      *resolution,
      std::nullopt,
      oms::PrivateEventDisposition::ExactTradeDuplicate,
      std::span{&unchanged, 1U},
      trace::CallbackDecision::SuppressedDuplicate,
      std::nullopt};
  const auto rejected = fixture.store->prepare_known_private_business_evidence(
      input, *resolution, std::nullopt, replay, std::span{&invalid_trade_duplicate, 1U},
      std::nullopt, {});
  REQUIRE_FALSE(rejected);
  CHECK_FALSE(fixture.store->has_outstanding_preparation());
  check_store_prefix_empty(*fixture.store);
  CHECK(fixture.owner.order().private_projection().execution_evidence_observed == false);
  CHECK(fixture.owner.owner().account_safety_state(
            fixture.owner.order().provenance().logical_account_id) ==
        risk::AccountSafetyState::Synchronized);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Definitive terminal projections cannot retain pending fills. Preserve the execution cache when
// forging a pending row so the closed-terminal check, rather than cache absence, rejects the frame.
TEST_CASE("business evidence rejects pending fills on definitive terminal projections",
          "[private-business-evidence-store]") {
  AcknowledgementEvidenceStoreFixture fixture;
  const auto replay = create_store_replay_provenance_or_throw();
  const auto callback_ordinal =
      test_support::create_m4_ordinal_or_throw<model::CallbackOrdinal>(11U);
  const auto range = extract_evidence_value_or_throw(
      trace::CallbackOrdinalRange::create_callback_ordinal_range(callback_ordinal, 1U));
  for (const auto terminal :
       {BusinessEvidenceInput::CancellationAtZero, BusinessEvidenceInput::ExchangeRejection}) {
    CAPTURE(terminal);
    const auto input = fixture.owner.normalize_business_input_or_throw(terminal);
    auto genuine = fixture.owner.owner().prepare_initial_known_authoritative_business_evidence(
        input, replay, callback_ordinal);
    REQUIRE(genuine);
    auto malformed = *genuine.value().primary_audit_record_at(0U)->effect_at(0U);
    REQUIRE(
        trace::validate_private_order_audit_effect(malformed, fixture.store->root_provenance()));
    const auto resolution = genuine.value().private_event_evidence()->first_admission_resolution();
    const auto callback = *genuine.value().planned_callback_record()->callback_at(0U);
    malformed.oms_after.pending_fill_count = 1U;
    malformed.oms_after.execution_evidence_observed = true;
    CHECK_FALSE(
        trace::validate_private_order_audit_effect(malformed, fixture.store->root_provenance()));
    const runtime::PrimaryPrivateAuditInput primary{input,
                                                    resolution,
                                                    std::nullopt,
                                                    oms::PrivateEventDisposition::Applied,
                                                    std::span{&malformed, 1U},
                                                    trace::CallbackDecision::None,
                                                    std::nullopt};
    const auto rejected = fixture.store->prepare_known_private_business_evidence(
        input, resolution, std::nullopt, replay, std::span{&primary, 1U}, range,
        std::span{&callback, 1U});
    REQUIRE_FALSE(rejected);
    CHECK_FALSE(fixture.store->has_outstanding_preparation());
    check_store_prefix_empty(*fixture.store);
  }
}

// --------------------------------------------------------

} // namespace
