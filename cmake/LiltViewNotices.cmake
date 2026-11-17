# Preserve redistribution notices even in the single-EXE distribution.
set(liltview_notice_files
    "LICENSE"
    "NOTICE"
    "assets/fonts/LICENSE.HarmonyOS-Sans.txt"
    "third_party/SDL/LICENSE.txt"
    "third_party/imgui/LICENSE.txt"
    "third_party/spdlog/LICENSE"
    "third_party/CLI11/LICENSE"
    "third_party/iguana/LICENSE"
    "third_party/iguana/LICENSE-Boost"
    "third_party/iguana/THIRD_PARTY_NOTICES.txt"
    "third_party/libjpeg-turbo/LICENSE.md"
    "third_party/libjpeg-turbo/README.ijg"
    "third_party/dav1d-ref/LICENSE"
    "third_party/lz4/lib/LICENSE"
    "third_party/libpng/LICENSE"
    "third_party/zlib/LICENSE"
    "third_party/plutosvg/LICENSE"
    "third_party/libwebp/COPYING"
    "third_party/libwebp/PATENTS"
    "third_party/giflib/COPYING"
    "third_party/libtiff/LICENSE.md"
    "third_party/plutosvg/plutovg/LICENSE"
    "third_party/plutosvg/plutovg/source/FTL.TXT"
    "third_party/PlutoVG-NOTICES.txt"
    "third_party/plasma-wayland-protocols/LICENSE.MIT"
)
set(LILTVIEW_NOTICES "LiltView - licenses and third-party notices\n")
foreach(relative IN LISTS liltview_notice_files)
    set(path "${PROJECT_SOURCE_DIR}/${relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    if(relative STREQUAL "assets/fonts/LICENSE.HarmonyOS-Sans.txt")
        # The original upstream notice has trailing NUL padding. Keep that file
        # untouched for redistribution, but embed its readable text, not NULs
        # that would truncate --licenses on GCC/Clang.
        file(STRINGS "${path}" lines ENCODING UTF-8)
        string(JOIN "\n" text ${lines})
    else()
        file(READ "${path}" text)
    endif()
    string(APPEND LILTVIEW_NOTICES "\n========== ${relative} ==========\n${text}\n")
endforeach()
configure_file("${PROJECT_SOURCE_DIR}/app/resources/notices.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/notices.hpp" @ONLY)
