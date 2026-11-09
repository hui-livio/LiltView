#include "settings.hpp"

#include <iguana/yaml_reader.hpp>
#include <iguana/yaml_writer.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace liltview
{
namespace
{
constexpr std::array<std::string_view, 7> log_levels{"trace", "debug", "info", "warn", "error", "critical", "off"};

LogLevel parse_log_level(std::string_view value)
{
    const auto found = std::find(log_levels.begin(), log_levels.end(), value);
    return found == log_levels.end() ? default_log_level() : static_cast<LogLevel>(found - log_levels.begin());
}

// Persistence names are stable strings, independent of internal enum ordinals.
struct SettingsDocument
{
    int width             = 1120;
    int height            = 760;
    bool maximized        = false;
    std::string theme     = "system";
    std::string language  = "system";
    std::string log_level = std::string(log_level_name(default_log_level()));
    bool system_titlebar  = false;
    bool disable_rounding = false;
};
YLT_REFL(SettingsDocument, width, height, maximized, theme, language, log_level, system_titlebar, disable_rounding);

Theme parse_theme(std::string_view value)
{
    if (value == "dark")
        return Theme::dark;
    if (value == "light")
        return Theme::light;
    return Theme::system;
}

std::string yaml_input(std::string_view data)
{
    if (data.starts_with("\xef\xbb\xbf"))
        data.remove_prefix(3);
    std::string yaml = "\n";
    bool started = false, ended = false;
    while (!data.empty())
    {
        const auto end = data.find('\n');
        auto line      = data.substr(0, end);
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.find('\r') != std::string_view::npos)
            throw std::runtime_error("Unexpected carriage return");
        auto content     = line.substr(0, line.find('#'));
        const auto first = content.find_first_not_of(" \t");
        if (first != std::string_view::npos)
        {
            content.remove_prefix(first);
            content = content.substr(0, content.find_last_not_of(" \t") + 1);
            if (content == "---")
            {
                if (started || ended)
                    throw std::runtime_error("Multiple YAML documents");
                started = true;
            }
            else if (content == "...")
            {
                if (ended)
                    throw std::runtime_error("Repeated YAML document end");
                ended = true;
            }
            else
            {
                if (ended)
                    throw std::runtime_error("Content after YAML document end");
                started = true;
                yaml.append(line).push_back('\n');
            }
        }
        if (end == std::string_view::npos)
            break;
        data.remove_prefix(end + 1);
    }
    return yaml;
}
} // namespace

LogLevel default_log_level()
{
#ifdef NDEBUG
    return LogLevel::warn;
#else
    return LogLevel::debug;
#endif
}

std::string_view log_level_name(LogLevel level)
{
    const auto index = static_cast<std::size_t>(level);
    return index < log_levels.size() ? log_levels[index] : log_levels[static_cast<std::size_t>(default_log_level())];
}

Settings parse_settings(std::string_view data)
{
    if (data.size() > 4096 || data.find('\0') != std::string_view::npos)
        return {};
    SettingsDocument document;
    try
    {
        // Normalize only document framing; field/value parsing remains iguana's job.
        const auto yaml = yaml_input(data);
        iguana::from_yaml(document, yaml);
    } catch (const std::runtime_error &)
    {
        return {}; // No partially parsed configuration escapes on malformed YAML.
    }
    Settings settings;
    if (document.width >= 640 && document.width <= 8192)
        settings.width = document.width;
    if (document.height >= 480 && document.height <= 8192)
        settings.height = document.height;
    settings.maximized        = document.maximized;
    settings.system_titlebar  = document.system_titlebar;
    settings.disable_rounding = document.disable_rounding;
    settings.theme            = parse_theme(document.theme);
    settings.language         = parse_language(document.language);
    settings.log_level        = parse_log_level(document.log_level);
    return settings;
}

std::string serialize_settings(const Settings &settings)
{
    const SettingsDocument document{settings.width,
                                    settings.height,
                                    settings.maximized,
                                    settings.theme == Theme::dark      ? "dark" :
                                        settings.theme == Theme::light ? "light" :
                                                                         "system",
                                    std::string(language_code(settings.language)),
                                    std::string(log_level_name(settings.log_level)),
                                    settings.system_titlebar,
                                    settings.disable_rounding};
    std::string yaml;
    iguana::to_yaml(document, yaml);
    return yaml;
}

Theme resolve_theme(Theme preference, Theme system_theme)
{
    if (preference != Theme::system)
        return preference;
    return system_theme == Theme::light ? Theme::light : Theme::dark;
}
} // namespace liltview
