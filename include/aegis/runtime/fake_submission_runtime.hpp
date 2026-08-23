// Purpose: define MarketRuntime's public construction parameters for credential-free deterministic
// fake submission without exposing private callback authority or coordinator internals.

#pragma once

#include "aegis/execution/fake_order_encoder.hpp"
#include "aegis/execution/fake_transport_initiator.hpp"
#include "aegis/execution/submission_measurement_clock.hpp"
#include "aegis/execution/submission_policy.hpp"
#include "aegis/model/order_id.hpp"
#include "aegis/risk/risk_policy.hpp"

#include <memory>

namespace aegis::runtime {

// ########################################################################
// Runtime composition authors the immutable risk rulebook, bounded fake-only storage, scripts, a
// dedicated measurement clock, and one closed move-only identity source as one capability.
struct FakeSubmissionRuntimeParams {
  risk::RiskPolicyParams risk_policy;
  execution::SubmissionPolicyCapacities capacities;
  execution::FakeEncoderScript encoder_script;
  execution::FakeInitiatorScript initiator_script;
  std::unique_ptr<execution::SubmissionMeasurementClock> measurement_clock;
  model::DeterministicOrderIdSource order_ids;
};

// ########################################################################

} // namespace aegis::runtime
