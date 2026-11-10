#pragma once
#include <filesystem>
#include <string>

namespace liltview::test
{
inline std::string fixture_path_bytes(const std::filesystem::path &path)
{
#ifdef _WIN32
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
#else
    return path.native();
#endif
}
} // namespace liltview::test
