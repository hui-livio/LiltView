add_library(liltview_api_layout OBJECT contract/layout.c)
target_link_libraries(liltview_api_layout PRIVATE LiltView::core)
liltview_configure_c_target(liltview_api_layout)

add_executable(liltview_contract_c contract/core_c.c)
target_link_libraries(liltview_contract_c PRIVATE LiltView::core)
liltview_configure_c_target(liltview_contract_c)
add_test(NAME contract.c${LILTVIEW_C_STANDARD} COMMAND liltview_contract_c)

set(contract_cxx_standards 20)
if(LILTVIEW_CXX_STANDARD STREQUAL "23")
    list(APPEND contract_cxx_standards 23)
endif()
foreach(standard IN LISTS contract_cxx_standards)
    add_executable(liltview_contract_cpp${standard} contract/core_cpp.cpp $<TARGET_OBJECTS:liltview_api_layout>)
    target_link_libraries(liltview_contract_cpp${standard} PRIVATE LiltView::core)
    liltview_configure_cxx_target(liltview_contract_cpp${standard})
    set_target_properties(liltview_contract_cpp${standard} PROPERTIES CXX_STANDARD ${standard})
    add_test(NAME contract.cpp${standard} COMMAND liltview_contract_cpp${standard})
endforeach()

# Public ABI 2 animation type and layout checks (C23 / C++20 / C++23).
add_library(liltview_animation_layout OBJECT contract/animation.c)
target_link_libraries(liltview_animation_layout PRIVATE LiltView::core)
liltview_configure_c_target(liltview_animation_layout)
foreach(standard IN LISTS contract_cxx_standards)
    add_executable(liltview_animation_cpp${standard} contract/animation.cpp
        $<TARGET_OBJECTS:liltview_animation_layout>)
    target_link_libraries(liltview_animation_cpp${standard} PRIVATE LiltView::core)
    liltview_configure_cxx_target(liltview_animation_cpp${standard})
    set_target_properties(liltview_animation_cpp${standard} PROPERTIES CXX_STANDARD ${standard})
    add_test(NAME contract.animation.cpp${standard} COMMAND liltview_animation_cpp${standard})
endforeach()

if(LILTVIEW_BUILD_UNIT_TESTS)
    add_executable(liltview_pixel_ownership core/pixel_ownership.c)
    target_link_libraries(liltview_pixel_ownership PRIVATE LiltView::core)
    liltview_configure_c_target(liltview_pixel_ownership)
    add_test(NAME core.pixel_ownership COMMAND liltview_pixel_ownership "${PROJECT_SOURCE_DIR}/tests/fixtures/red.tiff")
    liltview_require_catch2()
    add_library(liltview_test_main STATIC support/catch_main.cpp)
    target_link_libraries(liltview_test_main PUBLIC Catch2::Catch2)
    liltview_configure_cxx_target(liltview_test_main)
    set(liltview_skip_marker "LILTVIEW_ALL_TESTS_SKIPPED")
    add_executable(liltview_runner_cases support/runner_cases.cpp)
    target_link_libraries(liltview_runner_cases PRIVATE liltview_test_main)
    liltview_configure_cxx_target(liltview_runner_cases)
    add_test(NAME test.runner_exit_codes COMMAND "${CMAKE_COMMAND}"
        "-DRUNNER=$<TARGET_FILE:liltview_runner_cases>" "-DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/runner-contract"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/support/runner_contract.cmake")
    set_tests_properties(test.runner_exit_codes PROPERTIES TIMEOUT 60)
    add_executable(liltview_core_tests core/config_tests.cpp core/cache_tests.cpp core/jpeg_tests.cpp core/format_tests.cpp core/gif_tiff_tests.cpp core/animation_tests.cpp core/region_tests.cpp core/file_offset_tests.cpp)
    target_link_libraries(liltview_core_tests PRIVATE LiltView::core liltview_jpeg png_static webp
        liltview_gif_encoder TIFF::tiff liltview_test_main)
    liltview_configure_cxx_target(liltview_core_tests)
    add_executable(liltview_make_checkerboard fixtures/make_checkerboard.cpp)
    target_link_libraries(liltview_make_checkerboard PRIVATE liltview_jpeg png_static TIFF::tiff)
    liltview_configure_cxx_target(liltview_make_checkerboard)
    include("${LILTVIEW_CATCH_SCRIPT}")
    catch_discover_tests(liltview_core_tests TEST_PREFIX "core."
        PROPERTIES TIMEOUT 30 SKIP_REGULAR_EXPRESSION "${liltview_skip_marker}")
    if(TARGET LiltView::app_model)
        add_executable(liltview_app_tests app/live_resize_tests.cpp app/window_chrome_tests.cpp app/theme_tests.cpp app/image_details_tests.cpp app/information_shell_tests.cpp app/navigation_button_tests.cpp app/close_timing_tests.cpp app/model_tests.cpp app/resource_tests.cpp app/renderer_tests.cpp app/cli_tests.cpp app/logging_tests.cpp)
        target_link_libraries(liltview_app_tests PRIVATE liltview_cli LiltView::logging spdlog::spdlog LiltView::app_model
            liltview_fonts LiltView::renderer LiltView::ui liltview_sdl_platform SDL3::SDL3-static liltview_test_main)
        target_compile_definitions(liltview_app_tests PRIVATE LILTVIEW_TEST_FONT_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
        liltview_configure_cxx_target(liltview_app_tests)
        if(WIN32)
            target_sources(liltview_app_tests PRIVATE app/windows_resources_tests.cpp app/windows_window_tests.cpp app/windows_live_resize_tests.cpp
                "${PROJECT_SOURCE_DIR}/app/platform/window_shape.cpp" "${PROJECT_SOURCE_DIR}/app/platform/window_theme.cpp")
            target_include_directories(liltview_app_tests PRIVATE "${PROJECT_BINARY_DIR}/app/generated")
            target_compile_definitions(liltview_app_tests PRIVATE
                LILTVIEW_TEST_APP_PATH="$<TARGET_FILE:liltview>"
                LILTVIEW_TEST_GUI=$<IF:$<CONFIG:Release>,1,0>
                LILTVIEW_TEST_DEBUG=$<IF:$<CONFIG:Debug>,1,0>)
            target_link_libraries(liltview_app_tests PRIVATE version dwmapi comctl32)
            add_dependencies(liltview_app_tests liltview)
        endif()
        catch_discover_tests(liltview_app_tests TEST_PREFIX "app."
            PROPERTIES TIMEOUT 30 SKIP_REGULAR_EXPRESSION "${liltview_skip_marker}")
        add_executable(liltview_viewer_tests app/viewer_tests.cpp app/navigation_tests.cpp app/prefetch_tests.cpp app/animation_warmup_tests.cpp app/playback_tests.cpp app/viewport_tests.cpp app/tile_reader_tests.cpp)
        target_link_libraries(liltview_viewer_tests PRIVATE liltview_viewer SDL3::SDL3-static liltview_jpeg png_static TIFF::tiff liltview_gif_encoder liltview_test_main)
        target_compile_definitions(liltview_viewer_tests PRIVATE
            LILTVIEW_IMAGE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures"
            LILTVIEW_JPEG_FIXTURES="${PROJECT_SOURCE_DIR}/third_party/libjpeg-turbo/testimages")
        liltview_configure_cxx_target(liltview_viewer_tests)
        catch_discover_tests(liltview_viewer_tests TEST_PREFIX "viewer."
            PROPERTIES TIMEOUT 30 SKIP_REGULAR_EXPRESSION "${liltview_skip_marker}")
    endif()
endif()

if(TARGET liltview)
    add_test(NAME app.help COMMAND liltview --help)
    set_tests_properties(app.help PROPERTIES PASS_REGULAR_EXPRESSION "settings.yaml")
    add_test(NAME app.version COMMAND liltview --version)
    set_tests_properties(app.version PROPERTIES PASS_REGULAR_EXPRESSION "LiltView ${PROJECT_VERSION}")
    add_test(NAME app.invalid_language COMMAND liltview --language invalid)
    set_tests_properties(app.invalid_language PROPERTIES WILL_FAIL TRUE)
endif()

option(LILTVIEW_BUILD_DESKTOP_TESTS "Run Linux offscreen/DBus integration tests (requires working EGL/OpenGL)" OFF)
if(LILTVIEW_BUILD_DESKTOP_TESTS)
    if(NOT TARGET liltview OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT LILTVIEW_ENABLE_GLOBAL_MENU)
        message(FATAL_ERROR "Desktop regressions require the Linux application and global menu")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
    find_program(GDBUS gdbus REQUIRED)
    find_package(OpenGL REQUIRED)
    add_executable(liltview_renderer_contract integration/renderer_contract.cpp)
    target_link_libraries(liltview_renderer_contract PRIVATE LiltView::renderer liltview_sdl_platform OpenGL::GL)
    liltview_configure_cxx_target(liltview_renderer_contract)
    add_executable(liltview_animation_render integration/animation_render.cpp)
    target_link_libraries(liltview_animation_render PRIVATE liltview_viewer liltview_sdl_platform)
    liltview_configure_cxx_target(liltview_animation_render)
    add_executable(liltview_navigation_render integration/navigation_render.cpp)
    target_link_libraries(liltview_navigation_render PRIVATE liltview_viewer liltview_sdl_platform
        liltview_jpeg png_static TIFF::tiff)
    liltview_configure_cxx_target(liltview_navigation_render)
    add_test(NAME desktop.navigation_render COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_navigation_render>)
    set_tests_properties(desktop.navigation_render PROPERTIES TIMEOUT 30)
    # Opt-in real-image benchmark; user files are never CTest fixtures.
    add_executable(liltview_viewer_benchmark integration/viewer_benchmark.cpp)
    target_link_libraries(liltview_viewer_benchmark PRIVATE liltview_viewer liltview_sdl_platform)
    liltview_configure_cxx_target(liltview_viewer_benchmark)
    add_executable(liltview_viewer_sharpness integration/viewer_sharpness.cpp)
    target_link_libraries(liltview_viewer_sharpness PRIVATE liltview_viewer liltview_sdl_platform
        liltview_jpeg png_static TIFF::tiff)
    liltview_configure_cxx_target(liltview_viewer_sharpness)
    add_test(NAME desktop.viewer_sharpness COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_viewer_sharpness>)
    set_tests_properties(desktop.viewer_sharpness PROPERTIES TIMEOUT 60)
    add_test(NAME desktop.viewport_restore COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_viewer_sharpness> --restore)
    set_tests_properties(desktop.viewport_restore PROPERTIES TIMEOUT 60)
    add_test(NAME desktop.interaction COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_viewer_sharpness> --interaction)
    set_tests_properties(desktop.interaction PROPERTIES TIMEOUT 60)
    add_test(NAME desktop.covered_motion COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_viewer_sharpness> --motion)
    set_tests_properties(desktop.covered_motion PROPERTIES TIMEOUT 60)
    add_test(NAME desktop.animation COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_animation_render> "${PROJECT_SOURCE_DIR}/tests/fixtures/red-first-frame.gif")
    set_tests_properties(desktop.animation PROPERTIES TIMEOUT 15)
    add_test(NAME desktop.animation_warm COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_animation_render> "${PROJECT_SOURCE_DIR}/tests/fixtures/red-first-frame.gif" --warm)
    set_tests_properties(desktop.animation_warm PROPERTIES TIMEOUT 30)
    add_test(NAME desktop.renderer COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen $<TARGET_FILE:liltview_renderer_contract>)
    set_tests_properties(desktop.renderer PROPERTIES TIMEOUT 30)
    add_test(NAME desktop.renderer_direct COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
        $<TARGET_FILE:liltview_renderer_contract> --direct)
    set_tests_properties(desktop.renderer_direct PROPERTIES TIMEOUT 30)
    foreach(case menu font save jpeg formats animation_app settings logging status close chrome navigation info prefetch dialog_failure)
        add_test(NAME desktop.${case}
            COMMAND "${DBUS_RUN_SESSION}" -- "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/integration/desktop_regressions.py" "$<TARGET_FILE:liltview>" "${case}"
                "$<IF:$<CONFIG:Debug>,debug,warn>")
        set_tests_properties(desktop.${case} PROPERTIES TIMEOUT 30)
    endforeach()
endif()


option(LILTVIEW_BUILD_LARGE_IMAGE_TESTS "Generate and decode 16K/32K checkerboards (larger disk/time cost)" OFF)
if(LILTVIEW_BUILD_LARGE_IMAGE_TESTS)
    if(NOT LILTVIEW_BUILD_UNIT_TESTS)
        message(FATAL_ERROR "Large image regressions require LILTVIEW_BUILD_UNIT_TESTS")
    endif()
    add_executable(liltview_large_image_tests core/large_tests.cpp)
    target_link_libraries(liltview_large_image_tests PRIVATE LiltView::core liltview_jpeg png_static TIFF::tiff liltview_test_main)
    target_compile_definitions(liltview_large_image_tests PRIVATE LILTVIEW_LARGE_FIXTURE_DIR="${PROJECT_BINARY_DIR}/large-images")
    liltview_configure_cxx_target(liltview_large_image_tests)
    if(LILTVIEW_BUILD_DESKTOP_TESTS)
        add_executable(liltview_large_render integration/large_render.cpp)
        target_link_libraries(liltview_large_render PRIVATE liltview_viewer liltview_sdl_platform)
        liltview_configure_cxx_target(liltview_large_render)
        add_test(NAME large.render COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=offscreen
            $<TARGET_FILE:liltview_large_render> "${PROJECT_BINARY_DIR}/large-images/checkerboard-32768.tiff"
            "${PROJECT_BINARY_DIR}/large-images")
        set_tests_properties(large.render PROPERTIES TIMEOUT 180 LABELS large RUN_SERIAL TRUE FIXTURES_REQUIRED checkerboard_tiff)
    endif()
    foreach(format png jpeg bmp tiff)
        add_test(NAME large.${format} COMMAND liltview_large_image_tests "[large_${format}]")
        set_tests_properties(large.${format} PROPERTIES TIMEOUT 180 LABELS large RUN_SERIAL TRUE
            SKIP_REGULAR_EXPRESSION "${liltview_skip_marker}")
        if(format STREQUAL "tiff")
            set_tests_properties(large.tiff PROPERTIES FIXTURES_SETUP checkerboard_tiff)
        endif()
    endforeach()
endif()
