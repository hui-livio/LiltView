#include "configure.hpp"

#include <spdlog/spdlog.h>

#include <array>

namespace liltview
{
void configure_logging(LogLevel level)
{
    constexpr std::array<spdlog::level::level_enum, 7> levels{spdlog::level::trace, spdlog::level::debug, spdlog::level::info,
                                                              spdlog::level::warn,  spdlog::level::err,   spdlog::level::critical,
                                                              spdlog::level::off};
    const auto index = static_cast<std::size_t>(level);
    spdlog::set_level(index < levels.size() ? levels[index] : levels[static_cast<std::size_t>(default_log_level())]);
}
} // namespace liltview
