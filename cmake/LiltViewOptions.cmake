include_guard(GLOBAL)

# MSVC has C17 plus C11 atomics/threads, but not the complete C23 dialect.
# Keep the compatibility subset private to our C targets; never apply it to vendors.
set(LILTVIEW_C_STANDARD 23)
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    set(LILTVIEW_C_STANDARD 17)
    message(STATUS "MSVC core: C17 with private C23 spelling compatibility and C11 atomics/threads")
endif()

function(liltview_configure_c_target target)
    set_target_properties(${target} PROPERTIES C_STANDARD ${LILTVIEW_C_STANDARD} C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /utf-8 /experimental:c11atomics
            "/FI${PROJECT_SOURCE_DIR}/cmake/compat/msvc_c.h")
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    liltview_target_warnings(${target})
endfunction()

function(liltview_configure_cxx_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${LILTVIEW_CXX_STANDARD} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        CXX_SCAN_FOR_MODULES OFF)
    # No C++ modules are used. Avoid unnecessary GCC/Ninja dynamic dependency
    # scanning, including stale dyndep edges when switching C++20/23 in a build.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /Zc:__cplusplus /permissive-)
    endif()
    liltview_target_warnings(${target})
endfunction()

function(liltview_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(LILTVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(LILTVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(liltview_check_toolchain)
    include(CheckCSourceCompiles)
    include(CheckCXXSourceCompiles)
    set(CMAKE_C_STANDARD ${LILTVIEW_C_STANDARD})
    set(CMAKE_C_STANDARD_REQUIRED YES)
    set(CMAKE_C_EXTENSIONS NO)
    set(CMAKE_CXX_STANDARD ${LILTVIEW_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED YES)
    set(CMAKE_CXX_EXTENSIONS NO)
    if(MSVC)
        set(CMAKE_REQUIRED_FLAGS "/Zc:__cplusplus /utf-8")
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        set(CMAKE_REQUIRED_FLAGS "/utf-8 /experimental:c11atomics")
        set(CMAKE_REQUIRED_INCLUDES "${PROJECT_SOURCE_DIR}/cmake/compat")
        check_c_source_compiles([[
            #include "msvc_c.h"
            #include <stdatomic.h>
            #include <threads.h>
            static_assert(sizeof(size_t) >= 4);
            static int worker(void *data) { return *(int *)data; }
            int main(void) {
                constexpr size_t channels = 4;
                atomic_bool stopped;
                atomic_init(&stopped, false);
                mtx_t mutex;
                cnd_t condition;
                thrd_t thread;
                int result = 0;
                if (mtx_init(&mutex, mtx_plain) != thrd_success || cnd_init(&condition) != thrd_success)
                    return 1;
                if (thrd_create(&thread, worker, &result) != thrd_success)
                    return 1;
                thrd_join(thread, &result);
                cnd_destroy(&condition);
                mtx_destroy(&mutex);
                void *ptr = nullptr;
                return atomic_load(&stopped) || ptr != nullptr || channels != 4;
            }
        ]] LILTVIEW_HAS_REQUIRED_MSVC_C)
        if(NOT LILTVIEW_HAS_REQUIRED_MSVC_C)
            message(FATAL_ERROR "MSVC requires C17 and C11 atomics/threads. Install a current VS 2022/2026 C++ toolset and Windows SDK. See CMakeConfigureLog.yaml.")
        endif()
        unset(CMAKE_REQUIRED_INCLUDES)
        set(CMAKE_REQUIRED_FLAGS "/Zc:__cplusplus /utf-8")
    else()
        check_c_source_compiles([[
            #include <stddef.h>
            #include <stdckdint.h>
            #if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
            #error C23 is required
            #endif
            static_assert(sizeof(size_t) >= 4);
            int main(void) {
                constexpr size_t channels = 4;
                size_t bytes = 0;
                void *ptr = nullptr;
                return ckd_mul(&bytes, (size_t)2, channels) || ptr != nullptr;
            }
        ]] LILTVIEW_HAS_REQUIRED_C23)
        if(NOT LILTVIEW_HAS_REQUIRED_C23)
            message(FATAL_ERROR "C23 with constexpr, nullptr and <stdckdint.h> is required. See CMakeConfigureLog.yaml.")
        endif()
    endif()

    if(LILTVIEW_CXX_STANDARD STREQUAL "23")
        check_cxx_source_compiles([[
            #include <expected>
            #if __cplusplus < 202302L && !(defined(_MSVC_LANG) && _MSVC_LANG > 202002L)
            #error C++23 is required
            #endif
            int main() {
                std::expected<int, int> value = 42;
                return value.value() == 42 ? 0 : 1;
            }
        ]] LILTVIEW_HAS_REQUIRED_CXX23)
        if(NOT LILTVIEW_HAS_REQUIRED_CXX23)
            message(FATAL_ERROR "C++23 and std::expected are required. Explicitly use -DLILTVIEW_CXX_STANDARD=20 for compatibility mode.")
        endif()
    else()
        check_cxx_source_compiles([[
            #include <span>
            #if __cplusplus < 202002L
            #error C++20 is required
            #endif
            int main() {
                const int values[] = {42};
                return std::span<const int>(values).front() == 42 ? 0 : 1;
            }
        ]] LILTVIEW_HAS_REQUIRED_CXX20)
        if(NOT LILTVIEW_HAS_REQUIRED_CXX20)
            message(FATAL_ERROR "C++20 with std::span is required for compatibility mode.")
        endif()
    endif()
endfunction()
