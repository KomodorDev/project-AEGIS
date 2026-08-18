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
option(AEGIS_BUILD_BENCHMARKS "Build the M0 benchmark harness" ON)

# Load reusable project policies from cmake/ProjectOptions.cmake and create their interface targets.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

aegis_create_project_options()

# Build the small M0 library and provide the namespaced alias future consumers should link against.
add_library(aegis src/aegis/version.cpp)
add_library(aegis::aegis ALIAS aegis)

# Interesting syntax: BUILD_INTERFACE exposes headers while building, without defining install rules.
target_include_directories(
  aegis
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(aegis PRIVATE AEGIS_PROJECT_VERSION="${PROJECT_VERSION}")
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

# Discover every owned C++ and Python source at configure time and pass both lists to the format
# targets. Keeping the Python list automatic prevents a newly added tool from escaping CI linting.
# Interesting syntax: CONFIGURE_DEPENDS asks CMake to reconfigure when matching files appear/disappear.
file(
  GLOB_RECURSE AEGIS_CXX_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp")
file(
  GLOB_RECURSE AEGIS_PYTHON_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/*.py")
aegis_add_format_targets(CXX_FILES ${AEGIS_CXX_FORMAT_FILES} PYTHON_FILES
                         ${AEGIS_PYTHON_FORMAT_FILES})
