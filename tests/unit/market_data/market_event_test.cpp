// Purpose: prove normalized market identity, validation, controls, and post-commit event invariants
// remain deterministic and nominally separated from transport timing.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/market_data/market_event.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "reference_configuration.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace aegis;

// ########################################################################
// Runtime, transport, callback, and book counters share storage but must remain unrelated nominal
// domains at every call boundary.
static_assert(!std::is_same_v<model::MarketSourceOrdinal, model::AdmissionOrdinal>);
static_assert(!std::is_same_v<model::AdmissionOrdinal, model::ReceiveSequence>);
static_assert(!std::is_same_v<model::ReceiveSequence, model::TurnOrdinal>);
static_assert(!std::is_same_v<model::TurnOrdinal, model::CallbackOrdinal>);
static_assert(!std::is_same_v<model::BookGeneration, model::BookRevision>);
static_assert(!std::is_convertible_v<model::BookGeneration, model::BookRevision>);
static_assert(!std::is_aggregate_v<market_data::MarketSourceIdentity>);
static_assert(!std::is_constructible_v<market_data::MarketSourceIdentity, model::MarketSourceId,
                                       model::MarketSourceOrdinal, model::VenueId,
                                       model::InstrumentId, model::VenueInstrumentId>);

// ########################################################################

// --------------------------------------------------------
// Fail fast when an authored identifier breaks the typed test fixture rather than a tested case.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view value) {
  auto parsed = Identifier::parse(value);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in market-event test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Unwrap checked one-based positions used to assemble valid event fixtures.
template <typename Ordinal> [[nodiscard]] Ordinal ordinal(std::uint64_t value) {
  auto parsed = Ordinal::from_value(value);
  if (!parsed) {
    throw std::logic_error{"invalid ordinal in market-event test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact nominal price text for update fixtures.
[[nodiscard]] model::Price price(std::string_view value) {
  auto parsed = model::Price::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid price in market-event test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Parse exact nominal quantity text for update fixtures.
[[nodiscard]] model::Quantity quantity(std::string_view value) {
  auto parsed = model::Quantity::parse_ascii(value);
  if (!parsed) {
    throw std::logic_error{"invalid quantity in market-event test fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Build one validated runtime source once so no test can forge an ordinal-to-identity relationship.
[[nodiscard]] const runtime::RuntimeSource& configured_source() {
  static const runtime::RuntimePolicy policy = [] {
    auto configuration_result =
        configuration::StartupConfiguration::create(test_support::reference_configuration_params());
    if (!configuration_result) {
      throw std::logic_error{"invalid configuration in market-event test fixture"};
    }
    auto configuration = std::move(configuration_result).value();
    runtime::RuntimePolicyParams params{
        runtime::RuntimePolicyLimits{8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 128U, 256U, 32U,
                                     100'000U},
        {{id<model::MarketSourceId>("source.deribit-btc-perpetual"), id<model::VenueId>("deribit"),
          id<model::InstrumentId>("BTC-USD-PERPETUAL"),
          id<model::VenueInstrumentId>("BTC-PERPETUAL"),
          model::InstrumentMetadataRevision::initial()}}};
    auto policy_result = runtime::RuntimePolicy::create(configuration, std::move(params));
    if (!policy_result) {
      throw std::logic_error{"invalid runtime policy in market-event test fixture"};
    }
    return std::move(policy_result).value();
  }();
  return policy.sources().front();
}

// --------------------------------------------------------
// Build the complete configured identity reused by normalized commands and callback events.
[[nodiscard]] market_data::MarketSourceIdentity source_identity() {
  return market_data::MarketSourceIdentity::from_runtime_source(configured_source());
}

// --------------------------------------------------------
// Validate an alternate source ID against the same sealed market tuple before copying its identity.
[[nodiscard]] market_data::MarketSourceIdentity source_identity_named(std::string_view source_id) {
  auto configuration_result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  if (!configuration_result) {
    throw std::logic_error{"invalid alternate configuration in market-event test fixture"};
  }
  auto configuration = std::move(configuration_result).value();
  runtime::RuntimePolicyParams params{
      runtime::RuntimePolicyLimits{8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 128U, 256U, 32U,
                                   100'000U},
      {{id<model::MarketSourceId>(source_id), id<model::VenueId>("deribit"),
        id<model::InstrumentId>("BTC-USD-PERPETUAL"), id<model::VenueInstrumentId>("BTC-PERPETUAL"),
        model::InstrumentMetadataRevision::initial()}}};
  auto policy_result = runtime::RuntimePolicy::create(configuration, std::move(params));
  if (!policy_result) {
    throw std::logic_error{"invalid alternate runtime policy in market-event test fixture"};
  }
  return market_data::MarketSourceIdentity::from_runtime_source(
      policy_result.value().sources().front());
}

// --------------------------------------------------------
// Produce a fixed integrity identity while letting each case select accepted or rejected meaning.
[[nodiscard]] market_data::MarketIntegrity
integrity(market_data::IntegrityVerdict verdict = market_data::IntegrityVerdict::Accepted,
          std::string_view token = "fixture-checksum-001") {
  auto identity = market_data::IntegrityTokenIdentity::from_token(token);
  if (!identity) {
    throw std::logic_error{"invalid integrity token in market-event test fixture"};
  }
  return market_data::MarketIntegrity{verdict, std::move(identity).value()};
}

// --------------------------------------------------------
// Assemble one valid update field set while retaining authored change order for canonicalization
// tests.
[[nodiscard]] market_data::NormalizedMarketUpdateFields
update_fields(std::vector<market_data::MarketLevelChange> changes,
              market_data::MarketUpdateKind kind = market_data::MarketUpdateKind::Delta,
              model::ReceiveSequence receive_sequence = model::ReceiveSequence::initial(),
              model::ReceiveTimestamp receive_timestamp = model::ReceiveTimestamp{50U},
              market_data::MarketIntegrity update_integrity = integrity()) {
  return market_data::NormalizedMarketUpdateFields{source_identity(),
                                                   model::SessionEpoch{7U},
                                                   model::SequenceNumber{101U},
                                                   model::SequenceNumber{100U},
                                                   model::SourceTimestamp{1'000U},
                                                   receive_sequence,
                                                   receive_timestamp,
                                                   model::InstrumentMetadataRevision::initial(),
                                                   kind,
                                                   std::move(update_integrity),
                                                   std::move(changes)};
}

// --------------------------------------------------------
// Build a representative two-sided delta whose authored order is intentionally noncanonical.
[[nodiscard]] std::vector<market_data::MarketLevelChange> representative_changes() {
  return {{market_data::BookSide::Ask, price("101.50"), quantity("2.0")},
          {market_data::BookSide::Bid, price("100.00"), quantity("3.5")},
          {market_data::BookSide::Ask, price("101.00"), quantity("1.0")}};
}

// --------------------------------------------------------
// Create one valid normalized update or fail the test fixture immediately.
[[nodiscard]] market_data::NormalizedMarketUpdate
normalized_update(market_data::IntegrityVerdict verdict = market_data::IntegrityVerdict::Accepted) {
  auto result = market_data::NormalizedMarketUpdate::create(
      update_fields(representative_changes(), market_data::MarketUpdateKind::Delta,
                    model::ReceiveSequence::initial(), model::ReceiveTimestamp{50U},
                    integrity(verdict)),
      8U);
  if (!result) {
    throw std::logic_error{"invalid normalized update in market-event test fixture"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Derive one valid normalized digest or fail before a sensitivity assertion can become misleading.
[[nodiscard]] model::Sha256Digest
normalized_digest(market_data::NormalizedMarketUpdateFields fields) {
  auto result = market_data::NormalizedMarketUpdate::create(std::move(fields), 8U);
  if (!result) {
    throw std::logic_error{"invalid digest mutation in market-event test fixture"};
  }
  return result.value().payload_digest();
}

// --------------------------------------------------------
// Render one digest as its fixed lowercase hexadecimal compatibility representation.
[[nodiscard]] std::string digest_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Build state-transition fields with no transport input or committed book by default.
[[nodiscard]] market_data::MarketStateEventFields
state_fields(market_data::MarketReadiness readiness,
             std::optional<market_data::MarketReadiness> previous = std::nullopt) {
  return market_data::MarketStateEventFields{source_identity(),
                                             std::nullopt,
                                             std::nullopt,
                                             std::nullopt,
                                             std::nullopt,
                                             std::nullopt,
                                             model::TurnOrdinal::initial(),
                                             model::ProcessingTimestamp{60U},
                                             std::nullopt,
                                             std::nullopt,
                                             std::nullopt,
                                             previous,
                                             readiness};
}

// --------------------------------------------------------

// --------------------------------------------------------
// One-based shared counters reject absence and retain subsystem-specific exhaustion identities.
TEST_CASE("M2 nominal counters fail before zero entry or unsigned wrap",
          "[market_data][market_event]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero cannot be forged as an assigned source, receive, turn, callback, or book position.
  const auto absent_source = model::MarketSourceOrdinal::from_value(0U);
  const auto absent_generation = model::BookGeneration::from_value(0U);
  REQUIRE_FALSE(absent_source);
  REQUIRE_FALSE(absent_generation);
  CHECK(absent_source.error().context.field == "market_source_ordinal");
  CHECK(absent_generation.error().context.field == "book_generation");

  // ++++++++++++++++++++++++++++++++++++++++
  // Market and callback counters report their own stable exhaustion bands before wrapping.
  const auto maximum_generation =
      model::BookGeneration::from_value(std::numeric_limits<std::uint64_t>::max());
  const auto maximum_callback =
      model::CallbackOrdinal::from_value(std::numeric_limits<std::uint64_t>::max());
  REQUIRE(maximum_generation);
  REQUIRE(maximum_callback);

  const auto generation_overflow = maximum_generation.value().next();
  const auto callback_overflow = maximum_callback.value().next();
  REQUIRE_FALSE(generation_overflow);
  REQUIRE_FALSE(callback_overflow);
  CHECK(generation_overflow.error() ==
        model::DomainError::at_field(model::DomainErrorCode::MarketCounterExhausted,
                                     "book_generation"));
  CHECK(callback_overflow.error() ==
        model::DomainError::at_field(model::DomainErrorCode::CallbackCounterExhausted,
                                     "callback_ordinal"));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Canonical update order and payload identity must not depend on fixture ordering or owner timing.
TEST_CASE("normalized updates derive a deterministic timing-independent payload digest",
          "[market_data][market_event]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Normalize the same semantic changes under distinct receive identities and authored orders.
  auto authored = representative_changes();
  auto reversed = authored;
  std::reverse(reversed.begin(), reversed.end());

  const auto first =
      market_data::NormalizedMarketUpdate::create(update_fields(std::move(authored)), 8U);
  const auto second = market_data::NormalizedMarketUpdate::create(
      update_fields(std::move(reversed), market_data::MarketUpdateKind::Delta,
                    ordinal<model::ReceiveSequence>(9U), model::ReceiveTimestamp{900U}),
      8U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Both outputs use side-then-price order and one semantic digest despite timing differences.
  REQUIRE(first);
  REQUIRE(second);
  REQUIRE(first.value().changes().size() == 3U);
  CHECK(first.value().changes()[0U].side == market_data::BookSide::Bid);
  CHECK(first.value().changes()[1U].price == price("101"));
  CHECK(first.value().changes()[2U].price == price("101.5"));
  CHECK(first.value().payload_digest() == second.value().payload_digest());
  CHECK_FALSE(first.value() == second.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A semantic source-time change must produce a different duplicate identity.
  auto changed_fields = update_fields(representative_changes());
  changed_fields.source_timestamp = model::SourceTimestamp{1'001U};
  const auto changed = market_data::NormalizedMarketUpdate::create(std::move(changed_fields), 8U);
  REQUIRE(changed);
  CHECK_FALSE(first.value().payload_digest() == changed.value().payload_digest());

  // ++++++++++++++++++++++++++++++++++++++++
  // One known-answer vector anchors the schema and every semantic input changes that identity.
  const auto baseline_fields = update_fields(representative_changes());
  const auto baseline_digest = normalized_digest(baseline_fields);
  CHECK(digest_hex(baseline_digest) ==
        "5f266876bf5df373778cdc8d1487d3c983f6b8d7cd35747d837e934e0584676d");
  const auto check_changed = [&](market_data::NormalizedMarketUpdateFields fields) {
    CHECK_FALSE(normalized_digest(std::move(fields)) == baseline_digest);
  };

  auto changed_kind = baseline_fields;
  changed_kind.kind = market_data::MarketUpdateKind::Snapshot;
  check_changed(std::move(changed_kind));
  auto changed_source = baseline_fields;
  changed_source.source = source_identity_named("source.alternate-deribit-btc");
  check_changed(std::move(changed_source));
  auto changed_session = baseline_fields;
  changed_session.session_epoch = model::SessionEpoch{8U};
  check_changed(std::move(changed_session));
  auto changed_sequence = baseline_fields;
  changed_sequence.source_sequence = model::SequenceNumber{102U};
  check_changed(std::move(changed_sequence));
  auto missing_predecessor = baseline_fields;
  missing_predecessor.predecessor_sequence.reset();
  check_changed(std::move(missing_predecessor));
  auto changed_predecessor = baseline_fields;
  changed_predecessor.predecessor_sequence = model::SequenceNumber{99U};
  check_changed(std::move(changed_predecessor));
  auto changed_source_time = baseline_fields;
  changed_source_time.source_timestamp = model::SourceTimestamp{1'001U};
  check_changed(std::move(changed_source_time));
  auto changed_metadata = baseline_fields;
  changed_metadata.metadata_revision = model::InstrumentMetadataRevision::from_value(2U).value();
  check_changed(std::move(changed_metadata));
  auto changed_verdict = baseline_fields;
  changed_verdict.integrity = integrity(market_data::IntegrityVerdict::Rejected);
  check_changed(std::move(changed_verdict));
  auto changed_token = baseline_fields;
  changed_token.integrity = integrity(market_data::IntegrityVerdict::Accepted, "other-token");
  check_changed(std::move(changed_token));
  auto changed_side = baseline_fields;
  changed_side.changes[0U].side = market_data::BookSide::Bid;
  check_changed(std::move(changed_side));
  auto changed_price = baseline_fields;
  changed_price.changes[0U].price = price("101.75");
  check_changed(std::move(changed_price));
  auto changed_quantity = baseline_fields;
  changed_quantity.changes[0U].quantity = quantity("2.5");
  check_changed(std::move(changed_quantity));

  // ++++++++++++++++++++++++++++++++++++++++
  // Receive identity remains transport provenance and is independently excluded from the payload.
  auto changed_receive_sequence = baseline_fields;
  changed_receive_sequence.receive_sequence = ordinal<model::ReceiveSequence>(9U);
  CHECK(normalized_digest(std::move(changed_receive_sequence)) == baseline_digest);
  auto changed_receive_time = baseline_fields;
  changed_receive_time.receive_timestamp = model::ReceiveTimestamp{900U};
  CHECK(normalized_digest(std::move(changed_receive_time)) == baseline_digest);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Primitive sequence absence, malformed content, duplicate keys, and bound breaches fail before
// hashing can publish semantic identity.
TEST_CASE("normalized updates reject invalid shape and preserve stable errors",
          "[market_data][market_event]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero represents absent protocol sequence identity and cannot enter normalized market state.
  auto zero_source_fields = update_fields(representative_changes());
  zero_source_fields.source_sequence = model::SequenceNumber{0U};
  const auto zero_source =
      market_data::NormalizedMarketUpdate::create(std::move(zero_source_fields), 8U);
  REQUIRE_FALSE(zero_source);
  CHECK(zero_source.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketEvent,
                                     "market_update.source_sequence"));

  auto zero_predecessor_fields = update_fields(representative_changes());
  zero_predecessor_fields.predecessor_sequence = model::SequenceNumber{0U};
  const auto zero_predecessor =
      market_data::NormalizedMarketUpdate::create(std::move(zero_predecessor_fields), 8U);
  REQUIRE_FALSE(zero_predecessor);
  CHECK(zero_predecessor.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketEvent,
                                     "market_update.predecessor_sequence"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Unknown update and integrity enum values cannot silently acquire protocol meaning.
  auto invalid_kind_fields = update_fields(representative_changes());
  invalid_kind_fields.kind = static_cast<market_data::MarketUpdateKind>(std::uint8_t{99U});
  const auto invalid_kind =
      market_data::NormalizedMarketUpdate::create(std::move(invalid_kind_fields), 8U);
  REQUIRE_FALSE(invalid_kind);
  CHECK(invalid_kind.error() ==
        model::DomainError::at_field(model::DomainErrorCode::InvalidMarketEvent,
                                     "market_update.kind"));

  auto invalid_integrity_fields = update_fields(representative_changes());
  invalid_integrity_fields.integrity.verdict =
      static_cast<market_data::IntegrityVerdict>(std::uint8_t{99U});
  const auto invalid_integrity =
      market_data::NormalizedMarketUpdate::create(std::move(invalid_integrity_fields), 8U);
  REQUIRE_FALSE(invalid_integrity);
  CHECK(invalid_integrity.error().context.field == "market_update.integrity_verdict");

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity rejection is distinct from malformed content and names the complete change group.
  const auto oversized =
      market_data::NormalizedMarketUpdate::create(update_fields(representative_changes()), 2U);
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() ==
        model::DomainError::at_field(model::DomainErrorCode::MarketBookCapacityExceeded,
                                     "market_update.changes"));

  const auto invalid_compiled_bound = market_data::NormalizedMarketUpdate::create(
      update_fields(representative_changes()), market_data::maximum_changes_per_market_update + 1U);
  REQUIRE_FALSE(invalid_compiled_bound);
  CHECK(invalid_compiled_bound.error().context.field == "market_update.maximum_changes");

  // ++++++++++++++++++++++++++++++++++++++++
  // An unassigned side and a repeated side/price key both fail at an indexed change boundary.
  auto invalid_side_fields = update_fields(representative_changes());
  invalid_side_fields.changes[0U].side = static_cast<market_data::BookSide>(std::uint8_t{99U});
  const auto invalid_side =
      market_data::NormalizedMarketUpdate::create(std::move(invalid_side_fields), 8U);
  REQUIRE_FALSE(invalid_side);
  CHECK(invalid_side.error().context.field == "market_update.changes.side");
  CHECK(invalid_side.error().context.collection_index == 0U);

  const auto duplicate = market_data::NormalizedMarketUpdate::create(
      update_fields({{market_data::BookSide::Bid, price("100"), quantity("1")},
                     {market_data::BookSide::Bid, price("100.0"), quantity("2")}}),
      8U);
  REQUIRE_FALSE(duplicate);
  CHECK(duplicate.error().code == model::DomainErrorCode::InvalidMarketEvent);
  CHECK(duplicate.error().context.field == "market_update.changes.price");

  // ++++++++++++++++++++++++++++++++++++++++
  // Zero is a valid delta deletion but not a retained snapshot level; negative values never pass.
  const auto deletion = market_data::NormalizedMarketUpdate::create(
      update_fields({{market_data::BookSide::Ask, price("101"), quantity("0")}}), 8U);
  REQUIRE(deletion);

  const auto snapshot_zero = market_data::NormalizedMarketUpdate::create(
      update_fields({{market_data::BookSide::Ask, price("101"), quantity("0")}},
                    market_data::MarketUpdateKind::Snapshot),
      8U);
  REQUIRE_FALSE(snapshot_zero);
  CHECK(snapshot_zero.error().context.field == "market_update.changes.quantity");

  const auto negative_price = market_data::NormalizedMarketUpdate::create(
      update_fields({{market_data::BookSide::Bid, price("-1"), quantity("1")}}), 8U);
  REQUIRE_FALSE(negative_price);
  CHECK(negative_price.error().context.field == "market_update.changes.price");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Integrity token identity is fixed-width and refuses absence or input beyond the normalized bound.
TEST_CASE("integrity token identity is bounded before canonical hashing",
          "[market_data][market_event]") {
  const auto first = market_data::IntegrityTokenIdentity::from_token("fixture-checksum");
  const auto same = market_data::IntegrityTokenIdentity::from_token("fixture-checksum");
  const auto different = market_data::IntegrityTokenIdentity::from_token("other-checksum");
  const auto empty = market_data::IntegrityTokenIdentity::from_token("");
  const auto oversized = market_data::IntegrityTokenIdentity::from_token(
      std::string(market_data::maximum_integrity_token_bytes + 1U, 'x'));

  REQUIRE(first);
  REQUIRE(same);
  REQUIRE(different);
  CHECK(first.value() == same.value());
  CHECK_FALSE(first.value() == different.value());
  REQUIRE_FALSE(empty);
  REQUIRE_FALSE(oversized);
  CHECK(empty.error().context.field == "market_update.integrity_token");
  CHECK(oversized.error().code == model::DomainErrorCode::InvalidMarketEvent);
}

// --------------------------------------------------------
// Explicit control alternatives keep session reset and freshness turns out of book-update payloads.
TEST_CASE("normalized market commands preserve distinct deterministic control types",
          "[market_data][market_event]") {
  const market_data::SessionStarted session{
      source_identity(), model::SessionEpoch{8U}, model::SourceTimestamp{2'000U},
      ordinal<model::ReceiveSequence>(2U), model::ReceiveTimestamp{70U}};
  const market_data::StalenessCheck freshness{
      source_identity(), model::SessionEpoch{8U}, ordinal<model::ReceiveSequence>(3U),
      model::ReceiveTimestamp{75U}, model::ProcessingTimestamp{80U}};

  const market_data::NormalizedRecordedMarketCommand session_command{session};
  const market_data::NormalizedRecordedMarketCommand freshness_command{freshness};
  const market_data::NormalizedRecordedMarketCommand update_command{normalized_update()};

  CHECK(std::holds_alternative<market_data::SessionStarted>(session_command));
  CHECK(std::holds_alternative<market_data::StalenessCheck>(freshness_command));
  CHECK(std::holds_alternative<market_data::NormalizedMarketUpdate>(update_command));
  CHECK(std::get<market_data::SessionStarted>(session_command) == session);
}

// --------------------------------------------------------
// Strategy-facing market events are values only the future transactional state owner can mint.
TEST_CASE("MarketEvent publication is owner restricted", "[market_data][market_event]") {
  static_assert(
      !std::is_constructible_v<market_data::MarketEvent, market_data::NormalizedMarketUpdate,
                               market_data::MarketCommitContext>);
  CHECK(market_data::MarketEvent::readiness() == market_data::MarketReadiness::Ready);
}

// --------------------------------------------------------
// State callbacks require actual assigned transitions and coherent optional transport/book groups.
TEST_CASE("MarketStateEvent validates sanitized readiness transitions",
          "[market_data][market_event]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Initial Synchronizing has no fabricated prior state or zero-valued committed book identity.
  const auto initial = market_data::validate_market_state_transition(
      state_fields(market_data::MarketReadiness::Synchronizing));
  REQUIRE(initial);

  // ++++++++++++++++++++++++++++++++++++++++
  // Explicit owner resynchronization alone may publish Synchronizing while already there.
  const auto owner_resynchronization = market_data::validate_market_state_transition(state_fields(
      market_data::MarketReadiness::Synchronizing, market_data::MarketReadiness::Synchronizing));
  REQUIRE(owner_resynchronization);

  auto envelope_same_state = state_fields(market_data::MarketReadiness::Synchronizing,
                                          market_data::MarketReadiness::Synchronizing);
  envelope_same_state.session_epoch = model::SessionEpoch{7U};
  envelope_same_state.receive_sequence = model::ReceiveSequence::initial();
  envelope_same_state.receive_timestamp = model::ReceiveTimestamp{50U};
  envelope_same_state.admission_ordinal = model::AdmissionOrdinal::initial();
  const auto rejected_envelope = market_data::validate_market_state_transition(envelope_same_state);
  REQUIRE_FALSE(rejected_envelope);
  CHECK(rejected_envelope.error().context.field == "market_state.transition");

  // ++++++++++++++++++++++++++++++++++++++++
  // A Ready transition must carry a paired coherent book identity and may retain receive context.
  auto ready_fields = state_fields(market_data::MarketReadiness::Ready,
                                   market_data::MarketReadiness::Synchronizing);
  ready_fields.session_epoch = model::SessionEpoch{7U};
  ready_fields.source_sequence = model::SequenceNumber{101U};
  ready_fields.receive_sequence = model::ReceiveSequence::initial();
  ready_fields.receive_timestamp = model::ReceiveTimestamp{50U};
  ready_fields.admission_ordinal = model::AdmissionOrdinal::initial();
  ready_fields.metadata_revision = model::InstrumentMetadataRevision::initial();
  ready_fields.book_generation = model::BookGeneration::initial();
  ready_fields.book_revision = ordinal<model::BookRevision>(3U);
  const auto ready = market_data::validate_market_state_transition(ready_fields);
  REQUIRE(ready);

  // ++++++++++++++++++++++++++++++++++++++++
  // Initial non-Synchronizing publication and every other same-state notification are incoherent.
  const auto invalid_initial = market_data::validate_market_state_transition(
      state_fields(market_data::MarketReadiness::Invalid));
  REQUIRE_FALSE(invalid_initial);
  CHECK(invalid_initial.error().context.field == "market_state.previous_readiness");

  const auto unchanged = market_data::validate_market_state_transition(
      state_fields(market_data::MarketReadiness::Stale, market_data::MarketReadiness::Stale));
  REQUIRE_FALSE(unchanged);
  CHECK(unchanged.error().context.field == "market_state.transition");

  // ++++++++++++++++++++++++++++++++++++++++
  // Unknown enum values and a Ready transition without a committed book fail closed.
  auto unknown_fields = state_fields(market_data::MarketReadiness::Ready,
                                     market_data::MarketReadiness::Synchronizing);
  unknown_fields.readiness = static_cast<market_data::MarketReadiness>(std::uint8_t{99U});
  const auto unknown = market_data::validate_market_state_transition(unknown_fields);
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error().code == model::DomainErrorCode::InvalidMarketState);

  auto missing_book_fields = ready_fields;
  missing_book_fields.book_generation.reset();
  missing_book_fields.book_revision.reset();
  const auto missing_book = market_data::validate_market_state_transition(missing_book_fields);
  REQUIRE_FALSE(missing_book);
  CHECK(missing_book.error().context.field == "market_state.required_book_identity");

  // ++++++++++++++++++++++++++++++++++++++++
  // Partial receive and book groups, clock regression, and revision ordering cannot escape.
  auto partial_receive =
      state_fields(market_data::MarketReadiness::Invalid, market_data::MarketReadiness::Ready);
  partial_receive.receive_sequence = model::ReceiveSequence::initial();
  partial_receive.book_generation = model::BookGeneration::initial();
  partial_receive.book_revision = model::BookRevision::initial();
  const auto receive_failure = market_data::validate_market_state_transition(partial_receive);
  REQUIRE_FALSE(receive_failure);
  CHECK(receive_failure.error().context.field == "market_state.input_context");

  auto regressing =
      state_fields(market_data::MarketReadiness::Invalid, market_data::MarketReadiness::Ready);
  regressing.session_epoch = model::SessionEpoch{7U};
  regressing.receive_sequence = model::ReceiveSequence::initial();
  regressing.receive_timestamp = model::ReceiveTimestamp{61U};
  regressing.admission_ordinal = model::AdmissionOrdinal::initial();
  regressing.book_generation = model::BookGeneration::initial();
  regressing.book_revision = model::BookRevision::initial();
  const auto clock_failure = market_data::validate_market_state_transition(regressing);
  REQUIRE_FALSE(clock_failure);
  CHECK(clock_failure.error().context.field == "market_state.processing_timestamp");

  auto incoherent_book =
      state_fields(market_data::MarketReadiness::Invalid, market_data::MarketReadiness::Ready);
  incoherent_book.admission_ordinal = model::AdmissionOrdinal::initial();
  incoherent_book.book_generation = ordinal<model::BookGeneration>(2U);
  incoherent_book.book_revision = model::BookRevision::initial();
  const auto book_failure = market_data::validate_market_state_transition(incoherent_book);
  REQUIRE_FALSE(book_failure);
  CHECK(book_failure.error().context.field == "market_state.book_identity");

  // ++++++++++++++++++++++++++++++++++++++++
  // Startup cannot fabricate an accepted session or a committed-book history.
  auto initial_with_session = state_fields(market_data::MarketReadiness::Synchronizing);
  initial_with_session.session_epoch = model::SessionEpoch{7U};
  CHECK_FALSE(market_data::validate_market_state_transition(initial_with_session));
  auto initial_with_book = state_fields(market_data::MarketReadiness::Synchronizing);
  initial_with_book.book_generation = model::BookGeneration::initial();
  initial_with_book.book_revision = model::BookRevision::initial();
  const auto fabricated_book = market_data::validate_market_state_transition(initial_with_book);
  REQUIRE_FALSE(fabricated_book);
  CHECK(fabricated_book.error().context.field == "market_state.initial_context");

  // ++++++++++++++++++++++++++++++++++++++++
  // Staleness is possible only from Ready, through a complete accepted control envelope, while
  // retaining the last committed book identity.
  auto stale_fields =
      state_fields(market_data::MarketReadiness::Stale, market_data::MarketReadiness::Ready);
  stale_fields.session_epoch = model::SessionEpoch{7U};
  stale_fields.receive_sequence = ordinal<model::ReceiveSequence>(2U);
  stale_fields.receive_timestamp = model::ReceiveTimestamp{50U};
  stale_fields.admission_ordinal = ordinal<model::AdmissionOrdinal>(2U);
  stale_fields.book_generation = model::BookGeneration::initial();
  stale_fields.book_revision = ordinal<model::BookRevision>(3U);
  REQUIRE(market_data::validate_market_state_transition(stale_fields));

  auto synchronizing_to_stale = stale_fields;
  synchronizing_to_stale.previous_readiness = market_data::MarketReadiness::Synchronizing;
  CHECK_FALSE(market_data::validate_market_state_transition(synchronizing_to_stale));
  auto invalid_to_stale = stale_fields;
  invalid_to_stale.previous_readiness = market_data::MarketReadiness::Invalid;
  CHECK_FALSE(market_data::validate_market_state_transition(invalid_to_stale));

  // ++++++++++++++++++++++++++++++++++++++++
  // Discontinuity carries only the failed attempt; accepted receive identity always carries that
  // attempt plus a complete session envelope.
  auto precommit_discontinuity = state_fields(market_data::MarketReadiness::Invalid,
                                              market_data::MarketReadiness::Synchronizing);
  precommit_discontinuity.admission_ordinal = ordinal<model::AdmissionOrdinal>(3U);
  REQUIRE(market_data::validate_market_state_transition(precommit_discontinuity));
  auto fence_with_receive = precommit_discontinuity;
  fence_with_receive.receive_sequence = ordinal<model::ReceiveSequence>(2U);
  fence_with_receive.receive_timestamp = model::ReceiveTimestamp{50U};
  CHECK_FALSE(market_data::validate_market_state_transition(fence_with_receive));

  auto update_without_attempt = ready_fields;
  update_without_attempt.admission_ordinal.reset();
  CHECK_FALSE(market_data::validate_market_state_transition(update_without_attempt));
  auto ready_without_metadata = ready_fields;
  ready_without_metadata.metadata_revision.reset();
  CHECK_FALSE(market_data::validate_market_state_transition(ready_without_metadata));

  // ++++++++++++++++++++++++++++++++++++++++
  // Validation is public for preflight, but only MarketStateMachine can mint the callback value.
  static_assert(
      !std::is_constructible_v<market_data::MarketStateEvent, market_data::MarketStateEventFields>);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
