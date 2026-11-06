#include <liltview/core.h>

// Export sizes/alignment from the C compiler for a C++ consumer to compare.
#define LV_LAYOUT(type)                                                                                                          \
    size_t lv_test_size_##type(void)                                                                                             \
    {                                                                                                                            \
        return sizeof(type);                                                                                                     \
    }                                                                                                                            \
    size_t lv_test_align_##type(void)                                                                                            \
    {                                                                                                                            \
        return alignof(type);                                                                                                    \
    }
#include "layout_types.inc"
#undef LV_LAYOUT

size_t lv_test_config_wakeup_offset(void)
{
    return offsetof(lv_core_config, wakeup);
}

size_t lv_test_event_payload_offset(void)
{
    return offsetof(lv_event_view, as);
}
