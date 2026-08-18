add_executable(aegis_unit_tests unit/version_test.cpp)
target_link_libraries(aegis_unit_tests PRIVATE aegis::aegis Catch2::Catch2WithMain)
aegis_configure_owned_target(aegis_unit_tests)

include(Catch)
catch_discover_tests(aegis_unit_tests PROPERTIES LABELS unit)
