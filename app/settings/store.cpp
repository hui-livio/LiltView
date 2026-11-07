#include "store.hpp"

#include <cerrno>
#include <cstdio>
#include <random>
#include <utility>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace liltview
{
namespace
{
std::error_code io_error()
{
    return {errno != 0 ? errno : EIO, std::generic_category()};
}

struct TemporaryFile
{
    std::filesystem::path path;
    std::FILE *file = nullptr;
    ~TemporaryFile()
    {
        if (file != nullptr)
            std::fclose(file);
        if (!path.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }
};

bool open_temporary(const std::filesystem::path &target, TemporaryFile &temp, std::error_code &error)
{
    std::random_device random;
    for (unsigned attempt = 0; attempt < 16; ++attempt)
    {
        auto candidate  = target;
        candidate      += ".tmp-" + std::to_string(random()) + "-" + std::to_string(random());
#ifdef _WIN32
        auto *file = _wfopen(candidate.c_str(), L"wbx");
#else
        auto *file = std::fopen(candidate.c_str(), "wbx");
#endif
        if (file != nullptr)
        {
            temp.path = std::move(candidate);
            temp.file = file;
            return true;
        }
        if (errno != EEXIST)
        {
            error = io_error();
            return false;
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return false;
}

bool write_and_close(TemporaryFile &temp, const std::string &data, std::error_code &error)
{
    if (std::fwrite(data.data(), 1, data.size(), temp.file) != data.size() || std::fflush(temp.file) != 0)
    {
        error = io_error();
        return false;
    }
    const auto result = std::fclose(std::exchange(temp.file, nullptr));
    if (result != 0)
        error = io_error();
    return result == 0;
}

bool replace_file(const std::filesystem::path &source, const std::filesystem::path &target, std::error_code &error)
{
#ifdef _WIN32
    if (!MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        error = {static_cast<int>(GetLastError()), std::system_category()};
#else
    std::filesystem::rename(source, target, error);
#endif
    return !error;
}
} // namespace

bool save_settings_file(const std::filesystem::path &path, const Settings &settings, std::error_code &error)
{
    error.clear();
    auto target            = path;
    const auto link_status = std::filesystem::symlink_status(target, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return false;
    error.clear();
    if (std::filesystem::is_symlink(link_status))
    {
        target = std::filesystem::canonical(target, error);
        if (error)
            return false;
    }
    const auto status = std::filesystem::status(target, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return false;
    error.clear();
    if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))
    {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    TemporaryFile temp;
    if (!open_temporary(target, temp, error) || !write_and_close(temp, serialize_settings(settings), error))
        return false;
    if (std::filesystem::exists(status))
    {
        std::filesystem::permissions(temp.path, status.permissions(), error);
        if (error)
            return false;
    }
    if (!replace_file(temp.path, target, error))
        return false;
    temp.path.clear();
    return true;
}
} // namespace liltview
