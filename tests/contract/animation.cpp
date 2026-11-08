#include <cstdio>
#include <liltview/core.h>
#include <type_traits>

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
static_assert(offsetof(lv_core_config, abi_version) == 0);
static_assert(offsetof(lv_event_view, as) >= sizeof(lv_request_id) * 3);

static_assert(LV_CORE_ABI_VERSION == 2);


int main()
{
#define LV_ANIMATION_LAYOUT(type)                                                                                                \
    static_assert(std::is_standard_layout_v<type> && std::is_trivially_copyable_v<type>);                                        \
    if (sizeof(type) != lv_animation_test_size_##type() || alignof(type) != lv_animation_test_align_##type())                    \
    {                                                                                                                            \
        std::fputs("Animation C/C++ layout mismatch: " #type "\n", stderr);                                                      \
        return 1;                                                                                                                \
    }
#include "animation_layout_types.inc"
#undef LV_ANIMATION_LAYOUT
    lv_animation_limits limits{};
    lv_animation_limits_init(&limits);
    if (limits.max_sessions != 2 || limits.resident_budget_bytes || limits.reserved)
        return 2;
    lv_animation_open_desc open{};
    lv_animation_open_desc_init(&open);
    if (open.struct_size != sizeof open || open.target.kind != LV_TARGET_FIT || open.priority != LV_PRIORITY_CURRENT)
        return 3;
    lv_animation_request_desc frame{};
    lv_animation_request_desc_init(&frame);
    if (frame.struct_size != sizeof frame || frame.action != LV_ANIMATION_NEXT || frame.priority != LV_PRIORITY_VISIBLE_DETAIL)
        return 4;
    lv_request_id request   = 123;
    lv_animation_id session = 123;
    if (lv_core_open_animation(nullptr, &open, &session, &request) != LV_INVALID_ARGUMENT || session || request)
        return 5;
    lv_animation_limits_init(nullptr);
    lv_animation_open_desc_init(nullptr);
    lv_animation_request_desc_init(nullptr);
    return 0;
}
