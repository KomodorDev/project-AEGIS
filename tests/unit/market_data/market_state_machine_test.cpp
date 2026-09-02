// Purpose: prove transactional book mutation, deterministic continuity/readiness policy, exact
// trace fan-out preflight, and sanitized post-commit publication for one M2 market source.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/market_data/market_state_machine.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Strategy-facing views and mutable state owners cannot be forged or copied across owner domains.
static_assert(
    !std::is_constructible_v<market_data::ReadyBookView, std::span<const market_data::BookLevel>,
                             std::span<const market_data::BookLevel>>);
static_assert(!std::is_copy_constructible_v<market_data::ReadyBookView>);
static_assert(!std::is_copy_assignable_v<market_data::ReadyBookView>);
static_assert(std::is_move_constructible_v<market_data::ReadyBookView>);
static_assert(!std::is_copy_constructible_v<market_data::MarketTurnOutcome>);
static_assert(std::is_move_constructible_v<market_data::MarketTurnOutcome>);
static_assert(std::is_nothrow_move_constructible_v<market_data::MarketEvent>);
static_assert(std::is_nothrow_move_constructible_v<market_data::MarketStateEvent>);
static_assert(!std::is_copy_constructible_v<market_data::MarketStateMachine>);

// ########################################################################

// --------------------------------------------------------
// Treat invalid identifier literals as fixture-authoring defects rather than tested domain input.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse_identifier(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in market-state test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact nominal prices without introducing binary floating-point values into fixtures.
[[nodiscard]] model::Price parse_price_or_throw(std::string_view text) {
  auto parsed = model::Price::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid price in market-state test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact nominal quantities without introducing binary floating-point values into fixtures.
[[nodiscard]] model::Quantity parse_quantity_or_throw(std::string_view text) {
  auto parsed = model::Quantity::parse_ascii(text);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in market-state test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Construct checked one-based test positions and fail fast on fixture defects.
template <typename Ordinal> [[nodiscard]] Ordinal create_ordinal_or_throw(std::uint64_t value) {
  auto parsed = Ordinal::from_value(value);
  if (!parsed) {
    throw std::logic_error{"invalid ordinal in market-state test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Produce ordinary small runtime limits while allowing individual tests to tighten relevant bounds.
[[nodiscard]] runtime::RuntimePolicyLimits
create_runtime_limits(std::uint32_t depth = 3U, std::uint64_t stale_threshold = 100U,
                      std::uint32_t maximum_callbacks = 4U, std::uint32_t trace_capacity = 128U) {
  return runtime::RuntimePolicyLimits{
      8U,  4096U,          64U, depth,   stale_threshold, maximum_callbacks,
      32U, trace_capacity, 32U, 100'000U};
}

// --------------------------------------------------------
// Construct the sole source definition used by the reference startup configuration.
[[nodiscard]] runtime::RuntimeSourceDefinition
create_source_definition_or_throw(std::string_view source_id = "source.deribit-btc-perpetual") {
  return runtime::RuntimeSourceDefinition{
      parse_identifier_or_throw<model::MarketSourceId>(source_id),
      parse_identifier_or_throw<model::VenueId>("deribit"),
      parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
      parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
      model::InstrumentMetadataRevision::create_initial()};
}

// --------------------------------------------------------
// Produce a fixed opaque integrity identity with caller-selected deterministic verdict.
[[nodiscard]] market_data::MarketIntegrity create_market_integrity_or_throw(
    market_data::IntegrityVerdict verdict = market_data::IntegrityVerdict::Accepted,
    std::string_view token = "fixture-book-001") {
  auto parsed = market_data::IntegrityTokenIdentity::from_token(token);
  if (!parsed) {
    throw std::logic_error{"invalid integrity token in market-state test fixture"};
  }
  return market_data::MarketIntegrity{verdict, std::move(parsed).value()};
}

// ########################################################################
// Fixture owns configuration, policy, trace, and state machine in lifetime order, ensuring all
// source and metadata values originate from validated M1/M2 contracts.
class MarketStateMachineFixture final {
public:

  // --------------------------------------------------------
  // Build one isolated owner with caller-selected runtime bounds.
  explicit MarketStateMachineFixture(
      runtime::RuntimePolicyLimits selected_limits = create_runtime_limits())
      : configuration_{create_configuration_or_throw()}, limits_{selected_limits},
        policy_{create_policy_or_throw(configuration_, limits_)}, trace_{policy_},
        machine_{create_machine_or_throw(configuration_, policy_)} {}

  // --------------------------------------------------------
  // Publish initial Synchronizing using the ordinary first owner turn and return the production
  // result unchanged so each test can assert its intended outcome.
  [[nodiscard]] model::Result<market_data::MarketTurnOutcome> initialize_market_state_for_test() {
    return machine_.initialize_market_state(
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(1U),
                                            model::ProcessingTimestamp{100U}},
        trace_);
  }

  // --------------------------------------------------------
  // Normalize one complete update with exact caller-selected continuity and validation inputs.
  [[nodiscard]] market_data::NormalizedMarketUpdate create_normalized_update_or_throw(
      market_data::MarketUpdateKind kind, std::uint64_t session, std::uint64_t sequence,
      std::optional<std::uint64_t> predecessor, std::vector<market_data::MarketLevelChange> changes,
      std::uint64_t receive_sequence = 1U, std::uint64_t receive_time = 100U,
      model::InstrumentMetadataRevision metadata_revision =
          model::InstrumentMetadataRevision::create_initial(),
      market_data::MarketIntegrity update_integrity = create_market_integrity_or_throw(),
      std::uint64_t source_time = 1'000U) const {
    std::optional<model::SequenceNumber> predecessor_value;
    if (predecessor) {
      predecessor_value = model::SequenceNumber{*predecessor};
    }
    auto normalized = market_data::NormalizedMarketUpdate::create_normalized_market_update(
        market_data::NormalizedMarketUpdateFields{
            market_data::MarketSourceIdentity::from_runtime_source(policy_.sources().front()),
            model::SessionEpoch{session}, model::SequenceNumber{sequence}, predecessor_value,
            model::SourceTimestamp{source_time},
            create_ordinal_or_throw<model::ReceiveSequence>(receive_sequence),
            model::ReceiveTimestamp{receive_time}, metadata_revision, kind,
            std::move(update_integrity), std::move(changes)},
        limits_.maximum_changes_per_update);
    if (!normalized) {
      throw std::logic_error{"invalid normalized update in market-state test fixture"};
    }
    return std::move(normalized).value();
  }

  // --------------------------------------------------------
  // Apply one accepted update with explicit owner identities and return its production result
  // unchanged for the calling assertion.
  [[nodiscard]] model::Result<market_data::MarketTurnOutcome>
  apply_market_update_for_test(market_data::NormalizedMarketUpdate update, std::uint64_t admission,
                               std::uint64_t turn, std::uint64_t processing) {
    return machine_.apply_market_update(
        std::move(update),
        market_data::AcceptedMarketTurnContext{
            create_ordinal_or_throw<model::AdmissionOrdinal>(admission),
            create_ordinal_or_throw<model::TurnOrdinal>(turn),
            model::ProcessingTimestamp{processing}},
        trace_);
  }

  // --------------------------------------------------------
  // Return the canonical two-sided baseline snapshot used by transition and failure cases.
  [[nodiscard]] market_data::NormalizedMarketUpdate
  create_baseline_snapshot_or_throw(std::uint64_t receive_sequence = 1U) const {
    return create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Snapshot, 1U, 10U, std::nullopt,
        {{market_data::BookSide::Ask, parse_price_or_throw("102"), parse_quantity_or_throw("3")},
         {market_data::BookSide::Bid, parse_price_or_throw("99"), parse_quantity_or_throw("4")},
         {market_data::BookSide::Ask, parse_price_or_throw("101"), parse_quantity_or_throw("2")},
         {market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("1")}},
        receive_sequence);
  }

  // --------------------------------------------------------
  // Initialize and establish baseline readiness with no callback fan-out.
  void establish_ready_market_state_or_throw() {
    const auto initialized = initialize_market_state_for_test();
    REQUIRE(initialized);
    const auto ready =
        apply_market_update_for_test(create_baseline_snapshot_or_throw(), 1U, 2U, 200U);
    REQUIRE(ready);
    REQUIRE(ready.value().readiness() == market_data::MarketReadiness::Ready);
  }

  // --------------------------------------------------------
  // Borrow the mutable state owner for direct boundary and counter-failure assertions.
  [[nodiscard]] market_data::MarketStateMachine& market_state_machine() noexcept {
    return machine_;
  }

  // --------------------------------------------------------
  // Borrow the canonical trace sink owned for the fixture's complete lifetime.
  [[nodiscard]] trace::RuntimeTraceSink& runtime_trace_sink() noexcept { return trace_; }

  // --------------------------------------------------------
  // Borrow the sealed policy that authorizes every fixture source and capacity.
  [[nodiscard]] const runtime::RuntimePolicy& runtime_policy() const noexcept { return policy_; }

  // --------------------------------------------------------
  // Borrow the sealed startup authority from which fixture metadata was copied.
  [[nodiscard]] const configuration::StartupConfiguration& startup_configuration() const noexcept {
    return configuration_;
  }

  // --------------------------------------------------------
  // Borrow the exact capacity and timing limits selected at fixture construction.
  [[nodiscard]] const runtime::RuntimePolicyLimits& runtime_limits() const noexcept {
    return limits_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  // Seal the shared M1 reference input before any runtime policy is constructed.
  [[nodiscard]] static configuration::StartupConfiguration create_configuration_or_throw() {
    auto created = configuration::StartupConfiguration::create_startup_configuration(
        test_support::create_reference_configuration_params_or_throw());
    if (!created) {
      throw std::logic_error{"invalid startup configuration in market-state fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Bind one canonical source and the caller-selected limits to the sealed configuration.
  [[nodiscard]] static runtime::RuntimePolicy
  create_policy_or_throw(const configuration::StartupConfiguration& configuration,
                         runtime::RuntimePolicyLimits selected_limits) {
    auto created = runtime::RuntimePolicy::create_runtime_policy(
        configuration,
        runtime::RuntimePolicyParams{selected_limits, {create_source_definition_or_throw()}});
    if (!created) {
      throw std::logic_error{"invalid runtime policy in market-state fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  // Copy exact source metadata into the owner only after source-policy validation has succeeded.
  [[nodiscard]] static market_data::MarketStateMachine
  create_machine_or_throw(const configuration::StartupConfiguration& configuration,
                          const runtime::RuntimePolicy& policy) {
    const auto& source = policy.sources().front();
    const auto& definition = source.definition();
    const auto* metadata =
        configuration.find_instrument_metadata(definition.venue_id, definition.instrument_id);
    if (metadata == nullptr) {
      throw std::logic_error{"missing metadata in market-state fixture"};
    }
    auto created =
        market_data::MarketStateMachine::create_market_state_machine(policy, source, *metadata);
    if (!created) {
      throw std::logic_error{"invalid state machine in market-state fixture"};
    }
    return std::move(created).value();
  }

  // --------------------------------------------------------
  configuration::StartupConfiguration configuration_;
  runtime::RuntimePolicyLimits limits_;
  runtime::RuntimePolicy policy_;
  trace::RuntimeTraceSink trace_;
  market_data::MarketStateMachine machine_;
};

// ########################################################################
// Capture the exact coordinator-facing shape and optionally reject it to prove the transactional
// placement of the callback-fanout seam.
class RecordingMarketTurnPreflightAuthority final
    : public market_data::MarketTurnPreflightAuthority {
public:

  // --------------------------------------------------------
  // Record one complete request and return the caller-selected deterministic verdict.
  [[nodiscard]] model::Result<void>
  authorize_market_turn(const market_data::MarketTurnPreflight& preflight) override {
    ++call_count_;
    last_preflight_ = preflight;
    if (reject_) {
      return model::Result<void>::create_failure(model::DomainError::create_at_field(
          model::DomainErrorCode::DispatchCapacityExceeded, "test.preflight"));
    }
    return model::Result<void>::create_success();
  }

  // --------------------------------------------------------
  // Select rejection without changing the fixed failure identity asserted by the test.
  void select_rejection(bool selected = true) noexcept { reject_ = selected; }

  // --------------------------------------------------------
  // Return the number of coordinator preflight requests observed by this seam.
  [[nodiscard]] std::uint32_t authorization_call_count() const noexcept { return call_count_; }

  // --------------------------------------------------------
  // Borrow the most recent complete preflight request, if authorization was attempted.
  [[nodiscard]] const std::optional<market_data::MarketTurnPreflight>&
  last_preflight_request() const noexcept {
    return last_preflight_;
  }

  // --------------------------------------------------------
private:

  // --------------------------------------------------------
  bool reject_{false};
  std::uint32_t call_count_{0U};
  std::optional<market_data::MarketTurnPreflight> last_preflight_;
};

// ########################################################################

// --------------------------------------------------------
// Verify an outcome carries one exact assigned disposition.
void require_disposition(const market_data::MarketTurnOutcome& outcome,
                         trace::RuntimeInputDisposition expected) {
  REQUIRE(outcome.disposition().has_value());
  CHECK(*outcome.disposition() == expected);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Initialization, snapshot, delta, and later snapshot prove explicit state, canonical view order,
// absolute deletion, and globally monotonic generation/revision rules.
TEST_CASE("market state commits canonical books and exact fanout",
          "[market_data][market_state][book]") {
  MarketStateMachineFixture fixture;

  // ++++++++++++++++++++++++++++++++++++++++
  // Before the initial state event, no readiness or book view can be observed.
  CHECK_FALSE(fixture.market_state_machine().readiness().has_value());
  const auto unavailable = fixture.market_state_machine().create_ready_book_view();
  REQUIRE_FALSE(unavailable);
  CHECK(unavailable.error().code == model::DomainErrorCode::MarketNotReady);

  // ++++++++++++++++++++++++++++++++++++++++
  // Initialization reserves one callback and its possible first re-entry record for one grant.
  const auto initialized = fixture.initialize_market_state_for_test();
  REQUIRE(initialized);
  CHECK(initialized.value().source_ordinal() ==
        fixture.runtime_policy().sources().front().ordinal());
  CHECK(initialized.value().turn_ordinal() == create_ordinal_or_throw<model::TurnOrdinal>(1U));
  CHECK(initialized.value().readiness() == market_data::MarketReadiness::Synchronizing);
  CHECK(initialized.value().state_event().has_value());
  CHECK(initialized.value().expected_callback_count() == 1U);
  CHECK(initialized.value().reserved_callback_trace_records() == 2U);
  CHECK(fixture.runtime_trace_sink().record_count() == 1U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A recovery snapshot emits state then market semantics and exposes best-to-worst side order.
  const auto snapshot = fixture.apply_market_update_for_test(
      fixture.create_baseline_snapshot_or_throw(), 1U, 2U, 200U);
  REQUIRE(snapshot);
  require_disposition(snapshot.value(), trace::RuntimeInputDisposition::SnapshotApplied);
  CHECK(snapshot.value().state_event().has_value());
  CHECK(snapshot.value().market_event().has_value());
  CHECK(snapshot.value().ready_book().has_value());
  CHECK(snapshot.value().is_book_committed());
  CHECK(snapshot.value().expected_callback_count() == 2U);
  CHECK(snapshot.value().reserved_callback_trace_records() == 4U);
  const auto first_identity = fixture.market_state_machine().book_identity();
  REQUIRE(first_identity);
  CHECK(first_identity->generation() == model::BookGeneration::create_initial());
  CHECK(first_identity->revision() == model::BookRevision::create_initial());
  const auto& first_view = snapshot.value().ready_book().value();
  REQUIRE(first_view.bid_count() == 2U);
  REQUIRE(first_view.ask_count() == 2U);
  REQUIRE(first_view.bid_at(0U));
  REQUIRE(first_view.bid_at(1U));
  REQUIRE(first_view.ask_at(0U));
  REQUIRE(first_view.ask_at(1U));
  CHECK(first_view.bid_at(0U)->price == parse_price_or_throw("100"));
  CHECK(first_view.bid_at(1U)->price == parse_price_or_throw("99"));
  CHECK(first_view.ask_at(0U)->price == parse_price_or_throw("101"));
  CHECK(first_view.ask_at(1U)->price == parse_price_or_throw("102"));
  CHECK_FALSE(first_view.bid_at(2U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Delta quantities are absolute, zero deletes, and only revision advances.
  auto delta = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("5")},
       {market_data::BookSide::Ask, parse_price_or_throw("101"), parse_quantity_or_throw("0")},
       {market_data::BookSide::Ask, parse_price_or_throw("101.5"), parse_quantity_or_throw("7")}},
      2U);
  const auto applied_delta = fixture.apply_market_update_for_test(std::move(delta), 2U, 3U, 250U);
  REQUIRE(applied_delta);
  require_disposition(applied_delta.value(), trace::RuntimeInputDisposition::DeltaApplied);
  CHECK_FALSE(applied_delta.value().state_event().has_value());
  CHECK(applied_delta.value().expected_callback_count() == 1U);
  const auto delta_identity = fixture.market_state_machine().book_identity();
  REQUIRE(delta_identity);
  CHECK(delta_identity->generation().value() == 1U);
  CHECK(delta_identity->revision().value() == 2U);
  const auto& delta_view = applied_delta.value().ready_book().value();
  REQUIRE(delta_view.ask_count() == 2U);
  REQUIRE(delta_view.ask_at(0U));
  REQUIRE(delta_view.bid_at(0U));
  CHECK(delta_view.ask_at(0U)->price == parse_price_or_throw("101.5"));
  CHECK(delta_view.bid_at(0U)->quantity == parse_quantity_or_throw("5"));

  // ++++++++++++++++++++++++++++++++++++++++
  // A later authoritative one-sided snapshot removes every omitted old level and advances both
  // generation and global revision.
  auto replacement = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Snapshot, 1U, 12U, std::nullopt,
      {{market_data::BookSide::Ask, parse_price_or_throw("110"), parse_quantity_or_throw("9")}},
      3U);
  const auto replaced = fixture.apply_market_update_for_test(std::move(replacement), 3U, 4U, 300U);
  REQUIRE(replaced);
  const auto replacement_identity = fixture.market_state_machine().book_identity();
  REQUIRE(replacement_identity);
  CHECK(replacement_identity->generation().value() == 2U);
  CHECK(replacement_identity->revision().value() == 3U);
  REQUIRE(replaced.value().ready_book());
  CHECK(replaced.value().ready_book()->bid_count() == 0U);
  REQUIRE(replaced.value().ready_book()->best_ask());
  CHECK(*replaced.value().ready_book()->best_ask() == parse_price_or_throw("110"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Full-depth replacement deltas delete across both sides before canonical-price upserts, so
// authored order and side grouping cannot create a transient capacity failure.
TEST_CASE("full-depth delta replacement is order and side independent",
          "[market_data][market_state][book][regression]") {
  MarketStateMachineFixture fixture{create_runtime_limits(3U)};
  REQUIRE(fixture.initialize_market_state_for_test());

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish both sides exactly at retained depth before presenting simultaneous replacements.
  auto full_book = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Snapshot, 1U, 10U, std::nullopt,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("1")},
       {market_data::BookSide::Bid, parse_price_or_throw("99"), parse_quantity_or_throw("1")},
       {market_data::BookSide::Bid, parse_price_or_throw("98"), parse_quantity_or_throw("1")},
       {market_data::BookSide::Ask, parse_price_or_throw("101"), parse_quantity_or_throw("1")},
       {market_data::BookSide::Ask, parse_price_or_throw("102"), parse_quantity_or_throw("1")},
       {market_data::BookSide::Ask, parse_price_or_throw("103"), parse_quantity_or_throw("1")}});
  REQUIRE(fixture.apply_market_update_for_test(std::move(full_book), 1U, 2U, 200U));

  // ++++++++++++++++++++++++++++++++++++++++
  // Canonical ascending prices put each insertion before the deletion that makes capacity.
  auto replacements = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
      {{market_data::BookSide::Ask, parse_price_or_throw("103"), parse_quantity_or_throw("0")},
       {market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("0")},
       {market_data::BookSide::Ask, parse_price_or_throw("100.5"), parse_quantity_or_throw("2")},
       {market_data::BookSide::Bid, parse_price_or_throw("97"), parse_quantity_or_throw("2")}},
      2U);
  REQUIRE(replacements.changes().size() == 4U);
  CHECK(replacements.changes()[0].price == parse_price_or_throw("97"));
  CHECK(replacements.changes()[1].price == parse_price_or_throw("100"));
  CHECK(replacements.changes()[2].price == parse_price_or_throw("100.5"));
  CHECK(replacements.changes()[3].price == parse_price_or_throw("103"));

  const auto replaced = fixture.apply_market_update_for_test(std::move(replacements), 2U, 3U, 300U);
  REQUIRE(replaced);
  require_disposition(replaced.value(), trace::RuntimeInputDisposition::DeltaApplied);
  REQUIRE(replaced.value().ready_book());
  const auto& view = replaced.value().ready_book().value();
  REQUIRE(view.bid_count() == 3U);
  REQUIRE(view.ask_count() == 3U);
  REQUIRE(view.bid_at(0U));
  REQUIRE(view.bid_at(1U));
  REQUIRE(view.bid_at(2U));
  REQUIRE(view.ask_at(0U));
  REQUIRE(view.ask_at(1U));
  REQUIRE(view.ask_at(2U));
  CHECK(view.bid_at(0U)->price == parse_price_or_throw("99"));
  CHECK(view.bid_at(1U)->price == parse_price_or_throw("98"));
  CHECK(view.bid_at(2U)->price == parse_price_or_throw("97"));
  CHECK(view.ask_at(0U)->price == parse_price_or_throw("100.5"));
  CHECK(view.ask_at(1U)->price == parse_price_or_throw("101"));
  CHECK(view.ask_at(2U)->price == parse_price_or_throw("102"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Session, sequence, predecessor, and digest classification occurs before metadata or integrity and
// never mutates the book for ignored or rejected updates.
TEST_CASE("market continuity policies are deterministic",
          "[market_data][market_state][continuity]") {
  MarketStateMachineFixture fixture;
  fixture.establish_ready_market_state_or_throw();
  const auto original_identity = fixture.market_state_machine().book_identity();
  REQUIRE(original_identity);

  SECTION("exact duplicate excludes receive identity") {
    auto duplicate = fixture.create_baseline_snapshot_or_throw(9U);
    const auto outcome = fixture.apply_market_update_for_test(std::move(duplicate), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::ExactDuplicateIgnored);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Ready);
    CHECK_FALSE(outcome.value().state_event());
    CHECK_FALSE(outcome.value().market_event());
    CHECK(fixture.market_state_machine().book_identity() == original_identity);
  }

  SECTION("same sequence with different semantic digest invalidates") {
    auto conflict = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Snapshot, 1U, 10U, std::nullopt,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("99")}},
        2U);
    const auto outcome = fixture.apply_market_update_for_test(std::move(conflict), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::SequenceConflictRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK(outcome.value().state_event());
    CHECK_FALSE(outcome.value().market_event());
    CHECK(fixture.market_state_machine().book_identity() == original_identity);

    // A later delta remains non-ready even with the continuity predecessor that preceded conflict.
    auto delta = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
        3U);
    const auto nonready = fixture.apply_market_update_for_test(std::move(delta), 3U, 4U, 400U);
    REQUIRE(nonready);
    require_disposition(nonready.value(), trace::RuntimeInputDisposition::NonReadyDeltaRejected);
    CHECK(nonready.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK_FALSE(nonready.value().state_event());
  }

  SECTION("older sequence precedes bad metadata and checksum") {
    auto older = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 9U, 8U,
        {{market_data::BookSide::Bid, parse_price_or_throw("200"), parse_quantity_or_throw("1")}},
        2U, 100U, model::InstrumentMetadataRevision::from_value(2U).value(),
        create_market_integrity_or_throw(market_data::IntegrityVerdict::Rejected));
    const auto outcome = fixture.apply_market_update_for_test(std::move(older), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::OlderInputIgnored);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Ready);
    CHECK_FALSE(outcome.value().state_event());
    CHECK(fixture.market_state_machine().book_identity() == original_identity);
  }

  SECTION("wrong explicit predecessor invalidates a newer ready delta") {
    auto gap = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 12U, 9U,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
        2U);
    const auto outcome = fixture.apply_market_update_for_test(std::move(gap), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::GapRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK(fixture.market_state_machine().book_identity() == original_identity);
  }

  SECTION("newer-session delta resets to synchronizing without continuity") {
    auto delta = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 2U, 1U, std::nullopt,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
        2U);
    const auto outcome = fixture.apply_market_update_for_test(std::move(delta), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::NonReadyDeltaRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Synchronizing);
    CHECK(outcome.value().state_event());
    REQUIRE(fixture.market_state_machine().active_session());
    CHECK(fixture.market_state_machine().active_session()->value() == 2U);
    CHECK_FALSE(fixture.market_state_machine().last_source_sequence());
    CHECK(fixture.market_state_machine().book_identity() == original_identity);

    // Another delta in that active session remains Synchronizing and cannot create continuity.
    auto second = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 2U, 2U, 1U,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("3")}},
        3U);
    const auto still_sync = fixture.apply_market_update_for_test(std::move(second), 3U, 4U, 400U);
    REQUIRE(still_sync);
    require_disposition(still_sync.value(), trace::RuntimeInputDisposition::NonReadyDeltaRejected);
    CHECK(still_sync.value().readiness() == market_data::MarketReadiness::Synchronizing);
    CHECK_FALSE(still_sync.value().state_event());
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Every structural candidate failure keeps the prior revision, never publishes a market event, and
// can recover only through a strictly newer full snapshot.
TEST_CASE("book validation rejects complete candidates without partial commit",
          "[market_data][market_state][transaction]") {
  MarketStateMachineFixture fixture;
  fixture.establish_ready_market_state_or_throw();
  const auto identity_before = fixture.market_state_machine().book_identity();
  REQUIRE(identity_before);
  std::optional<market_data::NormalizedMarketUpdate> invalid;

  SECTION("crossed final book") {
    invalid = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("102"), parse_quantity_or_throw("2")}},
        2U);
  }
  SECTION("locked final book") {
    invalid = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("101"), parse_quantity_or_throw("2")}},
        2U);
  }
  SECTION("misaligned price") {
    invalid = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("99.25"), parse_quantity_or_throw("2")}},
        2U);
  }
  SECTION("misaligned quantity") {
    invalid = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("99.5"),
          parse_quantity_or_throw("1.5")}},
        2U);
  }
  SECTION("depth exceeds after an earlier scratch insertion") {
    invalid = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("98"), parse_quantity_or_throw("1")},
         {market_data::BookSide::Bid, parse_price_or_throw("97"), parse_quantity_or_throw("1")}},
        2U);
  }
  REQUIRE(invalid);

  // ++++++++++++++++++++++++++++++++++++++++
  // The whole candidate rejects, preserving old counter identity and suppressing a Ready view.
  const auto rejected = fixture.apply_market_update_for_test(std::move(*invalid), 2U, 3U, 300U);
  REQUIRE(rejected);
  require_disposition(rejected.value(), trace::RuntimeInputDisposition::StructuralBookRejected);
  CHECK(rejected.value().readiness() == market_data::MarketReadiness::Invalid);
  CHECK_FALSE(rejected.value().market_event());
  CHECK_FALSE(rejected.value().is_book_committed());
  CHECK(fixture.market_state_machine().book_identity() == identity_before);
  CHECK_FALSE(fixture.market_state_machine().create_ready_book_view());

  // ++++++++++++++++++++++++++++++++++++++++
  // A newer authoritative one-sided snapshot contains no partially applied scratch levels.
  auto recovery = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Snapshot, 1U, 12U, std::nullopt,
      {{market_data::BookSide::Ask, parse_price_or_throw("120"), parse_quantity_or_throw("8")}},
      3U);
  const auto recovered = fixture.apply_market_update_for_test(std::move(recovery), 3U, 4U, 400U);
  REQUIRE(recovered);
  REQUIRE(recovered.value().ready_book());
  CHECK(recovered.value().ready_book()->bid_count() == 0U);
  REQUIRE(recovered.value().ready_book()->ask_count() == 1U);
  REQUIRE(recovered.value().ready_book()->ask_at(0U));
  CHECK(recovered.value().ready_book()->ask_at(0U)->price == parse_price_or_throw("120"));
  REQUIRE(fixture.market_state_machine().book_identity());
  CHECK(fixture.market_state_machine().book_identity()->generation().value() == 2U);
  CHECK(fixture.market_state_machine().book_identity()->revision().value() == 2U);
}

// --------------------------------------------------------

// --------------------------------------------------------
// Metadata and integrity decisions have stable precedence over structural checks and leave the
// committed identity untouched when they invalidate the active stream.
TEST_CASE("metadata and integrity failures are deterministic",
          "[market_data][market_state][integrity]") {
  MarketStateMachineFixture fixture;
  fixture.establish_ready_market_state_or_throw();
  const auto identity_before = fixture.market_state_machine().book_identity();

  SECTION("metadata revision mismatch") {
    auto mismatched = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
        2U, 100U, model::InstrumentMetadataRevision::from_value(2U).value());
    const auto outcome = fixture.apply_market_update_for_test(std::move(mismatched), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::MetadataRevisionRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK(fixture.market_state_machine().book_identity() == identity_before);
  }

  SECTION("rejected integrity wins even when changes would cross") {
    auto checksum = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
        {{market_data::BookSide::Bid, parse_price_or_throw("200"), parse_quantity_or_throw("2")}},
        2U, 100U, model::InstrumentMetadataRevision::create_initial(),
        create_market_integrity_or_throw(market_data::IntegrityVerdict::Rejected));
    const auto outcome = fixture.apply_market_update_for_test(std::move(checksum), 2U, 3U, 300U);
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::ChecksumRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK(fixture.market_state_machine().book_identity() == identity_before);
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Explicit staleness changes at equality, never refreshes on ignored input, and requires snapshot
// recovery from Stale.
TEST_CASE("staleness transitions are explicit and snapshot-only recoverable",
          "[market_data][market_state][stale]") {
  MarketStateMachineFixture fixture{create_runtime_limits(3U, 100U)};
  fixture.establish_ready_market_state_or_throw();
  const auto source = market_data::MarketSourceIdentity::from_runtime_source(
      fixture.runtime_policy().sources().front());

  // ++++++++++++++++++++++++++++++++++++++++
  // One nanosecond before the deadline remains Ready without a state callback.
  const auto before = fixture.market_state_machine().apply_staleness_check(
      market_data::StalenessCheck{source, model::SessionEpoch{1U},
                                  create_ordinal_or_throw<model::ReceiveSequence>(2U),
                                  model::ReceiveTimestamp{210U}, model::ProcessingTimestamp{299U}},
      market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                                             create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                             model::ProcessingTimestamp{299U}},
      fixture.runtime_trace_sink());
  REQUIRE(before);
  require_disposition(before.value(), trace::RuntimeInputDisposition::StalenessChecked);
  CHECK(before.value().readiness() == market_data::MarketReadiness::Ready);
  CHECK_FALSE(before.value().state_event());

  // ++++++++++++++++++++++++++++++++++++++++
  // Equality with the sealed threshold changes Ready to Stale and hides the retained book.
  const auto at_deadline = fixture.market_state_machine().apply_staleness_check(
      market_data::StalenessCheck{source, model::SessionEpoch{1U},
                                  create_ordinal_or_throw<model::ReceiveSequence>(3U),
                                  model::ReceiveTimestamp{220U}, model::ProcessingTimestamp{300U}},
      market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(3U),
                                             create_ordinal_or_throw<model::TurnOrdinal>(4U),
                                             model::ProcessingTimestamp{300U}},
      fixture.runtime_trace_sink());
  REQUIRE(at_deadline);
  CHECK(at_deadline.value().readiness() == market_data::MarketReadiness::Stale);
  CHECK(at_deadline.value().state_event());
  CHECK_FALSE(fixture.market_state_machine().create_ready_book_view());

  // ++++++++++++++++++++++++++++++++++++++++
  // A continuity-correct delta while Stale remains rejected without changing the old revision.
  const auto stale_identity = fixture.market_state_machine().book_identity();
  auto delta = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
      4U);
  const auto ignored_delta = fixture.apply_market_update_for_test(std::move(delta), 4U, 5U, 400U);
  REQUIRE(ignored_delta);
  require_disposition(ignored_delta.value(), trace::RuntimeInputDisposition::NonReadyDeltaRejected);
  CHECK(ignored_delta.value().readiness() == market_data::MarketReadiness::Stale);
  CHECK(fixture.market_state_machine().book_identity() == stale_identity);

  // ++++++++++++++++++++++++++++++++++++++++
  // A strictly newer snapshot alone starts another generation and restores Ready.
  auto recovery = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Snapshot, 1U, 12U, std::nullopt,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("2")}},
      5U);
  const auto recovered = fixture.apply_market_update_for_test(std::move(recovery), 5U, 6U, 500U);
  REQUIRE(recovered);
  CHECK(recovered.value().readiness() == market_data::MarketReadiness::Ready);
  CHECK(recovered.value().state_event());
  CHECK(recovered.value().market_event());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Session, resynchronization, discontinuity, and malformed controls cover non-book transition
// profiles while retaining hidden book identity after a prior commit.
TEST_CASE("market controls enforce the four-state transition table",
          "[market_data][market_state][controls]") {
  MarketStateMachineFixture fixture;
  fixture.establish_ready_market_state_or_throw();
  const auto source = market_data::MarketSourceIdentity::from_runtime_source(
      fixture.runtime_policy().sources().front());
  const auto committed_identity = fixture.market_state_machine().book_identity();

  SECTION("same session-start control is ignored") {
    const auto outcome = fixture.market_state_machine().apply_session_start(
        market_data::SessionStarted{source, model::SessionEpoch{1U}, model::SourceTimestamp{2'000U},
                                    create_ordinal_or_throw<model::ReceiveSequence>(2U),
                                    model::ReceiveTimestamp{200U}},
        market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                                               create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                               model::ProcessingTimestamp{300U}},
        fixture.runtime_trace_sink());
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::SessionIgnored);
    CHECK(outcome.value().source_ordinal() == fixture.runtime_policy().sources().front().ordinal());
    CHECK(outcome.value().turn_ordinal() == create_ordinal_or_throw<model::TurnOrdinal>(3U));
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Ready);
    CHECK_FALSE(outcome.value().state_event());
  }

  SECTION("newer session resets continuity and requires snapshot") {
    const auto outcome = fixture.market_state_machine().apply_session_start(
        market_data::SessionStarted{source, model::SessionEpoch{2U}, model::SourceTimestamp{2'000U},
                                    create_ordinal_or_throw<model::ReceiveSequence>(2U),
                                    model::ReceiveTimestamp{200U}},
        market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                                               create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                               model::ProcessingTimestamp{300U}},
        fixture.runtime_trace_sink());
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::SessionReset);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Synchronizing);
    CHECK(outcome.value().state_event());
    CHECK_FALSE(fixture.market_state_machine().last_source_sequence());
    CHECK(fixture.market_state_machine().book_identity() == committed_identity);
  }

  SECTION("explicit resynchronization clears session without clearing book counters") {
    const auto outcome = fixture.market_state_machine().resynchronize_source(
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                            model::ProcessingTimestamp{300U}},
        fixture.runtime_trace_sink());
    REQUIRE(outcome);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Synchronizing);
    CHECK(outcome.value().state_event());
    CHECK_FALSE(outcome.value().disposition());
    CHECK_FALSE(fixture.market_state_machine().active_session());
    CHECK(fixture.market_state_machine().book_identity() == committed_identity);

    // Repeating explicit resynchronization remains an owner-context state event and exact fan-out.
    const auto prefix = fixture.runtime_trace_sink().record_count();
    const auto repeated = fixture.market_state_machine().resynchronize_source(
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(4U),
                                            model::ProcessingTimestamp{400U}},
        fixture.runtime_trace_sink());
    REQUIRE(repeated);
    REQUIRE(repeated.value().state_event());
    REQUIRE(repeated.value().state_event()->fields().previous_readiness);
    CHECK(*repeated.value().state_event()->fields().previous_readiness ==
          market_data::MarketReadiness::Synchronizing);
    CHECK(repeated.value().state_event()->fields().readiness ==
          market_data::MarketReadiness::Synchronizing);
    CHECK(repeated.value().expected_callback_count() == 1U);
    CHECK(repeated.value().reserved_callback_trace_records() == 2U);
    CHECK(fixture.runtime_trace_sink().record_count() == prefix + 1U);
  }

  SECTION("source discontinuity invalidates once and remains observable") {
    const auto first = fixture.market_state_machine().apply_source_discontinuity(
        create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                            model::ProcessingTimestamp{300U}},
        fixture.runtime_trace_sink());
    REQUIRE(first);
    require_disposition(first.value(), trace::RuntimeInputDisposition::SourceDiscontinuity);
    CHECK(first.value().readiness() == market_data::MarketReadiness::Invalid);
    CHECK(first.value().state_event());
    const auto second = fixture.market_state_machine().apply_source_discontinuity(
        create_ordinal_or_throw<model::AdmissionOrdinal>(3U),
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(4U),
                                            model::ProcessingTimestamp{400U}},
        fixture.runtime_trace_sink());
    REQUIRE(second);
    CHECK_FALSE(second.value().state_event());
    CHECK(fixture.market_state_machine().book_identity() == committed_identity);
  }

  SECTION("active malformed input exposes only a sanitized transition") {
    const auto outcome = fixture.market_state_machine().apply_attributable_failure(
        market_data::AttributableMarketFailure{
            source, model::SessionEpoch{1U}, create_ordinal_or_throw<model::ReceiveSequence>(2U),
            model::ReceiveTimestamp{200U}, trace::RuntimeInputDisposition::MalformedRejected},
        market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                                               create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                               model::ProcessingTimestamp{300U}},
        fixture.runtime_trace_sink());
    REQUIRE(outcome);
    require_disposition(outcome.value(), trace::RuntimeInputDisposition::MalformedRejected);
    CHECK(outcome.value().readiness() == market_data::MarketReadiness::Invalid);
    REQUIRE(outcome.value().state_event());
    CHECK_FALSE(outcome.value().market_event());
    CHECK_FALSE(outcome.value().ready_book());

    // A later valid full snapshot recovers without retaining malformed bytes or partial output.
    auto recovery = fixture.create_normalized_update_or_throw(
        market_data::MarketUpdateKind::Snapshot, 1U, 12U, std::nullopt,
        {{market_data::BookSide::Ask, parse_price_or_throw("105"), parse_quantity_or_throw("2")}},
        3U);
    const auto recovered = fixture.apply_market_update_for_test(std::move(recovery), 3U, 4U, 400U);
    REQUIRE(recovered);
    CHECK(recovered.value().readiness() == market_data::MarketReadiness::Ready);
    CHECK(recovered.value().market_event());
    REQUIRE(recovered.value().ready_book());
    CHECK(recovered.value().ready_book()->bid_count() == 0U);
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Exact callback/re-entry preflight fails before state or book mutation when either policy fan-out
// or the remaining canonical trace prefix cannot represent the complete turn.
TEST_CASE("trace and callback capacity fail closed before book commit",
          "[market_data][market_state][trace]") {
  SECTION("trace capacity preserves initialization prefix") {
    MarketStateMachineFixture fixture{create_runtime_limits(3U, 100U, 2U, 6U)};
    const auto initialized = fixture.initialize_market_state_for_test();
    REQUIRE(initialized);
    REQUIRE(fixture.runtime_trace_sink().record_count() == 1U);
    auto snapshot = fixture.create_baseline_snapshot_or_throw();
    const auto failed = fixture.apply_market_update_for_test(std::move(snapshot), 1U, 2U, 200U);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == model::DomainErrorCode::TraceCapacityExceeded);
    REQUIRE(fixture.market_state_machine().readiness());
    CHECK(*fixture.market_state_machine().readiness() ==
          market_data::MarketReadiness::Synchronizing);
    CHECK_FALSE(fixture.market_state_machine().book_identity());
    CHECK(fixture.runtime_trace_sink().record_count() == 1U);
  }

  SECTION("callback fanout is derived from the configured source") {
    MarketStateMachineFixture fixture{create_runtime_limits(3U, 100U, 2U, 64U)};
    const auto initialized = fixture.initialize_market_state_for_test();
    REQUIRE(initialized);
    CHECK(initialized.value().expected_callback_count() == 1U);
    CHECK(fixture.runtime_policy().sources().front().matching_subscription_count() == 1U);
    auto snapshot = fixture.create_baseline_snapshot_or_throw();
    const auto ready = fixture.apply_market_update_for_test(std::move(snapshot), 1U, 2U, 200U);
    REQUIRE(ready);
    CHECK(ready.value().expected_callback_count() == 2U);
    CHECK(ready.value().reserved_callback_trace_records() == 4U);
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// The coordinator seam sees the exact post-classification shape, and its failure cannot alter the
// published book, continuity, readiness, or canonical trace prefix.
TEST_CASE("exact callback preflight failure is transactional",
          "[market_data][market_state][preflight]") {
  MarketStateMachineFixture fixture;
  fixture.establish_ready_market_state_or_throw();
  REQUIRE(fixture.market_state_machine().readiness() == market_data::MarketReadiness::Ready);
  const auto identity_before = fixture.market_state_machine().book_identity();
  const auto session_before = fixture.market_state_machine().active_session();
  const auto sequence_before = fixture.market_state_machine().last_source_sequence();
  const auto trace_before = fixture.runtime_trace_sink().record_count();
  const auto book_before = fixture.market_state_machine().create_ready_book_view();
  REQUIRE(book_before);
  REQUIRE(book_before.value().best_bid());
  const auto best_bid_before = *book_before.value().best_bid();
  REQUIRE(book_before.value().bid_at(0U));
  const auto quantity_before = book_before.value().bid_at(0U)->quantity;

  RecordingMarketTurnPreflightAuthority authority;
  authority.select_rejection();
  auto rejected_delta = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("9")}},
      2U);
  const auto rejected = fixture.market_state_machine().apply_market_update(
      std::move(rejected_delta),
      market_data::AcceptedMarketTurnContext{create_ordinal_or_throw<model::AdmissionOrdinal>(2U),
                                             create_ordinal_or_throw<model::TurnOrdinal>(3U),
                                             model::ProcessingTimestamp{300U}},
      fixture.runtime_trace_sink(), authority);
  REQUIRE_FALSE(rejected);
  CHECK(authority.authorization_call_count() == 1U);
  CHECK(rejected.error() ==
        model::DomainError::create_at_field(model::DomainErrorCode::DispatchCapacityExceeded,
                                            "test.preflight"));
  REQUIRE(authority.last_preflight_request());
  CHECK((*authority.last_preflight_request() ==
         market_data::MarketTurnPreflight{fixture.runtime_policy().sources().front().ordinal(),
                                          create_ordinal_or_throw<model::TurnOrdinal>(3U), 1U, 1U,
                                          1U, 1U, 2U, 3U}));

  // ++++++++++++++++++++++++++++++++++++++++
  // Every externally visible owner field and trace byte prefix remains at its predecessor.
  CHECK(fixture.market_state_machine().readiness() == market_data::MarketReadiness::Ready);
  CHECK(fixture.market_state_machine().book_identity() == identity_before);
  CHECK(fixture.market_state_machine().active_session() == session_before);
  CHECK(fixture.market_state_machine().last_source_sequence() == sequence_before);
  CHECK(fixture.runtime_trace_sink().record_count() == trace_before);
  const auto book_after = fixture.market_state_machine().create_ready_book_view();
  REQUIRE(book_after);
  REQUIRE(book_after.value().best_bid());
  REQUIRE(book_after.value().bid_at(0U));
  CHECK(*book_after.value().best_bid() == best_bid_before);
  CHECK(book_after.value().bid_at(0U)->quantity == quantity_before);

  // ++++++++++++++++++++++++++++++++++++++++
  // The same valid delta succeeds afterward, proving rejected scratch work cannot poison recovery.
  auto accepted_delta = fixture.create_normalized_update_or_throw(
      market_data::MarketUpdateKind::Delta, 1U, 11U, 10U,
      {{market_data::BookSide::Bid, parse_price_or_throw("100"), parse_quantity_or_throw("9")}},
      2U);
  const auto accepted =
      fixture.apply_market_update_for_test(std::move(accepted_delta), 2U, 4U, 400U);
  REQUIRE(accepted);
  REQUIRE(accepted.value().ready_book());
  REQUIRE(accepted.value().ready_book()->bid_at(0U));
  CHECK(accepted.value().ready_book()->bid_at(0U)->quantity == parse_quantity_or_throw("9"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// A classified no-op reserves zero callback records, so one remaining canonical slot is sufficient
// for its sole input-disposition record.
TEST_CASE("exact no-op preflight uses only disposition capacity",
          "[market_data][market_state][preflight][trace]") {
  MarketStateMachineFixture fixture{create_runtime_limits(3U, 100U, 4U, 6U)};
  RecordingMarketTurnPreflightAuthority authority;
  REQUIRE(fixture.market_state_machine().initialize_market_state(
      market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(1U),
                                          model::ProcessingTimestamp{100U}},
      fixture.runtime_trace_sink(), authority));
  REQUIRE(fixture.runtime_trace_sink().record_count() == 1U);
  const auto source = market_data::MarketSourceIdentity::from_runtime_source(
      fixture.runtime_policy().sources().front());

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish then ignore the same session four times, leaving exactly one trace slot for the last
  // no-op under the policy's minimum evidence-capacity relationship.
  for (std::uint64_t index = 0U; index < 5U; ++index) {
    const auto turn = index + 2U;
    const auto outcome = fixture.market_state_machine().apply_session_start(
        market_data::SessionStarted{source, model::SessionEpoch{1U}, model::SourceTimestamp{1U},
                                    create_ordinal_or_throw<model::ReceiveSequence>(index + 1U),
                                    model::ReceiveTimestamp{index + 1U}},
        market_data::AcceptedMarketTurnContext{
            create_ordinal_or_throw<model::AdmissionOrdinal>(index + 1U),
            create_ordinal_or_throw<model::TurnOrdinal>(turn),
            model::ProcessingTimestamp{turn * 100U}},
        fixture.runtime_trace_sink(), authority);
    REQUIRE(outcome);
    CHECK_FALSE(outcome.value().state_event());
    CHECK(outcome.value().expected_callback_count() == 0U);
  }
  CHECK(fixture.runtime_trace_sink().record_count() == 6U);
  CHECK(authority.authorization_call_count() == 6U);
  REQUIRE(authority.last_preflight_request());
  CHECK((*authority.last_preflight_request() ==
         market_data::MarketTurnPreflight{fixture.runtime_policy().sources().front().ordinal(),
                                          create_ordinal_or_throw<model::TurnOrdinal>(6U), 0U, 1U,
                                          0U, 1U, 0U, 1U}));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Book counter helpers reject impossible restored pairs and fail before either nominal counter
// wraps.
TEST_CASE("book generation and revision counters fail before wrap",
          "[market_data][market_state][counter]") {
  const auto invalid =
      market_data::BookIdentity::from_committed(create_ordinal_or_throw<model::BookGeneration>(2U),
                                                create_ordinal_or_throw<model::BookRevision>(1U));
  REQUIRE_FALSE(invalid);

  // ++++++++++++++++++++++++++++++++++++++++
  // Snapshot advancement reports generation exhaustion without publishing a revision successor.
  const auto maximum_generation =
      create_ordinal_or_throw<model::BookGeneration>(std::numeric_limits<std::uint64_t>::max());
  const auto maximum_revision =
      create_ordinal_or_throw<model::BookRevision>(std::numeric_limits<std::uint64_t>::max());
  const auto terminal =
      market_data::BookIdentity::from_committed(maximum_generation, maximum_revision);
  REQUIRE(terminal);
  const auto snapshot_exhausted = terminal.value().derive_next_snapshot_identity();
  REQUIRE_FALSE(snapshot_exhausted);
  CHECK(snapshot_exhausted.error().code == model::DomainErrorCode::MarketCounterExhausted);
  CHECK(snapshot_exhausted.error().context.field == "book_generation");

  // ++++++++++++++++++++++++++++++++++++++++
  // Delta advancement preserves generation and reports the independently exhausted revision.
  const auto revision_terminal = market_data::BookIdentity::from_committed(
      model::BookGeneration::create_initial(), maximum_revision);
  REQUIRE(revision_terminal);
  const auto delta_exhausted = revision_terminal.value().derive_next_delta_identity();
  REQUIRE_FALSE(delta_exhausted);
  CHECK(delta_exhausted.error().code == model::DomainErrorCode::MarketCounterExhausted);
  CHECK(delta_exhausted.error().context.field == "book_revision");
}

// --------------------------------------------------------

// --------------------------------------------------------
// Wrong configured source and regressing owner time are operational contract failures: neither
// appends a semantic disposition nor changes explicit readiness.
TEST_CASE("source and owner-time defects do not mutate market state",
          "[market_data][market_state][boundary]") {
  MarketStateMachineFixture fixture;
  const auto initialized = fixture.initialize_market_state_for_test();
  REQUIRE(initialized);
  const auto prefix = fixture.runtime_trace_sink().record_count();

  SECTION("processing time precedes receive time") {
    auto update = fixture.create_baseline_snapshot_or_throw();
    const auto failed = fixture.apply_market_update_for_test(std::move(update), 1U, 2U, 99U);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == model::DomainErrorCode::InvalidMarketEvent);
    REQUIRE(fixture.market_state_machine().readiness());
    CHECK(*fixture.market_state_machine().readiness() ==
          market_data::MarketReadiness::Synchronizing);
    CHECK_FALSE(fixture.market_state_machine().book_identity());
    CHECK(fixture.runtime_trace_sink().record_count() == prefix);
  }

  SECTION("update source belongs to another valid runtime policy") {
    auto alternate_policy_result = runtime::RuntimePolicy::create_runtime_policy(
        fixture.startup_configuration(),
        runtime::RuntimePolicyParams{
            fixture.runtime_limits(),
            {create_source_definition_or_throw("source.deribit-alternate")}});
    REQUIRE(alternate_policy_result);
    auto alternate_policy = std::move(alternate_policy_result).value();
    auto normalized = market_data::NormalizedMarketUpdate::create_normalized_market_update(
        market_data::NormalizedMarketUpdateFields{
            market_data::MarketSourceIdentity::from_runtime_source(
                alternate_policy.sources().front()),
            model::SessionEpoch{1U},
            model::SequenceNumber{10U},
            std::nullopt,
            model::SourceTimestamp{1'000U},
            create_ordinal_or_throw<model::ReceiveSequence>(1U),
            model::ReceiveTimestamp{100U},
            model::InstrumentMetadataRevision::create_initial(),
            market_data::MarketUpdateKind::Snapshot,
            create_market_integrity_or_throw(),
            {{market_data::BookSide::Bid, parse_price_or_throw("100"),
              parse_quantity_or_throw("1")}}},
        fixture.runtime_limits().maximum_changes_per_update);
    REQUIRE(normalized);
    const auto failed =
        fixture.apply_market_update_for_test(std::move(normalized).value(), 1U, 2U, 200U);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == model::DomainErrorCode::RuntimeSourceNotConfigured);
    REQUIRE(fixture.market_state_machine().readiness());
    CHECK(*fixture.market_state_machine().readiness() ==
          market_data::MarketReadiness::Synchronizing);
    CHECK(fixture.runtime_trace_sink().record_count() == prefix);
  }
}

// --------------------------------------------------------

// --------------------------------------------------------
// Complete policy sealing rejects foreign source membership, foreign trace provenance, and updates
// normalized under a looser change bound before any owner state or evidence mutates.
TEST_CASE("market state retains complete policy authority", "[market_data][market_state][policy]") {
  SECTION("factory rejects a source absent from the selected policy") {
    MarketStateMachineFixture fixture;
    auto foreign_policy_result = runtime::RuntimePolicy::create_runtime_policy(
        fixture.startup_configuration(),
        runtime::RuntimePolicyParams{
            fixture.runtime_limits(),
            {create_source_definition_or_throw("source.deribit-foreign")}});
    REQUIRE(foreign_policy_result);
    auto foreign_policy = std::move(foreign_policy_result).value();
    const auto& source = foreign_policy.sources().front();
    const auto& definition = source.definition();
    const auto* metadata = fixture.startup_configuration().find_instrument_metadata(
        definition.venue_id, definition.instrument_id);
    REQUIRE(metadata != nullptr);

    const auto rejected = market_data::MarketStateMachine::create_market_state_machine(
        fixture.runtime_policy(), source, *metadata);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::RuntimeSourceNotConfigured);
    CHECK(rejected.error().context.field == "market_state.source");
  }

  SECTION("foreign trace provenance fails before initialization") {
    MarketStateMachineFixture fixture;
    auto foreign_limits = fixture.runtime_limits();
    ++foreign_limits.runtime_trace_capacity;
    auto foreign_policy_result = runtime::RuntimePolicy::create_runtime_policy(
        fixture.startup_configuration(),
        runtime::RuntimePolicyParams{foreign_limits, {create_source_definition_or_throw()}});
    REQUIRE(foreign_policy_result);
    auto foreign_policy = std::move(foreign_policy_result).value();
    REQUIRE(foreign_policy.fingerprint() != fixture.runtime_policy().fingerprint());
    trace::RuntimeTraceSink foreign_trace{foreign_policy};

    const auto rejected = fixture.market_state_machine().initialize_market_state(
        market_data::OwnerMarketTurnContext{create_ordinal_or_throw<model::TurnOrdinal>(1U),
                                            model::ProcessingTimestamp{100U}},
        foreign_trace);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::InvalidRelationship);
    CHECK(rejected.error().context.field == "market_state.trace_provenance");
    CHECK_FALSE(fixture.market_state_machine().readiness());
    CHECK(foreign_trace.record_count() == 0U);
    REQUIRE(fixture.initialize_market_state_for_test());
  }

  SECTION("looser normalization cannot bypass the retained policy change bound") {
    auto bounded_limits = create_runtime_limits();
    bounded_limits.maximum_changes_per_update = 2U;
    MarketStateMachineFixture fixture{bounded_limits};
    REQUIRE(fixture.initialize_market_state_for_test());
    const auto prefix = fixture.runtime_trace_sink().record_count();

    auto loosely_normalized = market_data::NormalizedMarketUpdate::create_normalized_market_update(
        market_data::NormalizedMarketUpdateFields{
            market_data::MarketSourceIdentity::from_runtime_source(
                fixture.runtime_policy().sources().front()),
            model::SessionEpoch{1U},
            model::SequenceNumber{10U},
            std::nullopt,
            model::SourceTimestamp{1'000U},
            create_ordinal_or_throw<model::ReceiveSequence>(1U),
            model::ReceiveTimestamp{100U},
            model::InstrumentMetadataRevision::create_initial(),
            market_data::MarketUpdateKind::Snapshot,
            create_market_integrity_or_throw(),
            {{market_data::BookSide::Bid, parse_price_or_throw("100"),
              parse_quantity_or_throw("1")},
             {market_data::BookSide::Bid, parse_price_or_throw("99"), parse_quantity_or_throw("1")},
             {market_data::BookSide::Bid, parse_price_or_throw("98"),
              parse_quantity_or_throw("1")}}},
        market_data::maximum_changes_per_market_update);
    REQUIRE(loosely_normalized);
    const auto rejected =
        fixture.apply_market_update_for_test(std::move(loosely_normalized).value(), 1U, 2U, 200U);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::MarketBookCapacityExceeded);
    CHECK(rejected.error().context.field == "market_update.changes");
    REQUIRE(fixture.market_state_machine().readiness());
    CHECK(*fixture.market_state_machine().readiness() ==
          market_data::MarketReadiness::Synchronizing);
    CHECK_FALSE(fixture.market_state_machine().book_identity());
    CHECK(fixture.runtime_trace_sink().record_count() == prefix);
  }
}

// --------------------------------------------------------

} // namespace
