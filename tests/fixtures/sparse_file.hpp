#pragma once
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <vector>
#ifdef _WIN32
    #include <windows.h>

    #include <io.h>
    #include <winioctl.h>
#else
    #include <sys/types.h>
#endif

namespace liltview::test
{
// Large logical offsets without allocating/writing gigabytes of zero padding.
// Skip on Windows filesystems that cannot explicitly enable sparse allocation.
struct SparseFile
{
    std::FILE *file = nullptr;
    explicit SparseFile(const std::filesystem::path &path)
    {
#ifdef _WIN32
        file = _wfopen(path.c_str(), L"w+b");
#else
        file = std::fopen(path.c_str(), "w+b");
#endif
        REQUIRE(file);
#ifdef _WIN32
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file)));
        DWORD returned    = 0;
        if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr))
        {
            std::fclose(file);
            file = nullptr;
            SKIP("The test filesystem does not support sparse files");
        }
#endif
    }
    ~SparseFile()
    {
        if (file)
            std::fclose(file);
    }
    SparseFile(const SparseFile &)            = delete;
    SparseFile &operator=(const SparseFile &) = delete;
    void write(std::uint64_t offset, const std::vector<unsigned char> &bytes)
    {
        REQUIRE(offset <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
#ifdef _WIN32
        REQUIRE(_fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0);
#else
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            SKIP("The test runtime does not support 64-bit file offsets");
        REQUIRE(fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0);
#endif
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    }
    void close()
    {
        const auto result = std::fclose(file);
        file              = nullptr;
        REQUIRE(result == 0);
    }
};
} // namespace liltview::test
