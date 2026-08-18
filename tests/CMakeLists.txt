# Purpose: compile and register AEGIS unit tests with CTest.

# Link the AEGIS public target and Catch2's variant that supplies the test-program main().
add_executable(
  aegis_unit_tests
  unit/execution/execution_route_test.cpp
  unit/market_data/subscription_test.cpp
  unit/model/domain_error_result_test.cpp
  unit/model/fixed_point_test.cpp
  unit/model/identifier_test.cpp
  unit/model/instrument_metadata_test.cpp
  unit/model/order_id_test.cpp
  unit/model/sha256_test.cpp
  unit/model/time_test.cpp
  unit/organization/organization_test.cpp
  unit/version_test.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis::aegis Catch2::Catch2WithMain)

# Treat test code as owned code, so it receives the project's strict compiler policies.
aegis_configure_owned_target(aegis_unit_tests)

# Catch2's helper asks the built test executable for its cases and registers each with CTest.
include(Catch)
catch_discover_tests(aegis_unit_tests PROPERTIES LABELS unit)
