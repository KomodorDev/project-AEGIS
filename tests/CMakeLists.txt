# Purpose: compile and register AEGIS unit tests with CTest.

# Keep reusable deterministic fixtures in a small owned library shared by unit and scenario tests.
add_library(aegis_test_support STATIC support/reference_configuration.cpp)
target_include_directories(aegis_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(aegis_test_support PUBLIC aegis::aegis)
aegis_configure_owned_target(aegis_test_support)

# Link the AEGIS public target and Catch2's variant that supplies the test-program main().
add_executable(
  aegis_unit_tests
  unit/configuration/startup_configuration_test.cpp
  unit/execution/execution_route_test.cpp
  unit/market_data/subscription_test.cpp
  # These primitive tests pin stable Result/error contracts and nominal identifier grammar.
  unit/model/domain_error_result_test.cpp
  unit/model/fixed_point_test.cpp
  # Identifier tests enforce the nominal grammar and immutable validated-value boundary.
  unit/model/identifier_test.cpp
  unit/model/instrument_metadata_test.cpp
  # Clock and order-ID tests share deterministic providers to pin replay and boundary behavior.
  unit/model/order_id_test.cpp
  # SHA-256 tests use published vectors to protect the canonical provenance fingerprint primitive.
  unit/model/sha256_test.cpp
  # Time tests prove nominal clock separation, checked deltas, revisions, and injected progression.
  unit/model/time_test.cpp
  unit/organization/organization_test.cpp
  unit/trace/trace_test.cpp
  # Version tests ensure the compiled library reports the configured project identity.
  unit/version_test.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis_test_support Catch2::Catch2WithMain)

# Treat test code as owned code, so it receives the project's strict compiler policies.
aegis_configure_owned_target(aegis_unit_tests)

# Catch2's helper asks the built test executable for its cases and registers each with CTest.
include(Catch)
catch_discover_tests(aegis_unit_tests PROPERTIES LABELS unit)

# Keep the milestone replay visible as its own gate rather than hiding it among unit cases.
add_executable(aegis_m1_reference_scenario_tests
               deterministic_scenarios/m1_reference_scenario_test.cpp)
target_link_libraries(aegis_m1_reference_scenario_tests PRIVATE aegis_test_support
                                                                Catch2::Catch2WithMain)
aegis_configure_owned_target(aegis_m1_reference_scenario_tests)
catch_discover_tests(aegis_m1_reference_scenario_tests PROPERTIES LABELS deterministic_scenario)
