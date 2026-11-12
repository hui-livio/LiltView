#include "i18n/translator.hpp"
#include "menu/menu_model.hpp"
#include "settings/settings.hpp"
#include "viewer/viewport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
    const auto settings = parse_settings("width: 99999999999999\nheight: -1\nlanguage: bad\n");
    CHECK(settings.width == defaults.width);
    CHECK(settings.height == defaults.height);
    CHECK(settings.language == Language::system);
    CHECK(parse_settings("width: 640junk\n").width == defaults.width);
    CHECK(parse_settings("width: 800\r\nheight: 600\r\n").height == 600);
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
        CHECK(translated->repeatable == item.repeatable);
        CHECK(translated->shortcut == item.shortcut);
        CHECK(translated->children.size() == item.children.size());
        CHECK(item.label.find("###") == std::string::npos);
        for (const auto &child : item.children)
            self(self, child);
    };
    check(check, english);
    REQUIRE(find_menu_item(english, 10) != nullptr);
    CHECK(find_menu_item(english, 10)->enabled);
    CHECK(find_menu_item(english, 10)->action == MenuAction::open);
    CHECK(find_menu_item(english, 9999) == nullptr);
}

TEST_CASE("toolbar operations live in the menus with shortcuts", "[menu]")
{
    for (const auto language : {Language::english, Language::chinese})
    {
        const auto menu  = build_menu(Settings{}, language, true, false, true, true);
        const auto *file = find_menu_item(menu, 1);
        const auto *view = find_menu_item(menu, 2);
        REQUIRE(file != nullptr);
        REQUIRE(view != nullptr);
        REQUIRE(find_menu_item(*file, 10) != nullptr);
        CHECK(find_menu_item(*file, 10)->shortcut == std::vector<std::string>{"Control", "o"});
        for (const auto id : {20, 21, 22, 24, 25})
        {
            const auto *entry = find_menu_item(*view, id);
            REQUIRE(entry != nullptr);
            CHECK(entry->action != MenuAction::none);
            CHECK_FALSE(entry->shortcut.empty());
        }
        CHECK(find_menu_item(*view, 21)->shortcut == std::vector<std::string>{"Left"});
        CHECK(find_menu_item(*view, 22)->shortcut == std::vector<std::string>{"Right"});
        CHECK_FALSE(find_menu_item(*view, 21)->enabled);
        CHECK_FALSE(find_menu_item(*view, 22)->enabled);
        CHECK(find_menu_item(*view, 24)->shortcut == std::vector<std::string>{"f"});
        CHECK(find_menu_item(*view, 25)->shortcut == std::vector<std::string>{"Control", "1"});
        CHECK(find_menu_item(*view, 20)->shortcut == std::vector<std::string>{"F11"});
    }
}

TEST_CASE("image menu enablement follows image and native size availability", "[menu]")
{
    for (const bool has_image : {false, true})
    {
        for (const bool can_actual_size : {false, true})
        {
            const auto menu = build_menu(Settings{}, Language::english, true, false, has_image, can_actual_size);
            CHECK(find_menu_item(menu, 24)->enabled == has_image);
            CHECK(find_menu_item(menu, 25)->enabled == (has_image && can_actual_size));
            CHECK(menu_action_for_shortcut(menu, {"f"}) == (has_image ? MenuAction::fit : MenuAction::none));
            CHECK(menu_action_for_shortcut(menu, {"Control", "1"}) ==
                  (has_image && can_actual_size ? MenuAction::actual_size : MenuAction::none));
        }
    }
}

TEST_CASE("shortcuts use the menu definitions and ignore unavailable actions", "[menu][shortcut]")
{
    auto menu = build_menu(Settings{}, Language::chinese, true, false, true, true);
    CHECK(menu_action_for_shortcut(menu, {"Control", "o"}) == MenuAction::open);
    CHECK(menu_action_for_shortcut(menu, {"Control", "q"}) == MenuAction::quit);
    CHECK(menu_action_for_shortcut(menu, {"F11"}) == MenuAction::fullscreen);
    CHECK(menu_action_for_shortcut(menu, {"Left"}) == MenuAction::none);
    CHECK(menu_action_for_shortcut(menu, {"Right"}) == MenuAction::none);
    CHECK(menu_action_for_shortcut(menu, {}) == MenuAction::none);
    CHECK(menu_action_for_shortcut(menu, {"o"}) == MenuAction::none);
    CHECK(menu_action_for_shortcut(menu, {"Control", "f"}) == MenuAction::none);
    CHECK(menu_action_for_shortcut(menu, {"Control", "Shift", "1"}) == MenuAction::none);
    menu.children.front().enabled = false;
    CHECK(menu_action_for_shortcut(menu, {"Control", "o"}) == MenuAction::none);
}

TEST_CASE("Shift arrows pan and Control up down zoom with key repeat", "[menu][shortcut]")
{
    struct Control
    {
        std::int32_t id;
        MenuAction action;
        std::vector<std::string> shortcut;
    };
    const Control controls[]{
        {50, MenuAction::pan_left, {"Shift", "Left"}}, {51, MenuAction::pan_right, {"Shift", "Right"}},
        {52, MenuAction::pan_up, {"Shift", "Up"}},     {53, MenuAction::pan_down, {"Shift", "Down"}},
        {54, MenuAction::zoom_in, {"Control", "Up"}},  {55, MenuAction::zoom_out, {"Control", "Down"}},
    };
    for (const auto language : {Language::english, Language::chinese})
        for (const bool has_image : {false, true})
        {
            const auto menu  = build_menu(Settings{}, language, true, false, has_image);
            const auto *view = find_menu_item(menu, 2);
            REQUIRE(view != nullptr);
            for (const auto &control : controls)
            {
                CAPTURE(control.id, has_image);
                const auto *entry = find_menu_item(*view, control.id);
                REQUIRE(entry != nullptr);
                CHECK(entry->action == control.action);
                CHECK(entry->shortcut == control.shortcut);
                CHECK(entry->enabled == has_image);
                CHECK(entry->repeatable);
                for (const bool repeat : {false, true})
                    CHECK(menu_action_for_shortcut(menu, control.shortcut, repeat) ==
                          (has_image ? control.action : MenuAction::none));
            }
            // The superseded Control+Left/Right bindings must not pan or navigate.
            CHECK(menu_action_for_shortcut(menu, {"Control", "Left"}) == MenuAction::none);
            CHECK(menu_action_for_shortcut(menu, {"Control", "Right"}) == MenuAction::none);
            CHECK(menu_action_for_shortcut(menu, {"Up"}) == MenuAction::none);
            CHECK(menu_action_for_shortcut(menu, {"Down"}) == MenuAction::none);
            CHECK(menu_action_for_shortcut(menu, {"Control", "Shift", "Up"}) == MenuAction::none);
        }
}

TEST_CASE("Shift arrows move the viewport in arrow direction rather than image content", "[menu][shortcut][viewport][pan_direction]")
{
    struct Direction
    {
        const char *key;
        MenuAction action;
        render::Point viewport_step;
    };
    const Direction directions[]{
        {"Left", MenuAction::pan_left, {-64, 0}},
        {"Right", MenuAction::pan_right, {64, 0}},
        {"Up", MenuAction::pan_up, {0, -64}},
        {"Down", MenuAction::pan_down, {0, 64}},
    };
    for (const auto language : {Language::english, Language::chinese})
        for (const bool repeat : {false, true})
            for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
                for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
                    for (const auto &direction : directions)
                    {
                        CAPTURE(language, repeat, density, orientation, direction.key);
                        const auto menu = build_menu(Settings{}, language, true, false, true);
                        const auto action = menu_action_for_input(menu, {"Shift", direction.key}, repeat, {});
                        REQUIRE(action == direction.action);
                        const auto delta = menu_pan_delta(action);
                        Viewport view;
                        view.update({17, 31}, {529, 543}, density, {8192, 4096}, orientation);
                        const auto fitted = view;
                        view.pan(delta[0], delta[1]);
                        CHECK(view.same_view(fitted)); // A fitted image has no room to move the viewport.
                        view.actual_size();
                        const auto before = view.quad({0, 0, 8192, 4096}, {8192, 4096});
                        view.pan(delta[0], delta[1]);
                        const auto after = view.quad({0, 0, 8192, 4096}, {8192, 4096});
                        // Moving the viewport right/down must move image content left/up by 64 logical pixels.
                        CHECK(std::abs(before[0].x - after[0].x - direction.viewport_step.x) < 0.001f);
                        CHECK(std::abs(before[0].y - after[0].y - direction.viewport_step.y) < 0.001f);
                        CHECK(view.scale() == 1);
                        view.pan(delta[0] * 1000000, delta[1] * 1000000);
                        const auto edge = view;
                        view.pan(delta[0], delta[1]);
                        CHECK(view.same_view(edge)); // Holding the shortcut cannot move past an image boundary.
                    }
}

TEST_CASE("unavailable and blocked Shift arrows do not produce viewport movement", "[menu][shortcut][viewport]")
{
    for (const bool has_image : {false, true})
        for (const bool blocked : {false, true})
            for (const bool repeat : {false, true})
                for (const auto *key : {"Left", "Right", "Up", "Down"})
                {
                    if (has_image && !blocked)
                        continue;
                    CAPTURE(has_image, blocked, repeat, key);
                    const auto menu = build_menu(Settings{}, Language::english, true, false, has_image);
                    const auto action = menu_action_for_input(menu, {"Shift", key}, repeat, {blocked, false, false});
                    CHECK(action == MenuAction::none);
                    CHECK(menu_pan_delta(action) == std::array<double, 2>{0, 0});
                }
}

TEST_CASE("non-panning menu actions have no viewport pan delta", "[menu][shortcut]")
{
    const auto menu = build_menu(Settings{}, Language::english, true, false, true, true, true, true);
    const auto check = [&](const auto &self, const MenuItem &item) -> void
    {
        if (item.action != MenuAction::pan_left && item.action != MenuAction::pan_right &&
            item.action != MenuAction::pan_up && item.action != MenuAction::pan_down)
        {
            CAPTURE(item.id, item.action);
            CHECK(menu_pan_delta(item.action) == std::array<double, 2>{0, 0});
        }
        for (const auto &child : item.children)
            self(self, child);
    };
    check(check, menu);
}

TEST_CASE("application shortcut routing gives blocked input and information Escape precedence over fullscreen",
          "[menu][shortcut][image_details]")
{
    const auto menu = build_menu(Settings{}, Language::english, true, true, true, true, true, true, true, true);
    for (const bool blocked : {false, true})
        for (const bool fullscreen : {false, true})
            for (const bool info : {false, true})
            {
                const ShortcutContext context{blocked, fullscreen, info};
                CHECK(menu_action_for_input(menu, {"i"}, false, context) ==
                      (blocked ? MenuAction::none : MenuAction::image_info));
                CHECK(menu_action_for_input(menu, {"i"}, true, context) == MenuAction::none);
                const auto escape = blocked ? MenuAction::none :
                    info                    ? MenuAction::image_info :
                    fullscreen              ? MenuAction::fullscreen :
                                              MenuAction::none;
                CHECK(menu_action_for_input(menu, {"Escape"}, false, context) == escape);
                CHECK(menu_action_for_input(menu, {"Escape"}, true, context) == MenuAction::none);
                CHECK(menu_action_for_input(menu, {"Control", "Escape"}, false, context) == MenuAction::none);
                CHECK(menu_action_for_input(menu, {"Left"}, false, context) ==
                      (blocked ? MenuAction::none : MenuAction::previous));
            }
    const auto empty = build_menu(Settings{}, Language::english, true, true);
    CHECK(menu_action_for_input(empty, {"i"}, false, {}) == MenuAction::none);
    CHECK(menu_action_for_input(empty, {"Escape"}, false, {false, true, true}) == MenuAction::image_info);
    CHECK(menu_action_for_input(empty, {"Escape"}, false, {false, true, false}) == MenuAction::fullscreen);
}

TEST_CASE("key repeat does not reopen dialogs toggle fullscreen or quit", "[menu][shortcut]")
{
    const auto menu = build_menu(Settings{}, Language::english, true, false, true, true);
    for (const auto &shortcut :
         std::vector<std::vector<std::string>>{{"Control", "o"}, {"Control", "q"}, {"F11"}, {"f"}, {"Control", "1"}})
    {
        CHECK(menu_action_for_shortcut(menu, shortcut) != MenuAction::none);
        CHECK(menu_action_for_shortcut(menu, shortcut, true) == MenuAction::none);
    }
}

TEST_CASE("directory navigation menus and shortcuts track both boundaries independently", "[menu][navigation]")
{
    for (const auto language : {Language::english, Language::chinese})
        for (const bool previous : {false, true})
            for (const bool next : {false, true})
            {
                // A failed/in-flight image still has a directory cursor: allow browsing away.
                const auto menu = build_menu(Settings{}, language, true, false, false, false, previous, next);
                CHECK(find_menu_item(menu, 21)->enabled == previous);
                CHECK(find_menu_item(menu, 22)->enabled == next);
                CHECK(menu_action_for_shortcut(menu, {"Left"}) == (previous ? MenuAction::previous : MenuAction::none));
                CHECK(menu_action_for_shortcut(menu, {"Right"}, true) == (next ? MenuAction::next : MenuAction::none));
                CHECK(menu_action_for_shortcut(menu, {"Control", "Left"}) == MenuAction::none);
                CHECK(menu_action_for_shortcut(menu, {"Control", "Right"}) == MenuAction::none);
                CHECK(menu_action_for_shortcut(menu, {"Shift", "Right"}) == MenuAction::none);
            }
}

TEST_CASE("image information menu shares checked state and a nonrepeating I shortcut", "[menu][image_details]")
{
    for (const auto language : {Language::english, Language::chinese})
        for (const bool visible : {false, true})
            for (const bool open : {false, true})
            {
                const auto menu  = build_menu(Settings{}, language, true, false, false, false, false, false, open, visible);
                const auto *item = find_menu_item(menu, 57);
                REQUIRE(item != nullptr);
                CHECK(item->checkable);
                CHECK(item->checked == open);
                CHECK(item->enabled == visible);
                CHECK_FALSE(item->repeatable);
                CHECK(item->shortcut == std::vector<std::string>{"i"});
                CHECK(menu_action_for_shortcut(menu, {"i"}) == (visible ? MenuAction::image_info : MenuAction::none));
                CHECK(menu_action_for_shortcut(menu, {"i"}, true) == MenuAction::none);
                CHECK(menu_action_for_shortcut(menu, {"Control", "i"}) == MenuAction::none);
            }
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

TEST_CASE("all theme preferences persist as YAML strings", "[theme][settings]")
{
    for (const auto theme : {Theme::system, Theme::dark, Theme::light})
    {
        Settings settings;
        settings.theme = theme;
        CHECK(parse_settings(serialize_settings(settings)).theme == theme);
    }
    CHECK(parse_settings("theme: dark\n").theme == Theme::dark);
    CHECK(parse_settings("theme: light\n").theme == Theme::light);
    CHECK(parse_settings("theme: system\n").theme == Theme::system);
    CHECK(parse_settings("theme: unknown\n").theme == Theme::system);
    CHECK(parse_settings("").theme == Theme::system);
}

TEST_CASE("YAML settings accept comments quoted values and missing fields", "[settings][yaml]")
{
    const auto settings = parse_settings("\xef\xbb\xbf---\n# User preferences\nwidth: 1440\nheight: 900\n"
                                         "maximized: true\ntheme: 'light' # appearance\nlanguage: \"zh-CN\"\n...\n");
    CHECK(settings.width == 1440);
    CHECK(settings.height == 900);
    CHECK(settings.maximized);
    CHECK(settings.theme == Theme::light);
    CHECK(settings.language == Language::chinese);
    const auto partial = parse_settings("theme: dark");
    CHECK(partial.width == Settings{}.width);
    CHECK(partial.height == Settings{}.height);
    CHECK(partial.theme == Theme::dark);
    const auto yaml = serialize_settings(settings);
    CHECK(yaml.find("width: 1440") != std::string::npos);
    CHECK(yaml.find("theme: light") != std::string::npos);
    CHECK(yaml.find("language: zh-CN") != std::string::npos);
}

TEST_CASE("invalid YAML never applies partially parsed settings", "[settings][yaml]")
{
    const Settings defaults;
    for (const auto input :
         {"width: 1440\nmaximized: yes\n", "width: 1440\nunknown: value\n", "width: 1440\nheight: [1, 2]\n",
          "theme: \"unclosed\n", "width 1440\n", "width: 1440\n  height: 900\n", "  width: 1440\nheight: 900\n",
          "width: 1440\ntheme: [light]\n", "width: 1440\nlanguage: {value: zh-CN}\n", "width: 1440\ntheme: light: bad\n",
          "width: 1440\n---\nheight: 900\n", "width: 1440\ntheme: \"li\rght\"\n"})
    {
        CAPTURE(input);
        const auto settings = parse_settings(input);
        CHECK(settings.width == defaults.width);
        CHECK(settings.maximized == defaults.maximized);
        CHECK(settings.theme == defaults.theme);
    }
    CHECK(parse_settings("width: 639\nheight: 479\n").width == defaults.width);
    CHECK(parse_settings("width: 8193\nheight: 8193\n").height == defaults.height);
    CHECK(parse_settings("language: unknown\n").language == Language::system);
    CHECK(parse_settings(std::string("width: 1440\0\n", 13)).width == defaults.width);
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

TEST_CASE("system titlebar setting defaults to borderless and persists in YAML", "[chrome][settings]")
{
    CHECK_FALSE(Settings{}.system_titlebar);
    CHECK_FALSE(parse_settings("").system_titlebar);
    CHECK_FALSE(parse_settings("theme: light\n").system_titlebar);
    for (const bool enabled : {false, true})
    {
        Settings settings;
        settings.system_titlebar = enabled;
        CHECK(parse_settings(serialize_settings(settings)).system_titlebar == enabled);
    }
    CHECK(parse_settings("system_titlebar: true\n").system_titlebar);
    CHECK_FALSE(parse_settings("system_titlebar: false\n").system_titlebar);
    CHECK(parse_settings("width: 1440\nsystem_titlebar: invalid\n").width == Settings{}.width);
}

TEST_CASE("system titlebar is a bilingual independent appearance checkbox", "[chrome][menu]")
{
    Settings settings;
    for (const auto language : {Language::english, Language::chinese})
    {
        const auto menu        = build_menu(settings, language, true, false);
        const auto *appearance = find_menu_item(menu, 34);
        REQUIRE(appearance != nullptr);
        const auto *entry = find_menu_item(*appearance, 38);
        REQUIRE(entry != nullptr);
        CHECK(entry->action == MenuAction::system_titlebar);
        CHECK(entry->label == translate(language, Text::system_titlebar));
        CHECK(entry->checkable);
        CHECK_FALSE(entry->checked);
    }
    apply_menu_setting(MenuAction::system_titlebar, settings);
    CHECK(settings.system_titlebar);
    CHECK(settings.theme == Theme::system);
    CHECK(find_menu_item(build_menu(settings, Language::chinese, true, false), 38)->checked);
    apply_menu_setting(MenuAction::system_titlebar, settings);
    CHECK_FALSE(settings.system_titlebar);
}

TEST_CASE("rounded borderless corners default on and can be independently disabled", "[chrome][rounding][settings]")
{
    Settings settings;
    CHECK_FALSE(settings.disable_rounding);
    CHECK_FALSE(parse_settings("").disable_rounding);
    apply_menu_setting(MenuAction::disable_rounding, settings);
    CHECK(settings.disable_rounding);
    CHECK(parse_settings(serialize_settings(settings)).disable_rounding);
    auto menu = build_menu(settings, Language::chinese, true, false);
    REQUIRE(find_menu_item(menu, 41) != nullptr);
    CHECK(find_menu_item(menu, 41)->checked);
    CHECK(find_menu_item(menu, 41)->enabled);
    CHECK(find_menu_item(menu, 41)->label == "关闭圆角");
    apply_menu_setting(MenuAction::system_titlebar, settings);
    menu = build_menu(settings, Language::english, true, false);
    CHECK_FALSE(find_menu_item(menu, 41)->enabled);
    CHECK(find_menu_item(menu, 41)->checked); // Retain preference while native decorations are enabled.
    apply_menu_setting(MenuAction::disable_rounding, settings);
    CHECK_FALSE(settings.disable_rounding);
}

TEST_CASE("loading blocks every application shortcut and repeat until completion", "[menu][shortcut][loading]")
{
    for (const auto language : {Language::english, Language::chinese})
        for (const bool fullscreen : {false, true})
            for (const bool info : {false, true})
            {
                const auto menu  = build_menu(Settings{}, language, true, fullscreen, true, true, true, true, info, true);
                const auto check = [&](const auto &self, const MenuItem &item) -> void
                {
                    if (!item.shortcut.empty())
                        for (const bool repeat : {false, true})
                        {
                            const ShortcutContext loading{false, fullscreen, info, true};
                            CHECK(menu_action_for_input(menu, item.shortcut, repeat, loading) == MenuAction::none);
                            const ShortcutContext ready{false, fullscreen, info, false};
                            CHECK(menu_action_for_input(menu, item.shortcut, repeat, ready) ==
                                  menu_action_for_shortcut(menu, item.shortcut, repeat));
                        }
                    for (const auto &child : item.children)
                        self(self, child);
                };
                check(check, menu);
                for (const bool repeat : {false, true})
                    CHECK(menu_action_for_input(menu, {"Escape"}, repeat, {false, fullscreen, info, true}) == MenuAction::none);
                const auto escape = info ? MenuAction::image_info : fullscreen ? MenuAction::fullscreen : MenuAction::none;
                CHECK(menu_action_for_input(menu, {"Escape"}, false, {false, fullscreen, info, false}) == escape);
                CHECK(menu_action_for_input(menu, {"Control", "q"}, false, {true, fullscreen, info, false}) == MenuAction::none);
            }
}
