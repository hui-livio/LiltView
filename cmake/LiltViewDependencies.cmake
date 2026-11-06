include_guard(GLOBAL)

# Versioned bootstrap baselines, not claims about the latest releases.
# Resolve and record immutable revisions before release. See docs/PROJECT_STRUCTURE.md.
set(LILTVIEW_IMGUI_REF "v1.92.0")
set(LILTVIEW_SPDLOG_REF "v1.15.3")
set(LILTVIEW_CATCH2_REF "v3.8.1")

function(liltview_require_imgui)
    if(TARGET LiltView::imgui)
        return()
    endif()
    add_library(liltview_imgui INTERFACE)
    add_library(LiltView::imgui ALIAS liltview_imgui)
    if(LILTVIEW_DEPENDENCY_MODE STREQUAL "SYSTEM")
        find_package(imgui 1.92 CONFIG QUIET)
        if(TARGET imgui::imgui)
            target_link_libraries(liltview_imgui INTERFACE imgui::imgui)
        else()
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(LILTVIEW_SYSTEM_IMGUI REQUIRED IMPORTED_TARGET "imgui>=1.92")
            target_link_libraries(liltview_imgui INTERFACE PkgConfig::LILTVIEW_SYSTEM_IMGUI)
        endif()
    else()
        include(FetchContent)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG ${LILTVIEW_IMGUI_REF}
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(imgui)
        add_library(liltview_imgui_vendor STATIC
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
        target_include_directories(liltview_imgui_vendor SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
        target_compile_features(liltview_imgui_vendor PUBLIC cxx_std_11)
        target_link_libraries(liltview_imgui INTERFACE liltview_imgui_vendor)
    endif()
endfunction()

function(liltview_require_spdlog)
    if(TARGET spdlog::spdlog)
        return()
    endif()
    if(LILTVIEW_DEPENDENCY_MODE STREQUAL "SYSTEM")
        find_package(spdlog 1.15 CONFIG REQUIRED)
    else()
        include(FetchContent)
        set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
        set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
        set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG ${LILTVIEW_SPDLOG_REF}
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(spdlog)
    endif()
endfunction()

function(liltview_require_catch2)
    if(LILTVIEW_DEPENDENCY_MODE STREQUAL "SYSTEM")
        find_package(Catch2 3.8 CONFIG REQUIRED)
        set(LILTVIEW_CATCH_SCRIPT "${Catch2_DIR}/Catch.cmake" PARENT_SCOPE)
    else()
        include(FetchContent)
        set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG ${LILTVIEW_CATCH2_REF}
            GIT_SHALLOW TRUE
            EXCLUDE_FROM_ALL)
        FetchContent_MakeAvailable(Catch2)
        set(LILTVIEW_CATCH_SCRIPT "${catch2_SOURCE_DIR}/extras/Catch.cmake" PARENT_SCOPE)
    endif()
endfunction()
