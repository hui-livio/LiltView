#include <cstdio>
#include <type_traits>
#include "core_animation_v0_2.h"

extern "C" {
#define LV_ANIMATION_LAYOUT(type)                                                                                                \
    size_t lv_animation_test_size_##type(void);                                                                                  \
    size_t lv_animation_test_align_##type(void);
#include "animation_layout_types.inc"
#undef LV_ANIMATION_LAYOUT
}

using open_signature   = lv_status(LV_CALL *)(lv_core *, const lv_animation_open_desc *, lv_animation_id *, lv_request_id *);
using submit_signature = lv_status(LV_CALL *)(lv_core *, const lv_animation_request_desc *, lv_request_id *);
using close_signature  = lv_status(LV_CALL *)(lv_core *, lv_animation_id);
static_assert(std::is_same_v<decltype(&lv_core_open_animation), open_signature>);
static_assert(std::is_same_v<decltype(&lv_core_submit_animation), submit_signature>);
static_assert(std::is_same_v<decltype(&lv_core_close_animation), close_signature>);
static_assert(sizeof(lv_animation_id) == 8);
static_assert(offsetof(lv_core_config_v2, common) == 0);
static_assert(offsetof(lv_event_view_v2, as) == offsetof(lv_event_view, as));
static_assert(sizeof(lv_event_view_v2) > sizeof(lv_event_view));
static_assert(LV_CORE_ABI_VERSION == 1); // The design must not silently change runtime ABI.
static_assert(LV_ANIMATION_DESIGN_ABI_VERSION == 2);

int main()
{
#define LV_ANIMATION_LAYOUT(type)                                                                                                \
    static_assert(std::is_standard_layout_v<type> && std::is_trivially_copyable_v<type>);                                        \
    if (sizeof(type) != lv_animation_test_size_##type() || alignof(type) != lv_animation_test_align_##type())                    \
    {                                                                                                                            \
        std::fputs("Animation draft C/C++ layout mismatch: " #type "\n", stderr);                                                \
        return 1;                                                                                                                \
    }
#include "animation_layout_types.inc"
#undef LV_ANIMATION_LAYOUT
    return 0;
}
