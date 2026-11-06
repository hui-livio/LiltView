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

struct Settings
{
    int width         = 1120;
    int height        = 760;
    bool maximized    = false;
    Theme theme       = Theme::system;
    Language language = Language::system;
};

Settings parse_settings(std::string_view data);
std::string serialize_settings(const Settings &settings);
// 未获得系统偏好时使用深色；显式选择始终优先于桌面主题。
Theme resolve_theme(Theme preference, Theme system_theme);
} // namespace liltview

#endif
