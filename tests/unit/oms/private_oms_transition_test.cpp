// Purpose: independently qualify the known-order OMS projection partition and malformed-snapshot
// rejection while proving detached proposals never mutate genuine OMS or private admission state.

#include "private_oms_transition_fixture.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace {

using namespace aegis;
using test_support::PrivateOmsTransitionFixture;

// ########################################################################
// Keep the independent literal matrices readable while retaining the exact production enum types.
using State = oms::OutboundOrderState;
using Classification = oms::ProposedPrivateOmsClassification;
using Economics = oms::ProposedPrivateEconomicsAction;

// ########################################################################

constexpr std::array all_states{State::PendingEncoding,  State::PendingInitiation,
                                State::WriteInitiated,   State::SubmissionUnknown,
                                State::LocallyFailed,    State::Working,
                                State::PartiallyFilled,  State::Filled,
                                State::ExchangeRejected, State::Cancelled,
                                State::ReconciledAbsent};

// Proposals are portable inert values; copying one cannot duplicate any owner mutation authority.
static_assert(std::is_copy_constructible_v<oms::PrivateOmsTransitionPlan>);
static_assert(std::is_copy_assignable_v<oms::PrivateOmsTransitionPlan>);

// --------------------------------------------------------
// The literal acknowledgement partition covers every primary state and both authoritative lanes.
// A late acknowledgement can update correlation without changing already final economics.
TEST_CASE("private OMS acknowledgement plans exhaust the primary state partition",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  const auto retained_before = fixture.order().private_projection();
  constexpr std::array expected{Classification::SafetyContained, Classification::SafetyContained,
                                Classification::Applied,         Classification::Applied,
                                Classification::SafetyContained, Classification::ProjectionOnly,
                                Classification::ProjectionOnly,  Classification::ProjectionOnly,
                                Classification::SafetyContained, Classification::ProjectionOnly,
                                Classification::SafetyContained};
  for (const bool reconciliation : {false, true}) {
    for (std::size_t index = 0U; index < all_states.size(); ++index) {
      const auto before = fixture.create_detached_projection_or_throw(all_states[index]);
      const auto saved = before;
      const auto input = fixture.normalize_acknowledgement_or_throw(1U, reconciliation);
      const auto saved_input = input;
      CAPTURE(index, reconciliation);
      const auto result = fixture.derive_transition(before, input);
      REQUIRE(result);
      const auto& plan = result.value();
      CHECK(plan.before == before);
      CHECK(plan.classification == expected[index]);
      CHECK(plan.economics_action == Economics::None);
      CHECK(plan.transition_effect_count == 1U);
      CHECK(plan.order_callback_count == 1U);
      CHECK_FALSE(plan.gap_insertion.has_value());
      CHECK_FALSE(plan.cancel_history_change.has_value());
      CHECK(plan.drained_pending_prefix_count == 0U);
      if (expected[index] == Classification::SafetyContained) {
        CHECK(plan.after == before);
        CHECK(plan.safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
      } else {
        CHECK_FALSE(plan.safety_reason.has_value());
        CHECK(plan.after.state == (index == 2U || index == 3U ? State::Working : before.state));
        CHECK(plan.after.exchange_acknowledged);
        CHECK(plan.after.exchange_order_id == fixture.create_exchange_order_id_or_throw());
        CHECK(plan.after.cumulative_filled_quantity == before.cumulative_filled_quantity);
        CHECK(plan.after.reconciliation_required == before.reconciliation_required);
      }
      CHECK(before == saved);
      CHECK(input == saved_input);
      CHECK(fixture.order().private_projection() == retained_before);
    }
  }
}

// --------------------------------------------------------
// Rejection can release only a previously uncertain unacknowledged order without execution or
// terminal evidence; a distinct late rejection cannot release the same reservation again.
TEST_CASE("private OMS rejection plans exhaust the primary state partition",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array expected{Classification::SafetyContained, Classification::SafetyContained,
                                Classification::Applied,         Classification::Applied,
                                Classification::SafetyContained, Classification::SafetyContained,
                                Classification::SafetyContained, Classification::SafetyContained,
                                Classification::ProjectionOnly,  Classification::SafetyContained,
                                Classification::SafetyContained};
  for (const bool reconciliation : {false, true}) {
    for (std::size_t index = 0U; index < all_states.size(); ++index) {
      const auto before = fixture.create_detached_projection_or_throw(all_states[index]);
      const auto input = fixture.normalize_rejection_or_throw(1U, reconciliation);
      CAPTURE(index, reconciliation);
      const auto result = fixture.derive_transition(before, input);
      REQUIRE(result);
      const auto& plan = result.value();
      CHECK(plan.classification == expected[index]);
      CHECK(plan.order_callback_count == 1U);
      if (expected[index] == Classification::Applied) {
        CHECK(plan.after.state == State::ExchangeRejected);
        CHECK_FALSE(plan.after.reconciliation_required);
        CHECK(plan.economics_action == Economics::ReleaseResidual);
        CHECK(plan.reservation_closure_cause == risk::ReservationClosureCause::ExchangeRejected);
      } else {
        auto expected_after = before;
        // An earlier local-only rejection can acquire its first exchange mapping from a distinct
        // accepted late projection, without reapplying reservation release or changing primary.
        if (expected[index] == Classification::ProjectionOnly) {
          expected_after.exchange_order_id = fixture.create_exchange_order_id_or_throw();
        }
        CHECK(plan.after == expected_after);
        CHECK(plan.economics_action == Economics::None);
        CHECK_FALSE(plan.reservation_closure_cause.has_value());
      }
      CHECK(plan.safety_reason.has_value() == (expected[index] == Classification::SafetyContained));
    }
  }
}

// --------------------------------------------------------
// Zero applied quantity is insufficient for pre-fill rejection: buffered or safety-contained
// execution evidence and an unreconciled authoritative cancellation target both block release.
TEST_CASE("private OMS rejection guards retain execution evidence and terminal targets",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  auto before = fixture.create_detached_projection_or_throw();
  SECTION("known execution evidence with zero applied quantity") {
    before.execution_evidence_observed = true;
  }
  SECTION("authoritative terminal target above the applied quantity") {
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(2);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
  }
  const auto result = fixture.derive_transition(before, fixture.normalize_rejection_or_throw());
  REQUIRE(result);
  CHECK(result.value().classification == Classification::SafetyContained);
  CHECK(result.value().safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  CHECK(result.value().economics_action == Economics::None);
  CHECK(result.value().after == before);
}

// --------------------------------------------------------
// Timeout changes only unresolved order uncertainty. Terminal orders retain stale evidence without
// reopening risk, while pre-initiation and local-terminal observations are forbidden local inputs.
TEST_CASE("private OMS order timeout plans preserve the local and terminal boundaries",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array expected{
      Classification::ForbiddenRejected, Classification::ForbiddenRejected,
      Classification::Applied,           Classification::Applied,
      Classification::ForbiddenRejected, Classification::Applied,
      Classification::Applied,           Classification::ProjectionOnly,
      Classification::ProjectionOnly,    Classification::ProjectionOnly,
      Classification::ProjectionOnly};
  for (std::size_t index = 0U; index < all_states.size(); ++index) {
    const auto before = fixture.create_detached_projection_or_throw(all_states[index]);
    CAPTURE(index);
    const auto result =
        fixture.derive_transition(before, fixture.normalize_order_timeout_or_throw());
    REQUIRE(result);
    const auto& plan = result.value();
    CHECK(plan.classification == expected[index]);
    CHECK(plan.rejection_code ==
          (expected[index] == Classification::ForbiddenRejected
               ? std::optional{model::DomainErrorCode::InvalidPrivateOmsState}
               : std::nullopt));
    CHECK(plan.economics_action == Economics::None);
    CHECK(plan.after.state == before.state);
    CHECK(plan.after.cumulative_filled_quantity == before.cumulative_filled_quantity);
    if (expected[index] == Classification::Applied) {
      CHECK(plan.after.reconciliation_required);
      CHECK(plan.safety_reason == risk::AccountSafetyReason::TimeoutObserved);
    } else {
      CHECK(plan.after == before);
      CHECK_FALSE(plan.safety_reason.has_value());
    }
    CHECK(plan.order_callback_count ==
          (expected[index] == Classification::ForbiddenRejected ? 0U : 1U));
  }
}

// --------------------------------------------------------
// A later normalized local failure can only restate the exact M3 certainty already retained; it
// cannot reopen the direct encoding/initiation transitions or release exposure a second time.
TEST_CASE("private OMS local failure plans match only the two inherited M3 outcomes",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  for (const auto certainty : {oms::LocalFailureCertainty::ProvenBeforeAcceptance,
                               oms::LocalFailureCertainty::AcceptanceCouldHaveOccurred}) {
    for (const auto state : all_states) {
      const auto before = fixture.create_detached_projection_or_throw(state);
      const bool accepted = (state == State::LocallyFailed &&
                             certainty == oms::LocalFailureCertainty::ProvenBeforeAcceptance) ||
                            (state == State::SubmissionUnknown &&
                             certainty == oms::LocalFailureCertainty::AcceptanceCouldHaveOccurred);
      CAPTURE(state, certainty);
      const auto result =
          fixture.derive_transition(before, fixture.normalize_local_failure_or_throw(certainty));
      REQUIRE(result);
      CHECK(result.value().classification ==
            (accepted ? Classification::ProjectionOnly : Classification::ForbiddenRejected));
      CHECK(result.value().after == before);
      CHECK(result.value().rejection_code ==
            (accepted ? std::nullopt
                      : std::optional{model::DomainErrorCode::InvalidPrivateOmsState}));
      CHECK(result.value().economics_action == Economics::None);
      CHECK_FALSE(result.value().safety_reason.has_value());
      CHECK(result.value().order_callback_count == (accepted ? 1U : 0U));
    }
  }
}

// --------------------------------------------------------
// A detached snapshot is untrusted data: internally contradictory states and side-table counts
// fail before an otherwise valid event can produce a candidate or modify the source snapshot.
TEST_CASE("private OMS planning rejects malformed detached projection invariants",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  auto before = fixture.create_detached_projection_or_throw();
  SECTION("unassigned primary state") { before.state = static_cast<State>(0); }
  SECTION("unassigned cancellation state") {
    before.cancellation_state = oms::CancellationState::Unassigned;
  }
  SECTION("negative cumulative quantity") {
    before.cumulative_filled_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(-1);
  }
  SECTION("cumulative quantity above original") {
    before.cumulative_filled_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(7);
  }
  SECTION("filled state below original") {
    before = fixture.create_detached_projection_or_throw(State::Filled, 5);
  }
  SECTION("partial state with zero cumulative") {
    before = fixture.create_detached_projection_or_throw(State::PartiallyFilled, 0);
  }
  SECTION("acknowledgement without exchange identity") { before.exchange_acknowledged = true; }
  SECTION("execution mapping cache without exchange identity or execution evidence") {
    before.exchange_mapping_established_by_execution = true;
  }
  SECTION("pending count without pending rows") { before.pending_fill_count = 1U; }
  SECTION("cancel count without history rows") { before.cancel_attempt_count = 1U; }
  SECTION("terminal target above original") {
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(7);
  }
  const auto saved = before;
  const auto result =
      fixture.derive_transition(before, fixture.normalize_acknowledgement_or_throw());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
  CHECK(before == saved);
  CHECK(fixture.order().state() == State::PendingEncoding);
}

// --------------------------------------------------------
// Confirmed cancellation is meaningful only with its monotonic terminal target. Neither an
// order-level flag nor a closed attempt may lose that target and let rejection restore retry.
TEST_CASE("private OMS cancellation confirmation requires a retained terminal target",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  const auto retained_before = fixture.order().private_projection();
  const auto attempt = fixture.create_cancel_attempt_id_or_throw();
  const std::array history{
      oms::RetainedCancelAttempt{attempt, fixture.normalize_cancel_request_or_throw(attempt),
                                 oms::CancellationState::Confirmed, std::nullopt, std::nullopt}};
  const auto input = fixture.normalize_cancel_rejection_or_throw();
  for (const bool order_confirmation : {true, false}) {
    CAPTURE(order_confirmation);
    auto before = fixture.create_detached_projection_or_throw(State::Working);
    before.cancellation_state =
        order_confirmation ? oms::CancellationState::Confirmed : oms::CancellationState::None;
    before.cancel_attempt_count = order_confirmation ? 0U : 1U;
    const auto saved = before;
    const auto result = fixture.derive_transition(
        before, input, {},
        order_confirmation ? std::span<const oms::RetainedCancelAttempt>{}
                           : std::span<const oms::RetainedCancelAttempt>{history});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
    CHECK(before == saved);
    CHECK(fixture.order().private_projection() == retained_before);
  }
}

// --------------------------------------------------------
// Target consistency is one-way: a venue-terminal primary state may still retain a Requested
// attempt and accept its first late write fact without losing the authoritative terminal target.
TEST_CASE("private OMS terminal targets preserve requested attempt late write eligibility",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  const auto attempt = fixture.create_cancel_attempt_id_or_throw();
  const std::array history{
      oms::RetainedCancelAttempt{attempt, fixture.normalize_cancel_request_or_throw(attempt),
                                 oms::CancellationState::Requested, std::nullopt, std::nullopt}};
  const auto input = fixture.normalize_cancel_write_outcome_or_throw(
      attempt, oms::CancelWriteOutcome::AcceptedAndInitiated, 2U);
  for (const auto state : {State::Filled, State::Cancelled}) {
    CAPTURE(state);
    auto before = fixture.create_detached_projection_or_throw(state);
    before.authoritative_terminal_cumulative_quantity = before.cumulative_filled_quantity;
    before.cancellation_state = oms::CancellationState::Requested;
    before.cancel_attempt_count = 1U;
    const auto saved = before;
    const auto result = fixture.derive_transition(before, input, {}, history);
    REQUIRE(result);
    CHECK(result.value().classification == Classification::Applied);
    CHECK(result.value().after.state == state);
    CHECK(result.value().after.authoritative_terminal_cumulative_quantity ==
          before.authoritative_terminal_cumulative_quantity);
    CHECK(result.value().after.cancellation_state == oms::CancellationState::WriteInitiated);
    CHECK(result.value().economics_action == Economics::None);
    CHECK_FALSE(result.value().reservation_closure_cause.has_value());
    REQUIRE(result.value().cancel_history_change);
    CHECK(result.value().cancel_history_change->record.write_outcome == input);
    CHECK(before == saved);
    CHECK(fixture.order().state() == State::PendingEncoding);
  }
}

// --------------------------------------------------------
// Account/source fan-out and authoritative complete-negative proof require their later joint
// planners; a one-order planner cannot silently claim those wider effects.
TEST_CASE("private OMS single-order planner rejects account and source observation inputs",
          "[oms][private-oms-transition]") {
  const PrivateOmsTransitionFixture fixture;
  const auto& source = fixture.source;
  const auto origin = source.create_local_private_event_origin_or_throw();
  auto input = source.private_event_factory().normalize_account_timeout(origin, source.account_id(),
                                                                        source.venue_id());
  SECTION("whole-account timeout") {}
  SECTION("private-source disconnect") {
    input = source.private_event_factory().normalize_disconnect(
        origin, source.account_id(), source.venue_id(),
        test_support::create_m4_opaque_identity_or_throw<oms::PrivateSourceEpochId>(0x71U));
  }
  REQUIRE(input);
  const auto before = fixture.create_detached_projection_or_throw();
  const auto result = fixture.derive_transition(before, input.value());
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
  CHECK(fixture.order().state() == State::PendingEncoding);
}

// --------------------------------------------------------

} // namespace
