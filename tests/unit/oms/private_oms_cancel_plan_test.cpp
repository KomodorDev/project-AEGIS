// Purpose: qualify detached cancellation proposals against ADR-0010's explicit attempt-history
// partition, including late outcomes and causal evidence, without publishing any live transition.

#include "private_oms_transition_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace {

using namespace aegis;
using test_support::PrivateOmsTransitionFixture;

// Literal contract sets keep cancellation eligibility independent of production predicates.
constexpr std::array open_venue_states{
    oms::OutboundOrderState::WriteInitiated, oms::OutboundOrderState::SubmissionUnknown,
    oms::OutboundOrderState::Working, oms::OutboundOrderState::PartiallyFilled};
constexpr std::array terminal_venue_states{
    oms::OutboundOrderState::Filled, oms::OutboundOrderState::ExchangeRejected,
    oms::OutboundOrderState::Cancelled, oms::OutboundOrderState::ReconciledAbsent};
constexpr std::array ineligible_local_states{oms::OutboundOrderState::PendingEncoding,
                                             oms::OutboundOrderState::PendingInitiation,
                                             oms::OutboundOrderState::LocallyFailed};
constexpr std::array write_outcomes{oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance,
                                    oms::CancelWriteOutcome::AcceptedAndInitiated,
                                    oms::CancelWriteOutcome::AcceptedThenOutcomeLost};
constexpr std::array write_result_states{oms::CancellationState::DefinitelyFailed,
                                         oms::CancellationState::WriteInitiated,
                                         oms::CancellationState::OutcomeUnknown};
constexpr std::array unresolved_states{oms::CancellationState::Requested,
                                       oms::CancellationState::WriteInitiated,
                                       oms::CancellationState::OutcomeUnknown};

// ########################################################################
// These independent corruptions cover complete history provenance, first-observation consistency,
// and the single unresolved latest-attempt invariant before an incoming fact can be proposed.
enum class MalformedCancelHistory : std::uint8_t {
  WrongRequestIdentity,
  WrongRequestPayload,
  MissingWriteOutcome,
  WrongWriteIdentity,
  WrongWritePayload,
  ContradictoryWriteOutcome,
  MissingCausalRejection,
  WrongCausalIdentity,
  UncorrelatedCausalEvidence,
  WrongCausalPayload,
  UnresolvedEarlierAttempt,
  DuplicateAttemptIdentity,
  ReversedOrdinals,
};

// ########################################################################

// --------------------------------------------------------
// Use only the local order locator so tests isolate cancellation authority from exchange mapping.
[[nodiscard]] oms::NormalizedPrivateOrderInput
normalize_cancel_rejection_or_throw(const PrivateOmsTransitionFixture& fixture,
                                    std::optional<oms::CancelAttemptId> causal = {},
                                    std::uint8_t event = 200U) {
  const auto locator = test_support::extract_private_oms_value_or_throw(
      oms::PrivateOrderLocator::create_private_order_locator(fixture.order().order_id(), {}));
  const auto origin = fixture.source.create_venue_private_event_origin_or_throw(event);
  const auto& factory = fixture.source.private_event_factory();
  return test_support::extract_private_oms_value_or_throw(
      causal ? factory.normalize_venue_cancel_rejection_with_causal_id(origin, locator, *causal)
             : factory.normalize_venue_cancellation_result(
                   origin, locator, oms::CancellationResult::CancelRejected, {}));
}

// --------------------------------------------------------
// Author unsolicited authoritative cancellation without inventing an exact causal attempt.
[[nodiscard]] oms::NormalizedPrivateOrderInput
normalize_cancelled_or_throw(const PrivateOmsTransitionFixture& fixture, std::int64_t target,
                             bool reconciliation = false) {
  const auto locator = test_support::extract_private_oms_value_or_throw(
      oms::PrivateOrderLocator::create_private_order_locator(fixture.order().order_id(), {}));
  const auto quantity = test_support::create_m4_decimal_or_throw<model::Quantity>(target);
  const auto& factory = fixture.source.private_event_factory();
  return test_support::extract_private_oms_value_or_throw(
      reconciliation ? factory.normalize_reconciliation_cancellation_result(
                           fixture.source.create_reconciliation_private_event_origin_or_throw(),
                           fixture.source.account_id(), fixture.source.venue_id(), locator,
                           oms::CancellationResult::Cancelled, quantity)
                     : factory.normalize_venue_cancellation_result(
                           fixture.source.create_venue_private_event_origin_or_throw(201U), locator,
                           oms::CancellationResult::Cancelled, quantity));
}

// --------------------------------------------------------
// Build a coherent literal history row; rejected rows retain causal proof and unresolved write
// states retain the corresponding first outcome. No hypothetical row changes the genuine OMS.
[[nodiscard]] oms::RetainedCancelAttempt
create_cancel_history_or_throw(const PrivateOmsTransitionFixture& fixture,
                               oms::CancellationState state, std::uint64_t ordinal = 1U,
                               std::optional<oms::CancelWriteOutcome> retained_outcome = {}) {
  const auto attempt = fixture.create_cancel_attempt_id_or_throw(ordinal);
  if (!retained_outcome) {
    if (state == oms::CancellationState::DefinitelyFailed) {
      retained_outcome = oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance;
    } else if (state == oms::CancellationState::WriteInitiated) {
      retained_outcome = oms::CancelWriteOutcome::AcceptedAndInitiated;
    } else if (state == oms::CancellationState::OutcomeUnknown) {
      retained_outcome = oms::CancelWriteOutcome::AcceptedThenOutcomeLost;
    }
  }
  return oms::RetainedCancelAttempt{
      attempt, fixture.normalize_cancel_request_or_throw(attempt, ordinal), state,
      retained_outcome ? std::optional{fixture.normalize_cancel_write_outcome_or_throw(
                             attempt, *retained_outcome, 100U + ordinal)}
                       : std::nullopt,
      state == oms::CancellationState::Rejected
          ? std::optional{normalize_cancel_rejection_or_throw(fixture, attempt, 100U)}
          : std::nullopt};
}

// --------------------------------------------------------
// Compare all immutable attempt facts because a history-only update must preserve its request and
// any earlier outcome while modifying only the exact named retained row.
void check_cancel_history(const oms::RetainedCancelAttempt& actual,
                          const oms::RetainedCancelAttempt& expected) {
  CHECK(actual.id == expected.id);
  CHECK(actual.request == expected.request);
  CHECK(actual.state == expected.state);
  CHECK(actual.write_outcome == expected.write_outcome);
  CHECK(actual.causal_rejection == expected.causal_rejection);
}

// --------------------------------------------------------
// Check the complete detached projection plus shared no-fill accounting for one cancel input.
void check_cancel_plan(
    const oms::PrivateOmsTransitionPlan& plan, const oms::PrivateOrderProjection& before,
    const oms::PrivateOrderProjection& after, oms::ProposedPrivateOmsClassification classification,
    oms::ProposedPrivateEconomicsAction economics = oms::ProposedPrivateEconomicsAction::None) {
  CHECK(plan.before == before);
  CHECK(plan.after == after);
  CHECK(plan.classification == classification);
  CHECK(plan.economics_action == economics);
  CHECK_FALSE(plan.gap_insertion.has_value());
  CHECK(plan.drained_pending_prefix_count == 0U);
  CHECK(plan.transition_effect_count == 1U);
  CHECK(plan.order_callback_count ==
        (classification == oms::ProposedPrivateOmsClassification::ForbiddenRejected ? 0U : 1U));
  if (classification == oms::ProposedPrivateOmsClassification::ForbiddenRejected) {
    CHECK(plan.rejection_code == model::DomainErrorCode::InvalidPrivateOmsState);
  } else {
    CHECK_FALSE(plan.rejection_code.has_value());
  }
  if (classification == oms::ProposedPrivateOmsClassification::SafetyContained) {
    CHECK(plan.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  } else {
    CHECK_FALSE(plan.safety_reason.has_value());
  }
  if (economics == oms::ProposedPrivateEconomicsAction::ReleaseResidual) {
    CHECK(plan.reservation_closure_cause == risk::ReservationClosureCause::DefinitiveCancellation);
  } else {
    CHECK_FALSE(plan.reservation_closure_cause.has_value());
  }
}

// --------------------------------------------------------
// A fresh explicit cancel can append only in open venue states after no attempt or a proven close.
TEST_CASE("Private OMS cancel request eligibility uses primary and retained attempt state",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array retry_states{oms::CancellationState::None,
                                    oms::CancellationState::DefinitelyFailed,
                                    oms::CancellationState::Rejected};
  for (const auto primary : open_venue_states) {
    for (const auto cancellation : retry_states) {
      CAPTURE(primary, cancellation);
      std::vector<oms::RetainedCancelAttempt> history;
      if (cancellation != oms::CancellationState::None) {
        history.push_back(create_cancel_history_or_throw(fixture, cancellation));
      }
      auto before = fixture.create_detached_projection_or_throw(primary);
      before.cancellation_state = cancellation;
      before.cancel_attempt_count = static_cast<std::uint32_t>(history.size());
      const auto ordinal = static_cast<std::uint64_t>(history.size()) + 1U;
      const auto attempt = fixture.create_cancel_attempt_id_or_throw(ordinal);
      const auto input = fixture.normalize_cancel_request_or_throw(attempt, 20U);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      auto after = before;
      after.cancellation_state = oms::CancellationState::Requested;
      ++after.cancel_attempt_count;
      check_cancel_plan(result.value(), before, after,
                        oms::ProposedPrivateOmsClassification::Applied);
      REQUIRE(result.value().cancel_history_change);
      CHECK(result.value().cancel_history_change->index == history.size());
      check_cancel_history(
          result.value().cancel_history_change->record,
          oms::RetainedCancelAttempt{attempt, input, oms::CancellationState::Requested, {}, {}});
    }
  }
  for (const auto states : {std::span<const oms::OutboundOrderState>{terminal_venue_states},
                            std::span<const oms::OutboundOrderState>{ineligible_local_states}}) {
    for (const auto primary : states) {
      CAPTURE(primary);
      const auto before = fixture.create_detached_projection_or_throw(primary);
      const auto input =
          fixture.normalize_cancel_request_or_throw(fixture.create_cancel_attempt_id_or_throw());
      const auto result = fixture.derive_transition(before, input);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ForbiddenRejected);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
  }
}

// --------------------------------------------------------
// Unresolved history and authoritative targets independently prevent automatic or explicit retry.
TEST_CASE("Private OMS cancel requests preserve unresolved and confirmed histories",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto primary : open_venue_states) {
    for (const auto state : unresolved_states) {
      CAPTURE(primary, state);
      const std::array history{create_cancel_history_or_throw(fixture, state)};
      auto before = fixture.create_detached_projection_or_throw(primary);
      before.cancellation_state = state;
      before.cancel_attempt_count = 1U;
      const auto input = fixture.normalize_cancel_request_or_throw(
          fixture.create_cancel_attempt_id_or_throw(2U), 20U);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ForbiddenRejected);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
    auto before = fixture.create_detached_projection_or_throw(primary);
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(5);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
    const auto input =
        fixture.normalize_cancel_request_or_throw(fixture.create_cancel_attempt_id_or_throw());
    const auto result = fixture.derive_transition(before, input);
    REQUIRE(result);
    check_cancel_plan(result.value(), before, before,
                      oms::ProposedPrivateOmsClassification::ForbiddenRejected);
    CHECK_FALSE(result.value().cancel_history_change.has_value());
  }
}

// --------------------------------------------------------
// All first write outcomes for a current Requested attempt retain their literal mapping, including
// each terminal venue state. A terminal primary does not erase an already accepted local request.
TEST_CASE("Private OMS first cancel write outcomes include terminal requested attempts",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{terminal_venue_states}}) {
    for (const auto primary : states) {
      for (std::size_t index = 0U; index < write_outcomes.size(); ++index) {
        CAPTURE(primary, index);
        const std::array history{
            create_cancel_history_or_throw(fixture, oms::CancellationState::Requested)};
        auto before = fixture.create_detached_projection_or_throw(primary);
        before.cancellation_state = oms::CancellationState::Requested;
        before.cancel_attempt_count = 1U;
        const auto input = fixture.normalize_cancel_write_outcome_or_throw(
            history[0].id, write_outcomes[index], 20U);
        const auto result = fixture.derive_transition(before, input, {}, history);
        REQUIRE(result);
        auto after = before;
        after.cancellation_state = write_result_states[index];
        check_cancel_plan(result.value(), before, after,
                          oms::ProposedPrivateOmsClassification::Applied);
        REQUIRE(result.value().cancel_history_change);
        CHECK(result.value().cancel_history_change->index == 0U);
        auto expected = history[0];
        expected.state = write_result_states[index];
        expected.write_outcome = input;
        check_cancel_history(result.value().cancel_history_change->record, expected);
        CHECK_FALSE(history[0].write_outcome.has_value());
      }
    }
  }
}

// --------------------------------------------------------
// Causal rejection permits only late proof of adapter acceptance; first definite local failure
// remains forbidden, and updating the older attempt cannot disturb a newer Requested attempt.
TEST_CASE("Private OMS rejected cancel histories accept only late acceptance proof",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{terminal_venue_states}}) {
    for (const auto primary : states) {
      for (const bool newer_attempt : {false, true}) {
        for (std::size_t outcome = 0U; outcome < write_outcomes.size(); ++outcome) {
          CAPTURE(primary, newer_attempt, outcome);
          std::vector history{
              create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected)};
          if (newer_attempt) {
            history.push_back(
                create_cancel_history_or_throw(fixture, oms::CancellationState::Requested, 2U));
          }
          auto before = fixture.create_detached_projection_or_throw(primary);
          before.cancellation_state = newer_attempt ? oms::CancellationState::Requested
                                      : primary == oms::OutboundOrderState::Cancelled
                                          ? oms::CancellationState::Confirmed
                                          : oms::CancellationState::Rejected;
          before.cancel_attempt_count = static_cast<std::uint32_t>(history.size());
          const auto input = fixture.normalize_cancel_write_outcome_or_throw(
              history[0].id, write_outcomes[outcome], 20U);
          const auto result = fixture.derive_transition(before, input, {}, history);
          REQUIRE(result);
          const auto classification = outcome == 0U
                                          ? oms::ProposedPrivateOmsClassification::ForbiddenRejected
                                          : oms::ProposedPrivateOmsClassification::ProjectionOnly;
          check_cancel_plan(result.value(), before, before, classification);
          if (outcome == 0U) {
            CHECK_FALSE(result.value().cancel_history_change.has_value());
          } else {
            REQUIRE(result.value().cancel_history_change);
            CHECK(result.value().cancel_history_change->index == 0U);
            auto expected = history[0];
            expected.write_outcome = input;
            check_cancel_history(result.value().cancel_history_change->record, expected);
          }
          CHECK_FALSE(history[0].write_outcome.has_value());
          if (newer_attempt) {
            CHECK(history[1].state == oms::CancellationState::Requested);
            CHECK_FALSE(history[1].write_outcome.has_value());
          }
        }
      }
    }
  }
}

// --------------------------------------------------------
// Noncausal terminal confirmation admits every first write observation as history without
// downgrading Confirmed; causal rejection history may coexist with that terminal fact afterwards.
TEST_CASE("Private OMS confirmed cancel histories retain all first late outcomes",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array confirmed_terminal_states{oms::OutboundOrderState::Filled,
                                                 oms::OutboundOrderState::Cancelled};
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{confirmed_terminal_states}}) {
    for (const auto primary : states) {
      for (const auto outcome : write_outcomes) {
        CAPTURE(primary, outcome);
        const std::array history{
            create_cancel_history_or_throw(fixture, oms::CancellationState::Confirmed)};
        auto before = fixture.create_detached_projection_or_throw(primary);
        before.cancellation_state = oms::CancellationState::Confirmed;
        before.cancel_attempt_count = 1U;
        before.authoritative_terminal_cumulative_quantity =
            test_support::create_m4_decimal_or_throw<model::Quantity>(
                primary == oms::OutboundOrderState::Cancelled ? 0 : 6);
        before.reconciliation_required = primary != oms::OutboundOrderState::Filled &&
                                         primary != oms::OutboundOrderState::Cancelled;
        const auto input =
            fixture.normalize_cancel_write_outcome_or_throw(history[0].id, outcome, 20U);
        const auto result = fixture.derive_transition(before, input, {}, history);
        REQUIRE(result);
        check_cancel_plan(result.value(), before, before,
                          oms::ProposedPrivateOmsClassification::ProjectionOnly);
        REQUIRE(result.value().cancel_history_change);
        CHECK(result.value().cancel_history_change->index == 0U);
        auto expected = history[0];
        expected.write_outcome = input;
        check_cancel_history(result.value().cancel_history_change->record, expected);
      }
    }
  }
}

// --------------------------------------------------------
// The planner receives first-seen event identities; even an equal second outcome for the same
// attempt is forbidden because only the retained first write observation has authority.
TEST_CASE("Private OMS second cancel write outcomes never replace the first observation",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (std::size_t first = 0U; first < write_outcomes.size(); ++first) {
    for (const auto second : write_outcomes) {
      CAPTURE(first, second);
      const std::array history{create_cancel_history_or_throw(fixture, write_result_states[first])};
      auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
      before.cancellation_state = write_result_states[first];
      before.cancel_attempt_count = 1U;
      const auto input =
          fixture.normalize_cancel_write_outcome_or_throw(history[0].id, second, 20U);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ForbiddenRejected);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
  }
  for (const auto closed : {oms::CancellationState::Rejected, oms::CancellationState::Confirmed}) {
    for (const auto first : write_outcomes) {
      if (closed == oms::CancellationState::Rejected &&
          first == oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance) {
        continue;
      }
      for (const auto second : write_outcomes) {
        CAPTURE(closed, first, second);
        const std::array history{create_cancel_history_or_throw(fixture, closed, 1U, first)};
        auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Filled);
        before.cancellation_state = closed;
        before.cancel_attempt_count = 1U;
        if (closed == oms::CancellationState::Confirmed) {
          before.authoritative_terminal_cumulative_quantity = fixture.order().economics().quantity;
        }
        const auto input =
            fixture.normalize_cancel_write_outcome_or_throw(history[0].id, second, 20U);
        const auto result = fixture.derive_transition(before, input, {}, history);
        REQUIRE(result);
        check_cancel_plan(result.value(), before, before,
                          oms::ProposedPrivateOmsClassification::ForbiddenRejected);
        CHECK_FALSE(result.value().cancel_history_change.has_value());
      }
    }
  }
}

// --------------------------------------------------------
// A causal rejection closes exactly the sole unresolved attempt in the explicit open/terminal
// partition, regardless of whether the request's write observation arrived before the rejection.
TEST_CASE("Private OMS causal cancel rejection closes only the exact unresolved attempt",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array rejection_terminal_states{oms::OutboundOrderState::Filled,
                                                 oms::OutboundOrderState::ExchangeRejected,
                                                 oms::OutboundOrderState::ReconciledAbsent};
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{rejection_terminal_states}}) {
    for (const auto primary : states) {
      for (const auto unresolved : unresolved_states) {
        CAPTURE(primary, unresolved);
        const std::array history{create_cancel_history_or_throw(fixture, unresolved)};
        auto before = fixture.create_detached_projection_or_throw(primary);
        before.cancellation_state = unresolved;
        before.cancel_attempt_count = 1U;
        const auto input = normalize_cancel_rejection_or_throw(fixture, history[0].id);
        const auto result = fixture.derive_transition(before, input, {}, history);
        REQUIRE(result);
        auto after = before;
        after.cancellation_state = oms::CancellationState::Rejected;
        check_cancel_plan(result.value(), before, after,
                          oms::ProposedPrivateOmsClassification::Applied);
        REQUIRE(result.value().cancel_history_change);
        CHECK(result.value().cancel_history_change->index == 0U);
        auto expected = history[0];
        expected.state = oms::CancellationState::Rejected;
        expected.causal_rejection = input;
        check_cancel_history(result.value().cancel_history_change->record, expected);
        CHECK(history[0].state == unresolved);
      }
    }
  }
}

// --------------------------------------------------------
// An older causally closed attempt can receive more rejection history without closing the newer
// request or changing its retry eligibility, including after the order itself becomes terminal.
TEST_CASE("Private OMS old causal rejection preserves every newer cancel request",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{terminal_venue_states}}) {
    for (const auto primary : states) {
      CAPTURE(primary);
      const std::array history{
          create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected),
          create_cancel_history_or_throw(fixture, oms::CancellationState::Requested, 2U)};
      auto before = fixture.create_detached_projection_or_throw(primary);
      before.cancellation_state = oms::CancellationState::Requested;
      before.cancel_attempt_count = 2U;
      const auto input = normalize_cancel_rejection_or_throw(fixture, history[0].id);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ProjectionOnly);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
      REQUIRE(history[0].causal_rejection);
      CHECK(*history[0].causal_rejection != input);
      CHECK(history[1].state == oms::CancellationState::Requested);
      CHECK_FALSE(history[1].causal_rejection.has_value());
    }
  }
}

// --------------------------------------------------------
// Uncorrelated rejection cannot identify an unresolved request. Without one, only open states may
// set an order-level Rejected projection; terminal observations remain history-only.
TEST_CASE("Private OMS uncorrelated cancel rejection never closes an attempt",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto primary : open_venue_states) {
    for (const bool unresolved : {false, true}) {
      CAPTURE(primary, unresolved);
      std::vector<oms::RetainedCancelAttempt> history;
      auto before = fixture.create_detached_projection_or_throw(primary);
      if (unresolved) {
        history.push_back(
            create_cancel_history_or_throw(fixture, oms::CancellationState::Requested));
        before.cancellation_state = oms::CancellationState::Requested;
        before.cancel_attempt_count = 1U;
      }
      const auto input = normalize_cancel_rejection_or_throw(fixture);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      auto after = before;
      if (!unresolved) {
        after.cancellation_state = oms::CancellationState::Rejected;
      }
      check_cancel_plan(result.value(), before, after,
                        unresolved ? oms::ProposedPrivateOmsClassification::ProjectionOnly
                                   : oms::ProposedPrivateOmsClassification::Applied);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
  }
  for (const auto primary : terminal_venue_states) {
    CAPTURE(primary);
    const auto before = fixture.create_detached_projection_or_throw(primary);
    const auto input = normalize_cancel_rejection_or_throw(fixture);
    const auto result = fixture.derive_transition(before, input);
    REQUIRE(result);
    check_cancel_plan(result.value(), before, before,
                      oms::ProposedPrivateOmsClassification::ProjectionOnly);
    CHECK_FALSE(result.value().cancel_history_change.has_value());
  }
}

// --------------------------------------------------------
// Authoritative cancellation dominates later rejection, including exact rejection evidence for a
// causally closed older attempt or a noncausally confirmed attempt; history never reopens the
// order.
TEST_CASE("Private OMS confirmed cancellation dominates later causal and uncorrelated rejection",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array terminal_states{oms::OutboundOrderState::Filled,
                                       oms::OutboundOrderState::Cancelled};
  constexpr std::array closed_states{oms::CancellationState::Rejected,
                                     oms::CancellationState::Confirmed};
  for (const auto states : {std::span<const oms::OutboundOrderState>{open_venue_states},
                            std::span<const oms::OutboundOrderState>{terminal_states}}) {
    for (const auto primary : states) {
      for (const auto closed : closed_states) {
        for (const bool causal : {false, true}) {
          CAPTURE(primary, closed, causal);
          const std::array history{create_cancel_history_or_throw(fixture, closed)};
          auto before = fixture.create_detached_projection_or_throw(primary);
          before.cancellation_state = oms::CancellationState::Confirmed;
          before.cancel_attempt_count = 1U;
          before.authoritative_terminal_cumulative_quantity =
              test_support::create_m4_decimal_or_throw<model::Quantity>(
                  primary == oms::OutboundOrderState::Cancelled ? 0 : 6);
          before.reconciliation_required = primary != oms::OutboundOrderState::Filled &&
                                           primary != oms::OutboundOrderState::Cancelled;
          const auto input = normalize_cancel_rejection_or_throw(
              fixture, causal ? std::optional{history[0].id} : std::nullopt);
          const auto result = fixture.derive_transition(before, input, {}, history);
          REQUIRE(result);
          check_cancel_plan(result.value(), before, before,
                            oms::ProposedPrivateOmsClassification::ProjectionOnly);
          if (causal && closed == oms::CancellationState::Confirmed) {
            REQUIRE(result.value().cancel_history_change);
            CHECK(result.value().cancel_history_change->index == 0U);
            auto expected = history[0];
            expected.causal_rejection = input;
            check_cancel_history(result.value().cancel_history_change->record, expected);
          } else {
            CHECK_FALSE(result.value().cancel_history_change.has_value());
          }
        }
      }
    }
  }
}

// --------------------------------------------------------
// Causal identity is exact evidence: unknown attempts, another order, and definite pre-acceptance
// failure all yield containment while preserving the complete primary and cancellation history.
TEST_CASE("Private OMS contradictory causal cancel identities are contained without mutation",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  const std::array histories{
      create_cancel_history_or_throw(fixture, oms::CancellationState::Requested),
      create_cancel_history_or_throw(fixture, oms::CancellationState::DefinitelyFailed)};
  for (const auto& record : histories) {
    for (const auto& causal : {fixture.create_cancel_attempt_id_or_throw(9U),
                               fixture.create_cancel_attempt_id_or_throw(
                                   1U, 1U, test_support::create_m4_order_id_or_throw(99U)),
                               fixture.create_cancel_attempt_id_or_throw(1U, 2U), record.id}) {
      if (record.state == oms::CancellationState::Requested && causal == record.id) {
        continue;
      }
      CAPTURE(record.state);
      const std::array history{record};
      auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
      before.cancellation_state = record.state;
      before.cancel_attempt_count = 1U;
      const auto input = normalize_cancel_rejection_or_throw(fixture, causal);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::SafetyContained);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
  }
}

// --------------------------------------------------------
// Noncausal cancellation may close the sole unresolved request because it ends the order's venue
// risk. Same-cumulative closure releases once; a future target retains exposure until fills arrive.
TEST_CASE(
    "Private OMS authoritative cancellation closes unresolved attempts without causal inference",
    "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto primary : open_venue_states) {
    for (const auto unresolved : unresolved_states) {
      for (const bool future_target : {false, true}) {
        for (const bool reconciliation : {false, true}) {
          CAPTURE(primary, unresolved, future_target, reconciliation);
          const std::array history{create_cancel_history_or_throw(fixture, unresolved)};
          auto before = fixture.create_detached_projection_or_throw(primary);
          before.cancellation_state = unresolved;
          before.cancel_attempt_count = 1U;
          const auto target = future_target ? 5 : before.cumulative_filled_quantity.coefficient();
          const auto input = normalize_cancelled_or_throw(fixture, target, reconciliation);
          const auto result = fixture.derive_transition(before, input, {}, history);
          REQUIRE(result);
          auto after = before;
          after.cancellation_state = oms::CancellationState::Confirmed;
          after.authoritative_terminal_cumulative_quantity =
              test_support::create_m4_decimal_or_throw<model::Quantity>(target);
          after.reconciliation_required = future_target;
          if (!future_target) {
            after.state = oms::OutboundOrderState::Cancelled;
          }
          check_cancel_plan(result.value(), before, after,
                            oms::ProposedPrivateOmsClassification::Applied,
                            future_target ? oms::ProposedPrivateEconomicsAction::None
                                          : oms::ProposedPrivateEconomicsAction::ReleaseResidual);
          REQUIRE(result.value().cancel_history_change);
          CHECK(result.value().cancel_history_change->index == 0U);
          auto expected = history[0];
          expected.state = oms::CancellationState::Confirmed;
          check_cancel_history(result.value().cancel_history_change->record, expected);
          CHECK_FALSE(result.value().cancel_history_change->record.causal_rejection.has_value());
        }
      }
    }
  }
}

// --------------------------------------------------------
// A full fill remains economically terminal when later cancellation closes its unresolved request;
// neither that late confirmation nor the next equal target may release the consumed reservation.
TEST_CASE("Private OMS full fill cancellation closes history without a second economic action",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto unresolved : unresolved_states) {
    const std::array history{create_cancel_history_or_throw(fixture, unresolved)};
    auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Filled);
    before.cancellation_state = unresolved;
    before.cancel_attempt_count = 1U;
    const auto input = normalize_cancelled_or_throw(fixture, 6);
    const auto result = fixture.derive_transition(before, input, {}, history);
    REQUIRE(result);
    auto after = before;
    after.cancellation_state = oms::CancellationState::Confirmed;
    after.authoritative_terminal_cumulative_quantity = fixture.order().economics().quantity;
    check_cancel_plan(result.value(), before, after,
                      oms::ProposedPrivateOmsClassification::ProjectionOnly);
    REQUIRE(result.value().cancel_history_change);
    auto expected = history[0];
    expected.state = oms::CancellationState::Confirmed;
    check_cancel_history(result.value().cancel_history_change->record, expected);
    const std::array confirmed_history{expected};
    const auto repeated_input = fixture.normalize_cancelled_or_throw(6, 202U);
    const auto repeated = fixture.derive_transition(after, repeated_input, {}, confirmed_history);
    REQUIRE(repeated);
    check_cancel_plan(repeated.value(), after, after,
                      oms::ProposedPrivateOmsClassification::ProjectionOnly);
    CHECK_FALSE(repeated.value().cancel_history_change.has_value());
    CHECK(fixture.order().state() == oms::OutboundOrderState::PendingEncoding);
    CHECK(fixture.order().private_projection().cancel_attempt_count == 0U);
  }
}

// --------------------------------------------------------
// Shared capacity counts every retained attempt, including closed history. A full table rejects an
// append atomically while still allowing a first observation to replace an existing request row.
TEST_CASE("Private OMS cancel history capacity preflights append without blocking replacements",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  const auto capacity =
      static_cast<std::uint32_t>(fixture.policy().capacities().max_cancel_attempts);
  REQUIRE(capacity > 1U);
  const auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
  const auto request =
      fixture.normalize_cancel_request_or_throw(fixture.create_cancel_attempt_id_or_throw());
  const auto last_slot = fixture.derive_transition(before, request, {}, {}, capacity - 1U);
  REQUIRE(last_slot);
  REQUIRE(last_slot.value().cancel_history_change);
  CHECK(last_slot.value().cancel_history_change->index == 0U);
  const auto exhausted = fixture.derive_transition(before, request, {}, {}, capacity);
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().code == model::DomainErrorCode::PrivateEventCapacityExceeded);
  const std::array history{
      create_cancel_history_or_throw(fixture, oms::CancellationState::Requested)};
  auto requested = before;
  requested.cancel_attempt_count = 1U;
  requested.cancellation_state = oms::CancellationState::Requested;
  const auto outcome = fixture.normalize_cancel_write_outcome_or_throw(
      history[0].id, oms::CancelWriteOutcome::AcceptedAndInitiated, 20U);
  const auto replacement = fixture.derive_transition(requested, outcome, {}, history, capacity);
  REQUIRE(replacement);
  REQUIRE(replacement.value().cancel_history_change);
  CHECK(replacement.value().cancel_history_change->index == 0U);
  CHECK(history[0].state == oms::CancellationState::Requested);
  CHECK_FALSE(history[0].write_outcome.has_value());
}

// --------------------------------------------------------
// A request must use the next exact current-runtime ordinal; neither skipped/reused ordinals nor
// another runtime's identity can obtain a new history row. Counter exhaustion never wraps to one.
TEST_CASE("Private OMS cancel request identity requires the next current runtime ordinal",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  const std::array history{
      create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected)};
  auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
  before.cancellation_state = oms::CancellationState::Rejected;
  before.cancel_attempt_count = 1U;
  const std::array attempts{fixture.create_cancel_attempt_id_or_throw(1U),
                            fixture.create_cancel_attempt_id_or_throw(3U),
                            fixture.create_cancel_attempt_id_or_throw(2U, 2U)};
  for (const auto& attempt : attempts) {
    const auto input = fixture.normalize_cancel_request_or_throw(attempt, 20U);
    const auto result = fixture.derive_transition(before, input, {}, history);
    REQUIRE(result);
    check_cancel_plan(result.value(), before, before,
                      oms::ProposedPrivateOmsClassification::ForbiddenRejected);
    CHECK_FALSE(result.value().cancel_history_change.has_value());
  }
  const auto maximum =
      fixture.create_cancel_attempt_id_or_throw(std::numeric_limits<std::uint64_t>::max());
  const std::array exhausted_history{
      oms::RetainedCancelAttempt{maximum,
                                 fixture.normalize_cancel_request_or_throw(maximum),
                                 oms::CancellationState::Rejected,
                                 {},
                                 normalize_cancel_rejection_or_throw(fixture, maximum)}};
  const auto wrapped =
      fixture.normalize_cancel_request_or_throw(fixture.create_cancel_attempt_id_or_throw(), 20U);
  const auto result = fixture.derive_transition(before, wrapped, {}, exhausted_history);
  REQUIRE(result);
  check_cancel_plan(result.value(), before, before,
                    oms::ProposedPrivateOmsClassification::ForbiddenRejected);
  CHECK_FALSE(result.value().cancel_history_change.has_value());
}

// --------------------------------------------------------
// Retained attempts survive a runtime change byte-for-byte. A late outcome still names the old
// exact attempt, while the current runtime starts at one only after all older requests close.
TEST_CASE(
    "Private OMS cancel histories retain old epochs independently of current request ordinals",
    "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  const auto older_attempt = fixture.create_cancel_attempt_id_or_throw(9U, 1U);
  const auto current_epoch = test_support::create_m4_runtime_epoch_or_throw(2U);
  const auto older_request = fixture.normalize_cancel_request_or_throw(older_attempt);
  const auto older_rejection = normalize_cancel_rejection_or_throw(fixture, older_attempt);
  const std::array history{oms::RetainedCancelAttempt{
      older_attempt, older_request, oms::CancellationState::Rejected, {}, older_rejection}};
  auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
  before.cancellation_state = oms::CancellationState::Rejected;
  before.cancel_attempt_count = 1U;
  const auto current = fixture.create_cancel_attempt_id_or_throw(1U, 2U);
  const auto request = fixture.normalize_cancel_request_or_throw(current, 20U);
  const auto appended = oms::derive_private_oms_transition(
      oms::PrivateOmsTransitionInputs{fixture.order().admission(),
                                      before,
                                      fixture.route(),
                                      fixture.policy(),
                                      current_epoch,
                                      request,
                                      {},
                                      history,
                                      1U});
  REQUIRE(appended);
  auto requested = before;
  requested.cancellation_state = oms::CancellationState::Requested;
  requested.cancel_attempt_count = 2U;
  check_cancel_plan(appended.value(), before, requested,
                    oms::ProposedPrivateOmsClassification::Applied);
  REQUIRE(appended.value().cancel_history_change);
  CHECK(appended.value().cancel_history_change->index == 1U);
  CHECK(appended.value().cancel_history_change->record.id == current);
  const std::array with_current{
      history[0],
      oms::RetainedCancelAttempt{current, request, oms::CancellationState::Requested, {}, {}}};
  const auto late = fixture.normalize_cancel_write_outcome_or_throw(
      older_attempt, oms::CancelWriteOutcome::AcceptedThenOutcomeLost, 21U);
  const auto updated = oms::derive_private_oms_transition(
      oms::PrivateOmsTransitionInputs{fixture.order().admission(),
                                      requested,
                                      fixture.route(),
                                      fixture.policy(),
                                      current_epoch,
                                      late,
                                      {},
                                      with_current,
                                      2U});
  REQUIRE(updated);
  check_cancel_plan(updated.value(), requested, requested,
                    oms::ProposedPrivateOmsClassification::ProjectionOnly);
  REQUIRE(updated.value().cancel_history_change);
  CHECK(updated.value().cancel_history_change->index == 0U);
  auto expected = history[0];
  expected.write_outcome = late;
  check_cancel_history(updated.value().cancel_history_change->record, expected);
  CHECK(with_current[1].state == oms::CancellationState::Requested);
}

// --------------------------------------------------------
// Shape-valid local observations for unknown attempts or forbidden primary states cannot bypass
// the exact retained-request guard, even though their genuine row attribution was normalized.
TEST_CASE("Private OMS cancel write observations require an eligible exact retained request",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  const std::array history{
      create_cancel_history_or_throw(fixture, oms::CancellationState::Requested)};
  for (const auto outcome : write_outcomes) {
    for (const auto& unknown : {fixture.create_cancel_attempt_id_or_throw(2U),
                                fixture.create_cancel_attempt_id_or_throw(1U, 2U)}) {
      auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
      before.cancellation_state = oms::CancellationState::Requested;
      before.cancel_attempt_count = 1U;
      const auto input = fixture.normalize_cancel_write_outcome_or_throw(unknown, outcome, 20U);
      const auto result = fixture.derive_transition(before, input, {}, history);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ForbiddenRejected);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
    }
    for (const auto primary : ineligible_local_states) {
      CAPTURE(primary, outcome);
      auto before = fixture.create_detached_projection_or_throw(primary);
      const auto input =
          fixture.normalize_cancel_write_outcome_or_throw(history[0].id, outcome, 20U);
      const auto result = fixture.derive_transition(before, input);
      REQUIRE(result);
      check_cancel_plan(result.value(), before, before,
                        oms::ProposedPrivateOmsClassification::ForbiddenRejected);
      CHECK_FALSE(result.value().cancel_history_change.has_value());
      // These primary states cannot have issued a request; fabricated history is a snapshot fault.
      before.cancellation_state = oms::CancellationState::Requested;
      before.cancel_attempt_count = 1U;
      const auto impossible_history = fixture.derive_transition(before, input, {}, history);
      REQUIRE_FALSE(impossible_history);
      CHECK(impossible_history.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
    }
  }
}

// --------------------------------------------------------
// A malformed borrowed history is rejected before any proposal, including when the incoming
// timeout would otherwise be harmless. Tests alter one complete-evidence constraint at a time.
TEST_CASE("Private OMS validates complete cancel histories before deriving a transition",
          "[oms][m4][private-oms][cancel]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array corruptions{MalformedCancelHistory::WrongRequestIdentity,
                                   MalformedCancelHistory::WrongRequestPayload,
                                   MalformedCancelHistory::MissingWriteOutcome,
                                   MalformedCancelHistory::WrongWriteIdentity,
                                   MalformedCancelHistory::WrongWritePayload,
                                   MalformedCancelHistory::ContradictoryWriteOutcome,
                                   MalformedCancelHistory::MissingCausalRejection,
                                   MalformedCancelHistory::WrongCausalIdentity,
                                   MalformedCancelHistory::UncorrelatedCausalEvidence,
                                   MalformedCancelHistory::WrongCausalPayload,
                                   MalformedCancelHistory::UnresolvedEarlierAttempt,
                                   MalformedCancelHistory::DuplicateAttemptIdentity,
                                   MalformedCancelHistory::ReversedOrdinals};
  for (const auto corruption : corruptions) {
    CAPTURE(corruption);
    std::vector history{create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected)};
    auto before = fixture.create_detached_projection_or_throw(oms::OutboundOrderState::Working);
    before.cancellation_state = oms::CancellationState::Rejected;
    const auto other = fixture.create_cancel_attempt_id_or_throw(2U);
    switch (corruption) {
    case MalformedCancelHistory::WrongRequestIdentity:
      history[0].request = fixture.normalize_cancel_request_or_throw(other, 20U);
      break;
    case MalformedCancelHistory::WrongRequestPayload:
      history[0].request = fixture.normalize_order_timeout_or_throw();
      break;
    case MalformedCancelHistory::MissingWriteOutcome:
      history[0] = create_cancel_history_or_throw(fixture, oms::CancellationState::WriteInitiated);
      history[0].write_outcome.reset();
      before.cancellation_state = oms::CancellationState::WriteInitiated;
      break;
    case MalformedCancelHistory::WrongWriteIdentity:
      history[0].write_outcome = fixture.normalize_cancel_write_outcome_or_throw(
          other, oms::CancelWriteOutcome::AcceptedAndInitiated, 20U);
      break;
    case MalformedCancelHistory::WrongWritePayload:
      history[0].write_outcome = fixture.normalize_order_timeout_or_throw();
      break;
    case MalformedCancelHistory::ContradictoryWriteOutcome:
      history[0].write_outcome = fixture.normalize_cancel_write_outcome_or_throw(
          history[0].id, oms::CancelWriteOutcome::DefiniteFailureBeforeAcceptance, 20U);
      break;
    case MalformedCancelHistory::MissingCausalRejection:
      history[0].causal_rejection.reset();
      break;
    case MalformedCancelHistory::WrongCausalIdentity:
      history[0].causal_rejection = normalize_cancel_rejection_or_throw(fixture, other);
      break;
    case MalformedCancelHistory::UncorrelatedCausalEvidence:
      history[0].causal_rejection = normalize_cancel_rejection_or_throw(fixture);
      break;
    case MalformedCancelHistory::WrongCausalPayload:
      history[0].causal_rejection = normalize_cancelled_or_throw(fixture, 0);
      break;
    case MalformedCancelHistory::UnresolvedEarlierAttempt:
      history[0] = create_cancel_history_or_throw(fixture, oms::CancellationState::Requested);
      history.push_back(
          create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected, 2U));
      before.cancellation_state = oms::CancellationState::Requested;
      break;
    case MalformedCancelHistory::DuplicateAttemptIdentity:
      history.push_back(history[0]);
      break;
    case MalformedCancelHistory::ReversedOrdinals:
      history.insert(history.begin(),
                     create_cancel_history_or_throw(fixture, oms::CancellationState::Rejected, 2U));
      break;
    }
    before.cancel_attempt_count = static_cast<std::uint32_t>(history.size());
    const auto input = fixture.normalize_order_timeout_or_throw(30U);
    const auto result = fixture.derive_transition(before, input, {}, history);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
    CHECK(fixture.order().private_projection().cancel_attempt_count == 0U);
  }
}

// --------------------------------------------------------

} // namespace
