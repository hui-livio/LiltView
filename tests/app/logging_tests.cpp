#include "logging/configure.hpp"
#include "settings/settings.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>

#include <array>

TEST_CASE("YAML log levels round trip and missing values use the build default", "[settings][logging]")
{
    using namespace liltview;
    constexpr std::array levels{LogLevel::trace, LogLevel::debug,    LogLevel::info, LogLevel::warn,
                                LogLevel::error, LogLevel::critical, LogLevel::off};
    for (const auto level : levels)
    {
        CAPTURE(log_level_name(level));
        Settings settings;
        settings.log_level = level;
        const auto yaml    = serialize_settings(settings);
        CHECK(yaml.find("log_level: " + std::string(log_level_name(level))) != std::string::npos);
        CHECK(parse_settings(yaml).log_level == level);
    }
    CHECK(Settings{}.log_level == default_log_level());
    CHECK(parse_settings("").log_level == default_log_level());
    CHECK(parse_settings("width: 1440\n").log_level == default_log_level());
    CHECK(parse_settings("log_level: invalid\n").log_level == default_log_level());
#ifdef NDEBUG
    CHECK(default_log_level() == LogLevel::warn);
#else
    CHECK(default_log_level() == LogLevel::debug);
#endif
}

TEST_CASE("configured log level controls the spdlog runtime filter", "[logging]")
{
    using namespace liltview;
    struct RestoreLevel
    {
        spdlog::level::level_enum previous = spdlog::get_level();
        ~RestoreLevel()
        {
            spdlog::set_level(previous);
        }
    } restore;

    configure_logging(parse_settings("log_level: warn\n").log_level);
    CHECK(spdlog::get_level() == spdlog::level::warn);
    CHECK_FALSE(spdlog::default_logger()->should_log(spdlog::level::debug));
    CHECK_FALSE(spdlog::default_logger()->should_log(spdlog::level::info));
    CHECK(spdlog::default_logger()->should_log(spdlog::level::warn));
    CHECK(spdlog::default_logger()->should_log(spdlog::level::err));
    configure_logging(parse_settings("log_level: debug\n").log_level);
    CHECK(spdlog::get_level() == spdlog::level::debug);
    CHECK(spdlog::default_logger()->should_log(spdlog::level::debug));
    configure_logging(LogLevel::off);
    CHECK_FALSE(spdlog::default_logger()->should_log(spdlog::level::critical));
    configure_logging(LogLevel::trace);
    CHECK(spdlog::default_logger()->should_log(spdlog::level::trace));
    configure_logging(LogLevel::error);
    CHECK(spdlog::get_level() == spdlog::level::err);
    configure_logging(LogLevel::critical);
    CHECK(spdlog::get_level() == spdlog::level::critical);
}
