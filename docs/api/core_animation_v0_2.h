#ifndef LILTVIEW_ANIMATION_V0_2_DRAFT_H
#define LILTVIEW_ANIMATION_V0_2_DRAFT_H

/* DESIGN ONLY. No function declared below is implemented or exported today.
 * This header is a compile-checked proposal, not a shipping public header.
 * It reuses v1 value types; the v2 config/event layouts below are replacement
 * layout models, not objects accepted by the current lv_core_create/poll API.
 * Contract: docs/CORE_ANIMATION-v0.2.md. No code may cast a v1 event to v2. */
#include <liltview/core.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LV_ANIMATION_DESIGN_ABI_VERSION UINT32_C(2)
#define LV_ANIMATION_NONE UINT64_C(0)
typedef uint64_t lv_animation_id; /* Core-scoped, nonzero, never reused. */

/* Proposed addition to lv_capabilities; do not infer current availability. */
enum { LV_CAP_ANIMATION = 1u << 5 };

typedef struct lv_animation_limits {
    uint32_t max_sessions; /* init: 2; 0 disables animation sessions. */
    uint32_t reserved;     /* Must be 0. */
    uint64_t resident_budget_bytes; /* CPU sub-budget; 0 = min(CPU/2, 128 MiB). */
} lv_animation_limits;

/* Proposed ABI-2 configuration layout. common.abi_version = 2 and
 * common.struct_size = sizeof(lv_core_config_v2). This is not accepted by v1.
 * On implementation, flatten common's fields into the canonical core.h config. */
typedef struct lv_core_config_v2 {
    lv_core_config common;
    lv_animation_limits animation;
} lv_core_config_v2;

typedef struct lv_animation_open_desc {
    uint32_t struct_size;
    lv_priority priority;       /* CURRENT or VISIBLE_DETAIL; init: CURRENT. */
    uint64_t user_tag;
    lv_path path;               /* Copied before acceptance; no submit I/O. */
    lv_target target;           /* Immutable session FIT; REGION unsupported. */
} lv_animation_open_desc;

typedef uint32_t lv_animation_action;
enum { LV_ANIMATION_NEXT = 1, LV_ANIMATION_RESET = 2 };

typedef struct lv_animation_request_desc {
    uint32_t struct_size;
    lv_animation_action action; /* init: NEXT; at most one frame per job. */
    lv_animation_id animation_id;
    uint64_t user_tag;
    lv_priority priority;       /* CURRENT or VISIBLE_DETAIL; init: VISIBLE_DETAIL. */
    uint32_t reserved;          /* Must be 0. */
} lv_animation_request_desc;

typedef uint32_t lv_animation_info_flags;
enum {
    LV_ANIMATION_FRAME_COUNT_KNOWN = 1u << 0,
    LV_ANIMATION_DURATION_KNOWN = 1u << 1,
    LV_ANIMATION_PLAY_COUNT_KNOWN = 1u << 2
};

typedef struct lv_animation_info {
    lv_animation_id animation_id;
    uint64_t epoch;             /* Starts at 1; successful RESET increments. */
    lv_image_info image;        /* Logical canvas, FIT grid, anchored source revision. */
    lv_animation_info_flags flags;
    uint32_t reserved;
    uint64_t frame_count;       /* Ignore unless corresponding KNOWN flag is set. */
    uint64_t duration_ns;       /* Source duration of ONE pass, not all repeats. */
    uint64_t total_plays;       /* KNOWN: 0=infinite, 1=once, N=N total passes. */
} lv_animation_info;

typedef struct lv_animation_frame_key {
    lv_animation_id animation_id;
    uint64_t epoch;
    uint64_t frame_index;       /* Zero-based within one pass. */
} lv_animation_frame_key;

typedef uint32_t lv_animation_frame_flags;
enum {
    LV_ANIMATION_FRAME_DURATION_KNOWN = 1u << 0,
    LV_ANIMATION_FRAME_PTS_KNOWN = 1u << 1
};

typedef struct lv_animation_frame_info {
    lv_animation_frame_key key;
    uint64_t source_duration_ns; /* Raw duration; known zero is legal, no UI clamp. */
    uint64_t source_pts_ns;      /* Sum of prior raw durations; ignore if unknown. */
    lv_animation_frame_flags flags;
    uint32_t reserved;
} lv_animation_frame_info;

typedef struct lv_animation_region_info {
    lv_animation_frame_key key;
    lv_region_info region; /* Composited FIT canvas tile, never an encoded delta. */
} lv_animation_region_info;

/* Emitted when NEXT reaches a valid pass boundary without producing a frame.
 * It precedes the request's COMPLETE (final_tile_count=0). No automatic loop. */
typedef struct lv_animation_end_info {
    lv_animation_info final_info; /* Final known count/duration/loop metadata. */
} lv_animation_end_info;

/* Proposed additions to lv_event_type. Existing values 1..5 remain unchanged. */
enum {
    LV_EVENT_ANIMATION_INFO = 6,
    LV_EVENT_ANIMATION_FRAME_INFO = 7,
    LV_EVENT_ANIMATION_REGION_READY = 8,
    LV_EVENT_ANIMATION_END = 9
};

/* ABI-2 replacement layout model for lv_event_view. After migration the usual
 * lv_core_poll_event/lv_event_get_view/release APIs serve both event families. */
typedef struct lv_event_view_v2 {
    lv_event_type type;
    lv_request_id request_id;
    uint64_t user_tag;
    uint64_t sequence;
    union {
        lv_image_info image;
        lv_region_info region;
        lv_completion_info completion;
        lv_error_info error;
        lv_animation_info animation;
        lv_animation_frame_info frame;
        lv_animation_region_info frame_region;
        lv_animation_end_info animation_end;
    } as;
} lv_event_view_v2;

/* Proposed ABI-2 initializers; declarations only, NULL is a no-op. */
LV_API void LV_CALL lv_animation_limits_init(lv_animation_limits *out);
LV_API void LV_CALL lv_animation_open_desc_init(lv_animation_open_desc *out);
LV_API void LV_CALL lv_animation_request_desc_init(lv_animation_request_desc *out);

/* Success reserves a session ID plus an asynchronous OPEN request ID. It does
 * not mean the source is open. Failed outputs clear. An accepted OPEN failure
 * retains the caller's session slot until close/shutdown; no dangling handle. */
LV_API lv_status LV_CALL lv_core_open_animation(
    lv_core *core, const lv_animation_open_desc *desc,
    lv_animation_id *out_animation_id, lv_request_id *out_request_id);

/* One outstanding OPEN/NEXT/RESET per session, including an unreleased terminal.
 * NEXT succeeds with one complete frame OR END, never multiple frames. RESET
 * is asynchronous and emits INFO(new epoch) -> COMPLETE, with no pixel output. */
LV_API lv_status LV_CALL lv_core_submit_animation(
    lv_core *core, const lv_animation_request_desc *desc,
    lv_request_id *out_request_id);

/* Nonblocking logical close, never frees borrowed event/pixel references. Active
 * jobs cancel; committed terminals retain their dependent tiles. Existing cancel,
 * release, stats and shutdown rules apply. Further submit for this ID: NOT_FOUND.
 * Repeated close: OK while cleanup is pending, NOT_FOUND once reclaimed. */
/* Once core shutdown begins, close returns CLOSING; shutdown owns cleanup. */
LV_API lv_status LV_CALL lv_core_close_animation(lv_core *core, lv_animation_id animation_id);

#ifdef __cplusplus
}
#endif
#endif
