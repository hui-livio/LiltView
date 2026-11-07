#include "application.hpp"
#include "cli/options.hpp"
#include "logging/configure.hpp"

#include <SDL3/SDL_main.h>
#include <spdlog/spdlog.h>

#include <exception>
#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        // Help, version and invalid arguments return before the graphics stack starts.
        const auto command = liltview::parse_command_line(argc, argv, std::cout, std::cerr);
        if (command.exit_code)
            return *command.exit_code;
        liltview::configure_logging(liltview::default_log_level());
        return liltview::run_application(command.options);
    } catch (const std::exception &error)
    {
        spdlog::error("LiltView stopped: {}", error.what());
        return 1;
    }
}
