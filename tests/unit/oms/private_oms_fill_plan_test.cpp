// Purpose: independently qualify contiguous fills, bounded pending intervals, terminal-target
// drains, malformed economics, and arrival-permutation invariance of detached OMS proposals.

#include "private_oms_transition_fixture.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using namespace aegis;
using test_support::PrivateOmsTransitionFixture;

// ########################################################################
// The aliases retain exact production enum types while keeping independent expected states clear.
using State = oms::OutboundOrderState;
using Classification = oms::ProposedPrivateOmsClassification;
using Economics = oms::ProposedPrivateEconomicsAction;

// ########################################################################

// --------------------------------------------------------
// Integer fixture intervals have independent endpoints; this helper only copies their complete
// normalized source values and does not calculate expected overlap, order, or application.
[[nodiscard]] oms::PendingPrivateExecution
create_pending_execution_or_throw(const PrivateOmsTransitionFixture& fixture, std::int64_t start,
                                  std::int64_t end, std::uint8_t event) {
  return oms::PendingPrivateExecution{
      fixture.normalize_execution_or_throw(end - start, end, event)};
}

// --------------------------------------------------------
// Seed only the literal invariant that earlier buffered known executions established this mapping.
// Pending contents and expected cumulative application are authored separately in every test.
void establish_hypothetical_execution_mapping(oms::PrivateOrderProjection& projection,
                                              const PrivateOmsTransitionFixture& fixture) {
  projection.execution_evidence_observed = true;
  projection.exchange_order_id = fixture.create_exchange_order_id_or_throw();
  projection.exchange_mapping_established_by_execution = true;
}

// --------------------------------------------------------
// Execution before acknowledgement establishes exact correlation without inventing acknowledgement;
// order uncertainty survives a partial fill and clears only when the complete quantity is filled.
TEST_CASE("private OMS fill before acknowledgement preserves independent flags and economics",
          "[oms][private-oms-transition][private-oms-fill]") {
  for (const auto side : {execution::OrderSide::Buy, execution::OrderSide::Sell}) {
    const PrivateOmsTransitionFixture fixture{6, side};
    for (const bool reconciliation : {false, true}) {
      for (const auto state : {State::WriteInitiated, State::SubmissionUnknown}) {
        for (const std::int64_t cumulative : {2, 6}) {
          const auto before = fixture.create_detached_projection_or_throw(state);
          const auto input =
              fixture.normalize_execution_or_throw(cumulative, cumulative, 1U, reconciliation);
          CAPTURE(side, reconciliation, state, cumulative);
          const auto result = fixture.derive_transition(before, input);
          REQUIRE(result);
          const auto& plan = result.value();
          CHECK(plan.before == before);
          CHECK(plan.classification == Classification::Applied);
          CHECK(plan.economics_action == Economics::ApplyExecutions);
          CHECK(plan.reservation_closure_cause ==
                (cumulative == 6 ? std::optional{risk::ReservationClosureCause::FullFill}
                                 : std::nullopt));
          CHECK(plan.after.state == (cumulative == 6 ? State::Filled : State::PartiallyFilled));
          CHECK(plan.after.cumulative_filled_quantity ==
                test_support::create_m4_decimal_or_throw<model::Quantity>(cumulative));
          CHECK_FALSE(plan.after.exchange_acknowledged);
          CHECK(plan.after.exchange_order_id == fixture.create_exchange_order_id_or_throw());
          CHECK(plan.after.execution_evidence_observed);
          CHECK(plan.after.exchange_mapping_established_by_execution);
          CHECK(plan.after.reconciliation_required ==
                (state == State::SubmissionUnknown && cumulative < 6));
          CHECK(plan.order_callback_count == 1U);
          CHECK(plan.transition_effect_count == 1U);
          CHECK(plan.drained_pending_prefix_count == 0U);
          CHECK_FALSE(plan.gap_insertion.has_value());
          CHECK_FALSE(plan.safety_reason.has_value());
          CHECK(fixture.order().state() == State::PendingEncoding);
        }
      }
    }
  }
}

// --------------------------------------------------------
// A mapping first established by acknowledgement never changes its cause when executions arrive.
// Terminal and pre-initiation first-seen executions are contained even with plausible economics.
TEST_CASE("private OMS executions obey every primary-state boundary and mapping cache cause",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  constexpr std::array all_states{State::PendingEncoding,  State::PendingInitiation,
                                  State::WriteInitiated,   State::SubmissionUnknown,
                                  State::LocallyFailed,    State::Working,
                                  State::PartiallyFilled,  State::Filled,
                                  State::ExchangeRejected, State::Cancelled,
                                  State::ReconciledAbsent};
  constexpr std::array accepted{false, false, true,  true,  false, true,
                                true,  false, false, false, false};
  for (std::size_t index = 0U; index < all_states.size(); ++index) {
    const auto before = fixture.create_detached_projection_or_throw(all_states[index]);
    const auto cumulative = before.cumulative_filled_quantity.coefficient() + 1;
    const auto input = fixture.normalize_execution_or_throw(1, cumulative);
    CAPTURE(index);
    const auto result = fixture.derive_transition(before, input);
    REQUIRE(result);
    CHECK(result.value().classification ==
          (accepted[index] ? Classification::Applied : Classification::SafetyContained));
    CHECK(result.value().after.execution_evidence_observed);
    CHECK(result.value().order_callback_count == 1U);
    if (!accepted[index]) {
      auto expected = before;
      expected.execution_evidence_observed = true;
      CHECK(result.value().after == expected);
      CHECK(result.value().economics_action == Economics::None);
      CHECK(result.value().safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
    } else if (before.exchange_acknowledged) {
      CHECK_FALSE(result.value().after.exchange_mapping_established_by_execution);
      CHECK(result.value().after.exchange_acknowledged);
    }
  }
}

// --------------------------------------------------------
// Missing prefixes retain the complete source fact without economic or callback work; once the
// prefix arrives the entire newly contiguous group is planned before any observer may run.
TEST_CASE("private OMS gap insertion and multi-fill drain retain exact source identities",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  const auto initial = fixture.create_detached_projection_or_throw();
  const auto gap_input = fixture.normalize_execution_or_throw(1, 3, 3U);
  const auto gap = fixture.derive_transition(initial, gap_input);
  REQUIRE(gap);
  CHECK(gap.value().classification == Classification::BufferedGap);
  CHECK(gap.value().economics_action == Economics::None);
  REQUIRE(gap.value().gap_insertion.has_value());
  CHECK(gap.value().gap_insertion->execution == gap_input);
  CHECK(gap.value().after.state == State::WriteInitiated);
  CHECK(gap.value().after.cumulative_filled_quantity.coefficient() == 0);
  CHECK(gap.value().after.pending_fill_count == 1U);
  CHECK(gap.value().order_callback_count == 0U);
  CHECK(gap.value().transition_effect_count == 1U);

  auto before = fixture.create_detached_projection_or_throw();
  establish_hypothetical_execution_mapping(before, fixture);
  const std::array pending{create_pending_execution_or_throw(fixture, 1, 2, 2U),
                           create_pending_execution_or_throw(fixture, 2, 4, 4U),
                           create_pending_execution_or_throw(fixture, 4, 6, 6U)};
  before.pending_fill_count = 3U;
  const auto saved_pending = pending;
  const auto result =
      fixture.derive_transition(before, fixture.normalize_execution_or_throw(1, 1), pending);
  REQUIRE(result);
  CHECK(result.value().classification == Classification::Applied);
  CHECK(result.value().economics_action == Economics::ApplyExecutions);
  CHECK(result.value().after.state == State::Filled);
  CHECK(result.value().after.cumulative_filled_quantity.coefficient() == 6);
  CHECK(result.value().after.pending_fill_count == 0U);
  CHECK(result.value().drained_pending_prefix_count == 3U);
  CHECK(result.value().transition_effect_count == 4U);
  CHECK(result.value().order_callback_count == 4U);
  for (std::size_t index = 0U; index < pending.size(); ++index) {
    CHECK(pending[index].execution == saved_pending[index].execution);
  }
  CHECK(fixture.order().state() == State::PendingEncoding);
}

// --------------------------------------------------------
// The final drained cumulative, not the incoming fill's intermediate endpoint, decides whether a
// target below original releases residual risk or a target at original consumes the full fill.
TEST_CASE("private OMS terminal targets classify the complete drained cumulative",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  for (const std::int64_t target : {5, 6}) {
    auto before = fixture.create_detached_projection_or_throw();
    establish_hypothetical_execution_mapping(before, fixture);
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(target);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
    before.pending_fill_count = 2U;
    const std::array pending{create_pending_execution_or_throw(fixture, 2, 3, 3U),
                             create_pending_execution_or_throw(fixture, 3, target, 5U)};
    CAPTURE(target);
    const auto result =
        fixture.derive_transition(before, fixture.normalize_execution_or_throw(2, 2), pending);
    REQUIRE(result);
    CHECK(result.value().after.state == (target == 5 ? State::Cancelled : State::Filled));
    CHECK(
        result.value().economics_action ==
        (target == 5 ? Economics::ApplyExecutionsThenReleaseResidual : Economics::ApplyExecutions));
    CHECK(result.value().reservation_closure_cause ==
          (target == 5 ? risk::ReservationClosureCause::DefinitiveCancellation
                       : risk::ReservationClosureCause::FullFill));
    CHECK(result.value().after.cumulative_filled_quantity.coefficient() == target);
    CHECK_FALSE(result.value().after.reconciliation_required);
    CHECK(result.value().after.cancellation_state == oms::CancellationState::Confirmed);
    CHECK(result.value().after.pending_fill_count == 0U);
    CHECK(result.value().drained_pending_prefix_count == 2U);
    CHECK(result.value().order_callback_count == 3U);
  }
}

// --------------------------------------------------------
// A newly normalized cancellation target must bound applied and pending fills and must equal any
// already retained target. Each independent violation contains the fact without releasing risk.
TEST_CASE("private OMS cancellation targets reject contradictory cumulative bounds",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  auto before = fixture.create_detached_projection_or_throw(State::PartiallyFilled, 2);
  std::vector<oms::PendingPrivateExecution> pending;
  std::int64_t incoming_target = 1;
  SECTION("terminal target falls below already applied cumulative") {}
  SECTION("terminal target exceeds original approved quantity") { incoming_target = 7; }
  SECTION("second terminal target differs from retained authority") {
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(4);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
    incoming_target = 5;
  }
  SECTION("terminal target excludes a retained pending interval") {
    pending.push_back(create_pending_execution_or_throw(fixture, 3, 5, 5U));
    before.pending_fill_count = 1U;
    incoming_target = 4;
  }
  const auto saved = before;
  const auto saved_pending = pending;
  const auto retained_before = fixture.order().private_projection();
  for (const bool reconciliation : {false, true}) {
    CAPTURE(incoming_target, reconciliation);
    const auto input = fixture.normalize_cancelled_or_throw(incoming_target, 9U, reconciliation);
    const auto result = fixture.derive_transition(before, input, pending);
    REQUIRE(result);
    CHECK(result.value().classification == Classification::SafetyContained);
    CHECK(result.value().safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
    CHECK(result.value().after == saved);
    CHECK(result.value().economics_action == Economics::None);
    CHECK_FALSE(result.value().reservation_closure_cause.has_value());
    CHECK_FALSE(result.value().gap_insertion.has_value());
    CHECK_FALSE(result.value().cancel_history_change.has_value());
    CHECK(result.value().drained_pending_prefix_count == 0U);
    CHECK(result.value().order_callback_count == 1U);
    CHECK(result.value().transition_effect_count == 1U);
    CHECK(before == saved);
    CHECK(fixture.order().private_projection() == retained_before);
    REQUIRE(pending.size() == saved_pending.size());
    for (std::size_t index = 0U; index < pending.size(); ++index) {
      CHECK(pending[index].execution == saved_pending[index].execution);
    }
  }
}

// --------------------------------------------------------
// The pending capacity applies to retained gaps, not to newly contiguous executions which free the
// complete existing prefix. A failed fifth gap leaves all detached inputs and genuine OMS
// unchanged.
TEST_CASE("private OMS pending-fill boundary admits a full drain and rejects additional gap",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  REQUIRE(fixture.policy().capacities().max_pending_fill_intervals_per_order == 4U);
  auto before = fixture.create_detached_projection_or_throw();
  establish_hypothetical_execution_mapping(before, fixture);
  before.pending_fill_count = 4U;
  const std::array pending{create_pending_execution_or_throw(fixture, 1, 2, 2U),
                           create_pending_execution_or_throw(fixture, 2, 3, 3U),
                           create_pending_execution_or_throw(fixture, 3, 4, 4U),
                           create_pending_execution_or_throw(fixture, 4, 5, 5U)};
  const auto saved = before;
  const auto rejected =
      fixture.derive_transition(before, fixture.normalize_execution_or_throw(1, 6, 6U), pending);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == model::DomainErrorCode::PrivateEventCapacityExceeded);
  CHECK(before == saved);
  const auto drained =
      fixture.derive_transition(before, fixture.normalize_execution_or_throw(1, 1), pending);
  REQUIRE(drained);
  CHECK(drained.value().after.state == State::PartiallyFilled);
  CHECK(drained.value().after.cumulative_filled_quantity.coefficient() == 5);
  CHECK(drained.value().after.pending_fill_count == 0U);
  CHECK(drained.value().drained_pending_prefix_count == 4U);
  CHECK(drained.value().order_callback_count == 5U);
  CHECK(fixture.order().private_projection().pending_fill_count == 0U);
}

// --------------------------------------------------------
// Distinct overlapping intervals and terminal-target violations are authoritative contradictions,
// regardless of whether their individual decimal values passed normalized shape validation.
TEST_CASE("private OMS fill overlap overfill and terminal target conflicts preserve economics",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  auto before = fixture.create_detached_projection_or_throw();
  establish_hypothetical_execution_mapping(before, fixture);
  before.pending_fill_count = 1U;
  const std::array pending{create_pending_execution_or_throw(fixture, 2, 4, 4U)};
  std::int64_t start = 1;
  std::int64_t end = 3;
  SECTION("overlap preceding the retained interval") {}
  SECTION("competing interval with the same start") { start = 2; }
  SECTION("interval contained in the retained interval") {
    start = 3;
    end = 4;
  }
  SECTION("overfill above the original quantity") {
    start = 6;
    end = 7;
  }
  SECTION("new gap ends beyond retained cancellation target") {
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(4);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
    start = 4;
    end = 5;
  }
  const auto saved = before;
  const auto result = fixture.derive_transition(
      before, fixture.normalize_execution_or_throw(end - start, end, 9U), pending);
  REQUIRE(result);
  CHECK(result.value().classification == Classification::SafetyContained);
  CHECK(result.value().safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  CHECK(result.value().economics_action == Economics::None);
  CHECK(result.value().after == saved);
  CHECK_FALSE(result.value().gap_insertion.has_value());
  CHECK(result.value().drained_pending_prefix_count == 0U);
}

// --------------------------------------------------------
// Invalid stored gap sequences fail as invalid snapshots, rather than being silently reordered,
// dropped, or treated as new source conflicts during an unrelated incoming fill.
TEST_CASE("private OMS planning validates complete retained pending interval sequences",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  auto before = fixture.create_detached_projection_or_throw();
  establish_hypothetical_execution_mapping(before, fixture);
  before.pending_fill_count = 2U;
  std::array pending{create_pending_execution_or_throw(fixture, 2, 3, 3U),
                     create_pending_execution_or_throw(fixture, 4, 5, 5U)};
  SECTION("not in cumulative endpoint order") { std::swap(pending[0], pending[1]); }
  SECTION("overlapping stored intervals") {
    pending[1] = create_pending_execution_or_throw(fixture, 2, 5, 5U);
  }
  SECTION("stored interval contiguous with applied quantity") {
    pending[0] = create_pending_execution_or_throw(fixture, 0, 1, 3U);
  }
  SECTION("stored interval exceeds original") {
    pending[1] = create_pending_execution_or_throw(fixture, 4, 7, 5U);
  }
  SECTION("stored interval exceeds terminal target") {
    before.authoritative_terminal_cumulative_quantity =
        test_support::create_m4_decimal_or_throw<model::Quantity>(4);
    before.cancellation_state = oms::CancellationState::Confirmed;
    before.reconciliation_required = true;
  }
  const auto saved = before;
  const auto result =
      fixture.derive_transition(before, fixture.normalize_execution_or_throw(1, 1), pending);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == model::DomainErrorCode::InvalidPrivateOmsState);
  CHECK(before == saved);
}

// --------------------------------------------------------
// Supported metadata and retained limit economics remain authoritative after normalization; a
// structurally valid execution with an inconsistent claim cannot become an economic proposal.
TEST_CASE("private OMS execution validation contains side metadata scale tick and limit defects",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture;
  const auto before = fixture.create_detached_projection_or_throw();
  auto instrument = fixture.order().provenance().instrument_id;
  auto revision = fixture.route().metadata().revision();
  auto incremental = test_support::create_m4_decimal_or_throw<model::Quantity>(1);
  auto cumulative = incremental;
  auto price = test_support::create_m4_decimal_or_throw<model::Price>(100);
  std::optional<execution::OrderSide> source_side;
  SECTION("supplied side differs from retained buy") { source_side = execution::OrderSide::Sell; }
  SECTION("raw instrument differs from retained admission") {
    instrument = test_support::parse_m4_identifier_or_throw<model::InstrumentId>("OTHER-PERPETUAL");
  }
  SECTION("claimed metadata revision differs") {
    revision = test_support::create_m4_ordinal_or_throw<model::InstrumentMetadataRevision>(2U);
  }
  SECTION("increment quantity has unsupported precision") {
    incremental = test_support::create_m4_decimal_or_throw<model::Quantity>(5, 1U);
  }
  SECTION("cumulative quantity has unsupported precision") {
    cumulative = test_support::create_m4_decimal_or_throw<model::Quantity>(15, 1U);
  }
  SECTION("price violates tick below the buy limit") {
    price = test_support::create_m4_decimal_or_throw<model::Price>(999, 1U);
  }
  SECTION("price exceeds the buy limit") {
    price = test_support::create_m4_decimal_or_throw<model::Price>(101);
  }
  const auto input = fixture.source.private_event_factory().normalize_venue_execution(
      fixture.source.create_venue_private_event_origin_or_throw(),
      fixture.create_local_exchange_locator_or_throw(),
      test_support::create_m4_opaque_identity_or_throw<oms::TradeId>(1U), instrument, revision,
      incremental, cumulative, price, source_side);
  REQUIRE(input);
  const auto result = fixture.derive_transition(before, input.value());
  REQUIRE(result);
  auto expected = before;
  expected.execution_evidence_observed = true;
  CHECK(result.value().classification == Classification::SafetyContained);
  CHECK(result.value().safety_reason == risk::AccountSafetyReason::AuthoritativeContradiction);
  CHECK(result.value().economics_action == Economics::None);
  CHECK_FALSE(result.value().reservation_closure_cause.has_value());
  CHECK(result.value().after == expected);
  CHECK(result.value().order_callback_count == 1U);
}

// --------------------------------------------------------
// Sell executions use the opposite limit inequality; a cheaper fill is an authoritative
// contradiction even though inverse contract-face notional does not depend on execution price.
TEST_CASE("private OMS execution validation enforces the retained sell limit",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture{6, execution::OrderSide::Sell};
  const auto before = fixture.create_detached_projection_or_throw();
  const auto result = fixture.derive_transition(
      before, fixture.normalize_execution_or_throw(1, 1, 1U, false, std::nullopt, 99));
  REQUIRE(result);
  CHECK(result.value().classification == Classification::SafetyContained);
  CHECK(result.value().economics_action == Economics::None);
  CHECK(result.value().after.cumulative_filled_quantity.coefficient() == 0);
}

// --------------------------------------------------------
// Every permutation of five unit trades is checked against an integer received-prefix oracle.
// Expected snapshots and retained intervals are advanced independently from the production plan.
TEST_CASE("private OMS fill plans converge across every five-trade arrival permutation",
          "[oms][private-oms-transition][private-oms-fill]") {
  const PrivateOmsTransitionFixture fixture{5};
  std::array<std::int64_t, 5U> permutation{1, 2, 3, 4, 5};
  std::size_t permutation_count = 0U;
  do {
    auto before = fixture.create_detached_projection_or_throw();
    std::array<bool, 6U> received{};
    std::vector<oms::PendingPrivateExecution> pending;
    std::int64_t applied = 0;
    std::uint32_t callback_count = 0U;
    for (const auto cumulative : permutation) {
      const auto previous_applied = applied;
      const auto input = fixture.normalize_execution_or_throw(
          1, cumulative, static_cast<std::uint8_t>(cumulative));
      received[static_cast<std::size_t>(cumulative)] = true;
      while (applied < 5 && received[static_cast<std::size_t>(applied + 1)]) {
        ++applied;
      }
      const auto newly_applied = static_cast<std::uint32_t>(applied - previous_applied);
      const bool buffered = cumulative > previous_applied + 1;
      CAPTURE(permutation_count, cumulative, previous_applied, applied);
      const auto result = fixture.derive_transition(before, input, pending);
      REQUIRE(result);
      const auto& plan = result.value();
      CHECK(plan.classification ==
            (buffered ? Classification::BufferedGap : Classification::Applied));
      CHECK(plan.economics_action == (buffered ? Economics::None : Economics::ApplyExecutions));
      CHECK(plan.order_callback_count == newly_applied);
      CHECK(plan.transition_effect_count == (buffered ? 1U : newly_applied));
      CHECK(plan.drained_pending_prefix_count == (buffered ? 0U : newly_applied - 1U));
      callback_count += newly_applied;

      // Build the expected next snapshot from integer set membership, without reading plan.after.
      if (buffered) {
        pending.push_back(oms::PendingPrivateExecution{input});
        std::sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
          return std::get<oms::ExecutionPayload>(left.execution.payload()).cumulative_quantity <
                 std::get<oms::ExecutionPayload>(right.execution.payload()).cumulative_quantity;
        });
      } else {
        std::erase_if(pending, [applied](const auto& row) {
          return std::get<oms::ExecutionPayload>(row.execution.payload())
                     .cumulative_quantity.coefficient() <= applied;
        });
      }
      establish_hypothetical_execution_mapping(before, fixture);
      before.cumulative_filled_quantity =
          test_support::create_m4_decimal_or_throw<model::Quantity>(applied);
      before.state = applied == 5   ? State::Filled
                     : applied == 0 ? State::WriteInitiated
                                    : State::PartiallyFilled;
      before.pending_fill_count = static_cast<std::uint32_t>(pending.size());
      CHECK(plan.after == before);
    }
    CHECK(applied == 5);
    CHECK(pending.empty());
    CHECK(callback_count == 5U);
    CHECK(fixture.order().state() == State::PendingEncoding);
    ++permutation_count;
  } while (std::next_permutation(permutation.begin(), permutation.end()));
  CHECK(permutation_count == 120U);
}

// --------------------------------------------------------

} // namespace
