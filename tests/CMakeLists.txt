# Purpose: compile and register AEGIS unit, tooling, and M1-M3 deterministic-scenario tests with
# CTest.

# Keep the exact accepted startup inputs in one owned fixture library so unit and scenario evidence
# cannot silently diverge on configuration provenance.
add_library(aegis_test_support STATIC support/reference_configuration.cpp)
target_include_directories(aegis_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(aegis_test_support PUBLIC aegis::aegis)
aegis_configure_owned_target(aegis_test_support)

# Link the AEGIS public target and Catch2's variant that supplies the test-program main().
add_executable(
  aegis_unit_tests
  # Startup tests bind atomic validation to its canonical bytes, revisions, and golden fingerprint.
  unit/configuration/startup_configuration_test.cpp
  # These tests prove an observation grant never synthesizes or enables execution permission.
  unit/execution/execution_route_test.cpp
  # M3 request, clock, and owner-local route tests pin vocabulary, timing, and authorization order.
  unit/execution/fake_order_encoder_test.cpp
  unit/execution/fake_transport_initiator_test.cpp
  unit/execution/order_request_test.cpp
  unit/execution/route_authorization_test.cpp
  unit/execution/submission_measurement_clock_test.cpp
  unit/market_data/subscription_test.cpp
  # Market-event tests pin normalized ordering, duplicate identity, and sanitized state publication.
  unit/market_data/market_event_test.cpp
  # Book-state tests pin transactional commit, continuity, readiness, and exact trace preflight.
  unit/market_data/market_state_machine_test.cpp
  # Fixture tests pin strict parsing, bounded failure, and policy-backed source normalization.
  unit/market_data/recorded_fixture_test.cpp
  # These primitive tests pin stable Result/error contracts and nominal identifier grammar.
  unit/model/domain_error_result_test.cpp
  # Fixed-point tests pin canonical storage, explicit rounding, and overflow behavior.
  unit/model/fixed_point_test.cpp
  # Identifier tests enforce the nominal grammar and immutable validated-value boundary.
  unit/model/identifier_test.cpp
  # M4 identity tests pin opaque bounds, fixed layouts, and epoch-qualified cancel uniqueness.
  unit/model/m4_identity_test.cpp
  # Metadata tests prove deterministic validation order and declared-unit conversion boundaries.
  unit/model/instrument_metadata_test.cpp
  # Clock and order-ID tests share deterministic providers to pin replay and boundary behavior.
  unit/model/order_id_test.cpp
  # SHA-256 tests use published vectors to protect the canonical provenance fingerprint primitive.
  unit/model/sha256_test.cpp
  # Time tests prove nominal clock separation, checked deltas, revisions, and injected progression.
  unit/model/time_test.cpp
  # OMS tests pin duplicate-before-capacity admission and the complete local M3 transition table.
  unit/oms/outbound_oms_test.cpp
  # These tests lock canonical multi-firm hierarchy validation and transitive bot attribution.
  unit/organization/organization_test.cpp
  # Risk tests pin immutable policy bytes, conservative exposure, atomic limits, and exact release.
  unit/risk/reservation_ledger_test.cpp
  unit/risk/risk_policy_test.cpp
  unit/risk/worst_case_exposure_test.cpp
  # Runtime-policy tests bind M2 capacities and source identity to sealed startup provenance.
  unit/runtime/runtime_policy_test.cpp
  # Runtime submission tests pin private composition, BotContext entry, diagnostics, and dispatch.
  unit/runtime/runtime_diagnostics_test.cpp
  unit/runtime/submission_coordinator_test.cpp
  unit/runtime/submission_diagnostics_test.cpp
  unit/runtime/bot_runtime_test.cpp
  # Coordinator tests prove recorded ingress reaches books and strategies within one owner turn.
  unit/runtime/market_runtime_test.cpp
  # Executor tests pin bounded admission, fence ordering, owner turns, and driver equivalence.
  unit/runtime/serialized_executor_test.cpp
  # Trace units pin bounded append failure, exact event schemas, and canonical byte vectors.
  unit/trace/trace_test.cpp
  unit/trace/runtime_trace_test.cpp
  unit/trace/submission_trace_test.cpp
  # Version tests ensure the compiled library reports the configured project identity.
  unit/version_test.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis_test_support Catch2::Catch2WithMain)

# Only focused repository-owned unit tests may include runtime-private coordinator contracts.
target_include_directories(aegis_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")

# Treat test code as owned code, so it receives the project's strict compiler policies.
aegis_configure_owned_target(aegis_unit_tests)

# Catch2's helper asks the built test executable for its cases and registers each with CTest.
include(Catch)
catch_discover_tests(aegis_unit_tests PROPERTIES LABELS unit)

# Run the deterministic forbidden-capability scanner and its signature self-test in every normal
# and sanitizer workflow; neither check mutates the worktree.
add_test(NAME aegis_forbidden_capability_check
         COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_forbidden_capabilities.py")
set_tests_properties(aegis_forbidden_capability_check PROPERTIES LABELS forbidden_capability
                                                                 WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_test(NAME aegis_forbidden_capability_scanner_test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tooling/forbidden_capabilities_test.py")
set_tests_properties(aegis_forbidden_capability_scanner_test PROPERTIES LABELS tooling)
add_test(NAME aegis_m3_benchmark_evidence_tooling_test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tooling/m3_benchmark_evidence_test.py")
set_tests_properties(aegis_m3_benchmark_evidence_tooling_test PROPERTIES LABELS tooling)

# Keep deterministic replay and accepted-prefix capacity evidence visible as a milestone gate rather
# than hiding the full reference scenario among isolated unit cases.
add_executable(aegis_m1_reference_scenario_tests
               deterministic_scenarios/m1_reference_scenario_test.cpp)
target_link_libraries(aegis_m1_reference_scenario_tests PRIVATE aegis_test_support
                                                                Catch2::Catch2WithMain)
aegis_configure_owned_target(aegis_m1_reference_scenario_tests)
catch_discover_tests(aegis_m1_reference_scenario_tests PROPERTIES LABELS deterministic_scenario)

# Replay the complete M2 validity table through both owner drivers and compare canonical evidence.
add_executable(aegis_m2_reference_scenario_tests
               deterministic_scenarios/m2_reference_scenario_test.cpp)
target_link_libraries(aegis_m2_reference_scenario_tests PRIVATE aegis_test_support
                                                                Catch2::Catch2WithMain)
aegis_configure_owned_target(aegis_m2_reference_scenario_tests)
catch_discover_tests(aegis_m2_reference_scenario_tests PROPERTIES LABELS deterministic_scenario)

# Replay every M3 local submission outcome under two manual owners and one dedicated owner, then
# compare the complete callback, result, trace, OMS, reservation, diagnostic, and fake-write bundle.
add_executable(aegis_m3_reference_scenario_tests
               deterministic_scenarios/m3_reference_scenario_test.cpp)
target_link_libraries(aegis_m3_reference_scenario_tests PRIVATE aegis_test_support
                                                                Catch2::Catch2WithMain)
aegis_configure_owned_target(aegis_m3_reference_scenario_tests)
catch_discover_tests(aegis_m3_reference_scenario_tests PROPERTIES LABELS deterministic_scenario)
