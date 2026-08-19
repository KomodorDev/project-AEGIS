# Purpose: build the named M0 calibration and M2 runtime benchmarks as one owned executable.

# Compile every named workload into one runner; the M0 harness remains the sole BENCHMARK_MAIN.
add_executable(aegis_benchmarks harness_benchmark.cpp m2_runtime_benchmark.cpp)

# Reuse the accepted reference fixture when tests are enabled. Standalone benchmark configurations
# build the same source under a benchmark-local target so BUILD_TESTING may remain disabled.
if(TARGET aegis_test_support)
  set(AEGIS_BENCHMARK_REFERENCE_TARGET aegis_test_support)
else()
  add_library(aegis_benchmark_reference_support STATIC
              ../tests/support/reference_configuration.cpp)
  target_include_directories(aegis_benchmark_reference_support PUBLIC ../tests/support)
  target_link_libraries(aegis_benchmark_reference_support PUBLIC aegis::aegis)
  aegis_configure_owned_target(aegis_benchmark_reference_support)
  set(AEGIS_BENCHMARK_REFERENCE_TARGET aegis_benchmark_reference_support)
endif()

# Link product/runtime fixtures and Google Benchmark without adding either to the production library.
target_link_libraries(aegis_benchmarks PRIVATE ${AEGIS_BENCHMARK_REFERENCE_TARGET}
                                               benchmark::benchmark)

# Apply the same language, warning, and optional sanitizer policies as other owned code.
aegis_configure_owned_target(aegis_benchmarks)

# Keep the executable at the stable path expected by tools/run_benchmarks.py.
set_target_properties(aegis_benchmarks PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                    "${CMAKE_BINARY_DIR}/benchmarks")
