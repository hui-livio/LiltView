include_guard(GLOBAL)
include(ExternalProject)
liltview_dependency_source(libjpeg-turbo CMakeLists.txt jpeg_source)
set(jpeg_binary "${PROJECT_BINARY_DIR}/third_party/libjpeg-turbo")
file(MAKE_DIRECTORY "${jpeg_binary}")
# Upstream explicitly requires a standalone build (not add_subdirectory).
ExternalProject_Add(liltview_jpeg_build
    SOURCE_DIR "${jpeg_source}" BINARY_DIR "${jpeg_binary}"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND "" INSTALL_COMMAND ""
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}" "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
        -DWITH_SIMD=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    BUILD_BYPRODUCTS "${jpeg_binary}/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_library(liltview_jpeg STATIC IMPORTED GLOBAL)
set_target_properties(liltview_jpeg PROPERTIES
    IMPORTED_LOCATION "${jpeg_binary}/${CMAKE_STATIC_LIBRARY_PREFIX}jpeg${CMAKE_STATIC_LIBRARY_SUFFIX}"
    INTERFACE_INCLUDE_DIRECTORIES "${jpeg_source}/src;${jpeg_binary}")
add_dependencies(liltview_jpeg liltview_jpeg_build)
