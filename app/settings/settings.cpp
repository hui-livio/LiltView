#include "settings.hpp"

#include <charconv>

namespace liltview
{
namespace
{
void parse_dimension(std::string_view value, int minimum, int &dimension)
{
    int parsed        = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= minimum && parsed <= 8192)
    {
        dimension = parsed;
    }
}

void apply_setting(Settings &settings, std::string_view key, std::string_view value)
{
    if (key == "width")
        parse_dimension(value, 640, settings.width);
    else if (key == "height")
        parse_dimension(value, 480, settings.height);
    else if (key == "language")
        settings.language = parse_language(value);
    else if (key == "maximized")
        settings.maximized = value == "true";
    else if (key == "theme")
    {
        if (value == "dark")
            settings.theme = Theme::dark;
        else if (value == "light")
            settings.theme = Theme::light;
        else
            settings.theme = Theme::system;
    }
}
} // namespace

Settings parse_settings(std::string_view data)
{
    Settings settings;
    if (data.size() > 4096)
        return settings;
    while (!data.empty())
    {
        const auto end = data.find('\n');
        auto line      = data.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        const auto equal = line.find('=');
        if (equal != std::string_view::npos)
            apply_setting(settings, line.substr(0, equal), line.substr(equal + 1));
        if (end == std::string_view::npos)
            break;
        data.remove_prefix(end + 1);
    }
    return settings;
}

std::string serialize_settings(const Settings &settings)
{
    const auto *theme = settings.theme == Theme::system ? "system" : (settings.theme == Theme::dark ? "dark" : "light");
    return "width=" + std::to_string(settings.width) + "\nheight=" + std::to_string(settings.height) +
        "\nmaximized=" + (settings.maximized ? "true" : "false") + "\nlanguage=" + std::string(language_code(settings.language)) +
        "\ntheme=" + theme + "\n";
}

Theme resolve_theme(Theme preference, Theme system_theme)
{
    if (preference != Theme::system)
        return preference;
    return system_theme == Theme::light ? Theme::light : Theme::dark;
}
} // namespace liltview
