include_guard(GLOBAL)

include(FetchContent)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LILTVIEW_LINUX_DEPENDENCY_MODE "AUTO" CACHE STRING
        "Linux dependency source: AUTO, SYSTEM, or FETCH")
    set_property(CACHE LILTVIEW_LINUX_DEPENDENCY_MODE PROPERTY STRINGS AUTO SYSTEM FETCH)
    option(LILTVIEW_DEPENDENCY_OFFLINE
        "Do not access the network while resolving fetched Linux dependencies" OFF)
    if(NOT LILTVIEW_LINUX_DEPENDENCY_MODE MATCHES "^(AUTO|SYSTEM|FETCH)$")
        message(FATAL_ERROR
            "LILTVIEW_LINUX_DEPENDENCY_MODE must be AUTO, SYSTEM, or FETCH")
    endif()
    if(LILTVIEW_DEPENDENCY_OFFLINE)
        set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL
            "Disable network access for FetchContent" FORCE)
    endif()
endif()

function(liltview_is_linux output)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(liltview_dependency_manifest_field name field output)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/dependencies.json" manifest)
    string(JSON value GET "${manifest}" submodules "${name}" "${field}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

# Resolve a system package only on Linux. The caller supplies the target that
# must be present so a package with headers but no usable CMake target is not
# treated as a successful resolution.
function(liltview_try_system_package package version target output)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
       OR LILTVIEW_LINUX_DEPENDENCY_MODE STREQUAL "FETCH")
        set(${output} FALSE PARENT_SCOPE)
        return()
    endif()

    if(version STREQUAL "")
        find_package(${package} QUIET CONFIG)
        if(NOT ${package}_FOUND)
            find_package(${package} QUIET MODULE)
        endif()
    else()
        find_package(${package} ${version} QUIET CONFIG)
        if(NOT ${package}_FOUND)
            find_package(${package} ${version} QUIET MODULE)
        endif()
    endif()

    if(${package}_FOUND AND TARGET ${target})
        set(${output} TRUE PARENT_SCOPE)
    else()
        set(${output} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Populate a fixed dependency into the build tree without adding a submodule
# or mutating third_party/. The caller still owns add_subdirectory and all
# upstream feature switches so Windows keeps its existing source path.
function(liltview_fetch_dependency_source name marker output)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "Fetched dependency sources are only enabled on Linux: ${name}")
    endif()

    liltview_dependency_manifest_field("${name}" path relative_path)
    liltview_dependency_manifest_field("${name}" url repository)
    liltview_dependency_manifest_field("${name}" commit revision)
    string(MAKE_C_IDENTIFIER "${name}" identifier)
    set(content_name "liltview_${identifier}")
    FetchContent_Declare(${content_name}
        GIT_REPOSITORY "${repository}"
        GIT_TAG "${revision}"
        GIT_SHALLOW FALSE)
    FetchContent_GetProperties(${content_name})
    if(NOT ${content_name}_POPULATED)
        FetchContent_Populate(${content_name})
    endif()
    set(source "${${content_name}_SOURCE_DIR}")
    if(NOT EXISTS "${source}/${marker}")
        message(FATAL_ERROR
            "Fetched ${name} source is incomplete: ${source}/${marker}\n"
            "Expected fixed revision ${revision} from ${repository}")
    endif()
    set(${output} "${source}" PARENT_SCOPE)
endfunction()

function(liltview_dependency_status name source target)
    if(source STREQUAL "system")
        message(STATUS "LiltView dependency ${name}: system target ${target}")
    elseif(source STREQUAL "fetched")
        message(STATUS "LiltView dependency ${name}: fetched fixed source")
    else()
        message(STATUS "LiltView dependency ${name}: vendored source")
    endif()
endfunction()

# Header-only dependencies are vendored directly, never fetched by CMake.
function(liltview_require_app_headers)
    if(TARGET LiltView::cli11)
        return()
    endif()
    set(vendor "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party")
    foreach(required CLI11/include/CLI/CLI.hpp iguana/include/iguana/yaml_reader.hpp iguana/include/iguana/yaml_writer.hpp)
        if(NOT EXISTS "${vendor}/${required}")
            message(FATAL_ERROR "Missing vendored header: third_party/${required}; restore the recorded source files")
        endif()
    endforeach()
    add_library(liltview_cli11 INTERFACE)
    add_library(LiltView::cli11 ALIAS liltview_cli11)
    target_include_directories(liltview_cli11 SYSTEM INTERFACE "${vendor}/CLI11/include")
    add_library(liltview_iguana INTERFACE)
    add_library(LiltView::iguana ALIAS liltview_iguana)
    target_include_directories(liltview_iguana SYSTEM INTERFACE "${vendor}/iguana/include")
endfunction()

function(liltview_require_lz4)
    if(TARGET LiltView::lz4)
        return()
    endif()
    liltview_dependency_source(lz4 lib/lz4.c source)
    add_library(liltview_lz4 STATIC "${source}/lib/lz4.c" "${source}/lib/xxhash.c")
    target_include_directories(liltview_lz4 SYSTEM PUBLIC "${source}/lib")
    add_library(LiltView::lz4 ALIAS liltview_lz4)
endfunction()

# CMake only reads local sources. Git gitlinks are the submodule version lock.
function(liltview_dependency_source name marker output)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/dependencies.json" manifest)
    string(JSON relative_path GET "${manifest}" submodules "${name}" path)
    get_filename_component(source "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../${relative_path}" ABSOLUTE)
    if(NOT EXISTS "${source}/${marker}")
        message(FATAL_ERROR
            "Missing ${name} sources: ${relative_path}/${marker}\n"
            "For first-time registration run: python3 scripts/init_submodules.py\n"
            "For clones with recorded gitlinks run: git submodule update --init --recursive\n"
            "CMake does not fetch dependencies automatically.")
    endif()
    set(${output} "${source}" PARENT_SCOPE)
endfunction()

function(liltview_require_imgui)
    if(TARGET LiltView::imgui)
        return()
    endif()
    liltview_dependency_source(imgui imgui.cpp source)
    foreach(required imgui.h imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp)
        if(NOT EXISTS "${source}/${required}")
            message(FATAL_ERROR "Incomplete ImGui source tree: ${source}/${required}")
        endif()
    endforeach()
    add_library(liltview_imgui_vendor STATIC
        "${source}/imgui.cpp" "${source}/imgui_draw.cpp"
        "${source}/imgui_tables.cpp" "${source}/imgui_widgets.cpp")
    target_include_directories(liltview_imgui_vendor SYSTEM PUBLIC "${source}")
    target_compile_features(liltview_imgui_vendor PUBLIC cxx_std_11)
    add_library(liltview_imgui INTERFACE)
    add_library(LiltView::imgui ALIAS liltview_imgui)
    target_link_libraries(liltview_imgui INTERFACE liltview_imgui_vendor)
endfunction()

function(liltview_require_spdlog)
    liltview_dependency_source(spdlog CMakeLists.txt source)
    if(TARGET spdlog::spdlog)
        return()
    endif()
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS_HO OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${source}" "${PROJECT_BINARY_DIR}/third_party/spdlog" EXCLUDE_FROM_ALL SYSTEM)
endfunction()

function(liltview_require_catch2)
    liltview_dependency_source(Catch2 CMakeLists.txt source)
    if(NOT EXISTS "${source}/extras/Catch.cmake")
        message(FATAL_ERROR "Incomplete Catch2 source tree: ${source}/extras/Catch.cmake")
    endif()
    if(NOT TARGET Catch2::Catch2WithMain)
        set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
        set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${source}" "${PROJECT_BINARY_DIR}/third_party/Catch2" EXCLUDE_FROM_ALL SYSTEM)
        # Catch2's compiled string_view/optional support must match our C++20/23
        # consumers; its default C++14 build otherwise leaves missing symbols.
        target_compile_features(Catch2 PUBLIC cxx_std_17)
    endif()
    set(LILTVIEW_CATCH_SCRIPT "${source}/extras/Catch.cmake" PARENT_SCOPE)
endfunction()

function(liltview_require_sdl3)
    liltview_dependency_source(SDL CMakeLists.txt source)
    if(TARGET SDL3::SDL3-static)
        return()
    endif()
    # Bind to the vendored, trimmed static target explicitly. Do not let the
    # SDL3::SDL3 alias or a system SDL installation select a shared library.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER ON CACHE BOOL "" FORCE)
    set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
    add_subdirectory("${source}" "${PROJECT_BINARY_DIR}/third_party/SDL" EXCLUDE_FROM_ALL SYSTEM)
    if(NOT TARGET SDL3::SDL3-static)
        message(FATAL_ERROR "The vendored SDL configuration must provide SDL3::SDL3-static")
    endif()
endfunction()
