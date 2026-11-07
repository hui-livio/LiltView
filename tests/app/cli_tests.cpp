#include "cli/options.hpp"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Parsed
{
    liltview::CommandLineResult command;
    std::string output;
    std::string error;
};

Parsed parse(std::initializer_list<const char *> arguments)
{
    const std::vector<const char *> argv(arguments);
    std::ostringstream out, error;
    auto command = liltview::parse_command_line(static_cast<int>(argv.size()), argv.data(), out, error);
    return {std::move(command), out.str(), error.str()};
}
} // namespace

TEST_CASE("CLI11 parses launch options and one positional image", "[cli]")
{
    const auto parsed = parse({"liltview", "--language", "zh-CN", "--config-dir", "/tmp/配置 path", "--smoke-test",
                               "--screenshot", "/tmp/screen shot.ppm", "/tmp/图片.jpg"});
    REQUIRE_FALSE(parsed.command.exit_code);
    CHECK(parsed.command.options.language == liltview::Language::chinese);
    CHECK(parsed.command.options.config_directory == "/tmp/配置 path");
    CHECK(parsed.command.options.smoke_test);
    CHECK(parsed.command.options.screenshot == "/tmp/screen shot.ppm");
    CHECK(parsed.command.options.image_path == "/tmp/图片.jpg");
    CHECK(parsed.output.empty());
    CHECK(parsed.error.empty());
    const auto defaults = parse({"liltview"});
    CHECK_FALSE(defaults.command.exit_code);
    CHECK_FALSE(defaults.command.options.language);
    CHECK_FALSE(defaults.command.options.smoke_test);
    const auto follow = parse({"liltview", "--language=system"});
    CHECK_FALSE(follow.command.exit_code);
    CHECK(follow.command.options.language == liltview::Language::system);
    const auto filename = parse({"liltview", "--", "-photo.jpg"});
    CHECK_FALSE(filename.command.exit_code);
    CHECK(filename.command.options.image_path == "-photo.jpg");
}

TEST_CASE("CLI11 help and version exit before launching graphics", "[cli]")
{
    for (const auto flag : {"--help", "-h", "--version"})
    {
        const auto parsed = parse({"liltview", flag});
        REQUIRE(parsed.command.exit_code);
        CHECK(*parsed.command.exit_code == 0);
        CHECK(parsed.output.find("LiltView") != std::string::npos);
        CHECK(parsed.error.empty());
    }
    const auto help = parse({"liltview", "--help"});
    CHECK(help.output.find("--language") != std::string::npos);
    CHECK(help.output.find("settings.yaml") != std::string::npos);
}

TEST_CASE("CLI11 rejects unknown missing duplicate and conflicting arguments", "[cli]")
{
    for (const auto args : {std::initializer_list<const char *>{"liltview", "--language", "invalid"},
                            {"liltview", "--language"},
                            {"liltview", "--config-dir"},
                            {"liltview", "--unknown"},
                            {"liltview", "--screenshot", "file.ppm"},
                            {"liltview", "--smoke-test=false", "--screenshot", "file.ppm"},
                            {"liltview", "--smoke-test=0", "--screenshot", "file.ppm"},
                            {"liltview", "one.jpg", "two.jpg"},
                            {"liltview", "--language", "en-US", "--language", "zh-CN"}})
    {
        const auto parsed = parse(args);
        REQUIRE(parsed.command.exit_code);
        CHECK(*parsed.command.exit_code != 0);
        CHECK_FALSE(parsed.error.empty());
    }
}
