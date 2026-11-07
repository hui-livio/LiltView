#ifndef LILTVIEW_SETTINGS_STORE_HPP
#define LILTVIEW_SETTINGS_STORE_HPP

#include <filesystem>
#include <system_error>
#include "settings.hpp"

namespace liltview
{
// Write/flush/close a sibling temporary file, then replace the old config.
// A failed write never truncates the existing settings; temporary files are removed.
bool save_settings_file(const std::filesystem::path &path, const Settings &settings, std::error_code &error);
} // namespace liltview

#endif
