cmake_minimum_required(VERSION 3.30)

project(LiltView VERSION 1.0.0 DESCRIPTION "A lightweight image viewer" LANGUAGES C CXX)

set(liltview_default_version_suffix "")
if(EXISTS "${PROJECT_SOURCE_DIR}/RELEASE_VERSION")
    file(STRINGS "${PROJECT_SOURCE_DIR}/RELEASE_VERSION" liltview_source_version LIMIT_COUNT 1)
    string(FIND "${liltview_source_version}" "${PROJECT_VERSION}" liltview_version_prefix)
    if(NOT liltview_version_prefix EQUAL 0)
        message(FATAL_ERROR "RELEASE_VERSION disagrees with project(VERSION)")
    endif()
    string(LENGTH "${PROJECT_VERSION}" liltview_version_length)
    string(SUBSTRING "${liltview_source_version}" ${liltview_version_length} -1 liltview_default_version_suffix)
endif()
set(LILTVIEW_VERSION_SUFFIX "${liltview_default_version_suffix}" CACHE STRING "Release candidate suffix, e.g. -rc.1; empty for stable")
if(NOT LILTVIEW_VERSION_SUFFIX STREQUAL "" AND NOT LILTVIEW_VERSION_SUFFIX MATCHES "^-rc\\.[1-9][0-9]*$")
    message(FATAL_ERROR "LILTVIEW_VERSION_SUFFIX must be empty or -rc.N")
endif()
set(LILTVIEW_VERSION "${PROJECT_VERSION}${LILTVIEW_VERSION_SUFFIX}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B build/debug")
endif()

set(LILTVIEW_CXX_STANDARD "23" CACHE STRING "Application C++ standard: 23 or explicit compatibility mode 20")
set_property(CACHE LILTVIEW_CXX_STANDARD PROPERTY STRINGS 23 20)
if(NOT LILTVIEW_CXX_STANDARD MATCHES "^(20|23)$")
    message(FATAL_ERROR "LILTVIEW_CXX_STANDARD must be 23 or 20; C++17 fallback is not supported")
endif()

option(LILTVIEW_BUILD_APP_COMPONENTS "Build the SDL3/SDL_Render application and spdlog adapter" OFF)
option(LILTVIEW_BUILD_UNIT_TESTS "Build Catch2 unit tests in addition to dependency-free contract checks" OFF)
option(LILTVIEW_WARNINGS_AS_ERRORS "Treat warnings in LiltView targets as errors" OFF)

option(LILTVIEW_RELEASE_PACKAGING "Build stripped, self-contained release artifacts" OFF)
if(LILTVIEW_RELEASE_PACKAGING)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" OR CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR "Release packaging requires a single-config generator with CMAKE_BUILD_TYPE=MinSizeRel")
    endif()
    # Set before adding vendors so the EXE and every static library agree.
    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "Static release CRT" FORCE)
        add_link_options(/DEBUG:NONE /INCREMENTAL:NO /NOCOFFGRPINFO)
    else()
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC>:-g0>")
    endif()
endif()

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

message(STATUS "LiltView ${PROJECT_VERSION}: C${LILTVIEW_C_STANDARD} core / C++${LILTVIEW_CXX_STANDARD} consumers")
message(STATUS "Dependencies: local third_party sources; app components=${LILTVIEW_BUILD_APP_COMPONENTS}; Catch2=${LILTVIEW_BUILD_UNIT_TESTS}")
message(STATUS "JPEG/PNG/SVG/WebP/BMP/GIF/APNG/TIFF/QOI/ICO/CUR/JPEG2000/AVIF/JPEGXL/OpenEXR DISPLAY/FIT enabled; renderer backend=${LILTVIEW_RENDERER_BACKEND}; native tiled streaming / cached camera transforms")

if(LILTVIEW_RELEASE_PACKAGING AND LILTVIEW_BUILD_APP_COMPONENTS)
    include(cmake/LiltViewPackaging.cmake)
endif()
