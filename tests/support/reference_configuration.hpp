// Purpose: share frozen M1/M2 configuration inputs and separately derived M3 risk fixtures across
// unit, scenario, and benchmark tests without changing accepted golden provenance.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/risk/risk_policy.hpp"

namespace aegis::test_support {

// --------------------------------------------------------
// Creates the exact credential-free golden scenario parameters.
[[nodiscard]] configuration::StartupConfigurationParams
create_reference_configuration_params_or_throw();

// --------------------------------------------------------
// Creates parameters with a peer subsidiary but no parent-company aggregation or cross-firm
// execution authority.
[[nodiscard]] configuration::StartupConfigurationParams
create_two_firm_configuration_params_or_throw();

// --------------------------------------------------------
// Creates M3 parameters with both explicit routes enabled without changing M1/M2 golden inputs.
[[nodiscard]] configuration::StartupConfigurationParams
create_m3_enabled_two_firm_configuration_params_or_throw();

// --------------------------------------------------------
// Creates the complete policy for the 10,000-order success workload; invalid sealed authority
// causes an exception rather than a partial policy.
[[nodiscard]] risk::RiskPolicyParams create_m3_reference_risk_policy_params_or_throw(
    const configuration::StartupConfiguration& configuration);

// --------------------------------------------------------
// Creates the paired rejection policy; missing baseline-bot authority causes an exception.
[[nodiscard]] risk::RiskPolicyParams create_m3_rejecting_risk_policy_params_or_throw(
    const configuration::StartupConfiguration& configuration);

// --------------------------------------------------------
} // namespace aegis::test_support
