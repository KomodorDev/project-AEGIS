# Purpose: define how AEGIS is configured, compiled, tested, benchmarked, and formatted.

# Refuse older CMake versions before any project-specific instructions are interpreted.
cmake_minimum_required(VERSION 3.25)

# Declare the project identity and enable only the C++ language.
project(
  AEGIS
  VERSION 0.1.0
  DESCRIPTION "Asynchronous Exchange Gateway and Inventory System"
  LANGUAGES CXX)

# Require portable C++20. Disabling extensions avoids compiler-specific dialects such as gnu++20.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Expose deliberate build switches to developers, presets, and continuous integration.
option(AEGIS_WARNINGS_AS_ERRORS "Treat warnings in AEGIS-owned targets as errors" ON)
option(AEGIS_ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer" OFF)
option(AEGIS_ENABLE_UNDEFINED_SANITIZER "Enable UndefinedBehaviorSanitizer" OFF)
option(AEGIS_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(AEGIS_BUILD_BENCHMARKS "Build the named M0, M2, and M3 benchmark workloads" ON)

# Load reusable project policies from cmake/ProjectOptions.cmake and create their interface targets.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

aegis_create_project_options()

# Resolve the standard thread runtime used by the dedicated serialized-owner driver.
find_package(Threads REQUIRED)

# Require one Python interpreter for formatting, scanners, tooling tests, and evidence scripts.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Build the AEGIS library and provide the namespaced alias consumers should link against.
add_library(
  aegis
  # Startup validation and provenance form one subsystem that derives and publishes identity only
  # with an accepted immutable snapshot.
  src/aegis/configuration/configuration_provenance.cpp
  src/aegis/configuration/startup_configuration.cpp
  # Execution owns explicit grants, canonical validation, a dedicated measurement clock, immutable
  # fake-only policy, and exact offline encoding/initiation leaf contracts.
  src/aegis/execution/execution_route.cpp
  src/aegis/execution/fake_order_encoder.cpp
  src/aegis/execution/fake_transport_initiator.cpp
  src/aegis/execution/order_validation.cpp
  src/aegis/execution/submission_measurement_clock.cpp
  src/aegis/execution/submission_policy.cpp
  src/aegis/execution/submission_route.cpp
  # Market-data subscriptions remain observation permissions and never grant execution authority.
  src/aegis/market_data/subscription.cpp
  # Normalized market events own venue-neutral input identity and post-commit callback contracts.
  src/aegis/market_data/market_event.cpp
  # Fixed-capacity books publish only complete Ready views through one transactional state owner.
  src/aegis/market_data/order_book.cpp
  src/aegis/market_data/market_state_machine.cpp
  # Recorded fixtures parse bounded credential-free bytes before policy-backed normalization.
  src/aegis/market_data/recorded_fixture.cpp
  # One compiled arithmetic kernel keeps every nominal decimal domain on identical checked rules.
  src/aegis/model/fixed_point.cpp
  # Identifier grammar is compiled; its Result and error primitives remain header-only.
  src/aegis/model/identifier.cpp
  # Metadata validation owns unit coherence, increments, and contract face-value conversion.
  src/aegis/model/instrument_metadata.cpp
  # Clock and order-ID providers own stateful behavior; their typed values remain in public headers.
  src/aegis/model/order_id.cpp
  # The owned SHA-256 implementation keeps provenance fingerprints portable and dependency-free.
  src/aegis/model/sha256.cpp
  # Time implementation owns checked cross-clock arithmetic and deterministic clock advancement.
  src/aegis/model/time.cpp
  # Outbound OMS retains local admission/state/provenance but owns no risk release or communication.
  src/aegis/oms/outbound_oms.cpp
  # Organization is the canonical source of Firm -> Desk -> Bot attribution used by permissions.
  src/aegis/organization/organization.cpp
  # Fixed risk seals complete scope limits, computes exact exposure, and atomically reserves/releases.
  src/aegis/risk/exposure.cpp
  src/aegis/risk/reservation_ledger.cpp
  src/aegis/risk/risk_policy.cpp
  # Runtime policy seals all M2 capacities, freshness rules, and configured public-market sources.
  src/aegis/runtime/runtime_policy.cpp
  # M4 policy binds every private/recovery capacity to the unchanged M1-M3 authority chain.
  src/aegis/runtime/m4_policy.cpp
  # Market runtime composes recorded ingress, source state, and bot dispatch on one owner.
  src/aegis/runtime/market_runtime.cpp
  # Owner-local diagnostics retain a bounded fixed-field prefix with explicit saturation counts.
  src/aegis/runtime/runtime_diagnostics.cpp
  # Runtime owns direct submission composition and its bounded noncanonical diagnostic prefix.
  src/aegis/runtime/submission_coordinator.cpp
  src/aegis/runtime/submission_diagnostics.cpp
  # Bot dispatch owns one strategy per bot and invokes canonical state-before-market callbacks.
  src/aegis/runtime/bot_runtime.cpp
  # Both runtime drivers share one bounded attempt-ordered serialized turn processor.
  src/aegis/runtime/serialized_executor.cpp
  src/aegis/runtime/dedicated_executor_driver.cpp
  # Trace owns bounded in-memory provenance and its canonical stream encoding without external I/O.
  src/aegis/trace/trace.cpp
  # Runtime trace adds bounded deterministic data-plane evidence without changing the M1 schema.
  src/aegis/trace/runtime_trace.cpp
  # Submission trace owns separate bounded AEGISSTS evidence so M2 callback preflight is unchanged.
  src/aegis/trace/submission_trace.cpp
  # Version reporting exposes the configured project identity without adding runtime dependencies.
  src/aegis/version.cpp)
add_library(aegis::aegis ALIAS aegis)

# Interesting syntax: BUILD_INTERFACE exposes headers while building, without defining install rules.
target_include_directories(
  aegis
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(aegis PRIVATE AEGIS_PROJECT_VERSION="${PROJECT_VERSION}")
target_link_libraries(aegis PRIVATE Threads::Threads)
aegis_configure_owned_target(aegis)

# Enable CTest and CMake's pinned source-dependency mechanism.
include(CTest)
include(FetchContent)

# Fetch and register unit tests only when the standard BUILD_TESTING option is enabled.
if(BUILD_TESTING)
  # Do not install Catch2 documentation or helper files as part of AEGIS.
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)

  # Pin both the source revision and archive checksum so dependency input is reproducible.
  FetchContent_Declare(
    Catch2
    URL
      https://github.com/catchorg/Catch2/archive/644821ce28cb25d7992a4d0375b1d83214392592.tar.gz
    URL_HASH SHA256=5536f5e936466e3d0ef6350630d17d976fee2dee3cc44940e77a769cafab1904
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(Catch2)

  # Catch2 ships its CTest discovery helper outside its default module search path.
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  add_subdirectory(tests)
endif()

# Fetch and register the benchmark harness when its option is enabled (ON by default).
if(AEGIS_BUILD_BENCHMARKS)
  # Disable upstream install/self-tests, its GTest bridge, and dependency-owned Werror.
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)

  # As with Catch2, an immutable revision plus SHA-256 guards the downloaded source.
  FetchContent_Declare(
    google_benchmark
    URL
      https://github.com/google/benchmark/archive/192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz
    URL_HASH SHA256=f82705a2726d8f6cdcda274b841f6314dbfc6f731cdda06c946f310ec1cc3ad9
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
  FetchContent_MakeAvailable(google_benchmark)

  add_subdirectory(benchmarks)
endif()

# Reject forbidden live/remote/blocking/handoff capabilities through one deterministic tracked
# scanner; compile-time API probes and owner-turn tests remain independent structural layers.
add_custom_target(
  forbidden-capability-check
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_forbidden_capabilities.py"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  VERBATIM)

# Discover every owned C++ and Python source at configure time and pass both lists to the format
# targets. Keeping headers and tooling tests automatic prevents new owned files from escaping CI.
# Interesting syntax: CONFIGURE_DEPENDS asks CMake to reconfigure when matching files appear/disappear.
file(
  GLOB_RECURSE AEGIS_CXX_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp")
file(
  GLOB_RECURSE AEGIS_PYTHON_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.py")
aegis_add_format_targets(CXX_FILES ${AEGIS_CXX_FORMAT_FILES} PYTHON_FILES
                         ${AEGIS_PYTHON_FORMAT_FILES})
