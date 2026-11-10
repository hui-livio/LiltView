#pragma once
#include <filesystem>
#include <liltview/core.h>
#include <string>

namespace liltview::platform
{
#ifdef _WIN32
inline constexpr lv_path_encoding path_encoding = LV_PATH_UTF8;
#else
inline constexpr lv_path_encoding path_encoding = LV_PATH_POSIX_BYTES;
#endif
inline std::string path_bytes(const std::filesystem::path &path)
{
#ifdef _WIN32
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
#else
    return path.native();
#endif
}
} // namespace liltview::platform
