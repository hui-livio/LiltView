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
    "third_party/dav1d/COPYING"
    "third_party/libjxl/LICENSE"
    "third_party/lz4/lib/LICENSE"
    "third_party/libpng/LICENSE"
    "third_party/zlib/LICENSE"
    "third_party/plutosvg/LICENSE"
    "third_party/libwebp/COPYING"
    "third_party/libwebp/PATENTS"
    "third_party/giflib/COPYING"
    "third_party/libtiff/LICENSE.md"
    "third_party/libavif/LICENSE"
    "third_party/openexr/LICENSE.md"
    "third_party/Imath/LICENSE.md"
    "third_party/openexr/external/deflate/COPYING"
    "third_party/openexr/external/zstd/LICENSE"
    "third_party/openexr/external/OpenJPH/LICENSE"
    "third_party/libheif/COPYING"
    "third_party/libde265/COPYING"
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
# MSVC before 14.51 limits each string literal to roughly 16 KiB. Encode
# fixed-size byte chunks instead of one raw literal. Hex escapes keep UTF-8 byte
# boundaries, quotes, backslashes and raw-string delimiters completely lossless.
# Adjacent literals concatenate into one NUL-terminated array without separators.
string(HEX "${LILTVIEW_NOTICES}" liltview_notices_hex)
string(LENGTH "${liltview_notices_hex}" liltview_notices_hex_length)
set(LILTVIEW_NOTICES_LITERALS "")
set(liltview_notices_offset 0)
while(liltview_notices_offset LESS liltview_notices_hex_length)
    # 2048 hex digits = at most 1024 decoded bytes (4096 source characters).
    string(SUBSTRING "${liltview_notices_hex}" ${liltview_notices_offset} 2048 chunk)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" escaped "${chunk}")
    string(APPEND LILTVIEW_NOTICES_LITERALS "\"${escaped}\"\n")
    math(EXPR liltview_notices_offset "${liltview_notices_offset} + 2048")
endwhile()
configure_file("${PROJECT_SOURCE_DIR}/app/resources/notices.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/notices.hpp" @ONLY)
