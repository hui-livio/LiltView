if(WIN32)
    set(liltview_file_adapter src/platform/file_win32.c)
elseif(UNIX)
    set(liltview_file_adapter src/platform/file.c)
else()
    message(FATAL_ERROR "The file adapter supports POSIX and Windows only")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/LiltViewJpeg.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/LiltViewCodecs.cmake")
find_package(Threads REQUIRED)
add_library(liltview_core STATIC src/api/memory.c src/api/config.c src/api/raw.c src/api/engine.c src/api/animation.c
    src/engine/objects.c src/engine/worker.c src/engine/animation.c src/engine/cache.c
    src/codecs/raster.c src/codecs/image.c src/codecs/jpeg.c src/codecs/png.c src/codecs/webp.c src/codecs/bmp.c src/codecs/svg.c
    src/codecs/gif.c src/codecs/gif_animation.c src/codecs/apng_animation.c src/codecs/webp_animation.c src/codecs/tiff.c src/codecs/qoi.c src/codecs/ico.c src/codecs/jpeg2000.c src/codecs/avif.c src/codecs/jpegxl.c src/codecs/openexr.c src/codecs/heif.c src/codecs/raw.c
    ${liltview_file_adapter})
add_library(LiltView::core ALIAS liltview_core)
target_include_directories(liltview_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(liltview_core SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
liltview_configure_c_target(liltview_core)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(liltview_core PRIVATE _FILE_OFFSET_BITS=64)
endif()
target_link_libraries(liltview_core PRIVATE liltview_jpeg png_static webpdecoder webpdemux openjp2 avif jxl OpenEXR::OpenEXRCore heif plutosvg::plutosvg
    liltview_gif TIFF::tiff Threads::Threads)
if(UNIX)
    target_compile_definitions(liltview_core PRIVATE LILTVIEW_HAS_LIBRAW=1)
    target_link_libraries(liltview_core PRIVATE LiltView::libraw)
endif()
add_dependencies(liltview_core liltview_jpeg_build)
