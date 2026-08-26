// Purpose: compose deterministic fake-only M3 authorities and derive sealed M4 value or owner
// fixtures through the same public factories used by production startup.

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
// Parse one fixture identifier or throw std::logic_error when its literal violates the nominal
// grammar.
template <typename Identifier>
[[nodiscard]] Identifier parse_identifier_or_throw(std::string_view text) {
  auto parsed = Identifier::parse(text);
  if (!parsed) {
    throw std::logic_error{"invalid identifier in M4 authority fixture"};
  }
  return std::move(parsed).value();
}

// --------------------------------------------------------
// Create the unchanged M2 runtime policy against the M3-enabled two-firm configuration, or throw
// std::logic_error when the sealed authority is inconsistent.
[[nodiscard]] runtime::RuntimePolicy
create_runtime_policy_or_throw(const configuration::StartupConfiguration& configuration) {
  auto created = runtime::RuntimePolicy::create(
      configuration,
      runtime::RuntimePolicyParams{
          runtime::RuntimePolicyLimits{2U, 4096U, 64U, 20U, 5'000'000'000U, 4U, 64U, 128U, 32U,
                                       100'000U},
          {{parse_identifier_or_throw<model::MarketSourceId>("source.deribit-btc-perpetual"),
            parse_identifier_or_throw<model::VenueId>("deribit"),
            parse_identifier_or_throw<model::InstrumentId>("BTC-USD-PERPETUAL"),
            parse_identifier_or_throw<model::VenueInstrumentId>("BTC-PERPETUAL"),
            model::InstrumentMetadataRevision::initial()}}});
  if (!created) {
    throw std::logic_error{"invalid runtime policy in M4 authority fixture"};
  }
  return std::move(created).value();
}

// --------------------------------------------------------
// Create the deterministic M3 fake coordinator used to obtain sealed risk/submission policy, or
// throw std::logic_error when any dependency or composition step fails.
[[nodiscard]] std::unique_ptr<runtime::SubmissionCoordinator>
create_submission_coordinator_or_throw(const configuration::StartupConfiguration& configuration,
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
runtime::M4PolicyCapacities create_ordinary_m4_policy_capacities() noexcept {
  return runtime::M4PolicyCapacities{
      32U, 32U, 32U, 32U, 32U, 32U, 32U, 4U,  32U, 32U, 32U, 32U, 32U,
      32U, 32U, 32U, 32U, 32U, 32U, 8U,  16U, 16U, 4U,  5U,  32U, 3U,
  };
}

// --------------------------------------------------------
// Build the real sealed M1-M3 chain and derive one matching M4 policy; invalid fixture authority
// throws std::logic_error without returning a partial value.
M4TestAuthority create_m4_test_authority_or_throw(runtime::M4PolicyCapacities capacities) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the unchanged reference configuration and runtime authority first.
  auto configured =
      configuration::StartupConfiguration::create(m3_enabled_two_firm_configuration_params());
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 authority fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = create_runtime_policy_or_throw(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Derive risk/submission authority from the deterministic offline M3 composition.
  auto submission = create_submission_coordinator_or_throw(configuration, runtime);
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
// Build the same authority with ordinary coherent capacities; invalid fixture authority throws
// std::logic_error without returning a partial value.
M4TestAuthority create_m4_test_authority_or_throw() {
  return create_m4_test_authority_or_throw(create_ordinary_m4_policy_capacities());
}

// --------------------------------------------------------

// --------------------------------------------------------
// Retain the sole M3 owner beside the M4 policy derived from authored sealed authorities; invalid
// fixture authority throws std::logic_error without returning a partial owner.
M4OwnerTestAuthority
create_m4_owner_test_authority_or_throw(configuration::StartupConfigurationParams params) {

  // ++++++++++++++++++++++++++++++++++++++++
  // Seal the authored M3-enabled startup and its matching runtime policy.
  auto configured = configuration::StartupConfiguration::create(std::move(params));
  if (!configured) {
    throw std::logic_error{"invalid startup configuration in M4 owner fixture"};
  }
  auto configuration = std::move(configured).value();
  auto runtime = create_runtime_policy_or_throw(configuration);

  // ++++++++++++++++++++++++++++++++++++++++
  // Build the one coordinator before deriving M4 authority from its retained policies.
  auto submission = create_submission_coordinator_or_throw(configuration, runtime);
  auto policy =
      runtime::M4Policy::create(configuration, runtime, submission->reservations().policy(),
                                submission->policy(), create_ordinary_m4_policy_capacities());
  if (!policy) {
    throw std::logic_error{"invalid M4 policy in M4 owner fixture"};
  }
  return M4OwnerTestAuthority{std::move(configuration), std::move(runtime), std::move(submission),
                              std::move(policy).value()};

  // ++++++++++++++++++++++++++++++++++++++++
}

// --------------------------------------------------------

// --------------------------------------------------------
// Build the unchanged reference owner through the authored owner fixture; invalid authority throws
// std::logic_error without returning a partial owner.
M4OwnerTestAuthority create_m4_owner_test_authority_or_throw() {
  return create_m4_owner_test_authority_or_throw(m3_enabled_two_firm_configuration_params());
}

// --------------------------------------------------------

} // namespace aegis::test_support
