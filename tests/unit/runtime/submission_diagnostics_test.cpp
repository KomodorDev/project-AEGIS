// Purpose: prove assigned M3 diagnostic profiles, exact provenance, bounded prefix retention,
// re-entry aggregation, and saturation accounting independent from canonical outcomes.

#include "aegis/runtime/submission_diagnostics.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Construct checked one-based identities used by deterministic diagnostic profiles.
template <typename Identity> [[nodiscard]] Identity identity(std::uint64_t value) {
  auto result = Identity::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid identity in submission diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build visually distinct fixed-width provenance identities without policy wrapper dependencies.
[[nodiscard]] model::Sha256Digest digest(std::byte value) {
  model::Sha256Digest result{};
  result.fill(value);
  return result;
}

// --------------------------------------------------------
// Produce the required callback-bound identity shared by ordinary M3 diagnostic profiles.
[[nodiscard]] runtime::SubmissionDiagnosticFields attempt_fields() {
  runtime::SubmissionDiagnosticFields fields;
  fields.attempt_id = identity<model::SubmissionAttemptId>(3U);
  fields.owner_turn_ordinal = identity<model::TurnOrdinal>(5U);
  fields.callback_ordinal = identity<model::CallbackOrdinal>(7U);
  return fields;
}

// --------------------------------------------------------
// Assigned profiles retain exact raw provenance and reject partial or unassigned field shapes.
TEST_CASE("submission diagnostics enforce fixed profiles and provenance",
          "[runtime][diagnostics][submission][m3]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Persisted kind assignments and raw digest identity are compatibility boundaries.
  static_assert(static_cast<std::uint16_t>(
                    runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded) == 1U);
  static_assert(
      static_cast<std::uint16_t>(runtime::SubmissionDiagnosticKind::MeasurementUnavailable) == 6U);

  const runtime::SubmissionDiagnosticProvenance provenance{
      digest(std::byte{0x11U}), digest(std::byte{0x22U}), digest(std::byte{0x33U})};
  runtime::SubmissionDiagnosticSink sink{provenance, 8U};
  CHECK(sink.provenance() == provenance);

  // ++++++++++++++++++++++++++++++++++++++++
  // Evidence exhaustion has one exact pre-order profile and validates without consuming a slot.
  auto evidence = attempt_fields();
  evidence.stage = execution::SubmissionStage::Evidence;
  evidence.reason = execution::SubmissionReason::EvidenceCapacityExceeded;
  REQUIRE(sink.validate(runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded, evidence));
  CHECK(sink.size() == 0U);
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded, evidence));

  // ++++++++++++++++++++++++++++++++++++++++
  // Re-entry may report the identities already present on the active outer attempt and coalesce an
  // exact occurrence count without creating a second canonical trace record.
  auto reentry = attempt_fields();
  reentry.stage = execution::SubmissionStage::Context;
  reentry.reason = execution::SubmissionReason::SubmissionReentry;
  reentry.occurrence_count = 4U;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::ReentryDetected, reentry));
  auto repeated_reentry = reentry;
  repeated_reentry.occurrence_count = std::numeric_limits<std::uint64_t>::max() - 3U;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::ReentryDetected, repeated_reentry));
  repeated_reentry.occurrence_count = 1U;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::ReentryDetected, repeated_reentry));

  auto measurement = attempt_fields();
  measurement.stage = execution::SubmissionStage::Risk;
  measurement.reason = execution::SubmissionReason::SingleOrderQuantityExceeded;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::MeasurementUnavailable, measurement));

  CHECK(sink.accepted_count() == 3U);
  REQUIRE(sink.at(0U) != nullptr);
  CHECK(sink.at(0U)->ordinal == 1U);
  CHECK(sink.at(0U)->provenance == provenance);
  REQUIRE(sink.at(1U) != nullptr);
  CHECK(sink.at(1U)->fields.occurrence_count == std::numeric_limits<std::uint64_t>::max());

  // ++++++++++++++++++++++++++++++++++++++++
  // Missing identity dependencies, zero counts, and unknown enum values never mutate the prefix.
  auto partial = evidence;
  partial.callback_ordinal.reset();
  CHECK_FALSE(sink.append(runtime::SubmissionDiagnosticKind::EvidenceCapacityExceeded, partial));
  auto zero_occurrence = reentry;
  zero_occurrence.occurrence_count = 0U;
  CHECK_FALSE(sink.append(runtime::SubmissionDiagnosticKind::ReentryDetected, zero_occurrence));
  CHECK_FALSE(sink.append(static_cast<runtime::SubmissionDiagnosticKind>(999U), reentry));
  CHECK(sink.accepted_count() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Release, uncertainty, and saturation preserve the exact conservative accounting observations.
TEST_CASE("submission diagnostics retain release and uncertainty observations before saturation",
          "[runtime][diagnostics][submission][m3]") {
  const runtime::SubmissionDiagnosticProvenance provenance{
      digest(std::byte{0x41U}), digest(std::byte{0x42U}), digest(std::byte{0x43U})};
  runtime::SubmissionDiagnosticSink sink{provenance, 2U};

  // ++++++++++++++++++++++++++++++++++++++++
  // Exact release evidence requires the complete attempt/order/reservation identity chain.
  auto released = attempt_fields();
  auto order_provider_result = model::DeterministicOrderIdProvider::create(
      model::OrderNamespace{std::array<std::uint8_t, model::OrderNamespace::byte_size>{}});
  REQUIRE(order_provider_result);
  auto order_provider = std::move(order_provider_result).value();
  auto order_result = order_provider.next();
  REQUIRE(order_result);
  released.order_id = std::move(order_result).value();
  released.reservation_id = identity<model::ReservationId>(3U);
  released.stage = execution::SubmissionStage::Encoding;
  released.reason = execution::SubmissionReason::EncodingFailed;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::ReservationReleased, released));

  // ++++++++++++++++++++++++++++++++++++++++
  // Uncertain acceptance retains the same complete identity under the assigned initiation reason.
  auto retained = released;
  retained.stage = execution::SubmissionStage::Initiation;
  retained.reason = execution::SubmissionReason::InitiationOutcomeUnknown;
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::UnknownExposureRetained, retained));
  CHECK(sink.saturated());
  CHECK(sink.dropped_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Later valid telemetry is dropped explicitly; invalid telemetry is neither retained nor counted.
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::UnknownExposureRetained, retained));
  REQUIRE(sink.append(runtime::SubmissionDiagnosticKind::UnknownExposureRetained, retained));
  CHECK(sink.size() == 2U);
  CHECK(sink.accepted_count() == 2U);
  CHECK(sink.dropped_count() == 2U);
  REQUIRE(sink.at(0U) != nullptr);
  REQUIRE(sink.at(1U) != nullptr);
  CHECK(sink.at(0U)->kind == runtime::SubmissionDiagnosticKind::ReservationReleased);
  CHECK(sink.at(1U)->kind == runtime::SubmissionDiagnosticKind::UnknownExposureRetained);
  CHECK(sink.at(2U) == nullptr);

  auto invalid = retained;
  invalid.reservation_id.reset();
  REQUIRE_FALSE(sink.append(runtime::SubmissionDiagnosticKind::UnknownExposureRetained, invalid));
  CHECK(sink.dropped_count() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
