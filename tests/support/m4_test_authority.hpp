// Purpose: build sealed fake-only M1-M4 value, owner, and recovery-bootstrap fixtures, then drive
// genuine owner-bound submissions without exposing private coordinator mechanics.

#pragma once

#include "aegis/configuration/startup_configuration.hpp"
#include "aegis/execution/order_request.hpp"
#include "aegis/execution/submit_result.hpp"
#include "aegis/model/time.hpp"
#include "aegis/recovery/deterministic_fake_recovery_medium.hpp"
#include "aegis/runtime/m4_policy.hpp"
#include "aegis/runtime/runtime_policy.hpp"
#include "aegis/runtime/submission_coordinator.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace aegis::test_support {

// ########################################################################
// The fixture retains only the sealed configuration and resulting M4 policy needed by public M4
// value/factory tests; temporary M3 composition objects do not escape construction.
struct M4TestAuthority {
  configuration::StartupConfiguration configuration;
  runtime::M4Policy m4_policy;
};

// ########################################################################

// ########################################################################
// The owner fixture keeps the exact sealed configuration, runtime policy, sole M3 coordinator,
// derived M4 policy, and longitudinal callback predecessors together. Its counters always name the
// next unused owner turn and processing timestamp for this coordinator.
struct M4OwnerTestAuthority {
  configuration::StartupConfiguration configuration;
  runtime::RuntimePolicy runtime_policy;
  std::unique_ptr<runtime::SubmissionCoordinator> submission;
  runtime::M4Policy m4_policy;
  std::optional<model::CallbackOrdinal> last_callback_ordinal;
  std::uint64_t completed_dispatch_count;
  model::TurnOrdinal next_owner_turn;
  std::uint64_t next_processing_timestamp_nanoseconds;
};

// ########################################################################

// ########################################################################
// The move-only recovery fixture retains an external fake-medium handle beside its sole live
// namespace bootstrap. Reverse destruction releases the bootstrap lease before the medium.
struct M4RecoveryBootstrapTestAuthority {
  std::unique_ptr<recovery::DeterministicFakeRecoveryMedium> medium;
  recovery::RecoveryBootstrap bootstrap;
};

// ########################################################################

// --------------------------------------------------------
// Return coherent generic capacities that satisfy every accepted M4 policy relationship.
[[nodiscard]] runtime::M4PolicyCapacities create_ordinary_m4_policy_capacities() noexcept;

// --------------------------------------------------------
// Build the real sealed M1-M3 chain and derive one matching M4 policy; invalid fixture authority
// throws std::logic_error without returning a partial value.
[[nodiscard]] M4TestAuthority
create_m4_test_authority_or_throw(runtime::M4PolicyCapacities capacities);

// --------------------------------------------------------
// Build the same authority with ordinary coherent capacities; invalid fixture authority throws
// std::logic_error without returning a partial value.
[[nodiscard]] M4TestAuthority create_m4_test_authority_or_throw();

// --------------------------------------------------------

// --------------------------------------------------------
// Build an authored sealed chain while retaining the sole coordinator; invalid fixture authority
// throws std::logic_error without returning a partial owner.
[[nodiscard]] M4OwnerTestAuthority
create_m4_owner_test_authority_or_throw(configuration::StartupConfigurationParams params);

// --------------------------------------------------------
// Build the reference sealed chain while retaining the sole coordinator; invalid fixture authority
// throws std::logic_error without returning a partial owner.
[[nodiscard]] M4OwnerTestAuthority create_m4_owner_test_authority_or_throw();

// --------------------------------------------------------

// --------------------------------------------------------
// Create one policy-sized fake medium and acknowledged live bootstrap from deterministic seeds;
// invalid recovery authority throws std::logic_error without returning a partial value.
[[nodiscard]] M4RecoveryBootstrapTestAuthority
create_m4_recovery_bootstrap_test_authority_or_throw(const runtime::M4Policy& policy,
                                                     std::uint8_t recovery_lineage_seed = 0x10U,
                                                     std::uint8_t order_namespace_seed = 0x30U);

// --------------------------------------------------------
// Install one ordinary seeded recovery bootstrap into a pristine exact owner; any failed
// production installation throws std::logic_error without returning partial fixture state.
void install_recovery_bound_private_order_reconciler_or_throw(
    M4OwnerTestAuthority& authority, std::uint8_t recovery_lineage_seed = 0x10U,
    std::uint8_t order_namespace_seed = 0x30U);

// --------------------------------------------------------

// --------------------------------------------------------
// Build the fixed limit/GTC request used by owner-bound M4 lifecycle tests; invalid fixture
// literals or economics throw std::logic_error without returning a request.
[[nodiscard]] execution::OrderRequest create_m4_reference_order_request_or_throw();

// --------------------------------------------------------
// Submit once through the configured route owner's genuine active BotContext and return only after
// canonical dispatch deactivates that context; missing or exhausted fixture authority throws
// std::logic_error without advancing longitudinal predecessor state.
[[nodiscard]] execution::SubmitResult
submit_m4_order_or_throw(M4OwnerTestAuthority& authority, const execution::OrderRequest& request);

// --------------------------------------------------------

} // namespace aegis::test_support
