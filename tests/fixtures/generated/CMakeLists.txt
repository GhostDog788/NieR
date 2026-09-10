cmake_minimum_required(VERSION 3.20)
project(GeneratedHello C)
include(CheckCSourceRuns)
check_c_source_runs("#include <stddef.h>\nint main(void) { return sizeof(void *) == sizeof(size_t) ? 0 : 1; }" NATIVE_PROBE_WORKS)
if(NOT NATIVE_PROBE_WORKS)
  message(FATAL_ERROR "The private native configure probe failed")
endif()
add_executable(generator generator.c)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.h"
  COMMAND generator "${CMAKE_CURRENT_BINARY_DIR}/generated.h"
  DEPENDS generator VERBATIM)
add_executable(hello hello.c "${CMAKE_CURRENT_BINARY_DIR}/generated.h")
target_include_directories(hello PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
