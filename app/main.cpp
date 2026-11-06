#include "application.hpp"
#include "version.hpp"

#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <exception>
#include <optional>
#include <string_view>

namespace
{
void print_help()
{
    std::puts("LiltView " LILTVIEW_VERSION "\n\nUsage: liltview [options]\n"
              "  --language system|en-US|zh-CN   Override the saved language\n"
              "  --config-dir PATH             Use a separate settings directory\n"
              "  --smoke-test                  Exercise the window and exit\n"
              "  --screenshot PATH             Save a PPM during --smoke-test\n"
              "  --version                     Print the version\n"
              "  --help                        Print this help\n\n"
              "Image opening and decoding are not implemented yet.");
}

std::optional<int> handle_flag(std::string_view argument, liltview::LaunchOptions &options)
{
    if (argument == "--help")
    {
        print_help();
        return 0;
    }
    if (argument == "--version")
    {
        std::puts("LiltView " LILTVIEW_VERSION);
        return 0;
    }
    if (argument == "--smoke-test")
    {
        options.smoke_test = true;
        return std::nullopt;
    }
    return std::nullopt;
}

bool handle_value_option(std::string_view option, std::string_view value, liltview::LaunchOptions &options)
{
    if (option == "--language")
    {
        if (value != "system" && value != "en-US" && value != "zh-CN")
        {
            std::fputs("Invalid --language. Use system, en-US or zh-CN.\n", stderr);
            return false;
        }
        options.language = liltview::parse_language(value);
    }
    else if (option == "--config-dir")
        options.config_directory = value;
    else if (option == "--screenshot")
        options.screenshot = value;
    else
        return false;
    return true;
}

std::optional<int> parse_arguments(int argc, char **argv, liltview::LaunchOptions &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument(argv[i]);
        if (const auto result = handle_flag(argument, options))
            return result;
        if (argument == "--smoke-test")
            continue;
        if (argument == "--language" || argument == "--config-dir" || argument == "--screenshot")
        {
            if (++i >= argc || !handle_value_option(argument, argv[i], options))
            {
                std::fprintf(stderr, "Missing or invalid value for option: %.*s\n", static_cast<int>(argument.size()),
                             argument.data());
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "Unsupported option: %s\n", argv[i]);
        return 2;
    }
    if (!options.screenshot.empty() && !options.smoke_test)
    {
        std::fputs("--screenshot requires --smoke-test.\n", stderr);
        return 2;
    }
    return std::nullopt;
}
} // namespace

int main(int argc, char **argv)
{
    try
    {
        // 参数解析和窗口生命周期分开；帮助 / 版本在初始化图形环境前返回。
        liltview::LaunchOptions options;
        if (const auto result = parse_arguments(argc, argv, options))
            return *result;
        return liltview::run_application(options);
    } catch (const std::exception &error)
    {
        spdlog::error("LiltView stopped: {}", error.what());
        return 1;
    }
}
