#include "version.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>
#include <winver.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
std::filesystem::path application_path()
{
    const std::string_view path(LILTVIEW_TEST_APP_PATH);
    return std::filesystem::path(std::u8string(path.begin(), path.end()));
}
}

TEST_CASE("Windows executable embeds the application icon", "[windows][resources]")
{
    struct ResourceModule
    {
        HMODULE handle = LoadLibraryExW(application_path().c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        ~ResourceModule()
        {
            if (handle)
                FreeLibrary(handle);
        }
    } module;
    REQUIRE(module.handle != nullptr);
    const auto group = FindResourceW(module.handle, MAKEINTRESOURCEW(101), reinterpret_cast<LPCWSTR>(RT_GROUP_ICON));
    REQUIRE(group != nullptr);
    CHECK(SizeofResource(module.handle, group) > 6);
    // RC expands the multi-resolution ICO into individual RT_ICON resources.
    CHECK(FindResourceW(module.handle, MAKEINTRESOURCEW(1), reinterpret_cast<LPCWSTR>(RT_ICON)) != nullptr);
}

TEST_CASE("Windows executable version matches the CMake project version", "[windows][resources]")
{
    const auto path = application_path();
    const auto size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    REQUIRE(size != 0);
    std::vector<std::byte> data(size);
    REQUIRE(GetFileVersionInfoW(path.c_str(), 0, size, data.data()));
    void *value = nullptr;
    UINT length = 0;
    REQUIRE(VerQueryValueW(data.data(), L"\\", &value, &length));
    REQUIRE(length >= sizeof(VS_FIXEDFILEINFO));
    const auto &fixed = *static_cast<const VS_FIXEDFILEINFO *>(value);
    CHECK(fixed.dwSignature == VS_FFI_SIGNATURE);
    std::array<unsigned int, 4> expected{};
    std::istringstream version(LILTVIEW_VERSION);
    for (auto &component : expected)
    {
        version >> component;
        if (version.peek() == '.')
            version.get();
    }
    CHECK(HIWORD(fixed.dwFileVersionMS) == expected[0]);
    CHECK(LOWORD(fixed.dwFileVersionMS) == expected[1]);
    CHECK(HIWORD(fixed.dwFileVersionLS) == expected[2]);
    CHECK(LOWORD(fixed.dwFileVersionLS) == expected[3]);
    CHECK(fixed.dwProductVersionMS == fixed.dwFileVersionMS);
    CHECK(fixed.dwProductVersionLS == fixed.dwFileVersionLS);
    CHECK(((fixed.dwFileFlags & VS_FF_DEBUG) != 0) == (LILTVIEW_TEST_DEBUG != 0));
    for (const auto key : {L"\\StringFileInfo\\040904B0\\FileVersion", L"\\StringFileInfo\\040904B0\\ProductVersion"})
    {
        REQUIRE(VerQueryValueW(data.data(), key, &value, &length));
        const std::string_view narrow_version(LILTVIEW_VERSION);
        CHECK(std::wstring_view(static_cast<const wchar_t *>(value)) ==
              std::wstring(narrow_version.begin(), narrow_version.end()));
    }
}

TEST_CASE("Windows Release uses the GUI subsystem while Debug keeps its console", "[windows][resources]")
{
    std::ifstream executable(application_path(), std::ios::binary);
    REQUIRE(executable);
    IMAGE_DOS_HEADER dos{};
    REQUIRE(executable.read(reinterpret_cast<char *>(&dos), sizeof(dos)));
    REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);
    REQUIRE(dos.e_lfanew > 0);
    executable.seekg(dos.e_lfanew);
    DWORD signature{};
    REQUIRE(executable.read(reinterpret_cast<char *>(&signature), sizeof(signature)));
    REQUIRE(signature == IMAGE_NT_SIGNATURE);
    // Subsystem has the same offset in PE32 and PE32+ optional headers.
    static_assert(offsetof(IMAGE_OPTIONAL_HEADER32, Subsystem) == offsetof(IMAGE_OPTIONAL_HEADER64, Subsystem));
    executable.seekg(dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + offsetof(IMAGE_OPTIONAL_HEADER32, Subsystem));
    WORD subsystem{};
    REQUIRE(executable.read(reinterpret_cast<char *>(&subsystem), sizeof(subsystem)));
    CHECK(subsystem == (LILTVIEW_TEST_GUI ? IMAGE_SUBSYSTEM_WINDOWS_GUI : IMAGE_SUBSYSTEM_WINDOWS_CUI));
}
