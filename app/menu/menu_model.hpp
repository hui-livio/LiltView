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
    previous,
    next,
    fit,
    actual_size,
    pan_left,
    pan_right,
    pan_up,
    pan_down,
    zoom_in,
    zoom_out,
    about,
    language_system,
    language_english,
    language_chinese,
    theme_dark,
    theme_light,
    theme_system,
    system_titlebar,
    disable_rounding,
    minimize,
    maximize_restore,
    close_window
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
    bool repeatable   = false;
    std::vector<std::string> shortcut;
    std::vector<MenuItem> children;
};

MenuItem build_menu(const Settings &settings, Language language, bool chinese_font, bool fullscreen, bool has_image = false,
                    bool can_actual_size = false, bool can_previous = false, bool can_next = false);
const MenuItem *find_menu_item(const MenuItem &root, std::int32_t id);
MenuAction menu_action_for_shortcut(const MenuItem &root, const std::vector<std::string> &shortcut, bool repeat = false);
void apply_menu_setting(MenuAction action, Settings &settings);
} // namespace liltview

#endif
