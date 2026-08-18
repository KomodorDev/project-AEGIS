# Purpose: build the M0 calibration benchmark as an AEGIS-owned executable.

# Compile the harness and link Google Benchmark's library; BENCHMARK_MAIN supplies program startup.
add_executable(aegis_benchmarks harness_benchmark.cpp)
target_link_libraries(aegis_benchmarks PRIVATE benchmark::benchmark)

# Apply the same language, warning, and optional sanitizer policies as other owned code.
aegis_configure_owned_target(aegis_benchmarks)

# Keep the executable at the stable path expected by tools/run_benchmarks.py.
set_target_properties(aegis_benchmarks PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                    "${CMAKE_BINARY_DIR}/benchmarks")
