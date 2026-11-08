#include <liltview/core.h>

// Public animation ABI checks.
typedef lv_status(LV_CALL *open_signature)(lv_core *, const lv_animation_open_desc *, lv_animation_id *, lv_request_id *);
typedef lv_status(LV_CALL *submit_signature)(lv_core *, const lv_animation_request_desc *, lv_request_id *);
typedef lv_status(LV_CALL *close_signature)(lv_core *, lv_animation_id);
static_assert(_Generic(&lv_core_open_animation, open_signature: 1, default: 0));
static_assert(_Generic(&lv_core_submit_animation, submit_signature: 1, default: 0));
static_assert(_Generic(&lv_core_close_animation, close_signature: 1, default: 0));
static_assert(sizeof(lv_animation_id) == 8);
static_assert(offsetof(lv_core_config, abi_version) == 0);
static_assert(offsetof(lv_event_view, as) >= sizeof(lv_request_id) * 3);

#define LV_ANIMATION_LAYOUT(type)                                                                                                \
    size_t lv_animation_test_size_##type(void)                                                                                   \
    {                                                                                                                            \
        return sizeof(type);                                                                                                     \
    }                                                                                                                            \
    size_t lv_animation_test_align_##type(void)                                                                                  \
    {                                                                                                                            \
        return alignof(type);                                                                                                    \
    }
#include "animation_layout_types.inc"
#undef LV_ANIMATION_LAYOUT
