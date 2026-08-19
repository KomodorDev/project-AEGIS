// Purpose: share pure M1 reference-configuration builders across unit and trace scenario tests.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"

namespace aegis::test_support {

// --------------------------------------------------------
// The first builder returns the exact credential-free golden scenario.
[[nodiscard]] configuration::StartupConfigurationParams reference_configuration_params();

// --------------------------------------------------------
// The second builder adds a peer subsidiary without introducing parent-company aggregation or
// cross-firm execution authority.
[[nodiscard]] configuration::StartupConfigurationParams two_firm_configuration_params();

// --------------------------------------------------------
} // namespace aegis::test_support
