cmake_minimum_required(VERSION 3.25)

project(
  WPCE
  VERSION 0.1.0
  DESCRIPTION "Windows Print Capture Engine"
  LANGUAGES CXX
)

option(WPCE_BUILD_TESTS "Build WPCE tests" ON)
option(WPCE_BUILD_UI "Build WPCE Qt6 GUI" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable AUTOMOC for Qt meta-object compilation.
if(WPCE_BUILD_UI)
  set(CMAKE_AUTOMOC ON)
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common/build_config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/wpce/common/build_config.h
  @ONLY
)

add_subdirectory(src)
add_subdirectory(tools)

if(WPCE_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

if(WPCE_BUILD_UI)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  add_subdirectory(src/ui)
endif()

