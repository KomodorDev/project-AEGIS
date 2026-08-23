// Purpose: compose deterministic fake-only M3 authorities and derive the sealed M4 unit-test root
// through the same public factories used by production startup.

#include "m4_test_authority.hpp"

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/runtime/submission_coordinator.hpp"
#include "reference_configuration.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::test_support {
namespace {

// --------------------------------------------------------
// Parse fixture identifiers once and treat invalid literals as test-authoring failures.
template <typename Identifier> [[nodiscard]] Identifier id(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 authority fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Seal the unchanged M2 runtime policy against the M3-enabled two-firm configuration.
[[nodiscard]] runtime::RuntimePolicy
runtime_policy(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U, 32U,
                                       100'000U},
          {{id<model::MarketSourceId>("source.deribit-btc-perpetual"),
            id<model::VenueId>("deribit"), id<model::InstrumentId>("BTC-USD-PERPETUAL"),
            id<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::initial()}}});
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M4 authority fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Build the deterministic M3 fake coordinator solely to obtain its sealed risk/submission policy.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
submission_coordinator(const configuration::StartupConfiguration& configuration,
                       const runtime::RuntimePolicy& policy) {
  constexpr std::uint64_t maximum_attempts = 10U;
  auto encoder =
      execution::FakeEncoderScript::create(execution::FakeEncodingAction::Encode, maximum_attempts,
                                           {{1U, execution::FakeEncodingAction::Fail}});
  auto initiator = execution::FakeInitiatorScript::create(
      execution::FakeInitiationOutcome::AcceptedAndInitiated, maximum_attempts,
      {{1U, execution::FakeInitiationOutcome::DefiniteFailureBeforeAcceptance},
       {2U, execution::FakeInitiationOutcome::AcceptedThenOutcomeLost}});
  model::OrderNamespace::Bytes namespace_bytes{};
  namespace_bytes.fill(0x42U);
  auto order_ids =
      model::DeterministicOrderIdProvider::create(model::OrderNamespace{namespace_bytes});
  if (!encoder || !initiator || !order_ids) {
    throw std::logic_error{"invalid deterministic fake in M4 authority fixture"};
  }

  std::vector<std::optional<std::uint64_t>> clock_readings;
  clock_readings.reserve(static_cast<std::size_t>(maximum_attempts * 2U));
  for (std::uint64_t index = 0U; index < maximum_attempts * 2U; ++index) {
    clock_readings.emplace_back(10'000U + index);
  }
  auto created = runtime::SubmissionCoordinator::create(
      configuration, policy,
      runtime::FakeSubmissionRuntimeParams{
          m3_reference_risk_policy_params(configuration),
          execution::SubmissionPolicyCapacities{maximum_attempts, 4U, 4U, 1'024U, 2U, 110U, 8U},
          std::move(encoder).value(), std::move(initiator).value(),
          std::make_unique<execution::DeterministicSubmissionMeasurementClock>(
              std::move(clock_readings)),
          model::DeterministicOrderIdSource{std::move(order_ids).value()}});
  if (!created) {
    throw std::logic_error{"invalid submission coordinator in M4 authority fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------

} // namespace

// --------------------------------------------------------
// Return coherent generic capacities that satisfy every accepted M4 policy relationship.
runtime::M4PolicyCapacities ordinary_m4_capacities() noexcept {
  return runtime::M4PolicyCapacities{
      32U, 32U, 32U, 32U, 32U, 32U, 32U, 4U,  32U, 32U, 32U, 32U, 32U,
      32U, 32U, 32U, 32U, 32U, 32U, 8U,  16U, 16U, 4U,  5U,  32U, 3U,
  };
}

// --------------------------------------------------------
// Build the real sealed M1-M3 chain and derive one matching M4 policy with authored capacities.
M4TestAuthority m4_test_authority(runtime::M4PolicyCapacities capacities) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the unchanged reference configuration and runtime authority first.
  auto configured =
      configuration::StartupConfiguration::create(m3_enabled_two_firm_configuration_params());
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 authority fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = runtime_policy(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive risk/submission authority from the deterministic offline M3 composition.
  auto submission = submission_coordinator(configuration, runtime);
  auto policy =
      runtime::M4Policy::create(configuration, runtime, submission->reservations().policy(),
                                submission->policy(), capacities);
  if (!policy) {
    throw std::logic_error{"invalid M4 policy in M4 authority fixture"};
  }

  // ++++++++++++++++++++++++++++++++++++++++
  // Retain only the immutable values needed by public M4 unit-test boundaries.
  return M4TestAuthority{std::move(configuration), std::move(policy).value()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build the same authority with the ordinary coherent capacity fixture.
M4TestAuthority m4_test_authority() { return m4_test_authority(ordinary_m4_capacities()); }

// --------------------------------------------------------

} // namespace aegis::test_support
