# Purpose: centralize compiler support, warnings, sanitizers, and source-formatting targets.

# Load these function definitions once even if several CMake files include this module.
include_guard(GLOBAL)

# Create reusable INTERFACE targets that carry policies without producing object files.
function(aegis_create_project_options)
  # Fail on accidental double initialization instead of silently changing existing targets.
  if(TARGET aegis_project_options OR TARGET aegis_project_warnings)
    message(FATAL_ERROR "AEGIS project-option targets already exist")
  endif()

  # ASan and TSan runtimes conflict, so callers must select separate configurations.
  if(AEGIS_ENABLE_ADDRESS_SANITIZER AND AEGIS_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
  endif()

  # Enforce the compiler/version support matrix recorded in the toolchain decision.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
      message(FATAL_ERROR "AEGIS requires AppleClang 16 or newer")
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
      message(FATAL_ERROR "AEGIS requires GCC 13 or newer")
    endif()
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
      message(FATAL_ERROR "AEGIS requires Clang 18 or newer")
    endif()
  else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()

  # Interesting syntax: an INTERFACE library carries requirements to consumers but has no binary.
  add_library(aegis_project_options INTERFACE)
  target_compile_features(aegis_project_options INTERFACE cxx_std_20)

  # Build a comma-separated sanitizer list from the independent feature switches.
  set(sanitizers "")
  if(AEGIS_ENABLE_ADDRESS_SANITIZER)
    list(APPEND sanitizers address)
  endif()
  if(AEGIS_ENABLE_UNDEFINED_SANITIZER)
    list(APPEND sanitizers undefined)
  endif()
  if(AEGIS_ENABLE_THREAD_SANITIZER)
    list(APPEND sanitizers thread)
  endif()

  # Apply instrumentation at both compile and link time; retain frames for useful diagnostics.
  if(sanitizers)
    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(
      aegis_project_options INTERFACE "-fsanitize=${sanitizer_list}" -fno-omit-frame-pointer
                                      -fno-sanitize-recover=all)
    target_link_options(aegis_project_options INTERFACE "-fsanitize=${sanitizer_list}")
  endif()

  # Keep the strict warning policy separate so it applies only to AEGIS-owned targets.
  add_library(aegis_project_warnings INTERFACE)
  target_compile_options(
    aegis_project_warnings
    INTERFACE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wsign-conversion
              -Wshadow
              -Wold-style-cast
              -Wcast-align
              -Woverloaded-virtual
              -Wnull-dereference
              -Wdouble-promotion
              -Wformat=2)

  # Continuous integration defaults to promoting every owned-code warning to a failure.
  if(AEGIS_WARNINGS_AS_ERRORS)
    target_compile_options(aegis_project_warnings INTERFACE -Werror)
  endif()
endfunction()

# Attach the shared language/instrumentation and warning policies to one owned target.
function(aegis_configure_owned_target target_name)
  # Catch misspelled or prematurely configured target names at configure time.
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "Cannot configure missing target: ${target_name}")
  endif()

  # Interesting syntax: PRIVATE propagates flags into this target, not onward to its consumers.
  target_link_libraries("${target_name}" PRIVATE aegis_project_options aegis_project_warnings)
endfunction()

# Add non-destructive checking and opt-in rewriting targets for all supplied source paths.
function(aegis_add_format_targets)
  # Split the named arguments into independent language lists and reject accidental extra inputs.
  cmake_parse_arguments(PARSE_ARGV 0 AEGIS_FORMAT "" "" "CXX_FILES;PYTHON_FILES")
  if(AEGIS_FORMAT_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown format-target arguments: ${AEGIS_FORMAT_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT AEGIS_FORMAT_CXX_FILES OR NOT AEGIS_FORMAT_PYTHON_FILES)
    message(FATAL_ERROR "Format targets require both CXX_FILES and PYTHON_FILES")
  endif()

  # Inspect the project virtual environment first, then every supported filename in PATH order. This
  # avoids rejecting a valid later candidate merely because an incompatible executable appears first.
  cmake_path(CONVERT "$ENV{PATH}" TO_CMAKE_PATH_LIST formatter_search_paths NORMALIZE)
  list(PREPEND formatter_search_paths "${PROJECT_SOURCE_DIR}/.venv/bin")
  list(REMOVE_DUPLICATES formatter_search_paths)
  set(clang_format_error "clang-format 18.1.8 was not found")
  set(clang_format_is_supported OFF)
  set(clang_format_observations "")
  foreach(search_directory IN LISTS formatter_search_paths)
    foreach(executable_name IN ITEMS clang-format clang-format-18)
      set(clang_format_candidate "${search_directory}/${executable_name}")
      if(EXISTS "${clang_format_candidate}" AND NOT IS_DIRECTORY "${clang_format_candidate}")
        execute_process(
          COMMAND "${clang_format_candidate}" --version
          RESULT_VARIABLE clang_format_result
          OUTPUT_VARIABLE clang_format_version
          ERROR_VARIABLE clang_format_version
          OUTPUT_STRIP_TRAILING_WHITESPACE)
        list(APPEND clang_format_observations "${clang_format_candidate}: ${clang_format_version}")

        if(clang_format_result EQUAL 0 AND clang_format_version MATCHES "version 18\\.1\\.8$")
          set(AEGIS_CLANG_FORMAT_EXECUTABLE "${clang_format_candidate}")
          set(clang_format_is_supported ON)
          break()
        endif()
      endif()
    endforeach()
    if(clang_format_is_supported)
      break()
    endif()
  endforeach()
  if(NOT clang_format_is_supported AND clang_format_observations)
    list(JOIN clang_format_observations ", " observed_clang_formats)
    set(clang_format_error
        "clang-format 18.1.8 is required; inspected: ${observed_clang_formats}")
  endif()

  # Probe every Ruff candidate independently so a wrong early PATH entry cannot hide the pinned tool.
  set(ruff_error "Ruff 0.16.3 was not found")
  set(ruff_is_supported OFF)
  set(ruff_observations "")
  foreach(search_directory IN LISTS formatter_search_paths)
    set(ruff_candidate "${search_directory}/ruff")
    if(EXISTS "${ruff_candidate}" AND NOT IS_DIRECTORY "${ruff_candidate}")
      execute_process(
        COMMAND "${ruff_candidate}" --version
        RESULT_VARIABLE ruff_result
        OUTPUT_VARIABLE ruff_version
        ERROR_VARIABLE ruff_version
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      list(APPEND ruff_observations "${ruff_candidate}: ${ruff_version}")

      if(ruff_result EQUAL 0 AND ruff_version MATCHES "^ruff 0\\.16\\.3$")
        set(AEGIS_RUFF_EXECUTABLE "${ruff_candidate}")
        set(ruff_is_supported ON)
        break()
      endif()
    endif()
  endforeach()
  if(NOT ruff_is_supported AND ruff_observations)
    list(JOIN ruff_observations ", " observed_ruff_versions)
    set(ruff_error "Ruff 0.16.3 is required; inspected: ${observed_ruff_versions}")
  endif()

  # When both tools match, create separate CI checking and developer rewriting targets.
  if(clang_format_is_supported AND ruff_is_supported)
    add_custom_target(
      format-check
      COMMAND "${AEGIS_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror
              ${AEGIS_FORMAT_CXX_FILES}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" format --check ${AEGIS_FORMAT_PYTHON_FILES}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" check ${AEGIS_FORMAT_PYTHON_FILES}
      COMMENT "Checking C++ and Python formatting"
      VERBATIM)
    add_custom_target(
      format
      COMMAND "${AEGIS_CLANG_FORMAT_EXECUTABLE}" -i ${AEGIS_FORMAT_CXX_FILES}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" check --fix ${AEGIS_FORMAT_PYTHON_FILES}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" format ${AEGIS_FORMAT_PYTHON_FILES}
      COMMENT "Formatting C++ and Python sources"
      VERBATIM)
  else()
    # Otherwise keep target names available, but make either target print the reasons and fail.
    set(format_errors "")
    if(NOT clang_format_is_supported)
      list(APPEND format_errors "${clang_format_error}")
    endif()
    if(NOT ruff_is_supported)
      list(APPEND format_errors "${ruff_error}")
    endif()
    # Interesting syntax: list(JOIN ...) turns multiple diagnostics into one readable command line.
    list(JOIN format_errors "; " format_error)
    add_custom_target(
      format-check
      COMMAND "${CMAKE_COMMAND}" -E echo "${format_error}"
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM)
    add_custom_target(
      format
      COMMAND "${CMAKE_COMMAND}" -E echo "${format_error}"
      COMMAND "${CMAKE_COMMAND}" -E false
      VERBATIM)
  endif()
endfunction()
