if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The file/thread adapter currently supports Linux; Windows adapter is pending")
endif()
include("${PROJECT_SOURCE_DIR}/cmake/LiltViewJpeg.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/LiltViewCodecs.cmake")
find_package(Threads REQUIRED)
add_library(liltview_core STATIC src/api/config.c src/api/engine.c src/api/animation.c
    src/engine/objects.c src/engine/worker.c src/engine/animation.c
    src/codecs/image.c src/codecs/jpeg.c src/codecs/png.c src/codecs/webp.c src/codecs/bmp.c src/codecs/svg.c
    src/codecs/gif.c src/codecs/gif_animation.c src/codecs/tiff.c
    src/platform/file.c)
add_library(LiltView::core ALIAS liltview_core)
target_include_directories(liltview_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
liltview_configure_c_target(liltview_core)
target_link_libraries(liltview_core PRIVATE liltview_jpeg png_static webpdecoder plutosvg::plutosvg
    liltview_gif TIFF::tiff Threads::Threads)
add_dependencies(liltview_core liltview_jpeg_build)
