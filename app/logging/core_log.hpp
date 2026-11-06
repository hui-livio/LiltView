#ifndef LILTVIEW_APP_CORE_LOG_HPP
#define LILTVIEW_APP_CORE_LOG_HPP

#include <liltview/core.h>

namespace liltview
{
// Assign config.log = core_log; config.log_userdata can remain null.
// Uses spdlog's default logger and contains all exceptions at the C boundary.
void LV_CALL core_log(void *userdata, lv_log_level level, lv_request_id request_id, const char *utf8, size_t byte_count) noexcept;
} // namespace liltview

#endif
