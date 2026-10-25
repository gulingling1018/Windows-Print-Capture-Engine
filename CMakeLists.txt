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

# Static release gates validate compile commands against actual target sources.
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  if(COMMAND cmake_file_api)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)
  else()
    # CMake 3.25/3.26: a second configure consumes the newly written query.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/.cmake/api/v1/query")
    file(WRITE "${CMAKE_BINARY_DIR}/.cmake/api/v1/query/codemodel-v2" "")
  endif()
endif()

# Enable AUTOMOC and AUTORCC for Qt meta-object and resource compilation.
if(WPCE_BUILD_UI)
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common/build_config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/wpce/common/build_config.h
  @ONLY
)

if(WPCE_BUILD_UI)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
endif()

add_subdirectory(src)
add_subdirectory(tools)

if(WPCE_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

if(WPCE_BUILD_UI)
  add_subdirectory(src/ui)
endif()
