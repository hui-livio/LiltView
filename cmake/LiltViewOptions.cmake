include_guard(GLOBAL)

function(liltview_configure_c_target target)
    set_target_properties(${target} PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    liltview_target_warnings(${target})
endfunction()

function(liltview_configure_cxx_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD ${LILTVIEW_CXX_STANDARD} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
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
    set(CMAKE_C_STANDARD 23)
    set(CMAKE_C_STANDARD_REQUIRED YES)
    set(CMAKE_C_EXTENSIONS NO)
    set(CMAKE_CXX_STANDARD ${LILTVIEW_CXX_STANDARD})
    set(CMAKE_CXX_STANDARD_REQUIRED YES)
    set(CMAKE_CXX_EXTENSIONS NO)
    if(MSVC)
        set(CMAKE_REQUIRED_FLAGS "/Zc:__cplusplus /utf-8")
    endif()
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

    if(LILTVIEW_CXX_STANDARD STREQUAL "23")
        check_cxx_source_compiles([[
            #include <expected>
            #if __cplusplus < 202302L
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
