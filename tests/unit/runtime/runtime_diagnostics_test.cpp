// Purpose: prove fixed diagnostic profiles, bounded overwrite accounting, and chronological ring
// order without making noncanonical detail a substitute for critical runtime evidence.

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
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto result = Identifier::parse(text);
  if (!result) {
    throw std::logic_error{"invalid identifier in runtime diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
template <typename Ordinal> [[nodiscard]] Ordinal ordinal(std::uint64_t value) {
  auto result = Ordinal::from_value(value);
  if (!result) {
    throw std::logic_error{"invalid ordinal in runtime diagnostic test"};
  }
  return std::move(result).value();
}

// --------------------------------------------------------
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
TEST_CASE("runtime diagnostics enforce assigned fixed-field profiles",
          "[runtime][diagnostics][m2]") {
  static_assert(static_cast<std::uint16_t>(runtime::RuntimeDiagnosticKind::SourceDiscontinuity) ==
                1U);
  static_assert(static_cast<std::uint16_t>(runtime::RuntimeDiagnosticKind::EvidenceExhausted) ==
                8U);

  const auto policy = runtime_policy(8U);
  runtime::RuntimeDiagnosticSink sink{policy};

  auto malformed = input_fields(3U);
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

  CHECK(sink.size() == 3U);
  CHECK(sink.accepted_count() == 3U);
  REQUIRE(sink.at(0U) != nullptr);
  CHECK(sink.at(0U)->kind == runtime::RuntimeDiagnosticKind::MalformedInput);

  auto partial = budget;
  partial.callback_ordinal.reset();
  CHECK_FALSE(sink.append(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, partial));
  auto nonexceeded = budget;
  nonexceeded.observed_value = nonexceeded.limit_value;
  CHECK_FALSE(sink.append(runtime::RuntimeDiagnosticKind::CallbackBudgetExceeded, nonexceeded));
  auto zero_occurrences = dispatch_reentry;
  zero_occurrences.occurrence_count = 0U;
  CHECK_FALSE(
      sink.append(runtime::RuntimeDiagnosticKind::DispatchReentryDetected, zero_occurrences));
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
}

// --------------------------------------------------------
TEST_CASE("runtime diagnostic ring exposes every overwrite in chronological order",
          "[runtime][diagnostics][m2]") {
  const auto policy = runtime_policy(2U);
  runtime::RuntimeDiagnosticSink sink{policy};

  for (std::uint32_t code = 1U; code <= 4U; ++code) {
    REQUIRE(sink.append(runtime::RuntimeDiagnosticKind::MalformedInput, input_fields(code)));
  }

  CHECK(sink.capacity() == 2U);
  CHECK(sink.size() == 2U);
  CHECK(sink.accepted_count() == 4U);
  CHECK(sink.overwritten_count() == 2U);
  REQUIRE(sink.at(0U) != nullptr);
  REQUIRE(sink.at(1U) != nullptr);
  CHECK(sink.at(0U)->ordinal == 3U);
  CHECK(sink.at(0U)->fields.detail_code == 3U);
  CHECK(sink.at(1U)->ordinal == 4U);
  CHECK(sink.at(1U)->fields.detail_code == 4U);
  CHECK(sink.at(2U) == nullptr);
}

// --------------------------------------------------------

} // namespace
