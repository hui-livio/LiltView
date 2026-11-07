#ifndef LILTVIEW_MENU_MODEL_HPP
#define LILTVIEW_MENU_MODEL_HPP

#include "settings/settings.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace liltview
{
enum class MenuAction
{
    none,
    open,
    quit,
    fullscreen,
    about,
    language_system,
    language_english,
    language_chinese,
    theme_dark,
    theme_light,
    theme_system
};

struct MenuItem
{
    std::int32_t id = 0;
    std::string label;
    MenuAction action = MenuAction::none;
    bool enabled      = true;
    bool separator    = false;
    bool checkable    = false;
    bool checked      = false;
    std::vector<std::string> shortcut;
    std::vector<MenuItem> children;
};

MenuItem build_menu(const Settings &settings, Language language, bool chinese_font, bool fullscreen);
const MenuItem *find_menu_item(const MenuItem &root, std::int32_t id);
void apply_menu_setting(MenuAction action, Settings &settings);
} // namespace liltview

#endif
