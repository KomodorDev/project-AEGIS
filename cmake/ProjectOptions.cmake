include_guard(GLOBAL)

function(aegis_create_project_options)
  if(TARGET aegis_project_options OR TARGET aegis_project_warnings)
    message(FATAL_ERROR "AEGIS project-option targets already exist")
  endif()

  if(AEGIS_ENABLE_ADDRESS_SANITIZER AND AEGIS_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
  endif()

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

  add_library(aegis_project_options INTERFACE)
  target_compile_features(aegis_project_options INTERFACE cxx_std_20)

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

  if(sanitizers)
    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(
      aegis_project_options INTERFACE "-fsanitize=${sanitizer_list}" -fno-omit-frame-pointer
                                      -fno-sanitize-recover=all)
    target_link_options(aegis_project_options INTERFACE "-fsanitize=${sanitizer_list}")
  endif()

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

  if(AEGIS_WARNINGS_AS_ERRORS)
    target_compile_options(aegis_project_warnings INTERFACE -Werror)
  endif()
endfunction()

function(aegis_configure_owned_target target_name)
  if(NOT TARGET "${target_name}")
    message(FATAL_ERROR "Cannot configure missing target: ${target_name}")
  endif()

  target_link_libraries("${target_name}" PRIVATE aegis_project_options aegis_project_warnings)
endfunction()

function(aegis_add_format_targets)
  find_program(AEGIS_CLANG_FORMAT_EXECUTABLE NAMES clang-format clang-format-18)
  find_program(AEGIS_RUFF_EXECUTABLE NAMES ruff)

  set(clang_format_error "clang-format 18.1.8 was not found")
  set(clang_format_is_supported OFF)
  if(AEGIS_CLANG_FORMAT_EXECUTABLE)
    execute_process(
      COMMAND "${AEGIS_CLANG_FORMAT_EXECUTABLE}" --version
      RESULT_VARIABLE clang_format_result
      OUTPUT_VARIABLE clang_format_version
      ERROR_VARIABLE clang_format_version
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(clang_format_result EQUAL 0 AND clang_format_version MATCHES "version 18\\.1\\.8$")
      set(clang_format_is_supported ON)
    else()
      set(clang_format_error
          "clang-format 18.1.8 is required; found: ${clang_format_version}")
    endif()
  endif()

  set(ruff_error "Ruff 0.16.3 was not found")
  set(ruff_is_supported OFF)
  if(AEGIS_RUFF_EXECUTABLE)
    execute_process(
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" --version
      RESULT_VARIABLE ruff_result
      OUTPUT_VARIABLE ruff_version
      ERROR_VARIABLE ruff_version
      OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(ruff_result EQUAL 0 AND ruff_version MATCHES "^ruff 0\\.16\\.3$")
      set(ruff_is_supported ON)
    else()
      set(ruff_error "Ruff 0.16.3 is required; found: ${ruff_version}")
    endif()
  endif()

  if(clang_format_is_supported AND ruff_is_supported)
    add_custom_target(
      format-check
      COMMAND "${AEGIS_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${ARGN}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" format --check "${PROJECT_SOURCE_DIR}/tools/run_benchmarks.py"
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" check "${PROJECT_SOURCE_DIR}/tools/run_benchmarks.py"
      COMMENT "Checking C++ and Python formatting"
      VERBATIM)
    add_custom_target(
      format
      COMMAND "${AEGIS_CLANG_FORMAT_EXECUTABLE}" -i ${ARGN}
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" check --fix "${PROJECT_SOURCE_DIR}/tools/run_benchmarks.py"
      COMMAND "${AEGIS_RUFF_EXECUTABLE}" format "${PROJECT_SOURCE_DIR}/tools/run_benchmarks.py"
      COMMENT "Formatting C++ and Python sources"
      VERBATIM)
  else()
    set(format_errors "")
    if(NOT clang_format_is_supported)
      list(APPEND format_errors "${clang_format_error}")
    endif()
    if(NOT ruff_is_supported)
      list(APPEND format_errors "${ruff_error}")
    endif()
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
