#include "i18n/translator.hpp"
#include "menu/menu_model.hpp"
#include "settings/settings.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace liltview;

TEST_CASE("locale preferences fall back when Chinese glyphs are unavailable", "[i18n]")
{
    CHECK(resolve_language(Language::system, "zh-CN", true) == Language::chinese);
    CHECK(resolve_language(Language::system, "en-US", true) == Language::english);
    CHECK(resolve_language(Language::english, "zh-CN", true) == Language::english);
    CHECK(resolve_language(Language::chinese, "en-US", true) == Language::chinese);
    CHECK(resolve_language(Language::chinese, "zh-CN", false) == Language::english);
    CHECK(resolve_language(Language::system, "", true) == Language::english);
    CHECK(parse_language("unknown") == Language::system);
}

TEST_CASE("every UI text has both languages and no widget ID in the translation", "[i18n]")
{
    for (std::size_t index = 0; index < static_cast<std::size_t>(Text::count); ++index)
    {
        for (auto language : {Language::english, Language::chinese})
        {
            const std::string_view text(translate(language, static_cast<Text>(index)));
            CHECK_FALSE(text.empty());
            CHECK(text.find("###") == std::string_view::npos);
        }
    }
}

TEST_CASE("settings preserve window and language state across serialization", "[settings]")
{
    Settings before;
    before.width     = 1440;
    before.height    = 900;
    before.maximized = true;
    before.language  = Language::chinese;
    before.theme     = Theme::light;
    const auto after = parse_settings(serialize_settings(before));
    CHECK(after.width == before.width);
    CHECK(after.height == before.height);
    CHECK(after.maximized == before.maximized);
    CHECK(after.language == before.language);
    CHECK(after.theme == before.theme);
}

TEST_CASE("malformed or oversized settings keep safe defaults", "[settings]")
{
    const Settings defaults;
    const auto settings = parse_settings("width=99999999999999\nheight=-1\nlanguage=bad\nunknown=ignored\n");
    CHECK(settings.width == defaults.width);
    CHECK(settings.height == defaults.height);
    CHECK(settings.language == Language::system);
    CHECK(parse_settings("width=640junk\n").width == defaults.width);
    CHECK(parse_settings("width=800\r\nheight=600\r\n").height == 600);
    CHECK(parse_settings(std::string(4097, 'x')).width == defaults.width);
}

TEST_CASE("system and fallback menus keep the same identities across language changes", "[menu]")
{
    Settings settings;
    const auto english = build_menu(settings, Language::english, true, false);
    const auto chinese = build_menu(settings, Language::chinese, true, false);
    std::set<std::int32_t> ids;
    const auto check = [&](const auto &self, const MenuItem &item) -> void
    {
        REQUIRE(ids.insert(item.id).second);
        const auto *translated = find_menu_item(chinese, item.id);
        REQUIRE(translated != nullptr);
        CHECK(translated->action == item.action);
        CHECK(translated->enabled == item.enabled);
        CHECK(translated->children.size() == item.children.size());
        CHECK(item.label.find("###") == std::string::npos);
        for (const auto &child : item.children)
            self(self, child);
    };
    check(check, english);
    REQUIRE(find_menu_item(english, 10) != nullptr);
    CHECK_FALSE(find_menu_item(english, 10)->enabled);
    CHECK(find_menu_item(english, 9999) == nullptr);
}

TEST_CASE("menu state reflects settings and unavailable Chinese font", "[menu]")
{
    Settings settings;
    apply_menu_setting(MenuAction::language_chinese, settings);
    apply_menu_setting(MenuAction::theme_light, settings);
    const auto menu = build_menu(settings, Language::chinese, false, true);
    CHECK(find_menu_item(menu, 33)->checked);
    CHECK_FALSE(find_menu_item(menu, 33)->enabled);
    CHECK(find_menu_item(menu, 36)->checked);
    CHECK_FALSE(find_menu_item(menu, 35)->checked);
    CHECK(find_menu_item(menu, 20)->checked);
    apply_menu_setting(MenuAction::quit, settings);
    CHECK(settings.language == Language::chinese);
    CHECK(settings.theme == Theme::light);
}

TEST_CASE("theme follows the system only when explicitly selected", "[theme]")
{
    CHECK(Settings{}.theme == Theme::system);
    CHECK(resolve_theme(Theme::system, Theme::light) == Theme::light);
    CHECK(resolve_theme(Theme::system, Theme::dark) == Theme::dark);
    CHECK(resolve_theme(Theme::system, Theme::system) == Theme::dark);
    CHECK(resolve_theme(Theme::dark, Theme::light) == Theme::dark);
    CHECK(resolve_theme(Theme::light, Theme::dark) == Theme::light);
}

TEST_CASE("all theme preferences persist and legacy light or dark settings remain valid", "[theme][settings]")
{
    for (const auto theme : {Theme::system, Theme::dark, Theme::light})
    {
        Settings settings;
        settings.theme = theme;
        CHECK(parse_settings(serialize_settings(settings)).theme == theme);
    }
    CHECK(parse_settings("theme=dark\n").theme == Theme::dark);
    CHECK(parse_settings("theme=light\n").theme == Theme::light);
    CHECK(parse_settings("theme=system\n").theme == Theme::system);
    CHECK(parse_settings("theme=unknown\n").theme == Theme::system);
    CHECK(parse_settings("").theme == Theme::system);
}

TEST_CASE("theme menu checks the preference rather than the current resolved appearance", "[theme][menu]")
{
    Settings settings;
    const auto followed = build_menu(settings, Language::english, true, false);
    CHECK(find_menu_item(followed, 37)->checked);
    CHECK_FALSE(find_menu_item(followed, 35)->checked);
    CHECK_FALSE(find_menu_item(followed, 36)->checked);

    apply_menu_setting(MenuAction::theme_dark, settings);
    const auto dark = build_menu(settings, Language::english, true, false);
    CHECK(find_menu_item(dark, 35)->checked);
    CHECK_FALSE(find_menu_item(dark, 37)->checked);
    apply_menu_setting(MenuAction::theme_system, settings);
    CHECK(settings.theme == Theme::system);
}
