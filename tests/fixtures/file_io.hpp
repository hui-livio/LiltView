#pragma once
#include <cstdio>
#include <filesystem>
#include <tiffio.h>

namespace liltview::test
{
inline FILE *fixture_file(const std::filesystem::path &path)
{
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}
inline TIFF *fixture_tiff(const std::filesystem::path &path, const char *mode)
{
#ifdef _WIN32
    return TIFFOpenW(path.c_str(), mode);
#else
    return TIFFOpen(path.c_str(), mode);
#endif
}
} // namespace liltview::test
