#include "menu_model.hpp"

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

MenuItem view_menu(Language language, bool fullscreen)
{
    auto view                = item(language, 2, Text::view);
    auto fullscreen_item     = toggle(language, 20, Text::fullscreen, MenuAction::fullscreen, fullscreen);
    fullscreen_item.shortcut = {"F11"};
    view.children            = {fullscreen_item};
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
    return appearance;
}
} // namespace

MenuItem build_menu(const Settings &settings, Language language, bool chinese_font, bool fullscreen)
{
    // 稳定 ID 属于菜单模型，翻译和平台渲染都不改变它们。
    auto preferences     = item(language, 3, Text::settings);
    preferences.children = {language_menu(settings, language, chinese_font), appearance_menu(settings, language)};
    auto help            = item(language, 4, Text::help);
    help.children        = {item(language, 40, Text::about, MenuAction::about)};
    MenuItem root;
    root.children = {file_menu(language), view_menu(language, fullscreen), preferences, help};
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

void apply_menu_setting(MenuAction action, Settings &settings)
{
    switch (action)
    {
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
