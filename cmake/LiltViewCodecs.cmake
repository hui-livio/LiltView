include_guard(GLOBAL)

# Only static runtime libraries; upstream tools, examples and tests are excluded.
liltview_dependency_source(zlib CMakeLists.txt zlib_source)
set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${zlib_source}" "${PROJECT_BINARY_DIR}/third_party/zlib" EXCLUDE_FROM_ALL SYSTEM)
add_library(ZLIB::ZLIB ALIAS zlibstatic)
# FindZLIB still runs in libpng; give it our local headers/library and target.
set(ZLIB_INCLUDE_DIR "${zlib_source}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)

liltview_dependency_source(libpng CMakeLists.txt png_source)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON)
add_subdirectory("${png_source}" "${PROJECT_BINARY_DIR}/third_party/libpng" EXCLUDE_FROM_ALL SYSTEM)
unset(SKIP_INSTALL_ALL)

liltview_dependency_source(libwebp CMakeLists.txt webp_source)
foreach(feature ANIM_UTILS CWEBP DWEBP GIF2WEBP IMG2WEBP VWEBP WEBPINFO LIBWEBPMUX WEBPMUX EXTRAS WEBP_JS FUZZTEST)
    set(WEBP_BUILD_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(WEBP_USE_THREAD OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${webp_source}" "${PROJECT_BINARY_DIR}/third_party/libwebp" EXCLUDE_FROM_ALL SYSTEM)

liltview_dependency_source(plutosvg CMakeLists.txt svg_source)
if(NOT EXISTS "${svg_source}/plutovg/CMakeLists.txt")
    message(FATAL_ERROR "Missing PlutoSVG's pinned PlutoVG: git submodule update --init --recursive")
endif()
set(PLUTOVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD ON CACHE BOOL "" FORCE)
set(PLUTOSVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PLUTOSVG_ENABLE_FREETYPE OFF CACHE BOOL "" FORCE)
# Add the recorded nested submodule first; never select a system PlutoVG.
add_subdirectory("${svg_source}/plutovg" "${PROJECT_BINARY_DIR}/third_party/plutovg" EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory("${svg_source}" "${PROJECT_BINARY_DIR}/third_party/plutosvg" EXCLUDE_FROM_ALL SYSTEM)

# giflib has a Makefile upstream; build just its unmodified library sources.
liltview_dependency_source(giflib gif_lib.h gif_source)
add_library(liltview_gif STATIC
    "${gif_source}/dgif_lib.c" "${gif_source}/gifalloc.c"
    "${gif_source}/gif_err.c" "${gif_source}/openbsd-reallocarray.c")
target_include_directories(liltview_gif SYSTEM PUBLIC "${gif_source}")
set_target_properties(liltview_gif PROPERTIES C_STANDARD 99)
if(LILTVIEW_BUILD_UNIT_TESTS)
    add_library(liltview_gif_encoder STATIC "${gif_source}/egif_lib.c" "${gif_source}/gif_hash.c")
    target_link_libraries(liltview_gif_encoder PUBLIC liltview_gif)
endif()

# Reuse static zlib/JPEG and keep optional system codecs out of the build.
liltview_dependency_source(libtiff CMakeLists.txt tiff_source)
foreach(feature tiff-tools tiff-tests tiff-contrib tiff-docs tiff-install tiff-cxx tiff-opengl
        libdeflate pixarlog old-jpeg jpeg12 jbig lerc lzma zstd webp thunder next logluv mdi extrasample-as-alpha)
    set(${feature} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(feature tiff-static ccitt packbits lzw zlib jpeg jpeg-prefer-standard)
    set(${feature} ON CACHE BOOL "" FORCE)
endforeach()
foreach(package Deflate JBIG LERC liblzma ZSTD WebP OpenGL)
    set(CMAKE_DISABLE_FIND_PACKAGE_${package} TRUE)
endforeach()
add_library(JPEG::JPEG ALIAS liltview_jpeg)
set(JPEG_FOUND TRUE)
set(JPEG_LIBRARIES liltview_jpeg)
# ExternalProject JPEG is not built during configure: use the 8-bit interface.
set(HAVE_JPEGTURBO_DUAL_MODE_8 OFF CACHE INTERNAL "" FORCE)
set(HAVE_JPEGTURBO_DUAL_MODE_12 OFF CACHE INTERNAL "" FORCE)
set(TIFF_MAX_DIR_COUNT 64 CACHE STRING "" FORCE)
add_subdirectory("${tiff_source}" "${PROJECT_BINARY_DIR}/third_party/libtiff" EXCLUDE_FROM_ALL SYSTEM)
add_dependencies(tiff liltview_jpeg_build)
