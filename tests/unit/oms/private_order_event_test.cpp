// Purpose: prove the assigned M4 private-event vocabulary, closed origin/payload shapes, bounded
// validation, source-limited provenance, and receive-time-independent ingress equality.

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

// --------------------------------------------------------
// Construct one exact raw local/exchange locator and fail fast on a fixture defect.
[[nodiscard]] oms::PrivateOrderLocator
locator(std::optional<model::OrderId> local_order_id,
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
both_locator(const test_support::M4PrivateEventFixture& fixture,
             std::uint8_t exchange_byte = 0x61U) {
  return locator(fixture.record().order_id(),
                 test_support::m4_opaque<oms::ExchangeOrderId>(exchange_byte));
}

// --------------------------------------------------------
// Move one successful result into its immutable input value or fail on a test fixture defect.
[[nodiscard]] oms::NormalizedPrivateOrderInput
input(model::Result<oms::NormalizedPrivateOrderInput> result) {
  if (!result) {
    throw std::logic_error{"invalid normalized private-event fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// All ten source-normalization rows available before owner-bound OMS reconciliation produce their
// exact assigned normalized vocabulary; four local-order rows remain owner-only C4 construction.
TEST_CASE("normalized private event factory covers every accepted source shape", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto exchange_id = test_support::m4_opaque<oms::ExchangeOrderId>(0x61U);
  const auto trade_id = test_support::m4_opaque<oms::TradeId>(0x71U);
  const std::array detail{std::byte{0x01U}, std::byte{0x02U}};
  const auto quantity_one = test_support::m4_decimal<model::Quantity>(1);
  const auto quantity_two = test_support::m4_decimal<model::Quantity>(2);
  const auto price = test_support::m4_decimal<model::Price>(10);
  const auto revision = fixture.record().provenance().metadata_revision;

  // ++++++++++++++++++++++++++++++++++++++++
  // The two authoritative origins accept acknowledgement, rejection, execution, and cancellation.
  const auto venue_ack = factory.venue_acknowledgement(fixture.venue_origin(1U), exchange_id,
                                                       fixture.record().order_id());
  const auto reconciliation_ack = factory.reconciliation_acknowledgement(
      fixture.reconciliation_origin(1U), fixture.account_id(), fixture.venue_id(), exchange_id,
      fixture.record().order_id(), fixture.instrument_id());
  const auto venue_reject =
      factory.venue_rejection(fixture.venue_origin(2U), both_locator(fixture),
                              oms::ExchangeRejectionCategory::InvalidOrder, detail);
  const auto reconciliation_reject = factory.reconciliation_rejection(
      fixture.reconciliation_origin(2U), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), oms::ExchangeRejectionCategory::InvalidOrder, detail);
  const auto venue_fill = factory.venue_execution(fixture.venue_origin(3U), both_locator(fixture),
                                                  trade_id, fixture.instrument_id(), revision,
                                                  quantity_one, quantity_two, price, std::nullopt);
  const auto reconciliation_fill = factory.reconciliation_execution(
      fixture.reconciliation_origin(3U), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), trade_id, fixture.instrument_id(), revision, quantity_one,
      quantity_two, price, execution::OrderSide::Buy);
  const auto venue_cancel =
      factory.venue_cancellation_result(fixture.venue_origin(4U), both_locator(fixture),
                                        oms::CancellationResult::Cancelled, quantity_two);
  const auto reconciliation_cancel = factory.reconciliation_cancellation_result(
      fixture.reconciliation_origin(4U), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), oms::CancellationResult::CancelRejected, std::nullopt);

  REQUIRE(venue_ack);
  REQUIRE(reconciliation_ack);
  REQUIRE(venue_reject);
  REQUIRE(reconciliation_reject);
  REQUIRE(venue_fill);
  REQUIRE(reconciliation_fill);
  REQUIRE(venue_cancel);
  REQUIRE(reconciliation_cancel);
  REQUIRE(venue_ack.value().kind() == oms::PrivateOrderEventKind::ExchangeAcknowledged);
  REQUIRE(reconciliation_ack.value().origin() == oms::PrivateEventOrigin::Reconciliation);
  REQUIRE(reconciliation_ack.value().provenance().subject()->instrument().has_value());
  REQUIRE(venue_reject.value().kind() == oms::PrivateOrderEventKind::ExchangeRejected);
  REQUIRE(reconciliation_reject.value().kind() == oms::PrivateOrderEventKind::ExchangeRejected);
  REQUIRE(venue_fill.value().kind() == oms::PrivateOrderEventKind::Execution);
  REQUIRE(reconciliation_fill.value().kind() == oms::PrivateOrderEventKind::Execution);
  REQUIRE(venue_cancel.value().kind() == oms::PrivateOrderEventKind::CancellationResult);
  REQUIRE(reconciliation_cancel.value().kind() == oms::PrivateOrderEventKind::CancellationResult);

  // ++++++++++++++++++++++++++++++++++++++++
  // Configured account and source facts cover the two non-order-scoped local rows.
  const auto account_timeout =
      factory.account_timeout(fixture.local_origin(14U), fixture.account_id(), fixture.venue_id());
  const auto disconnect =
      factory.disconnect(fixture.local_origin(15U), fixture.account_id(), fixture.venue_id(),
                         test_support::m4_opaque<oms::PrivateSourceEpochId>(0x41U));
  REQUIRE(account_timeout);
  REQUIRE(disconnect);
  REQUIRE(account_timeout.value().kind() == oms::PrivateOrderEventKind::TimeoutObserved);
  REQUIRE(disconnect.value().kind() == oms::PrivateOrderEventKind::DisconnectObserved);
  REQUIRE(account_timeout.value().subject_scope() == oms::PrivateEventSubjectScope::Account);
  REQUIRE(disconnect.value().subject_scope() == oms::PrivateEventSubjectScope::PrivateSource);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Venue and reconciliation input provenance remains correlation-independent even when a raw local
// locator exactly matches an existing test order.
TEST_CASE("normalized private events never infer local ownership from source locators",
          "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto fill = factory.venue_execution(
      fixture.venue_origin(), both_locator(fixture), test_support::m4_opaque<oms::TradeId>(0x71U),
      fixture.instrument_id(), fixture.record().provenance().metadata_revision,
      test_support::m4_decimal<model::Quantity>(1), test_support::m4_decimal<model::Quantity>(2),
      test_support::m4_decimal<model::Price>(10), execution::OrderSide::Buy);
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
  const auto one = test_support::m4_decimal<model::Quantity>(1);
  const auto two = test_support::m4_decimal<model::Quantity>(2);
  const auto zero_quantity = test_support::m4_decimal<model::Quantity>(0);
  const auto negative_quantity = test_support::m4_decimal<model::Quantity>(-1);
  const auto price = test_support::m4_decimal<model::Price>(10);
  const auto zero_price = test_support::m4_decimal<model::Price>(0);
  const auto trade = test_support::m4_opaque<oms::TradeId>(0x71U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A locator must contain at least one identity and rejection detail must fit exactly 256 bytes.
  REQUIRE_FALSE(oms::PrivateOrderLocator::create(std::nullopt, std::nullopt));
  const std::array<std::byte, 256U> maximum_detail{};
  const std::array<std::byte, 257U> over_detail{};
  REQUIRE(factory.venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                  oms::ExchangeRejectionCategory::Unspecified, maximum_detail));
  REQUIRE_FALSE(factory.venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                        oms::ExchangeRejectionCategory::Unspecified, over_detail));
  REQUIRE_FALSE(factory.venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                        static_cast<oms::ExchangeRejectionCategory>(0U),
                                        std::span<const std::byte>{}));
  REQUIRE_FALSE(factory.venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                        static_cast<oms::ExchangeRejectionCategory>(7U),
                                        std::span<const std::byte>{}));
  constexpr std::array categories{
      oms::ExchangeRejectionCategory::Unspecified,
      oms::ExchangeRejectionCategory::InvalidOrder,
      oms::ExchangeRejectionCategory::InsufficientAuthority,
      oms::ExchangeRejectionCategory::InsufficientFunds,
      oms::ExchangeRejectionCategory::PostOnlyWouldCross,
      oms::ExchangeRejectionCategory::VenueRiskRejected,
  };
  for (const auto category : categories) {
    REQUIRE(factory.venue_rejection(fixture.venue_origin(), both_locator(fixture), category,
                                    std::span<const std::byte>{}));
  }
  REQUIRE_FALSE(factory.reconciliation_rejection(
      fixture.reconciliation_origin(), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), static_cast<oms::ExchangeRejectionCategory>(7U),
      std::span<const std::byte>{}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Executions require positive economics, cumulative at least incremental, and assigned side.
  REQUIRE_FALSE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                        fixture.instrument_id(), revision, zero_quantity, two,
                                        price, std::nullopt));
  REQUIRE_FALSE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                        fixture.instrument_id(), revision, two, one, price,
                                        std::nullopt));
  REQUIRE_FALSE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                        fixture.instrument_id(), revision, one, two, zero_price,
                                        std::nullopt));
  REQUIRE_FALSE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                        fixture.instrument_id(), revision, one, two, price,
                                        static_cast<execution::OrderSide>(0U)));
  REQUIRE_FALSE(factory.reconciliation_execution(
      fixture.reconciliation_origin(), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), trade, fixture.instrument_id(), revision, one, two, price,
      static_cast<execution::OrderSide>(3U)));
  REQUIRE_FALSE(factory.reconciliation_execution(
      fixture.reconciliation_origin(), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), trade, fixture.instrument_id(), revision, zero_quantity, two, price,
      execution::OrderSide::Buy));
  REQUIRE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                  fixture.instrument_id(), revision, one, two, price,
                                  std::nullopt));
  REQUIRE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                  fixture.instrument_id(), revision, one, two, price,
                                  execution::OrderSide::Buy));
  REQUIRE(factory.venue_execution(fixture.venue_origin(), both_locator(fixture), trade,
                                  fixture.instrument_id(), revision, one, two, price,
                                  execution::OrderSide::Sell));

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation terminal cumulative is present iff Cancelled and cannot be negative.
  REQUIRE_FALSE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                                  oms::CancellationResult::Cancelled,
                                                  std::nullopt));
  REQUIRE_FALSE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                                  oms::CancellationResult::CancelRejected, one));
  REQUIRE_FALSE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                                  oms::CancellationResult::Cancelled,
                                                  negative_quantity));
  REQUIRE_FALSE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                                  static_cast<oms::CancellationResult>(0U), one));
  REQUIRE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                            oms::CancellationResult::Cancelled, zero_quantity));
  REQUIRE(factory.venue_cancellation_result(fixture.venue_origin(), both_locator(fixture),
                                            oms::CancellationResult::CancelRejected, std::nullopt));
  REQUIRE_FALSE(factory.reconciliation_cancellation_result(
      fixture.reconciliation_origin(), fixture.account_id(), fixture.venue_id(),
      both_locator(fixture), oms::CancellationResult::Cancelled, std::nullopt));

  // ++++++++++++++++++++++++++++++++++++++++
  // Local account/source facts require an exact configured venue binding.
  const auto other_venue = test_support::m4_id<model::VenueId>("other");
  REQUIRE_FALSE(factory.account_timeout(fixture.local_origin(), fixture.account_id(), other_venue));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Full structural equality retains receive time, while ingress duplicate equality excludes only
// receive time and reacts to each major semantic group independently.
TEST_CASE("normalized private event ingress equality excludes receive time only", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const auto& factory = fixture.factory();
  const auto locator_value = both_locator(fixture);
  const auto trade = test_support::m4_opaque<oms::TradeId>(0x71U);
  const auto revision = fixture.record().provenance().metadata_revision;
  const auto one = test_support::m4_decimal<model::Quantity>(1);
  const auto two = test_support::m4_decimal<model::Quantity>(2);
  const auto price = test_support::m4_decimal<model::Price>(10);
  const auto make = [&](oms::VenuePrivateEventOrigin origin, oms::PrivateOrderLocator event_locator,
                        oms::TradeId event_trade, model::InstrumentId instrument,
                        model::InstrumentMetadataRevision metadata_revision,
                        model::Quantity incremental, model::Quantity cumulative,
                        model::Price execution_price, std::optional<execution::OrderSide> side) {
    return input(factory.venue_execution(
        std::move(origin), std::move(event_locator), std::move(event_trade), std::move(instrument),
        metadata_revision, incremental, cumulative, execution_price, side));
  };

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue origin and every execution payload field participate independently in ingress equality.
  const auto baseline =
      make(fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Buy);
  const auto receive_only =
      make(fixture.venue_origin(1U, 100U, 999U), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline == receive_only);
  REQUIRE(baseline.ingress_semantically_equal(receive_only));
  REQUIRE(receive_only.ingress_semantically_equal(baseline));
  REQUIRE(baseline.ingress_semantically_equal(baseline));

  const auto changed_event =
      make(fixture.venue_origin(2U, 100U, 200U), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_time =
      make(fixture.venue_origin(1U, 101U, 200U), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Buy);
  auto changed_source_origin = fixture.venue_origin(1U, 100U, 200U);
  changed_source_origin.event_key.source_epoch_id =
      test_support::m4_opaque<oms::PrivateSourceEpochId>(0x42U);
  const auto changed_source =
      make(std::move(changed_source_origin), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_locator =
      make(fixture.venue_origin(1U, 100U, 200U), both_locator(fixture, 0x62U), trade,
           fixture.instrument_id(), revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_trade =
      make(fixture.venue_origin(1U, 100U, 200U), locator_value,
           test_support::m4_opaque<oms::TradeId>(0x72U), fixture.instrument_id(), revision, one,
           two, price, execution::OrderSide::Buy);
  const auto changed_instrument = make(fixture.venue_origin(1U, 100U, 200U), locator_value, trade,
                                       test_support::m4_id<model::InstrumentId>("ETH-USD"),
                                       revision, one, two, price, execution::OrderSide::Buy);
  const auto changed_revision =
      make(fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(),
           test_support::m4_ordinal<model::InstrumentMetadataRevision>(2U), one, two, price,
           execution::OrderSide::Buy);
  const auto changed_increment =
      make(fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(),
           revision, two, two, price, execution::OrderSide::Buy);
  const auto changed_cumulative = make(
      fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(), revision,
      one, test_support::m4_decimal<model::Quantity>(3), price, execution::OrderSide::Buy);
  const auto changed_price = make(
      fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(), revision,
      one, two, test_support::m4_decimal<model::Price>(11), execution::OrderSide::Buy);
  const auto changed_side =
      make(fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(),
           revision, one, two, price, execution::OrderSide::Sell);
  const std::array semantic_changes{&changed_event,    &changed_time,      &changed_source,
                                    &changed_locator,  &changed_trade,     &changed_instrument,
                                    &changed_revision, &changed_increment, &changed_cumulative,
                                    &changed_price,    &changed_side};
  for (const auto* const changed : semantic_changes) {
    REQUIRE_FALSE(baseline.ingress_semantically_equal(*changed));
  }

  // A changed source account also changes independently proved subject provenance and event key.
  const auto subsidiary =
      test_support::m4_id<model::LogicalAccountId>("account.deribit-testnet-subsidiary");
  auto other_origin = fixture.venue_origin(1U, 100U, 200U);
  other_origin.event_key.logical_account_id = subsidiary;
  const auto changed_account =
      make(std::move(other_origin), locator_value, trade, fixture.instrument_id(), revision, one,
           two, price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline.ingress_semantically_equal(changed_account));

  // ++++++++++++++++++++++++++++++++++++++++
  // Venue and sealed M4-policy root are independent semantic fields, not derived aliases.
  auto changed_venue_origin = fixture.venue_origin(1U, 100U, 200U);
  changed_venue_origin.event_key.venue_id = test_support::m4_id<model::VenueId>("other");
  const auto changed_venue =
      make(std::move(changed_venue_origin), locator_value, trade, fixture.instrument_id(), revision,
           one, two, price, execution::OrderSide::Buy);
  REQUIRE_FALSE(baseline.ingress_semantically_equal(changed_venue));

  auto changed_capacities = test_support::ordinary_m4_capacities();
  ++changed_capacities.max_event_identity_records;
  auto changed_authority = test_support::m4_test_authority(changed_capacities);
  auto changed_resolver = runtime::M4ProvenanceResolver::create(changed_authority.configuration,
                                                                changed_authority.m4_policy);
  REQUIRE(changed_resolver);
  runtime::PrivateOrderEventFactory changed_factory{std::move(changed_resolver).value()};
  const auto changed_root = input(changed_factory.venue_execution(
      fixture.venue_origin(1U, 100U, 200U), locator_value, trade, fixture.instrument_id(), revision,
      one, two, price, execution::OrderSide::Buy));
  REQUIRE_FALSE(baseline.ingress_semantically_equal(changed_root));

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
  const auto local = input(factory.account_timeout(fixture.local_origin(1U, 100U, 200U),
                                                   fixture.account_id(), fixture.venue_id()));
  const auto local_receive = input(factory.account_timeout(
      fixture.local_origin(1U, 100U, 999U), fixture.account_id(), fixture.venue_id()));
  const auto local_event = input(factory.account_timeout(fixture.local_origin(2U, 100U, 200U),
                                                         fixture.account_id(), fixture.venue_id()));
  const auto local_time = input(factory.account_timeout(fixture.local_origin(1U, 101U, 200U),
                                                        fixture.account_id(), fixture.venue_id()));
  REQUIRE(local.ingress_semantically_equal(local_receive));
  REQUIRE_FALSE(local == local_receive);
  REQUIRE_FALSE(local.ingress_semantically_equal(local_event));
  REQUIRE_FALSE(local.ingress_semantically_equal(local_time));

  const auto disconnected =
      input(factory.disconnect(fixture.local_origin(3U), fixture.account_id(), fixture.venue_id(),
                               test_support::m4_opaque<oms::PrivateSourceEpochId>(0x41U)));
  const auto changed_source =
      input(factory.disconnect(fixture.local_origin(3U), fixture.account_id(), fixture.venue_id(),
                               test_support::m4_opaque<oms::PrivateSourceEpochId>(0x42U)));
  REQUIRE_FALSE(disconnected.ingress_semantically_equal(changed_source));

  // ++++++++++++++++++++++++++++++++++++++++
  // Reconciliation facts retain epoch, cut, row, and cut time while excluding receive time only.
  const auto exchange_id = test_support::m4_opaque<oms::ExchangeOrderId>(0x61U);
  const auto make_ack = [&](oms::ReconciliationPrivateEventOrigin origin) {
    return input(factory.reconciliation_acknowledgement(
        std::move(origin), fixture.account_id(), fixture.venue_id(), exchange_id,
        fixture.record().order_id(), fixture.instrument_id()));
  };
  const auto reconciliation = make_ack(fixture.reconciliation_origin(1U, 100U, 200U));
  const auto reconciliation_receive = make_ack(fixture.reconciliation_origin(1U, 100U, 999U));
  const auto reconciliation_no_instrument = input(factory.reconciliation_acknowledgement(
      fixture.reconciliation_origin(1U, 100U, 200U), fixture.account_id(), fixture.venue_id(),
      exchange_id, fixture.record().order_id(), std::nullopt));
  auto changed_epoch_origin = fixture.reconciliation_origin(1U, 100U, 200U);
  changed_epoch_origin.reconciliation_epoch_id = test_support::m4_reconciliation_epoch(2U);
  auto changed_cut_origin = fixture.reconciliation_origin(1U, 100U, 200U);
  changed_cut_origin.authoritative_cut_id = test_support::m4_opaque<oms::AuthoritativeCutId>(0x52U);
  const auto reconciliation_epoch = make_ack(std::move(changed_epoch_origin));
  const auto reconciliation_cut = make_ack(std::move(changed_cut_origin));
  const auto reconciliation_row = make_ack(fixture.reconciliation_origin(2U, 100U, 200U));
  const auto reconciliation_time = make_ack(fixture.reconciliation_origin(1U, 101U, 200U));
  REQUIRE(reconciliation.ingress_semantically_equal(reconciliation_receive));
  REQUIRE_FALSE(reconciliation == reconciliation_receive);
  REQUIRE_FALSE(reconciliation.ingress_semantically_equal(reconciliation_no_instrument));
  const std::array reconciliation_changes{&reconciliation_epoch, &reconciliation_cut,
                                          &reconciliation_row, &reconciliation_time};
  for (const auto* const changed : reconciliation_changes) {
    REQUIRE_FALSE(reconciliation.ingress_semantically_equal(*changed));
  }

  const auto venue_acknowledgement = input(factory.venue_acknowledgement(
      fixture.venue_origin(1U, 100U, 200U), exchange_id, fixture.record().order_id()));
  REQUIRE_FALSE(reconciliation.ingress_semantically_equal(venue_acknowledgement));

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
  const auto exchange_id = test_support::m4_opaque<oms::ExchangeOrderId>(0x61U);
  const auto other_exchange_id = test_support::m4_opaque<oms::ExchangeOrderId>(0x62U);
  const auto local_only = locator(local_order_id, std::nullopt);
  const auto exchange_only = locator(std::nullopt, exchange_id);
  const auto both = locator(local_order_id, exchange_id);

  // ++++++++++++++++++++++++++++++++++++++++
  // All three nonempty raw locator shapes are valid and remain semantically distinct.
  const auto local_reject = input(factory.venue_rejection(
      fixture.venue_origin(), local_only, oms::ExchangeRejectionCategory::InvalidOrder,
      std::span<const std::byte>{}));
  const auto exchange_reject = input(factory.venue_rejection(
      fixture.venue_origin(), exchange_only, oms::ExchangeRejectionCategory::InvalidOrder,
      std::span<const std::byte>{}));
  const auto both_reject = input(factory.venue_rejection(
      fixture.venue_origin(), both, oms::ExchangeRejectionCategory::InvalidOrder,
      std::span<const std::byte>{}));
  const auto category_reject = input(factory.venue_rejection(
      fixture.venue_origin(), both, oms::ExchangeRejectionCategory::InsufficientFunds,
      std::span<const std::byte>{}));
  REQUIRE_FALSE(local_reject.ingress_semantically_equal(exchange_reject));
  REQUIRE_FALSE(local_reject.ingress_semantically_equal(both_reject));
  REQUIRE_FALSE(both_reject.ingress_semantically_equal(category_reject));

  // ++++++++++++++++++++++++++++++++++++++++
  // Acknowledgement exchange identity and optional raw client locator both remain source facts.
  const auto acknowledgement = input(factory.venue_acknowledgement(
      fixture.venue_origin(), exchange_id, fixture.record().order_id()));
  const auto changed_exchange = input(factory.venue_acknowledgement(
      fixture.venue_origin(), other_exchange_id, fixture.record().order_id()));
  const auto absent_local =
      input(factory.venue_acknowledgement(fixture.venue_origin(), exchange_id, std::nullopt));
  REQUIRE_FALSE(acknowledgement.ingress_semantically_equal(changed_exchange));
  REQUIRE_FALSE(acknowledgement.ingress_semantically_equal(absent_local));
  REQUIRE_FALSE(acknowledgement.ingress_semantically_equal(both_reject));

  // ++++++++++++++++++++++++++++++++++++++++
  // Cancellation result and terminal cumulative presence/value remain exact semantic fields.
  const auto cancelled = input(factory.venue_cancellation_result(
      fixture.venue_origin(), both, oms::CancellationResult::Cancelled,
      test_support::m4_decimal<model::Quantity>(1)));
  const auto changed_cumulative = input(factory.venue_cancellation_result(
      fixture.venue_origin(), both, oms::CancellationResult::Cancelled,
      test_support::m4_decimal<model::Quantity>(2)));
  const auto rejected = input(factory.venue_cancellation_result(
      fixture.venue_origin(), both, oms::CancellationResult::CancelRejected, std::nullopt));
  REQUIRE_FALSE(cancelled.ingress_semantically_equal(changed_cumulative));
  REQUIRE_FALSE(cancelled.ingress_semantically_equal(rejected));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Rejection detail participates byte-for-byte in ingress semantics, including exact empty detail.
TEST_CASE("normalized private rejection detail is bounded and semantic", "[m4][private]") {
  test_support::M4PrivateEventFixture fixture;
  const std::array first{std::byte{0x01U}};
  const std::array second{std::byte{0x02U}};
  const auto baseline =
      input(fixture.factory().venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                              oms::ExchangeRejectionCategory::InvalidOrder, first));
  const auto changed = input(
      fixture.factory().venue_rejection(fixture.venue_origin(), both_locator(fixture),
                                        oms::ExchangeRejectionCategory::InvalidOrder, second));
  const auto empty = input(fixture.factory().venue_rejection(
      fixture.venue_origin(), both_locator(fixture), oms::ExchangeRejectionCategory::InvalidOrder,
      std::span<const std::byte>{}));
  REQUIRE_FALSE(baseline.ingress_semantically_equal(changed));
  REQUIRE_FALSE(baseline.ingress_semantically_equal(empty));
  REQUIRE(std::get<oms::ExchangeRejectedPayload>(empty.payload()).detail.size() == 0U);
}

// --------------------------------------------------------
