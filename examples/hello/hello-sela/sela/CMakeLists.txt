cmake_minimum_required(VERSION 3.20)
project(HelloPublication NONE)

set(SELA_ROOT "" CACHE PATH "Absolute path of your Sela checkout")
if(NOT IS_ABSOLUTE "${SELA_ROOT}")
  message(FATAL_ERROR "Set SELA_ROOT to the absolute path of your Sela checkout")
endif()
set(SELA_BUILD_TOOL "${SELA_ROOT}/build/prealpha/sela-build"
  CACHE FILEPATH "Sela build coordinator")

include("${SELA_ROOT}/sdk/share/sela/Sela.cmake")
sela_add_publication(publish
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.."
  NATIVE_OUTPUT hello
  OUTPUT hello.sela
  BUILD_TOOL "${SELA_BUILD_TOOL}"
  BUILD_TARGETS hello)
