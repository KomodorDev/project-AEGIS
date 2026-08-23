// Purpose: build one sealed fake-only M1-M4 authority fixture for M4 unit tests without exposing
// private coordinator mechanics or changing any accepted M1-M3 canonical fixture.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/runtime/m4_policy.hpp"

namespace aegis::test_support {

// ########################################################################
// The fixture retains only the sealed configuration and resulting M4 policy needed by public M4
// value/factory tests; temporary M3 composition objects do not escape construction.
struct M4TestAuthority {
  configuration::StartupConfiguration configuration;
  runtime::M4Policy m4_policy;
};

// ########################################################################

// --------------------------------------------------------
// Return coherent generic capacities that satisfy every accepted M4 policy relationship.
[[nodiscard]] runtime::M4PolicyCapacities ordinary_m4_capacities() noexcept;

// --------------------------------------------------------
// Build the real sealed M1-M3 chain and derive one matching M4 policy with authored capacities.
[[nodiscard]] M4TestAuthority m4_test_authority(runtime::M4PolicyCapacities capacities);

// --------------------------------------------------------
// Build the same authority with the ordinary coherent capacity fixture.
[[nodiscard]] M4TestAuthority m4_test_authority();

// --------------------------------------------------------

} // namespace aegis::test_support
