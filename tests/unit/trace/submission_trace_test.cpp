// Purpose: prove the exact M3 AEGISSUP policy bytes and bounded AEGISSTS event shapes, causal
// sequences, re-entry exception, conservative outcomes, and deterministic replay identity.

#include "aegis/execution/submission_policy.hpp"
#include "aegis/trace/submission_trace.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Fail fixture construction before an invalid identifier can obscure trace behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in submission trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Construct checked one-based identities and revisions used by canonical fixtures.
template <typename Identity> [[nodiscard]] Identity identity(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid identity in submission trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Construct exact nominal decimals without binary floating point in fixture code.
template <typename Decimal>
[[nodiscard]] Decimal decimal(std::int64_t coefficient, std::uint8_t scale) {
  auto result = Decimal::from_scaled(coefficient, scale);
  if (!result) {
    throw std::logic_error{"invalid decimal in submission trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build visually distinct fixed-width canonical provenance values.
[[nodiscard]] model::Sha256Digest digest(std::byte value) {
  model::Sha256Digest result{};
  result.fill(value);
  return result;
}

// --------------------------------------------------------
// Mint one trusted collision-safe local order identity for cumulative evidence fixtures.
[[nodiscard]] model::OrderId order_id() {
  model::OrderNamespace::Bytes bytes{};
  bytes.fill(0xabU);
  auto provider_result = model::DeterministicOrderIdProvider::create(model::OrderNamespace{bytes});
  if (!provider_result) {
    throw std::logic_error{"invalid order provider in submission trace test"};
  }
  auto provider = std::move(provider_result).value();
  auto order_result = provider.next();
  if (!order_result) {
    throw std::logic_error{"exhausted order provider in submission trace test"};
  }
  return std::move(order_result).value();
}

// --------------------------------------------------------
// Render canonical bytes as fixed lowercase hexadecimal for exact golden assertions.
[[nodiscard]] std::string hexadecimal(std::span<const std::byte> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const std::byte value : bytes) {
    const auto byte = std::to_integer<unsigned int>(value);
    result.push_back(digits[(byte >> 4U) & 0x0fU]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

// --------------------------------------------------------
// Render a raw digest with the repository's canonical lowercase hexadecimal helper.
[[nodiscard]] std::string digest_hex(const model::Sha256Digest& value) {
  const auto encoded = model::sha256_hex(value);
  return std::string{encoded.begin(), encoded.end()};
}

// --------------------------------------------------------
// Build one canonical encoder script whose authored override order must be normalized.
[[nodiscard]] execution::FakeEncoderScript encoder_script(std::uint64_t maximum = 4U) {
  auto result = execution::FakeEncoderScript::create(
      execution::FakeEncodingAction::Encode, maximum,
      {{3U, execution::FakeEncodingAction::Fail}, {2U, execution::FakeEncodingAction::Encode}});
  if (!result) {
    throw std::logic_error{"invalid encoder script in submission trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build one canonical initiator script with the same normalized ordinal order.
[[nodiscard]] execution::FakeInitiatorScript initiator_script(std::uint64_t maximum = 4U) {
  auto result = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum,
      {{4U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost},
       {2U, execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance}});
  if (!result) {
    throw std::logic_error{"invalid initiator script in submission trace test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Assemble a fresh complete policy parameter object for success and fail-closed mutations.
[[nodiscard]] execution::SubmissionPolicyParams policy_params() {
  return execution::SubmissionPolicyParams{
      execution::SubmissionCapability::DeterministicFakeOnly,
      digest(std::byte{0x11U}),
      digest(std::byte{0x22U}),
      digest(std::byte{0x33U}),
      identity<model::RiskPolicyRevision>(9U),
      execution::SubmissionPolicyCapacities{4U, 4U, 4U, 512U, 4U, 44U, 7U},
      333U,
      encoder_script(),
      initiator_script(),
  };
}

// --------------------------------------------------------
// Bind every trace record to one complete immutable startup and M2/M3 policy identity.
[[nodiscard]] trace::SubmissionTraceProvenance provenance() {
  return trace::SubmissionTraceProvenance{
      digest(std::byte{0x11U}),
      identity<model::ConfigurationRevision>(2U),
      identity<model::OrganizationRevision>(3U),
      identity<model::RouteRevision>(4U),
      digest(std::byte{0x22U}),
      digest(std::byte{0x33U}),
      identity<model::RiskPolicyRevision>(9U),
      digest(std::byte{0x44U}),
  };
}

// --------------------------------------------------------
// Construct one bot-bound outer-attempt context with exact caller economics and runtime
// attribution.
[[nodiscard]] trace::SubmissionTraceContext context(std::uint64_t attempt = 1U) {
  return trace::SubmissionTraceContext{
      identity<model::SubmissionAttemptId>(attempt),
      identity<model::TurnOrdinal>(7U),
      identity<model::CallbackOrdinal>(5U),
      1'234'567U,
      trace::SubmissionTraceAttribution{
          id<model::FirmId>("firm.alpha"), id<model::DeskId>("desk.alpha"),
          id<model::BotId>("bot.alpha"), id<model::StrategyId>("strategy.alpha")},
      execution::OrderRequest{
          id<model::RouteId>("route.primary"), id<model::InstrumentId>("BTC-USD-PERPETUAL"),
          execution::OrderSide::Buy, execution::OrderType::Limit,
          execution::TimeInForce::GoodTilCancelled, decimal<model::Price>(6'500'050, 2U),
          decimal<model::Quantity>(10U, 0U)},
  };
}

// --------------------------------------------------------
// Copy the route/account/venue and metadata authority unlocked by route authorization.
[[nodiscard]] trace::AuthorizedSubmissionProjection projection() {
  return trace::AuthorizedSubmissionProjection{
      id<model::RouteId>("route.primary"),
      id<model::VenueId>("deribit"),
      id<model::LogicalAccountId>("account.alpha"),
      id<model::InstrumentId>("BTC-USD-PERPETUAL"),
      id<model::VenueInstrumentId>("BTC-PERPETUAL"),
      identity<model::InstrumentMetadataRevision>(6U),
  };
}

// --------------------------------------------------------
// Initialize every positional optional explicitly so strict aggregate warnings remain meaningful.
[[nodiscard]] trace::SubmissionTraceFields attempt_fields(std::uint64_t attempt = 1U) {
  return trace::SubmissionTraceFields{context(attempt), std::nullopt,
                                      std::nullopt,     std::nullopt,
                                      std::nullopt,     std::nullopt,
                                      std::nullopt,     std::nullopt,
                                      std::nullopt,     trace::SubmissionReleaseTransition::None,
                                      std::nullopt};
}

// --------------------------------------------------------
// Append the complete successful local fake-initiation sequence and return its final snapshot.
[[nodiscard]] trace::SubmissionTraceFields append_success(trace::SubmissionTraceSink& sink,
                                                          std::uint64_t attempt = 1U) {
  auto fields = attempt_fields(attempt);
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::Attempt, fields));
  fields.authorized_projection = projection();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::RouteAuthorized, fields));
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, fields));
  fields.order_id = order_id();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::IdentityGenerated, fields));
  fields.reservation_id = identity<model::ReservationId>(attempt);
  fields.approved_exposure =
      risk::OrderExposure{fields.context.request.quantity, decimal<model::Notional>(100U, 0U)};
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::RiskReserved, fields));
  fields.oms_state = oms::OutboundOrderState::PendingEncoding;
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::OmsAdmitted, fields));
  fields.oms_state = oms::OutboundOrderState::PendingInitiation;
  fields.encoding = trace::SubmissionEncodingEvidence{
      identity<model::EncoderInvocationOrdinal>(attempt), 311U, digest(std::byte{0x55U})};
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::Encoded, fields));
  fields.oms_state = oms::OutboundOrderState::WriteInitiated;
  fields.initiation =
      trace::SubmissionInitiationEvidence{identity<model::InitiatorInvocationOrdinal>(attempt),
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          identity<model::FakeWriteOrdinal>(attempt)};
  fields.release_transition = trace::SubmissionReleaseTransition::Retained;
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::WriteInitiated, fields));
  fields.final_result = trace::SubmissionFinalResult{execution::SubmitDisposition::WriteInitiated,
                                                     execution::SubmissionStage::Initiation,
                                                     execution::SubmissionReason::None};
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::SubmissionCompleted, fields));
  return fields;
}

// --------------------------------------------------------
// AEGISSUP locks every assigned byte, normalized script, capacity relationship, and fingerprint.
TEST_CASE("submission policy is exact and fails closed", "[execution][submission][policy][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Assigned vocabulary values and normalized override order are persisted compatibility rules.
  static_assert(execution::canonical_submission_policy_schema_version == 1U);
  static_assert(static_cast<std::uint8_t>(execution::SubmissionCapability::DeterministicFakeOnly) ==
                1U);
  static_assert(static_cast<std::uint8_t>(execution::FakeEncodingAction::Fail) == 2U);
  static_assert(
      static_cast<std::uint8_t>(execution::FakeInitiationOutcome::AcceptedThenOutcomeLost) == 3U);

  auto policy_result = execution::SubmissionPolicy::create(policy_params());
  REQUIRE(policy_result);
  const auto& policy = policy_result.value();
  REQUIRE(policy.encoder_script().overrides().size() == 2U);
  CHECK(policy.encoder_script().overrides().front().invocation_ordinal == 2U);
  REQUIRE(policy.initiator_script().overrides().size() == 2U);
  CHECK(policy.initiator_script().overrides().front().invocation_ordinal == 2U);
  CHECK(policy.required_encoded_order_bytes() == 333U);

  // ++++++++++++++++++++++++++++++++++++++++
  // This whole artifact is an external golden, including magic, widths, order, and script bytes.
  const std::string expected_bytes =
      "4145474953535550000101"
      "1111111111111111111111111111111111111111111111111111111111111111"
      "2222222222222222222222222222222222222222222222222222222222222222"
      "3333333333333333333333333333333333333333333333333333333333333333"
      "0000000000000009000000000000000400000004000000040200000000040000002c00000007"
      "0100000002000000000000000201000000000000000302"
      "0200000002000000000000000201000000000000000403";
  CHECK(hexadecimal(policy.canonical_bytes()) == expected_bytes);
  CHECK(policy.fingerprint().to_hex() ==
        "e89505b97cae8051d38537ad561c4e7588306593cf09e385f171592aff43d200");

  // ++++++++++++++++++++++++++++++++++++++++
  // Each invalid bound fails before publishing bytes, scripts, or a partially usable runtime.
  auto inverted = policy_params();
  inverted.capacities.reservation_capacity = 3U;
  auto inverted_result = execution::SubmissionPolicy::create(std::move(inverted));
  REQUIRE_FALSE(inverted_result);
  CHECK(inverted_result.error().code == model::DomainErrorCode::InvalidSubmissionPolicy);

  auto undersized = policy_params();
  undersized.required_encoded_order_bytes = 513U;
  auto undersized_result = execution::SubmissionPolicy::create(std::move(undersized));
  REQUIRE_FALSE(undersized_result);
  CHECK(undersized_result.error().context.field == "submission_policy.encoded_byte_capacity");

  auto mismatched_script = policy_params();
  mismatched_script.encoder_script = encoder_script(3U);
  auto mismatched_result = execution::SubmissionPolicy::create(std::move(mismatched_script));
  REQUIRE_FALSE(mismatched_result);
  CHECK(mismatched_result.error().context.field == "submission_policy.encoder_script");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// The initiated path retains exposure and records local initiation without any acknowledgement.
TEST_CASE("submission trace accepts the exact initiated sequence",
          "[trace][submission][sequence][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Numeric assignments lock the independently versioned AEGISSTS compatibility vocabulary.
  static_assert(trace::submission_trace_schema_version == 1U);
  static_assert(trace::maximum_submission_trace_records_per_attempt == 11U);
  static_assert(static_cast<std::uint16_t>(trace::SubmissionTraceEventKind::Attempt) == 1U);
  static_assert(static_cast<std::uint16_t>(trace::SubmissionTraceEventKind::SubmissionCompleted) ==
                16U);
  static_assert(static_cast<std::uint8_t>(trace::SubmissionReleaseTransition::Retained) == 2U);

  trace::SubmissionTraceSink sink{provenance(), 20U};
  const auto final_fields = append_success(sink);
  REQUIRE(sink.records().size() == 9U);
  CHECK(sink.records().front().ordinal().value() == 1U);
  CHECK(sink.records().back().ordinal().value() == 9U);
  CHECK(sink.records()[7U].kind() == trace::SubmissionTraceEventKind::WriteInitiated);
  CHECK(sink.records()[7U].fields().release_transition ==
        trace::SubmissionReleaseTransition::Retained);
  REQUIRE(final_fields.final_result);
  CHECK(final_fields.final_result->disposition == execution::SubmitDisposition::WriteInitiated);
  CHECK(final_fields.final_result->reason == execution::SubmissionReason::None);

  // ++++++++++++++++++++++++++++++++++++++++
  // A second attempt must advance identity while preserving the fixed callback/request fixture.
  const auto second = append_success(sink, 2U);
  CHECK(second.context.attempt_id.value() == 2U);
  CHECK(sink.size() == 18U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Schema one accepts only the five M3 OMS bytes even after the shared enum gains M4 lifecycle
// values; every rejection preserves the accepted evidence prefix.
TEST_CASE("submission trace rejects M4-only OMS lifecycle states",
          "[trace][submission][schema][m3][m4]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Establish the exact accepted prefix immediately before the M3 OMS-admission evidence row.
  trace::SubmissionTraceSink sink{provenance(), 12U};
  auto fields = attempt_fields();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::Attempt, fields));
  fields.authorized_projection = projection();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::RouteAuthorized, fields));
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, fields));
  fields.order_id = order_id();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::IdentityGenerated, fields));
  fields.reservation_id = identity<model::ReservationId>(1U);
  fields.approved_exposure =
      risk::OrderExposure{fields.context.request.quantity, decimal<model::Notional>(100U, 0U)};
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::RiskReserved, fields));
  REQUIRE(sink.size() == 5U);
  const auto accepted_prefix_bytes = sink.canonical_bytes();
  REQUIRE(accepted_prefix_bytes);

  // ++++++++++++++++++++++++++++++++++++++++
  // Each appended M4 assignment is meaningful to the OMS but absent from AEGISSTS schema one.
  constexpr std::array m4_states{
      oms::OutboundOrderState::Working,   oms::OutboundOrderState::PartiallyFilled,
      oms::OutboundOrderState::Filled,    oms::OutboundOrderState::ExchangeRejected,
      oms::OutboundOrderState::Cancelled, oms::OutboundOrderState::ReconciledAbsent};
  for (const auto state : m4_states) {
    fields.oms_state = state;
    const auto rejected = sink.append(trace::SubmissionTraceEventKind::OmsAdmitted, fields);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == model::DomainErrorCode::InvalidValue);
    CHECK(rejected.error().context.field == "submission_trace.fields");
    CHECK(sink.size() == 5U);
    const auto retained_prefix_bytes = sink.canonical_bytes();
    REQUIRE(retained_prefix_bytes);
    CHECK(retained_prefix_bytes.value() == accepted_prefix_bytes.value());
  }

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Definitive failures release once while uncertain acceptance retains conservative exposure.
TEST_CASE("submission trace distinguishes release and uncertainty branches",
          "[trace][submission][outcomes][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // OMS non-admission must transition through one explicit release before its final rejection.
  trace::SubmissionTraceSink oms_sink{provenance(), 16U};
  auto oms_fields = attempt_fields();
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::Attempt, oms_fields));
  oms_fields.authorized_projection = projection();
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::RouteAuthorized, oms_fields));
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, oms_fields));
  oms_fields.order_id = order_id();
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::IdentityGenerated, oms_fields));
  oms_fields.reservation_id = identity<model::ReservationId>(1U);
  oms_fields.approved_exposure =
      risk::OrderExposure{oms_fields.context.request.quantity, decimal<model::Notional>(100U, 0U)};
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::RiskReserved, oms_fields));
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::OmsNonAdmission, oms_fields));
  oms_fields.release_transition = trace::SubmissionReleaseTransition::Released;
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::ReservationReleased, oms_fields));
  oms_fields.final_result = trace::SubmissionFinalResult{
      execution::SubmitDisposition::LocallyRejected, execution::SubmissionStage::Oms,
      execution::SubmissionReason::DuplicateOrderIdentity};
  REQUIRE(oms_sink.append(trace::SubmissionTraceEventKind::SubmissionCompleted, oms_fields));

  // ++++++++++++++++++++++++++++++++++++++++
  // Skipping the release transition is a malformed causal sequence and cannot mutate the prefix.
  trace::SubmissionTraceSink skipped_release{provenance(), 16U};
  auto skipped = attempt_fields();
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::Attempt, skipped));
  skipped.authorized_projection = projection();
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::RouteAuthorized, skipped));
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::CanonicalValidated, skipped));
  skipped.order_id = order_id();
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::IdentityGenerated, skipped));
  skipped.reservation_id = identity<model::ReservationId>(1U);
  skipped.approved_exposure =
      risk::OrderExposure{skipped.context.request.quantity, decimal<model::Notional>(100U, 0U)};
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::RiskReserved, skipped));
  REQUIRE(skipped_release.append(trace::SubmissionTraceEventKind::OmsNonAdmission, skipped));
  skipped.final_result = trace::SubmissionFinalResult{
      execution::SubmitDisposition::LocallyRejected, execution::SubmissionStage::Oms,
      execution::SubmissionReason::OmsCapacityExceeded};
  REQUIRE_FALSE(
      skipped_release.append(trace::SubmissionTraceEventKind::SubmissionCompleted, skipped));
  CHECK(skipped_release.size() == 6U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Accepted-but-lost initiation keeps both reservation exposure and accepted-write evidence.
  trace::SubmissionTraceSink unknown_sink{provenance(), 16U};
  auto unknown = attempt_fields();
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::Attempt, unknown));
  unknown.authorized_projection = projection();
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::RouteAuthorized, unknown));
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, unknown));
  unknown.order_id = order_id();
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::IdentityGenerated, unknown));
  unknown.reservation_id = identity<model::ReservationId>(1U);
  unknown.approved_exposure =
      risk::OrderExposure{unknown.context.request.quantity, decimal<model::Notional>(100U, 0U)};
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::RiskReserved, unknown));
  unknown.oms_state = oms::OutboundOrderState::PendingEncoding;
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::OmsAdmitted, unknown));
  unknown.oms_state = oms::OutboundOrderState::PendingInitiation;
  unknown.encoding = trace::SubmissionEncodingEvidence{
      identity<model::EncoderInvocationOrdinal>(1U), 311U, digest(std::byte{0x55U})};
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::Encoded, unknown));
  unknown.oms_state = oms::OutboundOrderState::SubmissionUnknown;

  // ++++++++++++++++++++++++++++++++++++++++
  // Uncertainty is not a generic terminal label: it requires the exact accepted-then-lost outcome
  // and the accepted slot's write ordinal before it can enter canonical evidence.
  REQUIRE_FALSE(unknown_sink.append(trace::SubmissionTraceEventKind::SubmissionUnknown, unknown));
  unknown.initiation =
      trace::SubmissionInitiationEvidence{identity<model::InitiatorInvocationOrdinal>(1U),
                                          execution::FakeInitiationOutcome::AcceptedAndInitiated,
                                          identity<model::FakeWriteOrdinal>(1U)};
  unknown.release_transition = trace::SubmissionReleaseTransition::Retained;
  REQUIRE_FALSE(unknown_sink.append(trace::SubmissionTraceEventKind::SubmissionUnknown, unknown));
  unknown.initiation->outcome = execution::FakeInitiationOutcome::AcceptedThenOutcomeLost;
  unknown.initiation->accepted_write_ordinal.reset();
  REQUIRE_FALSE(unknown_sink.append(trace::SubmissionTraceEventKind::SubmissionUnknown, unknown));
  unknown.initiation =
      trace::SubmissionInitiationEvidence{identity<model::InitiatorInvocationOrdinal>(1U),
                                          execution::FakeInitiationOutcome::AcceptedThenOutcomeLost,
                                          identity<model::FakeWriteOrdinal>(1U)};
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::SubmissionUnknown, unknown));
  unknown.final_result = trace::SubmissionFinalResult{
      execution::SubmitDisposition::SubmissionUnknown, execution::SubmissionStage::Initiation,
      execution::SubmissionReason::InitiationOutcomeUnknown};
  REQUIRE(unknown_sink.append(trace::SubmissionTraceEventKind::SubmissionCompleted, unknown));
  CHECK(unknown_sink.records()[7U].fields().reservation_id.has_value());
  CHECK(unknown_sink.records()[7U].fields().release_transition ==
        trace::SubmissionReleaseTransition::Retained);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Risk evidence, malformed snapshots, and nested re-entry obey their exact exceptional profiles.
TEST_CASE("submission trace validates risk snapshots and the first nested reentry",
          "[trace][submission][validation][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // One risk limit rejection carries no reservation and matches its quantity measure at completion.
  trace::SubmissionTraceSink risk_sink{provenance(), 16U};
  auto fields = attempt_fields();
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::Attempt, fields));

  // ++++++++++++++++++++++++++++++++++++++++
  // The first nested request substitutes only request economics under the active outer identity.
  auto nested = fields;
  nested.context.request.price = decimal<model::Price>(6'500'100, 2U);
  nested.final_result = trace::SubmissionFinalResult{
      execution::SubmitDisposition::LocallyRejected, execution::SubmissionStage::Context,
      execution::SubmissionReason::SubmissionReentry};
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::ReentryRejected, nested));
  REQUIRE_FALSE(risk_sink.append(trace::SubmissionTraceEventKind::ReentryRejected, nested));
  CHECK(risk_sink.size() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Ordinary cumulative fields continue from the outer request, never from the nested request.
  fields.authorized_projection = projection();
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::RouteAuthorized, fields));
  auto changed_projection = fields;
  changed_projection.authorized_projection->venue_id = id<model::VenueId>("other");
  REQUIRE_FALSE(
      risk_sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, changed_projection));
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::CanonicalValidated, fields));
  fields.order_id = order_id();
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::IdentityGenerated, fields));
  fields.risk_rejection = execution::RiskLimitEvidence::quantity(risk::RiskScopeKind::Bot,
                                                                 decimal<model::Quantity>(11U, 0U),
                                                                 decimal<model::Quantity>(10U, 0U));
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::RiskRejected, fields));
  fields.final_result = trace::SubmissionFinalResult{
      execution::SubmitDisposition::LocallyRejected, execution::SubmissionStage::Risk,
      execution::SubmissionReason::SingleOrderQuantityExceeded};
  REQUIRE(risk_sink.append(trace::SubmissionTraceEventKind::SubmissionCompleted, fields));

  // ++++++++++++++++++++++++++++++++++++++++
  // The nested exception remains visible without altering the ordinary six-record risk sequence.
  REQUIRE(risk_sink.records().size() == 7U);
  CHECK(risk_sink.records()[1U].kind() == trace::SubmissionTraceEventKind::ReentryRejected);
  CHECK(risk_sink.records().back().fields().risk_rejection == fields.risk_rejection);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Preflight and full-sink failure preserve the accepted prefix and its exact canonical digest.
TEST_CASE("submission trace preflight and append failures never mutate evidence",
          "[trace][submission][capacity][m3]") {
  trace::SubmissionTraceSink sink{provenance(), 2U};

  // ++++++++++++++++++++++++++++++++++++++++
  // The complete eleven-slot attempt proof fails before assigning a record or ordinal.
  const auto preflight = sink.preflight(trace::maximum_submission_trace_records_per_attempt);
  REQUIRE_FALSE(preflight);
  CHECK(preflight.error() ==
        model::DomainError::at_index(model::DomainErrorCode::SubmissionEvidenceExhausted,
                                     "submission_trace.capacity", 2U));
  CHECK(sink.size() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // A complete route-rejection prefix fills exactly two slots.
  auto fields = attempt_fields();
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::Attempt, fields));
  fields.final_result = trace::SubmissionFinalResult{execution::SubmitDisposition::LocallyRejected,
                                                     execution::SubmissionStage::Route,
                                                     execution::SubmissionReason::RouteDisabled};
  REQUIRE(sink.append(trace::SubmissionTraceEventKind::SubmissionCompleted, fields));
  const auto prefix_bytes = sink.canonical_bytes();
  const auto prefix_digest = sink.digest();
  REQUIRE(prefix_bytes);
  REQUIRE(prefix_digest);

  // ++++++++++++++++++++++++++++++++++++++++
  // Capacity has deterministic precedence; even malformed later input leaves bytes unchanged.
  REQUIRE_FALSE(sink.append(static_cast<trace::SubmissionTraceEventKind>(999U), fields));
  CHECK(sink.size() == 2U);
  REQUIRE(sink.canonical_bytes());
  REQUIRE(sink.digest());
  CHECK(sink.canonical_bytes().value() == prefix_bytes.value());
  CHECK(sink.digest().value() == prefix_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Identical ordered inputs reproduce every record, positional byte, and SHA-256 identity exactly.
TEST_CASE("submission trace has deterministic canonical bytes", "[trace][submission][golden][m3]") {
  trace::SubmissionTraceSink first{provenance(), 16U};
  trace::SubmissionTraceSink second{provenance(), 16U};
  const auto first_fields = append_success(first);
  const auto second_fields = append_success(second);

  // ++++++++++++++++++++++++++++++++++++++++
  // Independent sinks with identical inputs retain structurally identical cumulative snapshots.
  REQUIRE(first_fields == second_fields);
  REQUIRE(first.records().size() == second.records().size());
  for (std::size_t index = 0U; index < first.records().size(); ++index) {
    CHECK(first.records()[index] == second.records()[index]);
  }
  const auto first_bytes = first.canonical_bytes();
  const auto second_bytes = second.canonical_bytes();
  const auto first_digest = first.digest();
  const auto second_digest = second.digest();
  REQUIRE(first_bytes);
  REQUIRE(second_bytes);
  REQUIRE(first_digest);
  REQUIRE(second_digest);
  CHECK(first_bytes.value() == second_bytes.value());
  CHECK(first_digest.value() == second_digest.value());

  // ++++++++++++++++++++++++++++++++++++++++
  // Magic/version/count and a whole-stream digest lock the positional AEGISSTS schema externally.
  REQUIRE(first_bytes.value().size() > 14U);
  CHECK(hexadecimal(std::span{first_bytes.value()}.first(14U)) == "4145474953535453000100000009");
  CHECK(digest_hex(first_digest.value()) ==
        "0d8f7287c0dacd71955595cc4a9370d4508eaa27f11e239c61133953ad9dd65f");

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
