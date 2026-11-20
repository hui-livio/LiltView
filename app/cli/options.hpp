#ifndef LILTVIEW_CLI_OPTIONS_HPP
#define LILTVIEW_CLI_OPTIONS_HPP

#include "i18n/translator.hpp"

#include <iosfwd>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace liltview
{
struct RawInputOptions
{
    std::string layout;
    std::string size;
    std::vector<std::string> planes;
    std::string row_order = "top-down";
    std::string byte_order = "native";
    std::string alpha = "opaque";
    std::string matrix;
    std::string range;
    std::string siting;
    std::string cfa;
    std::uint32_t black_level = 0;
    std::uint32_t white_level = 0;
    std::uint32_t red_gain_q16 = 65536;
    std::uint32_t green_red_gain_q16 = 65536;
    std::uint32_t green_blue_gain_q16 = 65536;
    std::uint32_t blue_gain_q16 = 65536;
};

struct LaunchOptions
{
    std::optional<Language> language;
    std::string config_directory;
    bool smoke_test = false;
    std::string screenshot;
    std::string image_path;
    std::optional<RawInputOptions> raw;
};

struct CommandLineResult
{
    LaunchOptions options;
    std::optional<int> exit_code;
};

// Does not initialize SDL or read settings. CLI11 owns validation and messages.
CommandLineResult parse_command_line(int argc, const char *const *argv, std::ostream &out, std::ostream &error);
} // namespace liltview
#endif
