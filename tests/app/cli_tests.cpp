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

TEST_CASE("CLI11 parses explicit raw layout options", "[cli][raw-layout]")
{
    const auto parsed = parse({"liltview", "--raw-layout", "i420", "--raw-size", "640x480", "--raw-plane",
                               "0,640,640,480", "--raw-plane", "307200,320,320,240", "--raw-plane",
                               "384000,320,320,240", "--raw-matrix", "bt601", "--raw-range", "limited",
                               "--raw-siting", "center", "--raw-row-order", "bottom-up", "--raw-byte-order",
                               "little", "frame.yuv"});
    REQUIRE_FALSE(parsed.command.exit_code);
    REQUIRE(parsed.command.options.raw);
    CHECK(parsed.command.options.raw->layout == "i420");
    CHECK(parsed.command.options.raw->size == "640x480");
    REQUIRE(parsed.command.options.raw->planes.size() == 3);
    CHECK(parsed.command.options.raw->planes[1] == "307200,320,320,240");
    CHECK(parsed.command.options.raw->row_order == "bottom-up");
    CHECK(parsed.command.options.raw->byte_order == "little");
    CHECK(parsed.command.options.raw->matrix == "bt601");
    CHECK(parsed.command.options.raw->range == "limited");
    CHECK(parsed.command.options.raw->siting == "center");
    CHECK(parsed.command.options.image_path == "frame.yuv");
}

TEST_CASE("CLI11 rejects incomplete or malformed explicit raw descriptions", "[cli][raw-layout]")
{
    for (const auto args : {
             std::initializer_list<const char *> {"liltview", "--raw-layout", "rgb24", "frame.raw"},
             {"liltview", "--raw-layout", "rgb24", "--raw-size", "640", "--raw-plane", "0,3,3,1", "frame.raw"},
             {"liltview", "--raw-layout", "rgb24", "--raw-size", "640x480", "--raw-plane", "0,3,3", "frame.raw"},
             {"liltview", "--raw-layout", "rgb24", "--raw-size", "640x480", "--raw-plane", "0,3,3,1", "--raw-row-order",
              "sideways", "frame.raw"},
             {"liltview", "--raw-layout", "i420", "--raw-size", "640x480", "--raw-plane", "0,640,640,480", "frame.yuv"},
             {"liltview", "--raw-layout", "bayer8", "--raw-size", "640x480", "--raw-plane", "0,640,640,480", "frame.raw"},
         })
    {
        const auto parsed = parse(args);
        REQUIRE(parsed.command.exit_code);
        CHECK(*parsed.command.exit_code != 0);
        CHECK_FALSE(parsed.error.empty());
    }
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
    CHECK(help.output.find("AVIF") != std::string::npos);
    CHECK(help.output.find("JPEG XL") != std::string::npos);
    CHECK(help.output.find("HEIF/HEIC") != std::string::npos);
    CHECK(help.output.find("RAW/DNG") != std::string::npos);
    CHECK(help.output.find("OpenEXR") != std::string::npos);
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

TEST_CASE("embedded redistribution notices are available without launching graphics", "[cli][resources]")
{
    const auto parsed = parse({"liltview", "--licenses"});
    REQUIRE(parsed.command.exit_code);
    CHECK(*parsed.command.exit_code == 0);
    CHECK(parsed.error.empty());
    for (const auto *component : {"Apache License", "HarmonyOS", "libjpeg-turbo", "FreeType", "SDL", "PlutoVG",
                                 "third_party/plasma-wayland-protocols/LICENSE.MIT"})
    {
        CAPTURE(component);
        CHECK(parsed.output.find(component) != std::string::npos);
    }
}

TEST_CASE("CLI parses explicit raw layout descriptors", "[cli][raw]")
{
    const auto parsed = parse({"liltview", "--raw-layout", "rgb24", "--raw-size", "640x480", "--raw-plane",
                               "3,1920,1920,480", "--raw-row-order", "bottom-up", "--raw-byte-order", "little",
                               "--raw-alpha", "opaque", "frame.raw"});
    REQUIRE_FALSE(parsed.command.exit_code);
    REQUIRE(parsed.command.options.raw);
    CHECK(parsed.command.options.raw->layout == "rgb24");
    CHECK(parsed.command.options.raw->size == "640x480");
    CHECK(parsed.command.options.raw->planes == std::vector<std::string>{"3,1920,1920,480"});
    CHECK(parsed.command.options.raw->row_order == "bottom-up");
    CHECK(parsed.command.options.raw->byte_order == "little");
    CHECK(parsed.command.options.image_path == "frame.raw");
}

TEST_CASE("CLI rejects incomplete or malformed raw descriptors", "[cli][raw]")
{
    for (const auto args : {std::initializer_list<const char *> {"liltview", "--raw-layout", "rgb24", "frame.raw"},
                            {"liltview", "--raw-layout", "rgb24", "--raw-size", "640x480", "--raw-plane", "0,3,x,2", "frame.raw"},
                            {"liltview", "--raw-layout", "rgb24", "--raw-size", "640", "--raw-plane", "0,3,3,480", "frame.raw"},
                            {"liltview", "--raw-layout", "unknown", "--raw-size", "640x480", "--raw-plane", "0,3,3,480", "frame.raw"}})
    {
        const auto parsed = parse(args);
        REQUIRE(parsed.command.exit_code);
        CHECK(*parsed.command.exit_code != 0);
        CHECK_FALSE(parsed.error.empty());
    }
}
