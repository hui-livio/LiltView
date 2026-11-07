liltview_require_imgui()
liltview_require_spdlog()
liltview_require_sdl3()
liltview_require_app_headers()
include(GNUInstallDirs)
liltview_dependency_source(imgui backends/imgui_impl_sdl3.cpp imgui_source)
add_subdirectory(renderer)

add_library(liltview_imgui_sdl_backend STATIC "${imgui_source}/backends/imgui_impl_sdl3.cpp")
target_include_directories(liltview_imgui_sdl_backend SYSTEM PUBLIC "${imgui_source}/backends")
target_link_libraries(liltview_imgui_sdl_backend PUBLIC LiltView::imgui SDL3::SDL3-static)
add_library(liltview_sdl_platform STATIC platform/sdl_render_host.cpp)
target_include_directories(liltview_sdl_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(liltview_sdl_platform SYSTEM PUBLIC "${imgui_source}/backends")
target_link_libraries(liltview_sdl_platform PUBLIC liltview_imgui_sdl_backend)
liltview_configure_cxx_target(liltview_sdl_platform)

add_library(liltview_app_model STATIC i18n/translator.cpp settings/settings.cpp settings/store.cpp menu/menu_model.cpp)
add_library(LiltView::app_model ALIAS liltview_app_model)
target_include_directories(liltview_app_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
liltview_configure_cxx_target(liltview_app_model)
target_link_libraries(liltview_app_model PRIVATE LiltView::iguana)
target_compile_definitions(liltview_app_model PRIVATE THROW_UNKNOWN_KEY)

configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

add_library(liltview_cli STATIC cli/options.cpp)
target_include_directories(liltview_cli PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liltview_cli PRIVATE LiltView::cli11 PUBLIC LiltView::app_model)
liltview_configure_cxx_target(liltview_cli)

add_library(liltview_viewer STATIC viewer/viewer.cpp)
target_include_directories(liltview_viewer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liltview_viewer PUBLIC LiltView::core LiltView::renderer PRIVATE SDL3::SDL3-static spdlog::spdlog)
liltview_configure_cxx_target(liltview_viewer)
add_library(liltview_ui STATIC ui/shell.cpp)
add_library(LiltView::ui ALIAS liltview_ui)
target_include_directories(liltview_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liltview_ui PUBLIC LiltView::app_model LiltView::imgui LiltView::renderer liltview_viewer)
liltview_configure_cxx_target(liltview_ui)

add_library(liltview_logging STATIC logging/core_log.cpp logging/configure.cpp)
add_library(LiltView::logging ALIAS liltview_logging)
target_include_directories(liltview_logging PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liltview_logging PUBLIC LiltView::core LiltView::app_model PRIVATE spdlog::spdlog)
liltview_configure_cxx_target(liltview_logging)

add_library(liltview_fonts STATIC platform/fonts.cpp)
target_include_directories(liltview_fonts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liltview_fonts PUBLIC LiltView::imgui PRIVATE SDL3::SDL3-static)
liltview_configure_cxx_target(liltview_fonts)
add_executable(liltview main.cpp application.cpp)
target_include_directories(liltview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liltview PRIVATE liltview_cli LiltView::ui LiltView::logging liltview_sdl_platform liltview_fonts spdlog::spdlog)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Fontconfig REQUIRED)
    target_link_libraries(liltview_fonts PRIVATE Fontconfig::Fontconfig)
    option(LILTVIEW_ENABLE_GLOBAL_MENU "Export menus to the KDE Plasma system panel" ON)
    if(LILTVIEW_ENABLE_GLOBAL_MENU)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LILTVIEW_GIO REQUIRED IMPORTED_TARGET gio-2.0)
        pkg_check_modules(LILTVIEW_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
        find_package(X11 REQUIRED)
        find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
        set(appmenu_xml "${PROJECT_SOURCE_DIR}/third_party/plasma-wayland-protocols/appmenu.xml")
        set(appmenu_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/appmenu")
        file(MAKE_DIRECTORY "${appmenu_generated}")
        add_custom_command(OUTPUT "${appmenu_generated}/appmenu-client-protocol.h"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${appmenu_xml}" "${appmenu_generated}/appmenu-client-protocol.h"
            DEPENDS "${appmenu_xml}" VERBATIM)
        add_custom_command(OUTPUT "${appmenu_generated}/appmenu-protocol.c"
            COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${appmenu_xml}" "${appmenu_generated}/appmenu-protocol.c"
            DEPENDS "${appmenu_xml}" VERBATIM)
        add_library(liltview_appmenu_protocol STATIC "${appmenu_generated}/appmenu-protocol.c")
        target_link_libraries(liltview_appmenu_protocol PUBLIC PkgConfig::LILTVIEW_WAYLAND)
        target_sources(liltview PRIVATE platform/global_menu_linux.cpp "${appmenu_generated}/appmenu-client-protocol.h")
        target_include_directories(liltview PRIVATE "${appmenu_generated}")
        target_link_libraries(liltview PRIVATE liltview_appmenu_protocol PkgConfig::LILTVIEW_GIO X11::X11)
        install(FILES "${PROJECT_SOURCE_DIR}/third_party/plasma-wayland-protocols/LICENSE.MIT"
            "${PROJECT_SOURCE_DIR}/third_party/plasma-wayland-protocols/appmenu.xml"
            "${PROJECT_SOURCE_DIR}/third_party/plasma-wayland-protocols/source.json"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/plasma-wayland-protocols")
    else()
        target_sources(liltview PRIVATE platform/global_menu_stub.cpp)
    endif()
else()
    target_sources(liltview PRIVATE platform/global_menu_stub.cpp)
endif()
liltview_configure_cxx_target(liltview)

# Ship the original font and its license alongside the executable; no subset or
# build-time font conversion. Resolve it relative to SDL_GetBasePath at runtime.
set(font_dir "${PROJECT_SOURCE_DIR}/assets/fonts")
add_custom_command(TARGET liltview POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:liltview>/assets/fonts"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${font_dir}/HarmonyOS_Sans_SC_Regular.ttf"
        "${font_dir}/LICENSE.HarmonyOS-Sans.txt"
        "${font_dir}/source.json"
        "$<TARGET_FILE_DIR:liltview>/assets/fonts"
    VERBATIM)
set_property(TARGET liltview APPEND PROPERTY LINK_DEPENDS
    "${font_dir}/HarmonyOS_Sans_SC_Regular.ttf" "${font_dir}/LICENSE.HarmonyOS-Sans.txt" "${font_dir}/source.json")

install(TARGETS liltview RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${font_dir}/HarmonyOS_Sans_SC_Regular.ttf" "${font_dir}/LICENSE.HarmonyOS-Sans.txt" "${font_dir}/source.json"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/assets/fonts")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/NOTICE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/SDL/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/SDL")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/imgui/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/imgui")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/spdlog/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/spdlog")
foreach(header_library CLI11 iguana)
    install(FILES "${PROJECT_SOURCE_DIR}/third_party/${header_library}/LICENSE"
        "${PROJECT_SOURCE_DIR}/third_party/${header_library}/source.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/${header_library}")
endforeach()
install(FILES "${PROJECT_SOURCE_DIR}/third_party/iguana/LICENSE-Boost"
    "${PROJECT_SOURCE_DIR}/third_party/iguana/THIRD_PARTY_NOTICES.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/iguana")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/LICENSE.md"
    "${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/README.ijg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/libjpeg-turbo")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(app_id "io.github.hui_livio.LiltView")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/${app_id}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${PROJECT_SOURCE_DIR}/assets/icons/liltview-color.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps" RENAME "${app_id}.svg")
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/assets/icons/png/liltview-color-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps" RENAME "${app_id}.png")
    endforeach()
endif()
