#ifndef LILTVIEW_SETTINGS_HPP
#define LILTVIEW_SETTINGS_HPP

#include "i18n/translator.hpp"

#include <string>
#include <string_view>

namespace liltview
{
enum class Theme
{
    system,
    dark,
    light
};

enum class StaticUploadMode { pipeline, batch };

enum class LogLevel
{
    trace,
    debug,
    info,
    warn,
    error,
    critical,
    off
};

// Defined in the settings library so every consumer uses its build configuration.
LogLevel default_log_level();
std::string_view log_level_name(LogLevel level);

struct Settings
{
    int width             = 1120;
    int height            = 760;
    bool maximized        = false;
    Theme theme           = Theme::system;
    Language language     = Language::system;
    LogLevel log_level    = default_log_level();
    bool system_titlebar  = false;
    bool disable_rounding = false;
    StaticUploadMode static_upload_mode = StaticUploadMode::pipeline;
};

// YAML only. Invalid syntax returns defaults; missing keys keep their defaults.
Settings parse_settings(std::string_view data);
std::string serialize_settings(const Settings &settings);
// 未获得系统偏好时使用深色；显式选择始终优先于桌面主题。
Theme resolve_theme(Theme preference, Theme system_theme);
} // namespace liltview

#endif
