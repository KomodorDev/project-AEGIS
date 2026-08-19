// Purpose: prove M2 runtime trace event shapes, preflight preservation, and canonical AEGISRTS
// schema-one bytes independently from the M1 provenance trace.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/trace/runtime_trace.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Invalid identifier literals are fixture defects and fail before exercising trace behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in runtime trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid revision literals use the same fail-fast fixture policy.
template <typename Revision> [[nodiscard]] Revision revision(std::uint64_t value) {
  auto result = Revision::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid revision in runtime trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid one-based counter literals fail before they can weaken a runtime trace fixture.
template <typename Ordinal> [[nodiscard]] Ordinal ordinal(std::uint64_t value) {
  auto result = Ordinal::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid ordinal in runtime trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Invalid price literals cannot contribute useful trace-test evidence.
[[nodiscard]] model::Price price(std::int64_t coefficient, std::uint8_t scale) {
  auto result = model::Price::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid price in runtime trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build one immutable policy with a selected valid evidence bound for source/coherence tests.
[[nodiscard]] runtime::RuntimePolicy
make_runtime_policy(std::uint32_t runtime_trace_capacity = 256U,
                    std::string_view source_id = "source.deribit-btc-perpetual") {
  auto configuration_result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  if (!configuration_result) {
    throw std::logic_error{"invalid configuration in runtime trace test"};
  }
  auto configuration = std::move(configuration_result).value();
  runtime::RuntimePolicyParams params{
      runtime::RuntimePolicyLimits{8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 128U,
                                   runtime_trace_capacity, 32U, 100'000U},
      {{id<model::MarketSourceId>(source_id), id<model::VenueId>("deribit"),
        id<model::InstrumentId>("BTC-USD-PERPETUAL"), id<model::VenueInstrumentId>("BTC-PERPETUAL"),
        model::InstrumentMetadataRevision::initial()}}};
  auto policy_result = runtime::RuntimePolicy::create(configuration, std::move(params));
  if (!policy_result) {
    throw std::logic_error{"invalid runtime policy in runtime trace test"};
  }
  return std::move(policy_result).value();
}

// --------------------------------------------------------
// Keep the canonical golden policy alive for every ordinary trace fixture.
[[nodiscard]] const runtime::RuntimePolicy& runtime_policy() {
  static const runtime::RuntimePolicy policy = make_runtime_policy();
  return policy;
}

// --------------------------------------------------------
// Build one complete source identity shared by market-runtime event fixtures.
[[nodiscard]] trace::RuntimeTraceSource source() {
  return trace::RuntimeTraceSource::from_runtime_source(runtime_policy().sources().front());
}

// --------------------------------------------------------
// Build complete accepted-input context before selecting an event-specific observation shape.
[[nodiscard]] trace::RuntimeTraceFields input_fields() {
  trace::RuntimeTraceFields fields;
  fields.source = source();
  fields.admission_ordinal = ordinal<model::AdmissionOrdinal>(7U);
  fields.turn_ordinal = ordinal<model::TurnOrdinal>(3U);
  fields.session_epoch = model::SessionEpoch{2U};
  fields.source_sequence = model::SequenceNumber{41U};
  fields.receive_sequence = ordinal<model::ReceiveSequence>(9U);
  fields.metadata_revision = revision<model::InstrumentMetadataRevision>(6U);
  fields.book_generation = ordinal<model::BookGeneration>(1U);
  fields.book_revision = ordinal<model::BookRevision>(4U);
  fields.input_disposition = trace::RuntimeInputDisposition::DeltaApplied;
  fields.state = trace::RuntimeMarketState::Ready;
  return fields;
}

// --------------------------------------------------------
// Envelope/control dispositions retain trusted source, attempt, session, and receive identity only.
[[nodiscard]] trace::RuntimeTraceFields
envelope_disposition_fields(trace::RuntimeInputDisposition disposition,
                            trace::RuntimeMarketState state) {
  auto fields = input_fields();
  fields.source_sequence.reset();
  fields.metadata_revision.reset();
  fields.input_disposition = disposition;
  fields.state = state;
  return fields;
}

// --------------------------------------------------------
// A rejected-admission fence has an attempt and owner turn but no accepted envelope sequence.
[[nodiscard]] trace::RuntimeTraceFields source_discontinuity_fields() {
  auto fields = input_fields();
  fields.session_epoch.reset();
  fields.source_sequence.reset();
  fields.receive_sequence.reset();
  fields.metadata_revision.reset();
  fields.input_disposition = trace::RuntimeInputDisposition::SourceDiscontinuity;
  fields.state = trace::RuntimeMarketState::Invalid;
  return fields;
}

// --------------------------------------------------------
// Convert accepted-input context into a complete post-commit market callback observation.
[[nodiscard]] trace::RuntimeTraceFields market_callback_fields() {
  auto fields = input_fields();
  fields.callback_ordinal = ordinal<model::CallbackOrdinal>(2U);
  fields.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  fields.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  fields.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  fields.best_bid = price(6'500'000, 2U);
  fields.best_ask = price(6'500'050, 2U);
  return fields;
}

// --------------------------------------------------------
// Build an attributed Ready transition for both owner and strategy state observations.
[[nodiscard]] trace::RuntimeTraceFields state_transition_fields(bool callback) {
  auto fields = input_fields();
  fields.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  fields.previous_state = trace::RuntimeMarketState::Synchronizing;
  if (callback) {
    fields.callback_ordinal = ordinal<model::CallbackOrdinal>(1U);
    fields.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
    fields.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  }
  return fields;
}

// --------------------------------------------------------
// Build the bookless initial Synchronizing publication before any source envelope is accepted.
[[nodiscard]] trace::RuntimeTraceFields initial_state_transition_fields(bool callback) {
  trace::RuntimeTraceFields fields;
  fields.source = source();
  fields.turn_ordinal = ordinal<model::TurnOrdinal>(1U);
  fields.state = trace::RuntimeMarketState::Synchronizing;
  if (callback) {
    fields.callback_ordinal = ordinal<model::CallbackOrdinal>(1U);
    fields.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
    fields.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  }
  return fields;
}

// --------------------------------------------------------
// Build owner-drive recursion evidence tied only to its active turn.
[[nodiscard]] trace::RuntimeTraceFields owner_reentry_fields() {
  trace::RuntimeTraceFields fields;
  fields.turn_ordinal = ordinal<model::TurnOrdinal>(3U);
  fields.failure_reason = trace::RuntimeTraceFailureReason::OwnerDriveReentry;
  return fields;
}

// --------------------------------------------------------
// Render canonical bytes as lowercase hexadecimal for exact golden comparison.
[[nodiscard]] std::string hexadecimal(std::span<const std::byte> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const std::byte value : bytes) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(digits[(byte >> 4U) & 0xfU]);
    result.push_back(digits[byte & 0xfU]);
  }
  return result;
}

// --------------------------------------------------------
// Render the fixed-width SHA-256 result without a terminator or locale dependence.
[[nodiscard]] std::string digest_hex(const model::Sha256Digest& digest) {
  const auto encoded = model::sha256_hex(digest);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Assigned enum values and five accepted shapes lock the public compatibility vocabulary.
TEST_CASE("runtime trace event vocabulary and shapes are explicit", "[trace][runtime][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Static values make accidental reordering a compile-time-visible schema break.
  static_assert(trace::runtime_trace_schema_version == 1U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeTraceEventKind::InputDisposition) == 1U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeTraceEventKind::ReentryDetected) == 5U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeInputDisposition::SessionReset) == 12U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeInputDisposition::StalenessChecked) ==
                13U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeInputDisposition::SourceDiscontinuity) ==
                14U);
  static_assert(
      static_cast<std::uint16_t>(trace::RuntimeInputDisposition::StructuralBookRejected) == 15U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeInputDisposition::SessionIgnored) == 16U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeMarketState::Synchronizing) == 1U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeMarketState::Ready) == 2U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeMarketState::Stale) == 3U);
  static_assert(static_cast<std::uint16_t>(trace::RuntimeMarketState::Invalid) == 4U);
  static_assert(!std::is_default_constructible_v<trace::RuntimeTraceSource>);
  static_assert(!std::is_default_constructible_v<trace::RuntimeTraceProvenance>);

  // ++++++++++++++++++++++++++++++++++++++++
  // Each event kind accepts exactly one complete representative shape in deterministic order.
  trace::RuntimeTraceSink sink{runtime_policy()};
  REQUIRE(sink.validate(trace::RuntimeTraceEventKind::InputDisposition, input_fields()));
  CHECK(sink.size() == 0U);
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition, input_fields()));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::MarketStateTransition,
                      state_transition_fields(false)));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::MarketCallback, market_callback_fields()));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::StateCallback, state_transition_fields(true)));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::ReentryDetected, owner_reentry_fields()));
  REQUIRE(sink.records().size() == 5U);
  CHECK(sink.records().front().ordinal().value() == 1U);
  CHECK(sink.records().back().ordinal().value() == 5U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Runtime initialization is the sole transition with no previous state or committed book.
  trace::RuntimeTraceSink initial{runtime_policy()};
  REQUIRE(initial.append(trace::RuntimeTraceEventKind::MarketStateTransition,
                         initial_state_transition_fields(false)));
  REQUIRE(initial.append(trace::RuntimeTraceEventKind::StateCallback,
                         initial_state_transition_fields(true)));

  // ++++++++++++++++++++++++++++++++++++++++
  // Nested owner drive from a callback retains that callback's exact attribution.
  auto callback_reentry = owner_reentry_fields();
  callback_reentry.callback_ordinal = ordinal<model::CallbackOrdinal>(4U);
  callback_reentry.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  callback_reentry.subscription_id =
      id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  trace::RuntimeTraceSink attributed_reentry{runtime_policy()};
  REQUIRE(attributed_reentry.append(trace::RuntimeTraceEventKind::ReentryDetected,
                                    std::move(callback_reentry)));

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Parsed updates, control envelopes, and rejected-admission fences have distinct context shapes.
TEST_CASE("runtime input dispositions enforce deterministic context profiles",
          "[trace][runtime][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Envelope-only failures and control turns retain receive identity without inventing parsed
  // sequence or metadata values.
  trace::RuntimeTraceSink sink{runtime_policy()};
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition,
                      envelope_disposition_fields(trace::RuntimeInputDisposition::MalformedRejected,
                                                  trace::RuntimeMarketState::Invalid)));
  REQUIRE(
      sink.append(trace::RuntimeTraceEventKind::InputDisposition,
                  envelope_disposition_fields(trace::RuntimeInputDisposition::UnsupportedRejected,
                                              trace::RuntimeMarketState::Invalid)));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition,
                      envelope_disposition_fields(trace::RuntimeInputDisposition::SessionReset,
                                                  trace::RuntimeMarketState::Synchronizing)));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition,
                      envelope_disposition_fields(trace::RuntimeInputDisposition::StalenessChecked,
                                                  trace::RuntimeMarketState::Ready)));
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition,
                      envelope_disposition_fields(trace::RuntimeInputDisposition::StalenessChecked,
                                                  trace::RuntimeMarketState::Stale)));

  // ++++++++++++++++++++++++++++++++++++++++
  // A source discontinuity has no accepted envelope identity, while structural book rejection
  // remains bound to the complete normalized update that failed atomically.
  REQUIRE(
      sink.append(trace::RuntimeTraceEventKind::InputDisposition, source_discontinuity_fields()));
  auto structural_rejection = input_fields();
  structural_rejection.input_disposition = trace::RuntimeInputDisposition::StructuralBookRejected;
  structural_rejection.state = trace::RuntimeMarketState::Invalid;
  REQUIRE(
      sink.append(trace::RuntimeTraceEventKind::InputDisposition, std::move(structural_rejection)));

  // ++++++++++++++++++++++++++++++++++++++++
  // Sanitized state records may retain the same envelope-only context without exposing raw input.
  auto state_transition = envelope_disposition_fields(
      trace::RuntimeInputDisposition::MalformedRejected, trace::RuntimeMarketState::Invalid);
  state_transition.input_disposition = trace::RuntimeInputDisposition::Unspecified;
  state_transition.previous_state = trace::RuntimeMarketState::Ready;
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::MarketStateTransition, state_transition));
  state_transition.callback_ordinal = ordinal<model::CallbackOrdinal>(3U);
  state_transition.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
  state_transition.subscription_id =
      id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::StateCallback, std::move(state_transition)));
  CHECK(sink.size() == 9U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A rejection before the first snapshot carries neither half nor a fabricated whole book ID.
  auto pre_snapshot_discontinuity = source_discontinuity_fields();
  pre_snapshot_discontinuity.book_generation.reset();
  pre_snapshot_discontinuity.book_revision.reset();
  trace::RuntimeTraceSink pre_snapshot{runtime_policy()};
  REQUIRE(pre_snapshot.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(pre_snapshot_discontinuity)));

  // ++++++++++++++++++++++++++++++++++++++++
  // Unchanged-state rows remain traceable outside Ready without pretending that freshness changed.
  trace::RuntimeTraceSink unchanged{runtime_policy()};
  auto stale_duplicate = input_fields();
  stale_duplicate.input_disposition = trace::RuntimeInputDisposition::ExactDuplicateIgnored;
  stale_duplicate.state = trace::RuntimeMarketState::Stale;
  REQUIRE(
      unchanged.append(trace::RuntimeTraceEventKind::InputDisposition, std::move(stale_duplicate)));
  auto invalid_duplicate = input_fields();
  invalid_duplicate.input_disposition = trace::RuntimeInputDisposition::ExactDuplicateIgnored;
  invalid_duplicate.state = trace::RuntimeMarketState::Invalid;
  REQUIRE(unchanged.append(trace::RuntimeTraceEventKind::InputDisposition,
                           std::move(invalid_duplicate)));
  REQUIRE(
      unchanged.append(trace::RuntimeTraceEventKind::InputDisposition,
                       envelope_disposition_fields(trace::RuntimeInputDisposition::SessionIgnored,
                                                   trace::RuntimeMarketState::Ready)));
  REQUIRE(
      unchanged.append(trace::RuntimeTraceEventKind::InputDisposition,
                       envelope_disposition_fields(trace::RuntimeInputDisposition::StalenessChecked,
                                                   trace::RuntimeMarketState::Synchronizing)));
  REQUIRE(
      unchanged.append(trace::RuntimeTraceEventKind::InputDisposition,
                       envelope_disposition_fields(trace::RuntimeInputDisposition::StalenessChecked,
                                                   trace::RuntimeMarketState::Invalid)));

  // ++++++++++++++++++++++++++++++++++++++++
  // Mixing context profiles is rejected before any malformed combination reaches the prefix.
  trace::RuntimeTraceSink rejected{runtime_policy()};
  auto malformed_with_parsed_context = input_fields();
  malformed_with_parsed_context.input_disposition =
      trace::RuntimeInputDisposition::MalformedRejected;
  malformed_with_parsed_context.state = trace::RuntimeMarketState::Invalid;
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(malformed_with_parsed_context)));

  auto stale_with_parsed_context = input_fields();
  stale_with_parsed_context.input_disposition = trace::RuntimeInputDisposition::StalenessChecked;
  stale_with_parsed_context.state = trace::RuntimeMarketState::Stale;
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(stale_with_parsed_context)));

  auto discontinuity_with_receive = source_discontinuity_fields();
  discontinuity_with_receive.receive_sequence = ordinal<model::ReceiveSequence>(10U);
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(discontinuity_with_receive)));

  auto structural_without_metadata = input_fields();
  structural_without_metadata.input_disposition =
      trace::RuntimeInputDisposition::StructuralBookRejected;
  structural_without_metadata.state = trace::RuntimeMarketState::Invalid;
  structural_without_metadata.metadata_revision.reset();
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(structural_without_metadata)));

  auto state_without_attempt = state_transition_fields(false);
  state_without_attempt.admission_ordinal.reset();
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::MarketStateTransition,
                              std::move(state_without_attempt)));

  auto stale_without_book = input_fields();
  stale_without_book.input_disposition = trace::RuntimeInputDisposition::ExactDuplicateIgnored;
  stale_without_book.state = trace::RuntimeMarketState::Stale;
  stale_without_book.book_generation.reset();
  stale_without_book.book_revision.reset();
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(stale_without_book)));

  auto revision_before_generation = input_fields();
  revision_before_generation.book_generation = ordinal<model::BookGeneration>(5U);
  revision_before_generation.book_revision = ordinal<model::BookRevision>(4U);
  CHECK_FALSE(rejected.append(trace::RuntimeTraceEventKind::InputDisposition,
                              std::move(revision_before_generation)));

  // ++++++++++++++++++++++++++++++++++++++++
  // A source proof minted under another registry cannot enter this policy-bound stream, even when
  // the venue/instrument tuple and one-based position happen to match.
  const auto foreign_policy = make_runtime_policy(256U, "source.deribit-btc-perpetual-secondary");
  auto foreign_source = input_fields();
  foreign_source.source =
      trace::RuntimeTraceSource::from_runtime_source(foreign_policy.sources().front());
  const auto foreign_validation =
      rejected.validate(trace::RuntimeTraceEventKind::InputDisposition, foreign_source);
  REQUIRE_FALSE(foreign_validation);
  CHECK(foreign_validation.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                     "runtime_trace.source"));

  // ++++++++++++++++++++++++++++++++++++++++
  // Transition and state-callback evidence share the exact ADR destination/context matrix.
  const auto reject_transition_and_callback = [&](trace::RuntimeTraceFields fields) {
    CHECK_FALSE(rejected.validate(trace::RuntimeTraceEventKind::MarketStateTransition, fields));
    fields.callback_ordinal = ordinal<model::CallbackOrdinal>(8U);
    fields.bot_id = id<model::BotId>("bot.deribit-btc-perpetual-reference");
    fields.subscription_id = id<model::SubscriptionId>("subscription.deribit-btc-perpetual-book");
    CHECK_FALSE(rejected.validate(trace::RuntimeTraceEventKind::StateCallback, fields));
  };

  auto synchronizing_to_stale_update = state_transition_fields(false);
  synchronizing_to_stale_update.state = trace::RuntimeMarketState::Stale;
  reject_transition_and_callback(std::move(synchronizing_to_stale_update));

  auto ready_to_synchronizing_update = state_transition_fields(false);
  ready_to_synchronizing_update.previous_state = trace::RuntimeMarketState::Ready;
  ready_to_synchronizing_update.state = trace::RuntimeMarketState::Synchronizing;
  reject_transition_and_callback(std::move(ready_to_synchronizing_update));

  auto synchronizing_to_invalid_owner = initial_state_transition_fields(false);
  synchronizing_to_invalid_owner.previous_state = trace::RuntimeMarketState::Synchronizing;
  synchronizing_to_invalid_owner.state = trace::RuntimeMarketState::Invalid;
  reject_transition_and_callback(std::move(synchronizing_to_invalid_owner));
  CHECK(rejected.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Shape validation prevents partial or non-Ready callback observations from entering the prefix.
TEST_CASE("runtime trace rejects ambiguous fixed-field combinations", "[trace][runtime][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // A market callback cannot claim a stale or crossed book.
  trace::RuntimeTraceSink sink{runtime_policy()};
  auto stale_callback = market_callback_fields();
  stale_callback.state = trace::RuntimeMarketState::Stale;
  const auto stale =
      sink.append(trace::RuntimeTraceEventKind::MarketCallback, std::move(stale_callback));
  REQUIRE_FALSE(stale);
  CHECK(stale.error().context.field == "runtime_trace.market_callback");

  auto crossed_book = market_callback_fields();
  crossed_book.best_ask = crossed_book.best_bid;
  const auto crossed =
      sink.append(trace::RuntimeTraceEventKind::MarketCallback, std::move(crossed_book));
  REQUIRE_FALSE(crossed);
  CHECK(crossed.error().context.field == "runtime_trace.market_callback");

  // ++++++++++++++++++++++++++++++++++++++++
  // One-sided books remain coherent and expose only the side that is actually retained.
  trace::RuntimeTraceSink one_sided{runtime_policy()};
  auto bid_only = market_callback_fields();
  bid_only.best_ask.reset();
  REQUIRE(one_sided.append(trace::RuntimeTraceEventKind::MarketCallback, std::move(bid_only)));
  auto ask_only = market_callback_fields();
  ask_only.best_bid.reset();
  REQUIRE(one_sided.append(trace::RuntimeTraceEventKind::MarketCallback, std::move(ask_only)));

  // ++++++++++++++++++++++++++++++++++++++++
  // Every present side must still name a strictly positive retained level.
  auto zero_bid = market_callback_fields();
  zero_bid.best_bid = price(0, 0U);
  CHECK_FALSE(sink.validate(trace::RuntimeTraceEventKind::MarketCallback, zero_bid));
  auto negative_ask = market_callback_fields();
  negative_ask.best_ask = price(-1, 0U);
  CHECK_FALSE(sink.validate(trace::RuntimeTraceEventKind::MarketCallback, negative_ask));

  // Unknown event values and enum representations fail without consuming an ordinal.
  const auto unknown_kind =
      sink.append(static_cast<trace::RuntimeTraceEventKind>(999U), market_callback_fields());
  REQUIRE_FALSE(unknown_kind);
  CHECK(unknown_kind.error().context.field == "runtime_trace.kind");

  auto unknown_state = market_callback_fields();
  unknown_state.state = static_cast<trace::RuntimeMarketState>(999U);
  const auto unknown_enum =
      sink.append(trace::RuntimeTraceEventKind::MarketCallback, std::move(unknown_state));
  REQUIRE_FALSE(unknown_enum);
  CHECK(unknown_enum.error().context.field == "runtime_trace.enum");
  CHECK(sink.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preflight and append failures preserve the exact accepted prefix and never consume ordinals.
TEST_CASE("runtime trace capacity is preflighted without overwrite", "[trace][runtime][unit]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // The smallest valid single-grant policy fixes capacity six, and the sink derives that exact
  // bound rather than accepting an independently authored value.
  const auto bounded_policy = make_runtime_policy(6U);
  trace::RuntimeTraceSink sink{bounded_policy};
  CHECK(sink.capacity() == 6U);
  REQUIRE(sink.preflight(6U));
  const auto empty_bytes = sink.canonical_bytes();
  const auto empty_digest = sink.digest();
  REQUIRE(empty_bytes);
  REQUIRE(empty_digest);
  const auto oversized_preflight = sink.preflight(7U);
  REQUIRE_FALSE(oversized_preflight);
  CHECK(oversized_preflight.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded,
                                     "runtime_trace.records", 6U));
  CHECK(sink.canonical_bytes().value() == empty_bytes.value());
  CHECK(sink.digest().value() == empty_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // A partial prefix exposes the same first unavailable index while retaining every accepted byte.
  REQUIRE(sink.append(trace::RuntimeTraceEventKind::InputDisposition, input_fields()));
  const auto failed_preflight = sink.preflight(6U);
  REQUIRE_FALSE(failed_preflight);
  CHECK(failed_preflight.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded,
                                     "runtime_trace.records", 6U));
  for (std::uint32_t index = 1U; index < sink.capacity(); ++index) {
    REQUIRE(sink.append(trace::RuntimeTraceEventKind::ReentryDetected, owner_reentry_fields()));
  }
  const auto prefix_bytes = sink.canonical_bytes();
  const auto prefix_digest = sink.digest();
  REQUIRE(prefix_bytes);
  REQUIRE(prefix_digest);

  // ++++++++++++++++++++++++++++++++++++++++
  // A full sink gives capacity deterministic precedence and retains every accepted byte and
  // ordinal.
  const auto rejected =
      sink.append(trace::RuntimeTraceEventKind::MarketCallback, trace::RuntimeTraceFields{});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() ==
        model::DomainError::at_index(model::DomainErrorCode::TraceCapacityExceeded,
                                     "runtime_trace.records", 6U));
  CHECK(sink.size() == 6U);
  CHECK(sink.records().back().ordinal().value() == 6U);
  REQUIRE(sink.canonical_bytes());
  REQUIRE(sink.digest());
  CHECK(sink.canonical_bytes().value() == prefix_bytes.value());
  CHECK(sink.digest().value() == prefix_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Callback and re-entry records lock magic, tags, optional markers, exact prices, and hash.
TEST_CASE("runtime trace has stable canonical bytes", "[trace][runtime][golden]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Independently constructed sinks must produce identical records and digest identity.
  trace::RuntimeTraceSink first{runtime_policy()};
  trace::RuntimeTraceSink second{runtime_policy()};
  REQUIRE(first.append(trace::RuntimeTraceEventKind::MarketCallback, market_callback_fields()));
  REQUIRE(second.append(trace::RuntimeTraceEventKind::MarketCallback, market_callback_fields()));
  REQUIRE(first.append(trace::RuntimeTraceEventKind::ReentryDetected, owner_reentry_fields()));
  REQUIRE(second.append(trace::RuntimeTraceEventKind::ReentryDetected, owner_reentry_fields()));
  const auto first_bytes = first.canonical_bytes();
  const auto second_bytes = second.canonical_bytes();
  const auto first_digest = first.digest();
  const auto second_digest = second.digest();
  REQUIRE(first_bytes);
  REQUIRE(second_bytes);
  REQUIRE(first_digest);
  REQUIRE(second_digest);
  REQUIRE(first.records().size() == second.records().size());
  CHECK(first.records().front() == second.records().front());
  CHECK(first.records().back() == second.records().back());
  CHECK(first_bytes.value() == second_bytes.value());
  CHECK(first_digest.value() == second_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // The whole two-record stream is a golden vector, not a reconstruction using production tags.
  const std::string expected_bytes =
      "4145474953525453000100000002000001e84145474953525452000100010000000800000000000000"
      "01000200000002000300030000000901000000000000000700040000000901000000000000000300"
      "050000000901000000000000000200060000000901000000000000000100100000000c010000000764"
      "65726962697400110000001601000000114254432d5553442d50455250455455414c00120000001201"
      "0000000d4254432d50455250455455414c0013000000280100000023626f742e646572696269742d62"
      "74632d70657270657475616c2d7265666572656e636500140000002c01000000277375627363726970"
      "74696f6e2e646572696269742d6274632d70657270657475616c2d626f6f6b001800000020e869459e"
      "338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310001900000020a33466efc85d31"
      "d1f3e413259910da4ed420a23b91b225265f70d99cee295bfc00200000000901000000000000000200"
      "2100000009010000000000000029002200000009010000000000000009002300000009010000000000"
      "0000060024000000090100000000000000010025000000090100000000000000040030000000020000"
      "00310000000200000032000000020002003300000002000000400000000a01000000000000fde80000"
      "410000000a01000000000009eb15010000010b41454749535254520001000100000008000000000000"
      "0002000200000002000500030000000100000400000009010000000000000003000500000001000006"
      "0000000100001000000001000011000000010000120000000100001300000001000014000000010000"
      "1800000020e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310001900000"
      "020a33466efc85d31d1f3e413259910da4ed420a23b91b225265f70d99cee295bfc00200000000100"
      "0021000000010000220000000100002300000001000024000000010000250000000100003000000002"
      "00000031000000020000003200000002000000330000000200010040000000010000410000000100";
  const std::string expected_digest =
      "0a057770c0ae1ae133303c7780dfffc5b604539bdf632dd14a2063e631de310c";
  CHECK(hexadecimal(first_bytes.value()) == expected_bytes);
  CHECK(digest_hex(first_digest.value()) == expected_digest);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
