# Publisher integration for existing CMake projects. This is a coordinator
# project, not a replacement C compiler/toolchain for native configure probes.
cmake_minimum_required(VERSION 3.20)
include_guard(GLOBAL)

function(nier_add_publication name)
  cmake_parse_arguments(PARSE_ARGV 1 NIER "" "SOURCE_DIR;NATIVE_OUTPUT;OUTPUT;BUILD_TOOL"
    "TARGETS;CONFIGURE_ARGS;CFLAGS")
  if(NIER_UNPARSED_ARGUMENTS OR NIER_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "Invalid nier_add_publication arguments")
  endif()
  foreach(required SOURCE_DIR NATIVE_OUTPUT OUTPUT)
    if(NOT NIER_${required})
      message(FATAL_ERROR "nier_add_publication requires ${required}")
    endif()
  endforeach()
  if(NOT NIER_BUILD_TOOL)
    find_program(NIER_BUILD_TOOL NAMES nier-build REQUIRED)
  endif()
  get_filename_component(nier_source "${NIER_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(nier_output "${NIER_OUTPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  set(nier_arguments --system cmake --source "${nier_source}"
    --output "${NIER_NATIVE_OUTPUT}" --artifact "${nier_output}")
  foreach(target IN LISTS NIER_TARGETS)
    list(APPEND nier_arguments --target "${target}")
  endforeach()
  foreach(argument IN LISTS NIER_CONFIGURE_ARGS)
    list(APPEND nier_arguments --configure-arg "${argument}")
  endforeach()
  foreach(flag IN LISTS NIER_CFLAGS)
    list(APPEND nier_arguments --cflag "${flag}")
  endforeach()
  # Re-enter the project build on every requested publication. A successful
  # final Clang link atomically replaces the old artifact; failure preserves it.
  add_custom_target("${name}"
    COMMAND "${NIER_BUILD_TOOL}" ${nier_arguments}
    BYPRODUCTS "${nier_output}"
    COMMENT "Publishing ${name} through stock Clang and NieR"
    VERBATIM USES_TERMINAL)
endfunction()
