#ifndef LILTVIEW_APPLICATION_HPP
#define LILTVIEW_APPLICATION_HPP

#include "i18n/translator.hpp"

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
};

int run_application(const LaunchOptions &options);
} // namespace liltview

#endif
