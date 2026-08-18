add_executable(aegis_benchmarks harness_benchmark.cpp)
target_link_libraries(aegis_benchmarks PRIVATE benchmark::benchmark)
aegis_configure_owned_target(aegis_benchmarks)

set_target_properties(aegis_benchmarks PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                    "${CMAKE_BINARY_DIR}/benchmarks")
