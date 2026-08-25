// Purpose: prove the assigned M4 private-event vocabulary, bounded receive-time-free attempts,
// source-limited provenance, compatibility normalization, and ingress equality.

#include "aegis/oms/private_order_event.hpp"
#include "aegis/runtime/private_order_event_factory.hpp"
#include "m4_private_event_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

using namespace aegis;

// ########################################################################
// Stable private-event assignments are compatibility contracts rather than declaration-order facts.
static_assert(static_cast<std::uint8_t>(oms::PrivateEventOrigin::Local) == 1U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventOrigin::Venue) == 2U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventOrigin::Reconciliation) == 3U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventSubjectScope::Order) == 1U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventSubjectScope::Account) == 2U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventSubjectScope::PrivateSource) == 3U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::ExchangeAcknowledged) == 1U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::ExchangeRejected) == 2U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::Execution) == 3U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::CancelRequested) == 4U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::CancelWriteOutcome) == 5U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::CancellationResult) == 6U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::LocalFailure) == 7U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::TimeoutObserved) == 8U);
static_assert(static_cast<std::uint8_t>(oms::PrivateOrderEventKind::DisconnectObserved) == 9U);
static_assert(static_cast<std::uint8_t>(oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance) ==
              1U);
static_assert(static_cast<std::uint8_t>(oms::CancelWriteOutcome::AcceptedAndInitiated) == 2U);
static_assert(static_cast<std::uint8_t>(oms::CancelWriteOutcome::AcceptedThenOutcomeLost) == 3U);
static_assert(static_cast<std::uint8_t>(oms::CancellationResult::Cancelled) == 1U);
static_assert(static_cast<std::uint8_t>(oms::CancellationResult::CancelRejected) == 2U);
static_assert(static_cast<std::uint8_t>(oms::LocalFailureCertainty::ProvenBeforeAcceptance) == 1U);
static_assert(static_cast<std::uint8_t>(oms::LocalFailureCertainty::AcceptanceCouldHaveOccurred) ==
              2U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::Unspecified) == 1U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::InvalidOrder) == 2U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::InsufficientAuthority) ==
              3U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::InsufficientFunds) == 4U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::PostOnlyWouldCross) == 5U);
static_assert(static_cast<std::uint16_t>(oms::ExchangeRejectionCategory::VenueRiskRejected) == 6U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventResolutionKind::Known) == 1U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventResolutionKind::Unknown) == 2U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventResolutionKind::Conflict) == 3U);
static_assert(static_cast<std::uint8_t>(oms::PrivateEventResolutionKind::NotOrderScoped) == 4U);

// ########################################################################

// ########################################################################
// Producer attempts are nominal bounded values with no receive timestamp or public normalization.
// Interesting syntax: the requires-expressions turn forbidden public members into compile-time
// facts without invoking or granting either operation.
template <typename Value>
concept ExposesReceiveTime = requires(const Value& value) { value.receive_time(); };

template <typename Factory>
concept ExposesAttemptNormalization =
    requires(const Factory& factory, const oms::PrivateOrderIngressAttempt& attempt) {
      factory.normalize_private_order_ingress_attempt(attempt, model::ReceiveTimestamp{1U});
    };

static_assert(!std::is_default_constructible_v<oms::PrivateEventIngressSemanticValue>);
static_assert(!std::is_aggregate_v<oms::PrivateEventIngressSemanticValue>);
static_assert(!std::is_default_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(!std::is_aggregate_v<oms::PrivateOrderIngressAttempt>);
static_assert(!std::is_constructible_v<oms::PrivateOrderIngressAttempt,
                                       oms::PrivateEventIngressSemanticValue>);
static_assert(std::is_copy_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_copy_assignable_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_move_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_move_assignable_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateEventIngressSemanticValue>);
static_assert(std::is_nothrow_copy_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_copy_assignable_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_move_constructible_v<oms::PrivateOrderIngressAttempt>);
static_assert(std::is_nothrow_move_assignable_v<oms::PrivateOrderIngressAttempt>);
static_assert(!ExposesReceiveTime<oms::PrivateOrderIngressAttempt>);
static_assert(!ExposesAttemptNormalization<runtime::PrivateOrderEventFactory>);

// ########################################################################

// --------------------------------------------------------
// Construct one exact raw local/exchange locator and fail fast on a fixture defect.
[[nodiscard]] oms::PrivateOrderLocator
create_private_order_locator_or_throw(std::optional<model::OrderId> local_order_id,
                                      std::optional<oms::ExchangeOrderId> exchange_order_id) {
  auto created =
      oms::PrivateOrderLocator::create(std::move(local_order_id), std::move(exchange_order_id));
  if (!created) {
    throw std::logic_error{"invalid locator in private-event fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build the common both-identity locator without claiming either side is correlated.
[[nodiscard]] oms::PrivateOrderLocator
create_both_identity_locator_or_throw(const test_support::M4PrivateEventFixture& fixture,
                                      std::uint8_t exchange_byte = 0x61U) {
  return create_private_order_locator_or_throw(
      fixture.record().order_id(),
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(exchange_byte));
}

// --------------------------------------------------------
// Move one successful result into its immutable input value or fail on a test fixture defect.
[[nodiscard]] oms::NormalizedPrivateOrderInput take_normalized_private_order_input_or_throw(
    model::Result<oms::NormalizedPrivateOrderInput> result) {
  if (!result) {
    throw std::logic_error{"invalid normalized private-event fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Prove attempt and compatibility paths reject the same malformed source fact exactly.
template <typename AttemptValue, typename NormalizedValue>
void require_matching_failure(const model::Result<AttemptValue>& attempt,
                              const model::Result<NormalizedValue>& normalized) {
  REQUIRE_FALSE(attempt);
  REQUIRE_FALSE(normalized);
  CHECK(attempt.error() == normalized.error());
}

// --------------------------------------------------------
// Require direct ingress equality and receive-time-free projection equality to remain equivalent.
void require_ingress_semantic_oracles_agree(const oms::NormalizedPrivateOrderInput& left,
                                            const oms::NormalizedPrivateOrderInput& right) {
  CHECK(left.is_ingress_semantically_equal_to(right) ==
        (oms::PrivateEventIngressSemanticValue::from_normalized_input(left) ==
         oms::PrivateEventIngressSemanticValue::from_normalized_input(right)));
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// All eleven source-normalization profiles available before owner-bound OMS reconciliation produce
// their exact assigned normalized vocabulary; four local-order rows remain later owner-local
// construction.
TEST_CASE("normalized private event factory covers every accepted source shape", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto exchange_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto trade_id = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);
  const std::array detail{std::byte{0x01U}, std::byte{0x02U}};
  const auto quantity_one = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto quantity_two = test_support::create_m4_decimal_or_throw<model::Quantity>(2);
  const auto price = test_support::create_m4_decimal_or_throw<model::Price>(10);
  const auto revision = fixture.record().provenance().metadata_revision;

  // ++++++++++++++++++++++++++++++++++++++++
  // The two authoritative origins accept acknowledgement, rejection, execution, and cancellation.
  const auto venue_ack = factory.normalize_venue_acknowledgement(
      fixture.create_venue_private_event_origin_or_throw(1U), exchange_id,
      fixture.record().order_id());
  const auto reconciliation_ack = factory.normalize_reconciliation_acknowledgement(
      fixture.create_reconciliation_private_event_origin_or_throw(1U), fixture.account_id(),
      fixture.venue_id(), exchange_id, fixture.record().order_id(), fixture.instrument_id());
  const auto venue_reject =
      factory.normalize_venue_rejection(fixture.create_venue_private_event_origin_or_throw(2U),
                                        create_both_identity_locator_or_throw(fixture),
                                        oms::ExchangeRejectionCategory::InvalidOrder, detail);
  const auto reconciliation_reject = factory.normalize_reconciliation_rejection(
      fixture.create_reconciliation_private_event_origin_or_throw(2U), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture),
      oms::ExchangeRejectionCategory::InvalidOrder, detail);
  const auto venue_fill = factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(3U),
      create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(), revision,
      quantity_one, quantity_two, price, std::nullopt);
  const auto reconciliation_fill = factory.normalize_reconciliation_execution(
      fixture.create_reconciliation_private_event_origin_or_throw(3U), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture), trade_id,
      fixture.instrument_id(), revision, quantity_one, quantity_two, price,
      execution::OrderSide::Buy);
  const auto venue_cancel = factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(4U),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::Cancelled,
      quantity_two);
  const auto reconciliation_cancel = factory.normalize_reconciliation_cancellation_result(
      fixture.create_reconciliation_private_event_origin_or_throw(4U), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture),
      oms::CancellationResult::CancelRejected, std::nullopt);
  const auto causal_cancel_attempt_id = fixture.create_cancel_attempt_id_or_throw();
  const auto correlated_venue_cancel_reject =
      factory.normalize_venue_cancel_rejection_with_causal_id(
          fixture.create_venue_private_event_origin_or_throw(5U),
          create_both_identity_locator_or_throw(fixture), causal_cancel_attempt_id);

  REQUIRE(venue_ack);
  REQUIRE(reconciliation_ack);
  REQUIRE(venue_reject);
  REQUIRE(reconciliation_reject);
  REQUIRE(venue_fill);
  REQUIRE(reconciliation_fill);
  REQUIRE(venue_cancel);
  REQUIRE(reconciliation_cancel);
  REQUIRE(correlated_venue_cancel_reject);
  REQUIRE(venue_ack.value().kind() == oms::PrivateOrderEventKind::ExchangeAcknowledged);
  REQUIRE(reconciliation_ack.value().origin() == oms::PrivateEventOrigin::Reconciliation);
  REQUIRE(reconciliation_ack.value().provenance().subject()->instrument().has_value());
  REQUIRE(venue_reject.value().kind() == oms::PrivateOrderEventKind::ExchangeRejected);
  REQUIRE(reconciliation_reject.value().kind() == oms::PrivateOrderEventKind::ExchangeRejected);
  REQUIRE(venue_fill.value().kind() == oms::PrivateOrderEventKind::Execution);
  REQUIRE(reconciliation_fill.value().kind() == oms::PrivateOrderEventKind::Execution);
  REQUIRE(venue_cancel.value().kind() == oms::PrivateOrderEventKind::CancellationResult);
  REQUIRE(reconciliation_cancel.value().kind() == oms::PrivateOrderEventKind::CancellationResult);
  const auto& correlated_cancel_payload =
      std::get<oms::CancellationResultPayload>(correlated_venue_cancel_reject.value().payload());
  REQUIRE(correlated_cancel_payload.causal_cancel_attempt_id == causal_cancel_attempt_id);
  const auto& reconciliation_cancel_payload =
      std::get<oms::CancellationResultPayload>(reconciliation_cancel.value().payload());
  REQUIRE_FALSE(reconciliation_cancel_payload.causal_cancel_attempt_id);

  // ++++++++++++++++++++++++++++++++++++++++
  // Configured account and source facts cover the two non-order-scoped local rows.
  const auto account_timeout =
      factory.normalize_account_timeout(fixture.create_local_private_event_origin_or_throw(14U),
                                        fixture.account_id(), fixture.venue_id());
  const auto disconnect = factory.normalize_disconnect(
      fixture.create_local_private_event_origin_or_throw(15U), fixture.account_id(),
      fixture.venue_id(),
      test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U));
  REQUIRE(account_timeout);
  REQUIRE(disconnect);
  REQUIRE(account_timeout.value().kind() == oms::PrivateOrderEventKind::TimeoutObserved);
  REQUIRE(disconnect.value().kind() == oms::PrivateOrderEventKind::DisconnectObserved);
  REQUIRE(account_timeout.value().subject_scope() == oms::PrivateEventSubjectScope::Account);
  REQUIRE(disconnect.value().subject_scope() == oms::PrivateEventSubjectScope::PrivateSource);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Each ordinary producer publishes a complete receive-time-free attempt, while compatibility
// normalization attaches only its supplied receive observation and preserves all other fields.
TEST_CASE("ordinary private ingress attempts preserve compatibility semantics without receive time",
          "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto exchange_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto trade_id = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);
  const auto source_epoch =
      test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U);
  const std::array detail{std::byte{0x01U}, std::byte{0x02U}};
  const auto one = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto two = test_support::create_m4_decimal_or_throw<model::Quantity>(2);
  const auto price = test_support::create_m4_decimal_or_throw<model::Price>(10);
  const auto revision = fixture.record().provenance().metadata_revision;
  const auto acknowledgement_origin =
      fixture.create_venue_private_event_origin_or_throw(21U, 101U, 201U);
  const auto rejection_origin = fixture.create_venue_private_event_origin_or_throw(22U, 102U, 202U);
  const auto execution_origin = fixture.create_venue_private_event_origin_or_throw(23U, 103U, 203U);
  const auto cancellation_origin =
      fixture.create_venue_private_event_origin_or_throw(24U, 104U, 204U);
  const auto correlated_cancellation_origin =
      fixture.create_venue_private_event_origin_or_throw(27U, 107U, 207U);
  const auto timeout_origin = fixture.create_local_private_event_origin_or_throw(25U, 105U, 205U);
  const auto disconnect_origin =
      fixture.create_local_private_event_origin_or_throw(26U, 106U, 206U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Each ordinary source builds an attempt from the same facts as its normalized wrapper.
  const auto acknowledgement_attempt = factory.create_venue_acknowledgement_attempt(
      oms::VenuePrivateIngressOrigin{acknowledgement_origin.event_key,
                                     acknowledgement_origin.source_time},
      exchange_id, fixture.record().order_id());
  const auto acknowledgement = factory.normalize_venue_acknowledgement(
      acknowledgement_origin, exchange_id, fixture.record().order_id());
  const auto rejection_attempt = factory.create_venue_rejection_attempt(
      oms::VenuePrivateIngressOrigin{rejection_origin.event_key, rejection_origin.source_time},
      create_both_identity_locator_or_throw(fixture), oms::ExchangeRejectionCategory::InvalidOrder,
      detail);
  const auto rejection = factory.normalize_venue_rejection(
      rejection_origin, create_both_identity_locator_or_throw(fixture),
      oms::ExchangeRejectionCategory::InvalidOrder, detail);
  const auto execution_attempt = factory.create_venue_execution_attempt(
      oms::VenuePrivateIngressOrigin{execution_origin.event_key, execution_origin.source_time},
      create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(), revision,
      one, two, price, execution::OrderSide::Buy);
  const auto execution = factory.normalize_venue_execution(
      execution_origin, create_both_identity_locator_or_throw(fixture), trade_id,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  const auto cancellation_attempt = factory.create_venue_cancellation_result_attempt(
      oms::VenuePrivateIngressOrigin{cancellation_origin.event_key,
                                     cancellation_origin.source_time},
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::Cancelled, two);
  const auto cancellation = factory.normalize_venue_cancellation_result(
      cancellation_origin, create_both_identity_locator_or_throw(fixture),
      oms::CancellationResult::Cancelled, two);
  const auto causal_cancel_attempt_id = fixture.create_cancel_attempt_id_or_throw();
  const auto correlated_cancellation_attempt =
      factory.create_venue_cancel_rejection_attempt_with_causal_id(
          oms::VenuePrivateIngressOrigin{correlated_cancellation_origin.event_key,
                                         correlated_cancellation_origin.source_time},
          create_both_identity_locator_or_throw(fixture), causal_cancel_attempt_id);
  const auto correlated_cancellation = factory.normalize_venue_cancel_rejection_with_causal_id(
      correlated_cancellation_origin, create_both_identity_locator_or_throw(fixture),
      causal_cancel_attempt_id);
  const auto timeout_attempt = factory.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{timeout_origin.event_id, timeout_origin.source_time},
      fixture.account_id(), fixture.venue_id());
  const auto timeout =
      factory.normalize_account_timeout(timeout_origin, fixture.account_id(), fixture.venue_id());
  const auto disconnect_attempt = factory.create_disconnect_attempt(
      oms::LocalPrivateIngressOrigin{disconnect_origin.event_id, disconnect_origin.source_time},
      fixture.account_id(), fixture.venue_id(), source_epoch);
  const auto disconnect = factory.normalize_disconnect(disconnect_origin, fixture.account_id(),
                                                       fixture.venue_id(), source_epoch);

  REQUIRE(acknowledgement_attempt);
  REQUIRE(acknowledgement);
  REQUIRE(rejection_attempt);
  REQUIRE(rejection);
  REQUIRE(execution_attempt);
  REQUIRE(execution);
  REQUIRE(cancellation_attempt);
  REQUIRE(cancellation);
  REQUIRE(correlated_cancellation_attempt);
  REQUIRE(correlated_cancellation);
  REQUIRE(timeout_attempt);
  REQUIRE(timeout);
  REQUIRE(disconnect_attempt);
  REQUIRE(disconnect);

  // ++++++++++++++++++++++++++++++++++++++++
  // Removing receive time from each wrapper reproduces its source attempt without another decision.
  CHECK(acknowledgement_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(acknowledgement.value()));
  CHECK(rejection_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(rejection.value()));
  CHECK(execution_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(execution.value()));
  CHECK(cancellation_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(cancellation.value()));
  CHECK(correlated_cancellation_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(
            correlated_cancellation.value()));
  CHECK(timeout_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(timeout.value()));
  CHECK(disconnect_attempt.value().semantic_value() ==
        oms::PrivateEventIngressSemanticValue::from_normalized_input(disconnect.value()));
  CHECK(acknowledgement.value().receive_time() == acknowledgement_origin.receive_time);
  CHECK(rejection.value().receive_time() == rejection_origin.receive_time);
  CHECK(execution.value().receive_time() == execution_origin.receive_time);
  CHECK(cancellation.value().receive_time() == cancellation_origin.receive_time);
  CHECK(timeout.value().receive_time() == timeout_origin.receive_time);
  CHECK(disconnect.value().receive_time() == disconnect_origin.receive_time);
  CHECK_FALSE(execution_attempt.value().semantic_value() ==
              acknowledgement_attempt.value().semantic_value());
  const auto execution_without_side = factory.create_venue_execution_attempt(
      oms::VenuePrivateIngressOrigin{execution_origin.event_key, execution_origin.source_time},
      create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(), revision,
      one, two, price, std::nullopt);
  REQUIRE(execution_without_side);
  CHECK_FALSE(execution_attempt.value().semantic_value() ==
              execution_without_side.value().semantic_value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A later observation changes full normalized equality but not the receive-time-free attempt.
  auto later_acknowledgement_origin = acknowledgement_origin;
  later_acknowledgement_origin.receive_time = model::ReceiveTimestamp{999U};
  const auto later_acknowledgement = factory.normalize_venue_acknowledgement(
      later_acknowledgement_origin, exchange_id, fixture.record().order_id());
  REQUIRE(later_acknowledgement);
  CHECK_FALSE(acknowledgement.value() == later_acknowledgement.value());
  CHECK(acknowledgement.value().is_ingress_semantically_equal_to(later_acknowledgement.value()));
  CHECK(
      acknowledgement_attempt.value().semantic_value() ==
      oms::PrivateEventIngressSemanticValue::from_normalized_input(later_acknowledgement.value()));

  // ++++++++++++++++++++++++++++++++++++++++
  // Every ordinary validation family has exact attempt/normalization failure parity.
  const auto zero = test_support::create_m4_decimal_or_throw<model::Quantity>(0);
  require_matching_failure(
      factory.create_venue_execution_attempt(
          oms::VenuePrivateIngressOrigin{execution_origin.event_key, execution_origin.source_time},
          create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(),
          revision, zero, two, price, execution::OrderSide::Buy),
      factory.normalize_venue_execution(
          execution_origin, create_both_identity_locator_or_throw(fixture), trade_id,
          fixture.instrument_id(), revision, zero, two, price, execution::OrderSide::Buy));

  const auto invalid_category = static_cast<oms::ExchangeRejectionCategory>(0U);
  require_matching_failure(
      factory.create_venue_rejection_attempt(
          oms::VenuePrivateIngressOrigin{rejection_origin.event_key, rejection_origin.source_time},
          create_both_identity_locator_or_throw(fixture), invalid_category, detail),
      factory.normalize_venue_rejection(rejection_origin,
                                        create_both_identity_locator_or_throw(fixture),
                                        invalid_category, detail));
  const std::array<std::byte, 257U> oversized_detail{};
  require_matching_failure(
      factory.create_venue_rejection_attempt(
          oms::VenuePrivateIngressOrigin{rejection_origin.event_key, rejection_origin.source_time},
          create_both_identity_locator_or_throw(fixture),
          oms::ExchangeRejectionCategory::InvalidOrder, oversized_detail),
      factory.normalize_venue_rejection(
          rejection_origin, create_both_identity_locator_or_throw(fixture),
          oms::ExchangeRejectionCategory::InvalidOrder, oversized_detail));

  const auto invalid_side = static_cast<execution::OrderSide>(0U);
  require_matching_failure(
      factory.create_venue_execution_attempt(
          oms::VenuePrivateIngressOrigin{execution_origin.event_key, execution_origin.source_time},
          create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(),
          revision, one, two, price, invalid_side),
      factory.normalize_venue_execution(
          execution_origin, create_both_identity_locator_or_throw(fixture), trade_id,
          fixture.instrument_id(), revision, one, two, price, invalid_side));
  require_matching_failure(factory.create_venue_cancellation_result_attempt(
                               oms::VenuePrivateIngressOrigin{cancellation_origin.event_key,
                                                              cancellation_origin.source_time},
                               create_both_identity_locator_or_throw(fixture),
                               oms::CancellationResult::Cancelled, std::nullopt),
                           factory.normalize_venue_cancellation_result(
                               cancellation_origin, create_both_identity_locator_or_throw(fixture),
                               oms::CancellationResult::Cancelled, std::nullopt));

  const auto other_venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("other");
  require_matching_failure(
      factory.create_account_timeout_attempt(
          oms::LocalPrivateIngressOrigin{timeout_origin.event_id, timeout_origin.source_time},
          fixture.account_id(), other_venue),
      factory.normalize_account_timeout(timeout_origin, fixture.account_id(), other_venue));
  require_matching_failure(
      factory.create_disconnect_attempt(
          oms::LocalPrivateIngressOrigin{disconnect_origin.event_id, disconnect_origin.source_time},
          fixture.account_id(), other_venue, source_epoch),
      factory.normalize_disconnect(disconnect_origin, fixture.account_id(), other_venue,
                                   source_epoch));

  // ++++++++++++++++++++++++++++++++++++++++
  // Validation precedence is identical when two independent input defects are present together.
  const auto category_precedence = factory.create_venue_rejection_attempt(
      oms::VenuePrivateIngressOrigin{rejection_origin.event_key, rejection_origin.source_time},
      create_both_identity_locator_or_throw(fixture), invalid_category, oversized_detail);
  REQUIRE_FALSE(category_precedence);
  CHECK(category_precedence.error().context.field == "private_event.rejection_category");
  const auto execution_precedence = factory.create_venue_execution_attempt(
      oms::VenuePrivateIngressOrigin{execution_origin.event_key, execution_origin.source_time},
      create_both_identity_locator_or_throw(fixture), trade_id, fixture.instrument_id(), revision,
      zero, two, price, invalid_side);
  REQUIRE_FALSE(execution_precedence);
  CHECK(execution_precedence.error().context.field == "private_event.execution");
  const auto unknown_account =
      test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>("account.unknown");
  const auto account_precedence = factory.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{timeout_origin.event_id, timeout_origin.source_time},
      unknown_account, other_venue);
  REQUIRE_FALSE(account_precedence);
  CHECK(account_precedence.error().context.field == "m4_provenance.logical_account_id");

  // ++++++++++++++++++++++++++++++++++++++++
  // The projection also removes receive time from a reconciliation row without altering its key.
  const auto reconciliation = factory.normalize_reconciliation_acknowledgement(
      fixture.create_reconciliation_private_event_origin_or_throw(30U, 130U, 230U),
      fixture.account_id(), fixture.venue_id(), exchange_id, fixture.record().order_id(),
      fixture.instrument_id());
  REQUIRE(reconciliation);
  const auto reconciliation_semantic =
      oms::PrivateEventIngressSemanticValue::from_normalized_input(reconciliation.value());
  const auto expected_reconciliation_origin = oms::ReconciliationPrivateIngressOrigin{
      test_support::create_m4_reconciliation_epoch_or_throw(),
      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x51U),
      test_support::create_m4_ordinal_or_throw<recovery::ReconciliationRowOrdinal>(30U),
      model::SourceTimestamp{130U}};
  REQUIRE(std::holds_alternative<oms::ReconciliationPrivateIngressOrigin>(
      reconciliation_semantic.origin()));
  CHECK(std::get<oms::ReconciliationPrivateIngressOrigin>(reconciliation_semantic.origin()) ==
        expected_reconciliation_origin);
  CHECK(reconciliation_semantic.provenance() == reconciliation.value().provenance());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Resolver, factory, and attempt ownership keep source authority valid after every caller-owned
// construction input and intermediate factory has left scope.
TEST_CASE("private ingress attempts own their complete bounded source value", "[m4][private]") {
  const auto account = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.deribit-testnet-aegis");
  const auto venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("deribit");

  // ++++++++++++++++++++++++++++++++++++++++
  // Return a self-owned factory only after its configuration and policy-bearing fixture disappear.
  auto surviving_factory = [] {
    auto authority = test_support::create_m4_test_authority_or_throw();
    auto resolver =
        runtime::M4ProvenanceResolver::create(authority.configuration, authority.m4_policy);
    if (!resolver) {
      throw std::logic_error{"invalid owning private-event factory fixture"};
    }
    return runtime::PrivateOrderEventFactory{std::move(resolver).value()};
  }();
  const auto surviving_factory_attempt = surviving_factory.create_account_timeout_attempt(
      oms::LocalPrivateIngressOrigin{test_support::create_m4_local_event_id_or_throw(70U),
                                     model::SourceTimestamp{170U}},
      account, venue);
  REQUIRE(surviving_factory_attempt);
  CHECK(surviving_factory_attempt.value().semantic_value().logical_account_id() == account);

  // ++++++++++++++++++++++++++++++++++++++++
  // Return the attempt itself only after the complete resolver/factory construction stack vanishes.
  auto retained_attempt = [] {
    auto authority = test_support::create_m4_test_authority_or_throw();
    auto resolver =
        runtime::M4ProvenanceResolver::create(authority.configuration, authority.m4_policy);
    if (!resolver) {
      throw std::logic_error{"invalid retained private-event attempt resolver"};
    }
    runtime::PrivateOrderEventFactory factory{std::move(resolver).value()};
    const auto account_id = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
        "account.deribit-testnet-aegis");
    const auto venue_id = test_support::parse_m4_identifier_or_throw<model::VenueId>("deribit");
    auto attempt = factory.create_disconnect_attempt(
        oms::LocalPrivateIngressOrigin{test_support::create_m4_local_event_id_or_throw(71U),
                                       model::SourceTimestamp{171U}},
        account_id, venue_id,
        test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U));
    if (!attempt) {
      throw std::logic_error{"invalid retained private-event attempt"};
    }
    return std::move(attempt).value();
  }();
  CHECK(retained_attempt.semantic_value().logical_account_id() == account);
  CHECK(retained_attempt.semantic_value().venue_id() == venue);
  CHECK(retained_attempt.semantic_value().subject_scope() ==
        oms::PrivateEventSubjectScope::PrivateSource);
  CHECK(std::holds_alternative<oms::LocalPrivateIngressOrigin>(
      retained_attempt.semantic_value().origin()));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Venue and reconciliation input provenance remains correlation-independent even when a raw local
// locator exactly matches an existing test order.
TEST_CASE("normalized private events never infer local ownership from source locators",
          "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto fill = factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture),
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U),
      fixture.instrument_id(), fixture.record().provenance().metadata_revision,
      test_support::create_m4_decimal_or_throw<model::Quantity>(1),
      test_support::create_m4_decimal_or_throw<model::Quantity>(2),
      test_support::create_m4_decimal_or_throw<model::Price>(10), execution::OrderSide::Buy);
  REQUIRE(fill);
  REQUIRE(fill.value().provenance().subject().has_value());
  const auto& source = *fill.value().provenance().subject();
  REQUIRE(source.firm_id().has_value());
  REQUIRE(source.instrument().has_value());
  REQUIRE_FALSE(source.desk_id().has_value());
  REQUIRE_FALSE(source.bot_id().has_value());
  REQUIRE_FALSE(source.strategy_id().has_value());
  REQUIRE_FALSE(source.route().has_value());
}

// --------------------------------------------------------
// Bounded details, assigned enums, execution intervals, cancellation shape, and configured account
// authority all fail before a normalized value is published.
TEST_CASE("normalized private event factory rejects every malformed dependent boundary",
          "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto revision = fixture.record().provenance().metadata_revision;
  const auto one = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto two = test_support::create_m4_decimal_or_throw<model::Quantity>(2);
  const auto zero_quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(0);
  const auto negative_quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(-1);
  const auto price = test_support::create_m4_decimal_or_throw<model::Price>(10);
  const auto zero_price = test_support::create_m4_decimal_or_throw<model::Price>(0);
  const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A locator must contain at least one identity and rejection detail must fit exactly 256 bytes.
  REQUIRE_FALSE(oms::PrivateOrderLocator::create(std::nullopt, std::nullopt));
  const std::array<std::byte, 256U> maximum_detail{};
  const std::array<std::byte, 257U> over_detail{};
  REQUIRE(factory.normalize_venue_rejection(fixture.create_venue_private_event_origin_or_throw(),
                                            create_both_identity_locator_or_throw(fixture),
                                            oms::ExchangeRejectionCategory::Unspecified,
                                            maximum_detail));
  REQUIRE_FALSE(
      factory.normalize_venue_rejection(fixture.create_venue_private_event_origin_or_throw(),
                                        create_both_identity_locator_or_throw(fixture),
                                        oms::ExchangeRejectionCategory::Unspecified, over_detail));
  REQUIRE_FALSE(factory.normalize_venue_rejection(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture),
      static_cast<oms::ExchangeRejectionCategory>(0U), std::span<const std::byte>{}));
  REQUIRE_FALSE(factory.normalize_venue_rejection(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture),
      static_cast<oms::ExchangeRejectionCategory>(7U), std::span<const std::byte>{}));
  constexpr std::array categories{
      oms::ExchangeRejectionCategory::Unspecified,
      oms::ExchangeRejectionCategory::InvalidOrder,
      oms::ExchangeRejectionCategory::InsufficientAuthority,
      oms::ExchangeRejectionCategory::InsufficientFunds,
      oms::ExchangeRejectionCategory::PostOnlyWouldCross,
      oms::ExchangeRejectionCategory::VenueRiskRejected,
  };
  for (const auto category : categories) {
    REQUIRE(factory.normalize_venue_rejection(fixture.create_venue_private_event_origin_or_throw(),
                                              create_both_identity_locator_or_throw(fixture),
                                              category, std::span<const std::byte>{}));
  }
  REQUIRE_FALSE(factory.normalize_reconciliation_rejection(
      fixture.create_reconciliation_private_event_origin_or_throw(), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture),
      static_cast<oms::ExchangeRejectionCategory>(7U), std::span<const std::byte>{}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Executions require positive economics, cumulative at least incremental, and assigned side.
  REQUIRE_FALSE(factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), trade, fixture.instrument_id(), revision,
      zero_quantity, two, price, std::nullopt));
  REQUIRE_FALSE(factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), trade, fixture.instrument_id(), revision, two,
      one, price, std::nullopt));
  REQUIRE_FALSE(factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), trade, fixture.instrument_id(), revision, one,
      two, zero_price, std::nullopt));
  REQUIRE_FALSE(factory.normalize_venue_execution(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), trade, fixture.instrument_id(), revision, one,
      two, price, static_cast<execution::OrderSide>(0U)));
  REQUIRE_FALSE(factory.normalize_reconciliation_execution(
      fixture.create_reconciliation_private_event_origin_or_throw(), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture), trade,
      fixture.instrument_id(), revision, one, two, price, static_cast<execution::OrderSide>(3U)));
  REQUIRE_FALSE(factory.normalize_reconciliation_execution(
      fixture.create_reconciliation_private_event_origin_or_throw(), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture), trade,
      fixture.instrument_id(), revision, zero_quantity, two, price, execution::OrderSide::Buy));
  REQUIRE(factory.normalize_venue_execution(fixture.create_venue_private_event_origin_or_throw(),
                                            create_both_identity_locator_or_throw(fixture), trade,
                                            fixture.instrument_id(), revision, one, two, price,
                                            std::nullopt));
  REQUIRE(factory.normalize_venue_execution(fixture.create_venue_private_event_origin_or_throw(),
                                            create_both_identity_locator_or_throw(fixture), trade,
                                            fixture.instrument_id(), revision, one, two, price,
                                            execution::OrderSide::Buy));
  REQUIRE(factory.normalize_venue_execution(fixture.create_venue_private_event_origin_or_throw(),
                                            create_both_identity_locator_or_throw(fixture), trade,
                                            fixture.instrument_id(), revision, one, two, price,
                                            execution::OrderSide::Sell));

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation terminal cumulative is present iff Cancelled; only a venue rejection may carry
  // exact causal cancel-attempt evidence.
  REQUIRE_FALSE(factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::Cancelled,
      std::nullopt));
  REQUIRE_FALSE(factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::CancelRejected,
      one));
  REQUIRE_FALSE(factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::Cancelled,
      negative_quantity));
  REQUIRE_FALSE(factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), static_cast<oms::CancellationResult>(0U),
      one));
  REQUIRE(factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::Cancelled,
      zero_quantity));
  const auto uncorrelated_venue_rejection = factory.normalize_venue_cancellation_result(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), oms::CancellationResult::CancelRejected,
      std::nullopt);
  REQUIRE(uncorrelated_venue_rejection);
  const auto& uncorrelated_venue_rejection_payload =
      std::get<oms::CancellationResultPayload>(uncorrelated_venue_rejection.value().payload());
  CHECK_FALSE(uncorrelated_venue_rejection_payload.causal_cancel_attempt_id);
  CHECK_FALSE(uncorrelated_venue_rejection_payload.terminal_cumulative_quantity);
  const auto causal_cancel_attempt_id = fixture.create_cancel_attempt_id_or_throw();
  const auto correlated_rejection = factory.normalize_venue_cancel_rejection_with_causal_id(
      fixture.create_venue_private_event_origin_or_throw(),
      create_both_identity_locator_or_throw(fixture), causal_cancel_attempt_id);
  REQUIRE(correlated_rejection);
  const auto& correlated_rejection_payload =
      std::get<oms::CancellationResultPayload>(correlated_rejection.value().payload());
  CHECK(correlated_rejection_payload.result == oms::CancellationResult::CancelRejected);
  CHECK(correlated_rejection_payload.causal_cancel_attempt_id == causal_cancel_attempt_id);
  CHECK_FALSE(correlated_rejection_payload.terminal_cumulative_quantity);
  REQUIRE_FALSE(factory.normalize_reconciliation_cancellation_result(
      fixture.create_reconciliation_private_event_origin_or_throw(), fixture.account_id(),
      fixture.venue_id(), create_both_identity_locator_or_throw(fixture),
      oms::CancellationResult::Cancelled, std::nullopt));
  const auto uncorrelated_reconciliation_rejection =
      factory.normalize_reconciliation_cancellation_result(
          fixture.create_reconciliation_private_event_origin_or_throw(), fixture.account_id(),
          fixture.venue_id(), create_both_identity_locator_or_throw(fixture),
          oms::CancellationResult::CancelRejected, std::nullopt);
  REQUIRE(uncorrelated_reconciliation_rejection);
  CHECK_FALSE(std::get<oms::CancellationResultPayload>(
                  uncorrelated_reconciliation_rejection.value().payload())
                  .causal_cancel_attempt_id);

  // ++++++++++++++++++++++++++++++++++++++++
  // Local account/source facts require an exact configured venue binding.
  const auto other_venue = test_support::parse_m4_identifier_or_throw<model::VenueId>("other");
  REQUIRE_FALSE(factory.normalize_account_timeout(
      fixture.create_local_private_event_origin_or_throw(), fixture.account_id(), other_venue));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Full structural equality retains receive time, while ingress duplicate equality excludes only
// receive time and reacts to each major semantic group independently.
TEST_CASE("normalized private event ingress equality excludes receive time only", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto locator_value = create_both_identity_locator_or_throw(fixture);
  const auto trade = test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x71U);
  const auto revision = fixture.record().provenance().metadata_revision;
  const auto one = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  const auto two = test_support::create_m4_decimal_or_throw<model::Quantity>(2);
  const auto price = test_support::create_m4_decimal_or_throw<model::Price>(10);
  // Create a normalized execution input from authored fields, or throw when the fixture is invalid.
  const auto create_execution_input_or_throw =
      [&](oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator event_locator,
          oms::TradeId event_trade, model::InstrumentId instrument,
          model::InstrumentMetadataRevision metadata_revision, model::Quantity incremental,
          model::Quantity cumulative, model::Price execution_price,
          std::optional<execution::OrderSide> side) {
        return take_normalized_private_order_input_or_throw(factory.normalize_venue_execution(
            std::move(origin), std::move(event_locator), std::move(event_trade),
            std::move(instrument), metadata_revision, incremental, cumulative, execution_price,
            side));
      };

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue origin and every execution payload field participate independently in ingress equality.
  const auto baseline = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  const auto receive_only = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 999U), locator_value, trade,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline == receive_only);
  REQUIRE(baseline.is_ingress_semantically_equal_to(receive_only));
  REQUIRE(receive_only.is_ingress_semantically_equal_to(baseline));
  REQUIRE(baseline.is_ingress_semantically_equal_to(baseline));
  require_ingress_semantic_oracles_agree(baseline, receive_only);
  require_ingress_semantic_oracles_agree(receive_only, baseline);
  require_ingress_semantic_oracles_agree(baseline, baseline);

  const auto changed_event = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(2U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_time = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 101U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  auto changed_source_origin = fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U);
  changed_source_origin.event_key.source_epoch_id =
      test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x42U);
  const auto changed_source = create_execution_input_or_throw(
      std::move(changed_source_origin), locator_value, trade, fixture.instrument_id(), revision,
      one, two, price, execution::OrderSide::Buy);
  const auto changed_locator = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U),
      create_both_identity_locator_or_throw(fixture, 0x62U), trade, fixture.instrument_id(),
      revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_trade = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value,
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(0x72U),
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_instrument = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      test_support::parse_m4_identifier_or_throw<model::InstrumentId>("ETH-USD"), revision, one,
      two, price, execution::OrderSide::Buy);
  const auto changed_revision = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(),
      test_support::create_m4_ordinal_or_throw<model::InstrumentMetadataRevision>(2U), one, two,
      price, execution::OrderSide::Buy);
  const auto changed_increment = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, two, two, price, execution::OrderSide::Buy);
  const auto changed_cumulative = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one,
      test_support::create_m4_decimal_or_throw<model::Quantity>(3), price,
      execution::OrderSide::Buy);
  const auto changed_price = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one, two,
      test_support::create_m4_decimal_or_throw<model::Price>(11), execution::OrderSide::Buy);
  const auto changed_side = create_execution_input_or_throw(
      fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
      fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Sell);
  const std::array semantic_changes{&changed_event,    &changed_time,      &changed_source,
                                    &changed_locator,  &changed_trade,     &changed_instrument,
                                    &changed_revision, &changed_increment, &changed_cumulative,
                                    &changed_price,    &changed_side};
  for (const auto* const changed : semantic_changes) {
    REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(*changed));
    require_ingress_semantic_oracles_agree(baseline, *changed);
  }

  // A changed source account also changes independently proved subject provenance and event key.
  const auto subsidiary = test_support::parse_m4_identifier_or_throw<model::LogicalAccountId>(
      "account.deribit-testnet-subsidiary");
  auto other_origin = fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U);
  other_origin.event_key.logical_account_id = subsidiary;
  const auto changed_account = create_execution_input_or_throw(
      std::move(other_origin), locator_value, trade, fixture.instrument_id(), revision, one, two,
      price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(changed_account));
  require_ingress_semantic_oracles_agree(baseline, changed_account);

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue and sealed M4-policy root are independent semantic fields, not derived aliases.
  auto changed_venue_origin = fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U);
  changed_venue_origin.event_key.venue_id =
      test_support::parse_m4_identifier_or_throw<model::VenueId>("other");
  const auto changed_venue = create_execution_input_or_throw(
      std::move(changed_venue_origin), locator_value, trade, fixture.instrument_id(), revision, one,
      two, price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(changed_venue));
  require_ingress_semantic_oracles_agree(baseline, changed_venue);

  auto changed_capacities = test_support::create_ordinary_m4_policy_capacities();
  ++changed_capacities.max_event_identity_records;
  auto changed_authority = test_support::create_m4_test_authority_or_throw(changed_capacities);
  auto changed_resolver = runtime::M4ProvenanceResolver::create(changed_authority.configuration,
                                                                changed_authority.m4_policy);
  REQUIRE(changed_resolver);
  runtime::PrivateOrderEventFactory changed_factory{std::move(changed_resolver).value()};
  const auto changed_root =
      take_normalized_private_order_input_or_throw(changed_factory.normalize_venue_execution(
          fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), locator_value, trade,
          fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy));
  REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(changed_root));
  require_ingress_semantic_oracles_agree(baseline, changed_root);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Local and reconciliation origin equality excludes only receive time and retains every identity,
// source/cut time, and non-order-scoped payload field.
TEST_CASE("normalized private event equality covers local and reconciliation origins",
          "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();

  // ++++++++++++++++++++++++++++++++++++++++
  // Local account facts ignore receive time but retain local identity and source time.
  const auto local = take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
      fixture.create_local_private_event_origin_or_throw(1U, 100U, 200U), fixture.account_id(),
      fixture.venue_id()));
  const auto local_receive =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(1U, 100U, 999U), fixture.account_id(),
          fixture.venue_id()));
  const auto local_event =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(2U, 100U, 200U), fixture.account_id(),
          fixture.venue_id()));
  const auto local_time =
      take_normalized_private_order_input_or_throw(factory.normalize_account_timeout(
          fixture.create_local_private_event_origin_or_throw(1U, 101U, 200U), fixture.account_id(),
          fixture.venue_id()));
  REQUIRE(local.is_ingress_semantically_equal_to(local_receive));
  REQUIRE_FALSE(local == local_receive);
  REQUIRE_FALSE(local.is_ingress_semantically_equal_to(local_event));
  REQUIRE_FALSE(local.is_ingress_semantically_equal_to(local_time));
  require_ingress_semantic_oracles_agree(local, local_receive);
  require_ingress_semantic_oracles_agree(local, local_event);
  require_ingress_semantic_oracles_agree(local, local_time);

  const auto disconnected =
      take_normalized_private_order_input_or_throw(factory.normalize_disconnect(
          fixture.create_local_private_event_origin_or_throw(3U), fixture.account_id(),
          fixture.venue_id(),
          test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x41U)));
  const auto changed_source =
      take_normalized_private_order_input_or_throw(factory.normalize_disconnect(
          fixture.create_local_private_event_origin_or_throw(3U), fixture.account_id(),
          fixture.venue_id(),
          test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x42U)));
  REQUIRE_FALSE(disconnected.is_ingress_semantically_equal_to(changed_source));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reconciliation facts retain epoch, cut, row, and cut time while excluding receive time only.
  const auto exchange_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  // Create a normalized reconciliation acknowledgement, or throw when the fixture is invalid.
  const auto create_reconciliation_acknowledgement_input_or_throw =
      [&](oms::ReconciliationPrivateEventOrigin origin) {
        return take_normalized_private_order_input_or_throw(
            factory.normalize_reconciliation_acknowledgement(
                std::move(origin), fixture.account_id(), fixture.venue_id(), exchange_id,
                fixture.record().order_id(), fixture.instrument_id()));
      };
  const auto reconciliation = create_reconciliation_acknowledgement_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U));
  const auto reconciliation_receive = create_reconciliation_acknowledgement_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 999U));
  const auto reconciliation_no_instrument =
      take_normalized_private_order_input_or_throw(factory.normalize_reconciliation_acknowledgement(
          fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U),
          fixture.account_id(), fixture.venue_id(), exchange_id, fixture.record().order_id(),
          std::nullopt));
  auto changed_epoch_origin =
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U);
  changed_epoch_origin.reconciliation_epoch_id =
      test_support::create_m4_reconciliation_epoch_or_throw(2U);
  auto changed_cut_origin =
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 100U, 200U);
  changed_cut_origin.authoritative_cut_id =
      test_support::create_m4_opaque_identity_or_throw<oms::AuthoritativeCutId>(0x52U);
  const auto reconciliation_epoch =
      create_reconciliation_acknowledgement_input_or_throw(std::move(changed_epoch_origin));
  const auto reconciliation_cut =
      create_reconciliation_acknowledgement_input_or_throw(std::move(changed_cut_origin));
  const auto reconciliation_row = create_reconciliation_acknowledgement_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(2U, 100U, 200U));
  const auto reconciliation_time = create_reconciliation_acknowledgement_input_or_throw(
      fixture.create_reconciliation_private_event_origin_or_throw(1U, 101U, 200U));
  REQUIRE(reconciliation.is_ingress_semantically_equal_to(reconciliation_receive));
  REQUIRE_FALSE(reconciliation == reconciliation_receive);
  REQUIRE_FALSE(reconciliation.is_ingress_semantically_equal_to(reconciliation_no_instrument));
  require_ingress_semantic_oracles_agree(reconciliation, reconciliation_receive);
  require_ingress_semantic_oracles_agree(reconciliation, reconciliation_no_instrument);
  const std::array reconciliation_changes{&reconciliation_epoch, &reconciliation_cut,
                                          &reconciliation_row, &reconciliation_time};
  for (const auto* const changed : reconciliation_changes) {
    REQUIRE_FALSE(reconciliation.is_ingress_semantically_equal_to(*changed));
    require_ingress_semantic_oracles_agree(reconciliation, *changed);
  }

  const auto venue_acknowledgement =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(1U, 100U, 200U), exchange_id,
          fixture.record().order_id()));
  REQUIRE_FALSE(reconciliation.is_ingress_semantically_equal_to(venue_acknowledgement));
  require_ingress_semantic_oracles_agree(reconciliation, venue_acknowledgement);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Locator presence, acknowledgement fields, rejection category, cancellation result, and event
// kind each participate independently in ingress equality.
TEST_CASE("normalized private event equality covers every source payload family", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto local_order_id = fixture.record().order_id();
  const auto exchange_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x61U);
  const auto other_exchange_id =
      test_support::create_m4_opaque_identity_or_throw<oms::ExchangeOrderId>(0x62U);
  const auto local_only = create_private_order_locator_or_throw(local_order_id, std::nullopt);
  const auto exchange_only = create_private_order_locator_or_throw(std::nullopt, exchange_id);
  const auto both = create_private_order_locator_or_throw(local_order_id, exchange_id);

  // ++++++++++++++++++++++++++++++++++++++++
  // All three nonempty raw locator shapes are valid and remain semantically distinct.
  const auto local_reject =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(), local_only,
          oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  const auto exchange_reject =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(), exchange_only,
          oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  const auto both_reject =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(), both,
          oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  const auto category_reject =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(), both,
          oms::ExchangeRejectionCategory::InsufficientFunds, std::span<const std::byte>{}));
  REQUIRE_FALSE(local_reject.is_ingress_semantically_equal_to(exchange_reject));
  REQUIRE_FALSE(local_reject.is_ingress_semantically_equal_to(both_reject));
  REQUIRE_FALSE(both_reject.is_ingress_semantically_equal_to(category_reject));

  // ++++++++++++++++++++++++++++++++++++++++
  // Acknowledgement exchange identity and optional raw client locator both remain source facts.
  const auto acknowledgement = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_acknowledgement(fixture.create_venue_private_event_origin_or_throw(),
                                              exchange_id, fixture.record().order_id()));
  const auto changed_exchange = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_acknowledgement(fixture.create_venue_private_event_origin_or_throw(),
                                              other_exchange_id, fixture.record().order_id()));
  const auto absent_local =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_acknowledgement(
          fixture.create_venue_private_event_origin_or_throw(), exchange_id, std::nullopt));
  REQUIRE_FALSE(acknowledgement.is_ingress_semantically_equal_to(changed_exchange));
  REQUIRE_FALSE(acknowledgement.is_ingress_semantically_equal_to(absent_local));
  REQUIRE_FALSE(acknowledgement.is_ingress_semantically_equal_to(both_reject));

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation result, terminal cumulative, and causal-attempt presence/value remain exact
  // semantic fields; receive time remains excluded.
  const auto cancelled =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_cancellation_result(
          fixture.create_venue_private_event_origin_or_throw(), both,
          oms::CancellationResult::Cancelled,
          test_support::create_m4_decimal_or_throw<model::Quantity>(1)));
  const auto changed_cumulative =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_cancellation_result(
          fixture.create_venue_private_event_origin_or_throw(), both,
          oms::CancellationResult::Cancelled,
          test_support::create_m4_decimal_or_throw<model::Quantity>(2)));
  const auto cancellation_rejection_origin =
      fixture.create_venue_private_event_origin_or_throw(61U, 161U, 261U);
  const auto rejected =
      take_normalized_private_order_input_or_throw(factory.normalize_venue_cancellation_result(
          cancellation_rejection_origin, both, oms::CancellationResult::CancelRejected,
          std::nullopt));
  const auto first_causal_id = fixture.create_cancel_attempt_id_or_throw(1U);
  const auto second_causal_id = fixture.create_cancel_attempt_id_or_throw(2U);
  const auto correlated_rejected = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_cancel_rejection_with_causal_id(cancellation_rejection_origin, both,
                                                              first_causal_id));
  const auto changed_causal_id = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_cancel_rejection_with_causal_id(cancellation_rejection_origin, both,
                                                              second_causal_id));
  const auto changed_receive_time = take_normalized_private_order_input_or_throw(
      factory.normalize_venue_cancel_rejection_with_causal_id(
          fixture.create_venue_private_event_origin_or_throw(61U, 161U, 262U), both,
          first_causal_id));
  REQUIRE_FALSE(cancelled.is_ingress_semantically_equal_to(changed_cumulative));
  REQUIRE_FALSE(cancelled.is_ingress_semantically_equal_to(rejected));
  REQUIRE_FALSE(rejected.is_ingress_semantically_equal_to(correlated_rejected));
  REQUIRE_FALSE(correlated_rejected.is_ingress_semantically_equal_to(changed_causal_id));
  REQUIRE(correlated_rejected.is_ingress_semantically_equal_to(changed_receive_time));
  REQUIRE_FALSE(correlated_rejected == changed_receive_time);
  require_ingress_semantic_oracles_agree(rejected, correlated_rejected);
  require_ingress_semantic_oracles_agree(correlated_rejected, changed_causal_id);
  require_ingress_semantic_oracles_agree(correlated_rejected, changed_receive_time);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Rejection detail participates byte-for-byte in ingress semantics, including exact empty detail.
TEST_CASE("normalized private rejection detail is bounded and semantic", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const std::array first{std::byte{0x01U}};
  const std::array second{std::byte{0x02U}};
  const auto baseline =
      take_normalized_private_order_input_or_throw(fixture.factory().normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(),
          create_both_identity_locator_or_throw(fixture),
          oms::ExchangeRejectionCategory::InvalidOrder, first));
  const auto changed =
      take_normalized_private_order_input_or_throw(fixture.factory().normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(),
          create_both_identity_locator_or_throw(fixture),
          oms::ExchangeRejectionCategory::InvalidOrder, second));
  const auto empty =
      take_normalized_private_order_input_or_throw(fixture.factory().normalize_venue_rejection(
          fixture.create_venue_private_event_origin_or_throw(),
          create_both_identity_locator_or_throw(fixture),
          oms::ExchangeRejectionCategory::InvalidOrder, std::span<const std::byte>{}));
  REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(changed));
  REQUIRE_FALSE(baseline.is_ingress_semantically_equal_to(empty));
  REQUIRE(std::get<oms::ExchangeRejectedPayload>(empty.payload()).detail.size() == 0U);
}

// --------------------------------------------------------
