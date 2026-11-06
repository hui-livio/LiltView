#include "core_log.hpp"

#include <spdlog/spdlog.h>

namespace liltview
{
void LV_CALL core_log(void *userdata, lv_log_level level, lv_request_id request_id, const char *utf8, size_t byte_count) noexcept
{
    (void)userdata;
    if (utf8 == nullptr || level >= LV_LOG_OFF)
    {
        return;
    }

    try
    {
        spdlog::level::level_enum output_level;
        switch (level)
        {
        case LV_LOG_TRACE:
            output_level = spdlog::level::trace;
            break;
        case LV_LOG_DEBUG:
            output_level = spdlog::level::debug;
            break;
        case LV_LOG_INFO:
            output_level = spdlog::level::info;
            break;
        case LV_LOG_WARN:
            output_level = spdlog::level::warn;
            break;
        default:
            output_level = spdlog::level::err;
            break;
        }
        // Data is a bounded string view, never treated as a format string.
        spdlog::log(output_level, "[core request={}] {}", request_id, spdlog::string_view_t(utf8, byte_count));
    } catch (...)
    {
        // Logging failures must not unwind into the C engine.
    }
}
} // namespace liltview
