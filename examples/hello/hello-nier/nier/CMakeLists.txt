cmake_minimum_required(VERSION 3.20)
project(HelloPublication NONE)

set(NIER_ROOT "" CACHE PATH "Absolute path of your Nier checkout")
if(NOT IS_ABSOLUTE "${NIER_ROOT}")
  message(FATAL_ERROR "Set NIER_ROOT to the absolute path of your Nier checkout")
endif()
set(NIER_BUILD_TOOL "${NIER_ROOT}/build/prealpha/nier-build"
  CACHE FILEPATH "Nier build coordinator")

include("${NIER_ROOT}/sdk/share/nier/Nier.cmake")
nier_add_publication(publish
  SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.."
  NATIVE_OUTPUT hello
  OUTPUT hello.nier
  BUILD_TOOL "${NIER_BUILD_TOOL}"
  TARGETS hello)
