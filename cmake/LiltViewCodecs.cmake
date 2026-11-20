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
# The decoder-only upstream target does not export src/, unlike webp.
target_include_directories(webpdecoder SYSTEM INTERFACE "$<BUILD_INTERFACE:${webp_source}/src>")

liltview_dependency_source(openjpeg CMakeLists.txt openjpeg_source)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_CODEC OFF CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_JPIP OFF CACHE BOOL "" FORCE)
set(BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_LUTS_GENERATOR OFF CACHE BOOL "" FORCE)
set(OPENJPEG_EMBEDDED ON CACHE BOOL "" FORCE)
add_subdirectory("${openjpeg_source}" "${PROJECT_BINARY_DIR}/third_party/openjpeg" EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_TESTING ON CACHE BOOL "" FORCE)
target_include_directories(openjp2 SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${openjpeg_source}/src/lib/openjp2>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/third_party/openjpeg/src/lib/openjp2>")

# libavif is vendored as a source snapshot. Build the pinned full dav1d
# decoder from the local submodule, and never let libavif fetch sources.
liltview_dependency_source(dav1d meson.build dav1d_source)
find_program(LILTVIEW_MESON_EXECUTABLE NAMES meson REQUIRED)
find_program(LILTVIEW_NINJA_EXECUTABLE NAMES ninja ninja-build REQUIRED)
set(dav1d_binary "${PROJECT_BINARY_DIR}/third_party/dav1d")
set(dav1d_install "${PROJECT_BINARY_DIR}/third_party/dav1d-install")
set(dav1d_library "${dav1d_install}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}dav1d${CMAKE_STATIC_LIBRARY_SUFFIX}")
file(MAKE_DIRECTORY "${dav1d_install}/include")
ExternalProject_Add(liltview_dav1d_build
    SOURCE_DIR "${dav1d_source}" BINARY_DIR "${dav1d_binary}" INSTALL_DIR "${dav1d_install}"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND "" PATCH_COMMAND ""
    CONFIGURE_COMMAND "${LILTVIEW_MESON_EXECUTABLE}" setup --buildtype=release --default-library=static
        --prefix=<INSTALL_DIR> --libdir=lib -Denable_asm=true -Denable_tools=false
        -Denable_examples=false -Denable_tests=false <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND "${LILTVIEW_NINJA_EXECUTABLE}" -C <BINARY_DIR>
    INSTALL_COMMAND "${LILTVIEW_NINJA_EXECUTABLE}" -C <BINARY_DIR> install
    BUILD_BYPRODUCTS "${dav1d_library}")
add_library(dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(dav1d::dav1d PROPERTIES
    IMPORTED_LOCATION "${dav1d_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${dav1d_install}/include")
add_dependencies(dav1d::dav1d liltview_dav1d_build)

# JPEG XL is built from its pinned recursive source tree. Keep only the
# decoder library and its local Highway/Brotli/CMS dependencies.
liltview_dependency_source(libjxl CMakeLists.txt libjxl_source)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_TRANSCODE_JPEG OFF CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_BOXES ON CACHE BOOL "" FORCE)
set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "" FORCE)
set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${libjxl_source}" "${PROJECT_BINARY_DIR}/third_party/libjxl" EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_TESTING ${LILTVIEW_BUILD_UNIT_TESTS} CACHE BOOL "" FORCE)

# OpenEXR is used through its C OpenEXRCore API. Build the pinned Imath
# submodule first so OpenEXR never invokes FetchContent or selects a system
# package during configuration. OpenEXR's own deflate, zstd and OpenJPH
# sources remain the only compression backends in this build.
liltview_dependency_source(Imath CMakeLists.txt imath_source)
set(liltview_openexr_saved_build_testing "${BUILD_TESTING}")
set(IMATH_INSTALL OFF CACHE BOOL "" FORCE)
set(IMATH_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(PYTHON OFF CACHE BOOL "" FORCE)
set(PYBIND11 OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${imath_source}" "${PROJECT_BINARY_DIR}/third_party/Imath" EXCLUDE_FROM_ALL SYSTEM)
set(Imath_VERSION "3.2.3")
set(Imath_VERSION_MAJOR 3)
set(Imath_VERSION_MINOR 2)
set(Imath_VERSION_PATCH 3)
set(Imath_SOVERSION 30)

liltview_dependency_source(openexr CMakeLists.txt openexr_source)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DEVELOPER_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_LIBS ON CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_ZSTD ON CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "" FORCE)
add_subdirectory("${openexr_source}" "${PROJECT_BINARY_DIR}/third_party/openexr" EXCLUDE_FROM_ALL SYSTEM)
if(liltview_openexr_saved_build_testing)
    set(BUILD_TESTING ON CACHE BOOL "" FORCE)
else()
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
endif()

# HEIF is kept behind a reproducible LGPL-3 dependency boundary. Build the
# pinned libde265 target first, then force libheif to use it as a built-in
# decoder. Every other codec, plugin, example and system discovery path is
# disabled so the static target cannot silently gain unrelated dependencies.
liltview_dependency_source(libde265 CMakeLists.txt libde265_source)
set(liltview_heif_saved_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_DECODER ON CACHE BOOL "" FORCE)
set(ENABLE_ENCODER OFF CACHE BOOL "" FORCE)
set(ENABLE_INTERNAL_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_SHERLOCK265 OFF CACHE BOOL "" FORCE)
add_subdirectory("${libde265_source}" "${PROJECT_BINARY_DIR}/third_party/libde265" EXCLUDE_FROM_ALL SYSTEM)
target_include_directories(de265 SYSTEM PRIVATE
    "$<BUILD_INTERFACE:${libde265_source}/libde265>"
    "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/third_party/libde265/libde265>")

liltview_dependency_source(libheif CMakeLists.txt libheif_source)
set(ENABLE_PLUGIN_LOADING OFF CACHE BOOL "" FORCE)
set(WITH_LIBDE265 ON CACHE BOOL "" FORCE)
set(WITH_LIBDE265_PLUGIN OFF CACHE BOOL "" FORCE)
foreach(codec X265 KVAZAAR UVG266 VVDEC VVENC AOM_DECODER AOM_ENCODER SvtEnc RAV1E
                FFMPEG_DECODER JPEG_DECODER JPEG_ENCODER OpenJPEG_DECODER OpenJPEG_ENCODER
                OPENJPH_ENCODER X264 OpenH264_DECODER OpenH264_ENCODER DAV1D)
    set(WITH_${codec} OFF CACHE BOOL "" FORCE)
    set(WITH_${codec}_PLUGIN OFF CACHE BOOL "" FORCE)
endforeach()
set(WITH_UNCOMPRESSED_CODEC OFF CACHE BOOL "" FORCE)
set(WITH_WEBCODECS OFF CACHE BOOL "" FORCE)
set(WITH_LIBSHARPYUV OFF CACHE BOOL "" FORCE)
set(WITH_LIBSHARPYUV_INTERNAL OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_THUMB OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLE_HEIF_VIEW OFF CACHE BOOL "" FORCE)
set(WITH_GDK_PIXBUF OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBDE265_FOUND TRUE CACHE BOOL "" FORCE)
set(LIBDE265_INCLUDE_DIRS
    "${libde265_source}/libde265;${PROJECT_BINARY_DIR}/third_party/libde265/libde265" CACHE STRING "" FORCE)
set(LIBDE265_LIBRARIES de265 CACHE STRING "" FORCE)
add_subdirectory("${libheif_source}" "${PROJECT_BINARY_DIR}/third_party/libheif" EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_SHARED_LIBS "${liltview_heif_saved_shared_libs}" CACHE BOOL "" FORCE)

set(liltview_avif_source "${PROJECT_SOURCE_DIR}/third_party/libavif")
if(NOT EXISTS "${liltview_avif_source}/CMakeLists.txt")
    message(FATAL_ERROR "Missing libavif source tree: third_party/libavif/CMakeLists.txt")
endif()
set(liltview_avif_saved_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(AVIF_CODEC_AOM OFF CACHE STRING "" FORCE)
set(AVIF_CODEC_DAV1D SYSTEM CACHE STRING "" FORCE)
set(AVIF_CODEC_LIBGAV1 OFF CACHE STRING "" FORCE)
set(AVIF_CODEC_RAV1E OFF CACHE STRING "" FORCE)
set(AVIF_CODEC_SVT OFF CACHE STRING "" FORCE)
set(AVIF_CODEC_AVM OFF CACHE STRING "" FORCE)
set(AVIF_LIBYUV OFF CACHE STRING "" FORCE)
set(AVIF_LIBSHARPYUV OFF CACHE STRING "" FORCE)
set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_MAN_PAGES OFF CACHE BOOL "" FORCE)
add_subdirectory("${liltview_avif_source}" "${PROJECT_BINARY_DIR}/third_party/libavif" EXCLUDE_FROM_ALL SYSTEM)
set(BUILD_SHARED_LIBS "${liltview_avif_saved_shared_libs}" CACHE BOOL "" FORCE)

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
