#ifndef LILTVIEW_CLI_OPTIONS_HPP
#define LILTVIEW_CLI_OPTIONS_HPP

#include "i18n/translator.hpp"

#include <iosfwd>
#include <optional>
#include <string>

namespace liltview
{
struct LaunchOptions
{
    std::optional<Language> language;
    std::string config_directory;
    bool smoke_test = false;
    std::string screenshot;
    std::string image_path;
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
