#include <liltview/core.h>

#include <cstdio>
#include <type_traits>

extern "C" {
#define LV_LAYOUT(type)                                                                                                          \
    size_t lv_test_size_##type(void);                                                                                            \
    size_t lv_test_align_##type(void);
#include "layout_types.inc"
#undef LV_LAYOUT
size_t lv_test_config_wakeup_offset(void);
size_t lv_test_event_payload_offset(void);
}

static_assert(std::is_standard_layout_v<lv_core_config>);
static_assert(std::is_trivially_copyable_v<lv_event_view>);
static_assert(sizeof(lv_request_id) == 8);

int main()
{
#define LV_LAYOUT(type)                                                                                                          \
    if (sizeof(type) != lv_test_size_##type() || alignof(type) != lv_test_align_##type())                                        \
    {                                                                                                                            \
        std::fputs("C/C++ layout mismatch: " #type "\n", stderr);                                                                \
        return 1;                                                                                                                \
    }
#include "layout_types.inc"
#undef LV_LAYOUT

    if (offsetof(lv_core_config, wakeup) != lv_test_config_wakeup_offset() ||
        offsetof(lv_event_view, as) != lv_test_event_payload_offset())
    {
        std::fputs("C/C++ member offset mismatch\n", stderr);
        return 2;
    }

    // Actual C-compiled API functions are linked, not C++ stand-ins.
    lv_core_config config;
    lv_core_config_init(&config);
    lv_request_desc request;
    lv_request_desc_init(&request);
    if (config.abi_version != lv_core_abi_version() || config.log != nullptr || config.wakeup != nullptr ||
        request.kind != LV_REQUEST_DISPLAY)
    {
        std::fputs("C API call from C++ failed\n", stderr);
        return 3;
    }
    return 0;
}
