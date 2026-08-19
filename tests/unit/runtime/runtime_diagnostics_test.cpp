// Purpose: prove fixed diagnostic profiles, immutable accepted-prefix retention, and explicit
// saturation accounting without making noncanonical detail alter canonical runtime behavior.

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/runtime/runtime_diagnostics.hpp"
#include "reference_configuration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

using namespace aegis;

// --------------------------------------------------------
// Parse identifiers with fixture failures kept separate from sink behavior.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in runtime diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Construct checked one-based ordinals used by deterministic diagnostic records.
template <typename Ordinal> [[nodiscard]] Ordinal ordinal(std::uint64_t value) {
  auto result = Ordinal::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid ordinal in runtime diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Build one sealed policy with a caller-selected diagnostic prefix capacity.
[[nodiscard]] runtime::RuntimePolicy runtime_policy(std::uint32_t diagnostic_capacity) {
  auto configuration_result =
      configuration::StartupConfiguration::create(test_support::reference_configuration_params());
  if (!configuration_result) {
    throw std::logic_error{"invalid configuration in runtime diagnostic test"};
  }
  runtime::RuntimePolicyParams params{
      runtime::RuntimePolicyLimits{8U, 4096U, 64U, 20U, 5'000'000'000U, 4U, diagnostic_capacity,
                                   64U, 32U, 100'000U},
      {{id<model::MarketSourceId>("source.deribit-btc-perpetual"), id<model::VenueId>("deribit"),
        id<model::InstrumentId>("BTC-USD-PERPETUAL"), id<model::VenueInstrumentId>("BTC-PERPETUAL"),
        model::InstrumentMetadataRevision::initial()}}};
  auto result = runtime::RuntimePolicy::create(configuration_result.value(), std::move(params));
  if (!result) {
    throw std::logic_error{"invalid runtime policy in runtime diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
// Populate one valid attributable-input diagnostic profile.
[[nodiscard]] runtime::RuntimeDiagnosticFields input_fields(std::uint32_t code) {
  runtime::RuntimeDiagnosticFields fields;
  fields.source_ordinal = ordinal<model::MarketSourceOrdinal>(1U);
  fields.admission_ordinal = ordinal<model::AdmissionOrdinal>(7U);
  fields.turn_ordinal = ordinal<model::TurnOrdinal>(4U);
  fields.detail_code = code;
  fields.observed_value = 12U;
  return fields;
}

// --------------------------------------------------------
// Assigned kinds accept only their exact context and measurement profiles.
TEST_CASE("runtime diagnostics enforce assigned fixed-field profiles",
          "[runtime][diagnostics][m2]") {

  // ++++++++++++++++++++++++++++++++++++++++
  // Persisted numeric assignments remain stable while policy provenance is retained exactly.
  static_assert(static_cast<std::uint16_t>(runtime::RuntimeDiagnosticKind::SourceDiscontinuity) ==
                1U);
  static_assert(static_cast<std::uint16_t>(runtime::RuntimeDiagnosticKind::EvidenceExhausted) ==
                8U);
  static_assert(
      static_cast<std::uint16_t>(runtime::RuntimeDiagnosticKind::CallbackClockRegression) == 9U);

  const auto policy = runtime_policy(8U);
  runtime::RuntimeDiagnosticSink sink{policy};
  CHECK(sink.configuration_fingerprint() == policy.configuration_fingerprint());
  CHECK(sink.runtime_policy_fingerprint() == policy.fingerprint());

  // ++++++++++++++++++++++++++++++++++++++++
  // Valid input, budget, and coalesced-reentry shapes append in accepted-prefix order.
  auto malformed = input_fields(3U);
  REQUIRE(sink.validate(runtime::RuntimeDiagnosticKind::MalformedInput, malformed));
  CHECK(sink.size() == 0U);
  CHECK(sink.accepted_count() == 0U);
  REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, malformed));

  runtime::RuntimeDiagnosticFields budget;
  budget.turn_ordinal = ordinal<model::TurnOrdinal>(4U);
  budget.callback_ordinal = ordinal<model::CallbackOrdinal>(2U);
  budget.observed_value = 101U;
  budget.limit_value = 100U;
  REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, budget));

  runtime::RuntimeDiagnosticFields dispatch_reentry;
  dispatch_reentry.turn_ordinal = ordinal<model::TurnOrdinal>(4U);
  dispatch_reentry.callback_ordinal = ordinal<model::CallbackOrdinal>(2U);
  dispatch_reentry.occurrence_count = 3U;
  REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::DispatchReentryDetected, dispatch_reentry));

  auto owner_reentry = dispatch_reentry;
  REQUIRE(sink.validate(runtime::RuntimeDiagnosticKind::OwnerReentryDetected, owner_reentry));

  // ++++++++++++++++++++++++++++++++++++++++
  // Accepted malformed and unsupported frames need admission/turn identity but may remain
  // unattributable; state/book diagnostics must never invent that missing source relationship.
  runtime::RuntimeDiagnosticSink unattributed_sink{policy};
  auto unattributed = input_fields(11U);
  unattributed.source_ordinal.reset();
  REQUIRE(unattributed_sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, unattributed));
  REQUIRE(unattributed_sink.append(runtime::RuntimeDiagnosticKind::UnsupportedInput, unattributed));
  CHECK(unattributed_sink.accepted_count() == 2U);
  CHECK_FALSE(unattributed_sink.append(runtime::RuntimeDiagnosticKind::StructuralBookRejected,
                                       unattributed));
  auto discontinuity = unattributed;
  discontinuity.detail_code = 0U;
  discontinuity.observed_value = 0U;
  CHECK_FALSE(
      unattributed_sink.append(runtime::RuntimeDiagnosticKind::SourceDiscontinuity, discontinuity));
  CHECK(unattributed_sink.accepted_count() == 2U);

  CHECK(sink.size() == 3U);
  CHECK(sink.accepted_count() == 3U);
  REQUIRE(sink.at(0U) != nullptr);
  CHECK(sink.at(0U)->kind == runtime::RuntimeDiagnosticKind::MalformedInput);

  // ++++++++++++++++++++++++++++++++++++++++
  // Partial, non-exceeded, empty-occurrence, unknown-source, and unknown-kind profiles fail
  // without changing the accepted prefix.
  auto partial = budget;
  partial.callback_ordinal.reset();
  CHECK_FALSE(sink.validate(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, partial));
  CHECK(sink.accepted_count() == 3U);
  CHECK_FALSE(sink.append(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, partial));
  auto nonexceeded = budget;
  nonexceeded.observed_value = nonexceeded.limit_value;
  CHECK_FALSE(sink.append(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, nonexceeded));
  auto zero_occurrences = dispatch_reentry;
  zero_occurrences.occurrence_count = 0U;
  CHECK_FALSE(
      sink.append(runtime::RuntimeDiagnosticKind::DispatchReentryDetected, zero_occurrences));
  owner_reentry.callback_ordinal.reset();
  CHECK_FALSE(sink.append(runtime::RuntimeDiagnosticKind::OwnerReentryDetected, owner_reentry));
  auto unknown_source = malformed;
  unknown_source.source_ordinal = ordinal<model::MarketSourceOrdinal>(2U);
  const auto unknown_source_result =
      sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, unknown_source);
  REQUIRE_FALSE(unknown_source_result);
  CHECK(unknown_source_result.error() ==
        model::DomainError::at_field(model::DomainErrorCode::RuntimeSourceNotConfigured,
                                     "runtime_diagnostic.source_ordinal"));
  CHECK_FALSE(sink.append(static_cast<runtime::RuntimeDiagnosticKind>(999U), malformed));
  CHECK(sink.accepted_count() == 3U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------
// Saturation preserves the earliest accepted evidence and treats only valid later observations as
// dropped.
TEST_CASE("runtime diagnostics preserve the first prefix and expose every dropped observation",
          "[runtime][diagnostics][m2]") {
  const auto policy = runtime_policy(2U);
  runtime::RuntimeDiagnosticSink sink{policy};

  // ++++++++++++++++++++++++++++++++++++++++
  // Filling the prefix exposes saturation before any otherwise-valid observation is dropped.
  for (std::uint32_t code = 1U; code <= 2U; ++code) {
    REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, input_fields(code)));
  }
  CHECK(sink.saturated());
  CHECK(sink.dropped_count() == 0U);

  // ++++++++++++++++++++++++++++++++++++++++
  // Later successful observations retain the first two and explicitly count both dropped details.
  for (std::uint32_t code = 3U; code <= 4U; ++code) {
    REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, input_fields(code)));
  }

  CHECK(sink.capacity() == 2U);
  CHECK(sink.size() == 2U);
  CHECK(sink.accepted_count() == 2U);
  CHECK(sink.saturated());
  CHECK(sink.dropped_count() == 2U);
  REQUIRE(sink.at(0U) != nullptr);
  REQUIRE(sink.at(1U) != nullptr);
  CHECK(sink.at(0U)->ordinal == 1U);
  CHECK(sink.at(0U)->fields.detail_code == 1U);
  CHECK(sink.at(1U)->ordinal == 2U);
  CHECK(sink.at(1U)->fields.detail_code == 2U);
  CHECK(sink.at(2U) == nullptr);

  // ++++++++++++++++++++++++++++++++++++++++
  // Invalid telemetry remains a contract failure even after saturation and is not counted as a
  // dropped valid observation.
  auto invalid = input_fields(5U);
  invalid.turn_ordinal.reset();
  REQUIRE_FALSE(sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, invalid));
  CHECK(sink.dropped_count() == 2U);

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

} // namespace
