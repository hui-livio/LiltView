liltview_require_imgui()
liltview_require_spdlog()
liltview_require_sdl3()
find_package(OpenGL REQUIRED)
include(GNUInstallDirs)

add_library(liltview_app_model STATIC i18n/translator.cpp settings/settings.cpp menu/menu_model.cpp)
add_library(LiltView::app_model ALIAS liltview_app_model)
target_include_directories(liltview_app_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
liltview_configure_cxx_target(liltview_app_model)

configure_file(version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

liltview_dependency_source(imgui backends/imgui_impl_sdl3.cpp imgui_source)
add_library(liltview_imgui_backends STATIC
    "${imgui_source}/backends/imgui_impl_sdl3.cpp"
    "${imgui_source}/backends/imgui_impl_opengl3.cpp")
target_include_directories(liltview_imgui_backends SYSTEM PUBLIC "${imgui_source}/backends")
target_link_libraries(liltview_imgui_backends PUBLIC LiltView::imgui SDL3::SDL3 OpenGL::GL)
target_compile_features(liltview_imgui_backends PRIVATE cxx_std_11)

add_library(liltview_ui STATIC ui/shell.cpp)
add_library(LiltView::ui ALIAS liltview_ui)
target_include_directories(liltview_ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liltview_ui PUBLIC LiltView::app_model LiltView::imgui)
liltview_configure_cxx_target(liltview_ui)

add_library(liltview_logging STATIC logging/core_log.cpp)
add_library(LiltView::logging ALIAS liltview_logging)
target_include_directories(liltview_logging PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(liltview_logging PUBLIC LiltView::core PRIVATE spdlog::spdlog)
liltview_configure_cxx_target(liltview_logging)

add_executable(liltview main.cpp application.cpp platform/fonts.cpp)
target_include_directories(liltview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(liltview PRIVATE LiltView::ui LiltView::logging liltview_imgui_backends spdlog::spdlog)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Fontconfig REQUIRED)
    target_link_libraries(liltview PRIVATE Fontconfig::Fontconfig)
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

if(UNIX AND NOT APPLE)
    set_target_properties(liltview PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS liltview RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS SDL3-shared
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${font_dir}/HarmonyOS_Sans_SC_Regular.ttf" "${font_dir}/LICENSE.HarmonyOS-Sans.txt" "${font_dir}/source.json"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/assets/fonts")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/SDL/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/SDL")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/imgui/LICENSE.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/imgui")
install(FILES "${PROJECT_SOURCE_DIR}/third_party/spdlog/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/LiltView/spdlog")

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
