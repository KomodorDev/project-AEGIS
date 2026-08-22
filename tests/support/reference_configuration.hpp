// Purpose: share frozen M1/M2 configuration inputs and separately derived M3 risk fixtures across
// unit, scenario, and benchmark tests without changing accepted golden provenance.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/risk/risk_policy.hpp"

namespace aegis::test_support {

// --------------------------------------------------------
// The first builder returns the exact credential-free golden scenario.
[[nodiscard]] configuration::StartupConfigurationParams reference_configuration_params();

// --------------------------------------------------------
// The second builder adds a peer subsidiary without introducing parent-company aggregation or
// cross-firm execution authority.
[[nodiscard]] configuration::StartupConfigurationParams two_firm_configuration_params();

// --------------------------------------------------------
// The M3-specific builder enables both explicit routes without changing M1/M2 golden inputs.
[[nodiscard]] configuration::StartupConfigurationParams m3_enabled_two_firm_configuration_params();

// --------------------------------------------------------
// The generous complete policy supports the 10,000-order success workload without weakening any
// configured route, attribution, metadata, or seven-scope provenance check.
[[nodiscard]] risk::RiskPolicyParams
m3_reference_risk_policy_params(const configuration::StartupConfiguration& configuration);

// --------------------------------------------------------
// The paired benchmark policy differs only at the baseline bot's first single-order quantity limit.
[[nodiscard]] risk::RiskPolicyParams
m3_rejecting_risk_policy_params(const configuration::StartupConfiguration& configuration);

// --------------------------------------------------------
} // namespace aegis::test_support
