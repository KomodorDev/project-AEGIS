// Purpose: share pure M1 reference-configuration builders across unit and trace scenario tests.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"

namespace aegis::test_support {

[[nodiscard]] configuration::StartupConfigurationParams reference_configuration_params();
[[nodiscard]] configuration::StartupConfigurationParams two_firm_configuration_params();

} // namespace aegis::test_support
