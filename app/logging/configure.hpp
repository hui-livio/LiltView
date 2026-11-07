#ifndef LILTVIEW_LOGGING_CONFIGURE_HPP
#define LILTVIEW_LOGGING_CONFIGURE_HPP

#include "settings/settings.hpp"

namespace liltview
{
// Apply before spawning workers; YAML is read once during startup.
void configure_logging(LogLevel level);
} // namespace liltview
#endif
