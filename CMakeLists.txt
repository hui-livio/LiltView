cmake_minimum_required(VERSION 3.30)

project(LiltView VERSION 0.1.0 DESCRIPTION "A lightweight image viewer" LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/debug")
endif()

set(LILTVIEW_CXX_STANDARD "23" CACHE STRING "Application C++ standard: 23 or explicit compatibility mode 20")
set_property(CACHE LILTVIEW_CXX_STANDARD PROPERTY STRINGS 23 20)
if(NOT LILTVIEW_CXX_STANDARD MATCHES "^(20|23)$")
    message(FATAL_ERROR "LILTVIEW_CXX_STANDARD must be 23 or 20; C17/C++17 fallback is not supported")
endif()

option(LILTVIEW_BUILD_APP_COMPONENTS "Build the SDL3/OpenGL application and spdlog adapter" OFF)
option(LILTVIEW_BUILD_UNIT_TESTS "Build Catch2 unit tests in addition to dependency-free contract checks" OFF)
option(LILTVIEW_WARNINGS_AS_ERRORS "Treat warnings in LiltView targets as errors" OFF)

# No update-check option or network target until that feature is implemented.
include(cmake/LiltViewOptions.cmake)
include(cmake/LiltViewDependencies.cmake)
include(CTest)

if(LILTVIEW_BUILD_UNIT_TESTS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "LILTVIEW_BUILD_UNIT_TESTS requires BUILD_TESTING=ON")
endif()

liltview_check_toolchain()
add_subdirectory(core)

if(LILTVIEW_BUILD_APP_COMPONENTS)
    add_subdirectory(app)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

message(STATUS "LiltView ${PROJECT_VERSION}: C23 core / C++${LILTVIEW_CXX_STANDARD} consumers")
message(STATUS "Dependencies: local third_party sources; app components=${LILTVIEW_BUILD_APP_COMPONENTS}; Catch2=${LILTVIEW_BUILD_UNIT_TESTS}")
message(STATUS "Linux JPEG/PNG/SVG/WebP/BMP DISPLAY/FIT enabled; renderer backend=${LILTVIEW_RENDERER_BACKEND}; large images pending")
