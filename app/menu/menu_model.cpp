#include "menu_model.hpp"

#include <utility>

namespace liltview
{
namespace
{
MenuItem item(Language language, std::int32_t id, Text text, MenuAction action = MenuAction::none)
{
    MenuItem result;
    result.id     = id;
    result.label  = translate(language, text);
    result.action = action;
    return result;
}

MenuItem toggle(Language language, std::int32_t id, Text text, MenuAction action, bool checked)
{
    auto result      = item(language, id, text, action);
    result.checkable = true;
    result.checked   = checked;
    return result;
}

MenuItem file_menu(Language language)
{
    auto file     = item(language, 1, Text::file);
    auto open     = item(language, 10, Text::open, MenuAction::open);
    open.shortcut = {"Control", "o"};
    MenuItem separator;
    separator.id        = 11;
    separator.separator = true;
    auto quit           = item(language, 12, Text::exit, MenuAction::quit);
    quit.shortcut       = {"Control", "q"};
    file.children       = {open, separator, quit};
    return file;
}

MenuItem view_control(Language language, std::int32_t id, Text text, MenuAction action, std::vector<std::string> shortcut,
                      bool enabled)
{
    auto result       = item(language, id, text, action);
    result.shortcut   = std::move(shortcut);
    result.enabled    = enabled;
    result.repeatable = true;
    return result;
}

MenuItem view_menu(Language language, bool fullscreen, bool has_image, bool can_actual_size)
{
    auto view                = item(language, 2, Text::view);
    auto fullscreen_item     = toggle(language, 20, Text::fullscreen, MenuAction::fullscreen, fullscreen);
    fullscreen_item.shortcut = {"F11"};
    auto previous            = item(language, 21, Text::previous, MenuAction::previous);
    previous.shortcut        = {"Left"};
    previous.enabled         = false;
    auto next                = item(language, 22, Text::next, MenuAction::next);
    next.shortcut            = {"Right"};
    next.enabled             = false;
    // 目录切图尚未实现，保留原工具栏中这两个操作的禁用状态。
    MenuItem navigation_separator;
    navigation_separator.id        = 23;
    navigation_separator.separator = true;
    auto fit                       = item(language, 24, Text::fit, MenuAction::fit);
    fit.shortcut                   = {"f"};
    fit.enabled                    = has_image;
    auto actual_size               = item(language, 25, Text::actual_size, MenuAction::actual_size);
    actual_size.shortcut           = {"Control", "1"};
    actual_size.enabled            = has_image && can_actual_size;
    MenuItem zoom_separator;
    zoom_separator.id        = 26;
    zoom_separator.separator = true;
    auto pan_left            = view_control(language, 50, Text::pan_left, MenuAction::pan_left, {"Shift", "Left"}, has_image);
    auto pan_right           = view_control(language, 51, Text::pan_right, MenuAction::pan_right, {"Shift", "Right"}, has_image);
    auto pan_up              = view_control(language, 52, Text::pan_up, MenuAction::pan_up, {"Shift", "Up"}, has_image);
    auto pan_down            = view_control(language, 53, Text::pan_down, MenuAction::pan_down, {"Shift", "Down"}, has_image);
    auto zoom_in             = view_control(language, 54, Text::zoom_in, MenuAction::zoom_in, {"Control", "Up"}, has_image);
    auto zoom_out            = view_control(language, 55, Text::zoom_out, MenuAction::zoom_out, {"Control", "Down"}, has_image);
    MenuItem pan_separator;
    pan_separator.id        = 56;
    pan_separator.separator = true;
    view.children           = {previous,
                               next,
                               navigation_separator,
                               fit,
                               actual_size,
                               zoom_in,
                               zoom_out,
                               pan_separator,
                               pan_left,
                               pan_right,
                               pan_up,
                               pan_down,
                               zoom_separator,
                               fullscreen_item};
    return view;
}

MenuItem language_menu(const Settings &settings, Language language, bool chinese_font)
{
    auto languages = item(language, 30, Text::language);
    auto system  = toggle(language, 31, Text::follow_system, MenuAction::language_system, settings.language == Language::system);
    auto english = toggle(language, 32, Text::english, MenuAction::language_english, settings.language == Language::english);
    auto chinese = toggle(language, 33, Text::chinese, MenuAction::language_chinese, settings.language == Language::chinese);
    if (!chinese_font)
    {
        chinese.label   = "Chinese (font missing)";
        chinese.enabled = false;
    }
    languages.children = {system, english, chinese};
    return languages;
}

MenuItem appearance_menu(const Settings &settings, Language language)
{
    auto appearance     = item(language, 34, Text::appearance);
    appearance.children = {toggle(language, 37, Text::follow_system, MenuAction::theme_system, settings.theme == Theme::system),
                           toggle(language, 35, Text::dark, MenuAction::theme_dark, settings.theme == Theme::dark),
                           toggle(language, 36, Text::light, MenuAction::theme_light, settings.theme == Theme::light)};
    MenuItem separator;
    separator.id        = 39;
    separator.separator = true;
    appearance.children.push_back(separator);
    appearance.children.push_back(
        toggle(language, 38, Text::system_titlebar, MenuAction::system_titlebar, settings.system_titlebar));
    auto square    = toggle(language, 41, Text::disable_rounding, MenuAction::disable_rounding, settings.disable_rounding);
    square.enabled = !settings.system_titlebar;
    appearance.children.push_back(square);
    return appearance;
}
} // namespace

MenuItem build_menu(const Settings &settings, Language language, bool chinese_font, bool fullscreen, bool has_image,
                    bool can_actual_size)
{
    // 稳定 ID 属于菜单模型，翻译和平台渲染都不改变它们。
    auto preferences     = item(language, 3, Text::settings);
    preferences.children = {language_menu(settings, language, chinese_font), appearance_menu(settings, language)};
    auto help            = item(language, 4, Text::help);
    help.children        = {item(language, 40, Text::about, MenuAction::about)};
    MenuItem root;
    root.children = {file_menu(language), view_menu(language, fullscreen, has_image, can_actual_size), preferences, help};
    return root;
}

const MenuItem *find_menu_item(const MenuItem &root, std::int32_t id)
{
    if (root.id == id)
        return &root;
    for (const auto &child : root.children)
    {
        if (const auto *result = find_menu_item(child, id))
            return result;
    }
    return nullptr;
}

MenuAction menu_action_for_shortcut(const MenuItem &root, const std::vector<std::string> &shortcut, bool repeat)
{
    if (!root.enabled || shortcut.empty())
        return MenuAction::none;
    if (!root.separator && root.action != MenuAction::none && root.shortcut == shortcut)
        return !repeat || root.repeatable ? root.action : MenuAction::none;
    for (const auto &child : root.children)
    {
        const auto action = menu_action_for_shortcut(child, shortcut, repeat);
        if (action != MenuAction::none)
            return action;
    }
    return MenuAction::none;
}

void apply_menu_setting(MenuAction action, Settings &settings)
{
    switch (action)
    {
    case MenuAction::disable_rounding:
        settings.disable_rounding = !settings.disable_rounding;
        break;
    case MenuAction::system_titlebar:
        settings.system_titlebar = !settings.system_titlebar;
        break;
    case MenuAction::language_system:
        settings.language = Language::system;
        break;
    case MenuAction::language_english:
        settings.language = Language::english;
        break;
    case MenuAction::language_chinese:
        settings.language = Language::chinese;
        break;
    case MenuAction::theme_dark:
        settings.theme = Theme::dark;
        break;
    case MenuAction::theme_light:
        settings.theme = Theme::light;
        break;
    case MenuAction::theme_system:
        settings.theme = Theme::system;
        break;
    default:
        break;
    }
}
} // namespace liltview
