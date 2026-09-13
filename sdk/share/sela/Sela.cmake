# Publisher integration for existing CMake projects. This is a coordinator
# project, not a replacement C compiler/toolchain for native configure probes.
# Each registered target gets its own genuine configure/build, with explicit
# cross/emulator settings for foreign targets. QEMU/binfmt must be provisioned
# before invoking this coordinator; configure results are never fabricated.
cmake_minimum_required(VERSION 3.20)
include_guard(GLOBAL)

function(sela_add_publication name)
  cmake_parse_arguments(PARSE_ARGV 1 SELA "" "SOURCE_DIR;NATIVE_OUTPUT;OUTPUT;BUILD_TOOL"
    "TARGETS;CONFIGURE_ARGS;CFLAGS")
  if(SELA_UNPARSED_ARGUMENTS OR SELA_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "Invalid sela_add_publication arguments")
  endif()
  foreach(required SOURCE_DIR NATIVE_OUTPUT OUTPUT)
    if(NOT SELA_${required})
      message(FATAL_ERROR "sela_add_publication requires ${required}")
    endif()
  endforeach()
  if(NOT SELA_BUILD_TOOL)
    find_program(SELA_BUILD_TOOL NAMES sela-build REQUIRED)
  endif()
  get_filename_component(sela_source "${SELA_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(sela_output "${SELA_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  set(sela_arguments --system cmake --source "${sela_source}"
    --output "${SELA_NATIVE_OUTPUT}" --artifact "${sela_output}")
  foreach(target IN LISTS SELA_TARGETS)
    list(APPEND sela_arguments --target "${target}")
  endforeach()
  foreach(argument IN LISTS SELA_CONFIGURE_ARGS)
    list(APPEND sela_arguments --configure-arg "${argument}")
  endforeach()
  foreach(flag IN LISTS SELA_CFLAGS)
    list(APPEND sela_arguments --cflag "${flag}")
  endforeach()
  # Re-enter the project build on every requested publication. A successful
  # final Clang link atomically replaces the old artifact; failure preserves it.
  add_custom_target("${name}"
    COMMAND "${SELA_BUILD_TOOL}" ${sela_arguments}
    BYPRODUCTS "${sela_output}"
    COMMENT "Publishing ${name} through stock Clang and Sela"
    VERBATIM USES_TERMINAL)
endfunction()
