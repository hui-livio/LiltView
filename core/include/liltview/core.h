#ifndef LILTVIEW_CORE_H
#define LILTVIEW_CORE_H

/* Draft API v0.1. Linux JPEG DISPLAY/FIT engine implemented; other features pending.
 * Contracts: docs/CORE_API-v0.1.md and docs/CORE_RUNTIME-v0.1.md.
 * C23 implementation, C++20/23 consumers.
 * ABI compatibility is for the same architecture and platform ABI only. */
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(LV_CORE_SHARED)
#  if defined(LV_CORE_BUILD)
#    define LV_API __declspec(dllexport)
#  else
#    define LV_API __declspec(dllimport)
#  endif
#else
#  define LV_API
#endif

#if defined(_WIN32)
#  define LV_CALL __cdecl
#else
#  define LV_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LV_CORE_ABI_VERSION UINT32_C(1)
#define LV_REQUEST_NONE UINT64_C(0)

typedef struct lv_core lv_core;
typedef struct lv_event lv_event;
typedef struct lv_pixels lv_pixels;
typedef uint64_t lv_request_id;

/* Fixed-width ABI fields; anonymous enums supply constants only. */
typedef uint32_t lv_status;
enum {
    LV_OK = 0,
    LV_EMPTY = 1,
    LV_TIMEOUT = 2,
    LV_INVALID_ARGUMENT = 3,
    LV_ABI_MISMATCH = 4,
    LV_UNSUPPORTED = 5,
    LV_OUT_OF_MEMORY = 6,
    LV_BUSY = 7,
    LV_CLOSING = 8,
    LV_NOT_FOUND = 9,
    LV_INVALID_STATE = 10,
    LV_LIMIT_REACHED = 11
};

typedef uint32_t lv_log_level;
enum {
    LV_LOG_TRACE = 0, LV_LOG_DEBUG = 1, LV_LOG_INFO = 2,
    LV_LOG_WARN = 3, LV_LOG_ERROR = 4, LV_LOG_OFF = 5
};

typedef uint32_t lv_path_encoding;
enum {
    LV_PATH_UTF8 = 1,
    LV_PATH_POSIX_BYTES = 2,
    LV_PATH_WINDOWS_UTF16 = 3
};

typedef struct lv_path {
    lv_path_encoding encoding;
    const void *data;
    size_t byte_count; /* Excludes terminator. UTF-16 length is in bytes. */
} lv_path;

typedef struct lv_size_u32 {
    uint32_t width;
    uint32_t height;
} lv_size_u32;

typedef struct lv_rect_u32 {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} lv_rect_u32;

typedef uint32_t lv_format;
enum {
    LV_FORMAT_UNKNOWN = 0, LV_FORMAT_JPEG = 1, LV_FORMAT_PNG = 2,
    LV_FORMAT_SVG = 3, LV_FORMAT_WEBP = 4, LV_FORMAT_BMP = 5,
    LV_FORMAT_GIF = 6, LV_FORMAT_TIFF = 7
};

typedef uint64_t lv_capabilities;
enum {
    LV_CAP_INCREMENTAL = 1u << 0,
    LV_CAP_REFINEMENT = 1u << 1,
    LV_CAP_SCALED_DECODE = 1u << 2,
    LV_CAP_REGION_DECODE = 1u << 3,
    LV_CAP_VECTOR_RASTERIZE = 1u << 4
};

typedef uint32_t lv_request_kind;
enum { LV_REQUEST_DISPLAY = 1, LV_REQUEST_PREFETCH = 2 };

typedef uint32_t lv_priority;
enum {
    LV_PRIORITY_CURRENT = 0,
    LV_PRIORITY_VISIBLE_DETAIL = 1,
    LV_PRIORITY_PREFETCH = 2
};

typedef uint32_t lv_target_kind;
enum { LV_TARGET_FIT = 1, LV_TARGET_REGION = 2 };

typedef struct lv_target {
    lv_target_kind kind;
    union {
        /* Physical-pixel bounds AFTER applying source orientation.
         * Bitmap FIT never enlarges the source. SVG may rasterize to fit. */
        lv_size_u32 fit_bounds;
        struct {
            /* Full unrotated output canvas, NOT a requested allocation. */
            lv_size_u32 canvas;
            /* Half-open rectangle in that output canvas. */
            lv_rect_u32 rect;
        } region;
    } as;
} lv_target;

typedef uint32_t lv_load_mode;
enum { LV_LOAD_SMALL = 1, LV_LOAD_INCREMENTAL = 2, LV_LOAD_LARGE = 3 };

typedef uint32_t lv_presentation;
enum { LV_PRESENT_AT_COMPLETE = 1, LV_PRESENT_IMMEDIATE = 2 };

typedef uint32_t lv_orientation;
enum {
    LV_ORIENTATION_NORMAL = 1,
    LV_ORIENTATION_FLIP_X = 2,
    LV_ORIENTATION_ROTATE_180 = 3,
    LV_ORIENTATION_FLIP_Y = 4,
    LV_ORIENTATION_TRANSPOSE = 5,
    LV_ORIENTATION_ROTATE_90_CW = 6,
    LV_ORIENTATION_TRANSVERSE = 7,
    LV_ORIENTATION_ROTATE_270_CW = 8
};

typedef uint32_t lv_pixel_format;
enum { LV_PIXEL_RGBA8 = 1 };

typedef uint32_t lv_color_space;
enum { LV_COLOR_UNKNOWN = 0, LV_COLOR_SRGB = 1 };

typedef struct lv_pixel_view {
    const uint8_t *data;
    uint64_t byte_count;
    uint64_t stride_bytes;
    lv_size_u32 size;
    lv_pixel_format format; /* v0.1: byte order R,G,B,A; straight alpha. */
    lv_color_space color_space; /* UNKNOWN is not a claim of sRGB accuracy. */
} lv_pixel_view;

/* Callback text is borrowed for this invocation only. Both callbacks may
 * run on worker threads; do not call core APIs or UI code from a callback.
 * The application must contain C++ exceptions within its adapter. */
typedef void (LV_CALL *lv_log_fn)(void *userdata, lv_log_level level,
                                lv_request_id request_id,
                                const char *utf8, size_t byte_count);
typedef void (LV_CALL *lv_wakeup_fn)(void *userdata);

typedef struct lv_core_config {
    uint32_t abi_version;
    uint32_t struct_size;
    uint64_t cpu_budget_bytes;       /* Required nonzero; not an RSS cap. */
    uint64_t cache_budget_bytes;     /* Sub-budget of CPU budget; 0 disables. */
    uint64_t queue_pixel_bytes;      /* Required nonzero; queue payload cap. */
    uint64_t small_file_bytes;       /* init: 16 MiB */
    uint64_t small_pixel_bytes;      /* init: 64 MiB */
    uint64_t large_pixel_bytes;      /* init: 256 MiB */
    uint32_t progressive_after_ms;   /* init: 100; UI also schedules reveal timer. */
    uint32_t update_interval_ms;     /* init: 33; 0 disables time coalescing. */
    uint32_t tile_edge;              /* init: 256; power of two, 16..1024. */
    uint32_t max_requests;           /* init: 16; includes undrained terminals. */
    uint32_t foreground_workers;     /* init: 1 */
    uint32_t prefetch_workers;       /* init: 1; 0 disables prefetch requests. */
    uint64_t disk_cache_budget_bytes;/* init: 0; disabled. */
    lv_path disk_cache_directory;    /* Required absolute path if enabled. */
    lv_log_level log_level;          /* init: LV_LOG_WARN */
    lv_log_fn log;
    void *log_userdata;
    lv_wakeup_fn wakeup;
    void *wakeup_userdata;
} lv_core_config;

typedef struct lv_request_desc {
    uint32_t struct_size;
    lv_request_kind kind;
    lv_priority priority;
    uint64_t user_tag;       /* Echoed unchanged; core never interprets it. */
    lv_path path;           /* Copied on success; no file I/O in submit. */
    lv_target target;       /* Immutable; a new view uses a new request ID. */
} lv_request_desc;

typedef uint32_t lv_info_flags;
enum {
    LV_INFO_SOURCE_SIZE_KNOWN = 1u << 0,
    LV_INFO_FILE_BYTES_KNOWN = 1u << 1
};

typedef struct lv_image_info {
    lv_format format;
    lv_info_flags flags;
    lv_size_u32 source_size;       /* Unrotated; SVG may have no natural size. */
    uint64_t file_bytes;
    uint64_t source_revision;     /* Retain INFO to anchor this source identity. */
    lv_orientation orientation;
    lv_capabilities capabilities;/* Actual file/backend, not format-wide claims. */
    lv_load_mode load_mode;
    lv_size_u32 canvas;           /* Unrotated output size for this request. */
    lv_rect_u32 region;           /* Entire requested coverage, in canvas pixels. */
    uint32_t tile_edge;
} lv_image_info;

typedef uint32_t lv_region_flags;
enum { LV_REGION_FINAL = 1u << 0 };

typedef struct lv_region_info {
    uint64_t tile_id;             /* Stable grid index within this request. */
    uint64_t revision;            /* Strictly increasing per tile, starts at 1. */
    lv_rect_u32 rect;             /* Grid tile clipped to requested coverage. */
    lv_region_flags flags;
    lv_presentation presentation;
    lv_pixels *pixels;            /* Borrowed from event; retain for later use. */
} lv_region_info;

typedef struct lv_completion_info {
    uint64_t final_tile_count;    /* DISPLAY: exact requested coverage is final. */
    uint64_t elapsed_ns;          /* Submit acceptance to core completion. */
    uint64_t source_bytes_read;   /* May exceed file size due to rereads. */
} lv_completion_info;

typedef uint32_t lv_error_code;
enum {
    LV_ERROR_IO = 1,
    LV_ERROR_FORMAT_UNSUPPORTED = 2,
    LV_ERROR_CORRUPT_DATA = 3,
    LV_ERROR_DIMENSION_LIMIT = 4,
    LV_ERROR_MEMORY_BUDGET = 5,
    LV_ERROR_OUT_OF_MEMORY = 6,
    LV_ERROR_SOURCE_CHANGED = 7,
    LV_ERROR_TARGET_UNSUPPORTED = 8,
    LV_ERROR_DISK_CACHE = 9,
    LV_ERROR_INTERNAL = 10
};

typedef uint32_t lv_system_error_domain;
enum { LV_SYSTEM_NONE = 0, LV_SYSTEM_ERRNO = 1, LV_SYSTEM_WIN32 = 2 };

typedef struct lv_error_info {
    lv_error_code code;
    lv_system_error_domain system_domain;
    uint32_t system_code;
    uint32_t has_partial_output;  /* 0 or 1; does not mark it complete. */
    uint64_t required_bytes;     /* Budget diagnostics; 0 when unknown/N/A. */
    uint64_t limit_bytes;
    const char *diagnostic_utf8; /* Borrowed, optional; not a localized UI label. */
    size_t diagnostic_byte_count;
} lv_error_info;

typedef uint32_t lv_event_type;
enum {
    LV_EVENT_IMAGE_INFO = 1,
    LV_EVENT_REGION_READY = 2,
    LV_EVENT_COMPLETE = 3,
    LV_EVENT_ERROR = 4,
    LV_EVENT_CANCELLED = 5
};

typedef struct lv_event_view {
    lv_event_type type;
    lv_request_id request_id;
    uint64_t user_tag;
    uint64_t sequence; /* Strictly increasing per request; gaps allowed. */
    union {
        lv_image_info image;
        lv_region_info region;
        lv_completion_info completion;
        lv_error_info error;
    } as; /* CANCELLED has no payload. Read only the member selected by type. */
} lv_event_view;

typedef struct lv_format_support {
    uint32_t available; /* 0 or 1; declared format IDs do not imply support. */
    lv_capabilities capabilities;
} lv_format_support;

typedef struct lv_core_stats {
    uint64_t accounted_cpu_bytes;
    uint64_t cache_bytes;           /* Subset of accounted_cpu_bytes. */
    uint64_t queued_pixel_bytes;    /* Logical queue payload, not extra RSS. */
    uint64_t pinned_pixel_bytes;    /* Live borrowed pixels; can overlap cache. */
    uint64_t disk_cache_bytes;
    uint32_t accepted_requests;     /* Includes unreleased terminal events. */
    uint32_t running_requests;
    uint32_t queued_requests;
    uint32_t borrowed_events;
    uint32_t external_pixel_refs;
} lv_core_stats;

/* Initialization: sets version/size and documented defaults. NULL is a no-op.
 * Config budgets and request paths/target sizes still require caller values. */
LV_API uint32_t LV_CALL lv_core_abi_version(void);
LV_API void LV_CALL lv_core_config_init(lv_core_config *out_config);
LV_API void LV_CALL lv_request_desc_init(lv_request_desc *out_request);

/* Control APIs are serialized on the create thread; no callback reentry.
 * create/submit copy input values/paths but borrow callback userdata until join.
 * Failed create leaves no callbacks in flight. Output handles/IDs clear on failure. */
LV_API lv_status LV_CALL lv_core_create(const lv_core_config *config,
                                      lv_core **out_core);
LV_API lv_status LV_CALL lv_core_submit(lv_core *core,
                                      const lv_request_desc *request,
                                      lv_request_id *out_request_id);
/* Active cancel invalidates publication, discards unpolled pixel events, then
 * cooperatively stops work. Once a terminal event is committed, cancel is a
 * no-op and preserves all queued events. Already borrowed objects stay valid. */
LV_API lv_status LV_CALL lv_core_cancel(lv_core *core, lv_request_id request_id);
LV_API lv_status LV_CALL lv_core_poll_event(lv_core *core, lv_event **out_event);
LV_API lv_status LV_CALL lv_core_query_format(lv_core *core, lv_format format,
                                            lv_format_support *out_support);
LV_API lv_status LV_CALL lv_core_get_stats(lv_core *core, lv_core_stats *out_stats);

/* poll transfers event ownership; the event owns any region-pixels reference.
 * Views are borrowed; retain pixels before releasing the event for delayed use.
 * Release an event exactly once. release/retain(NULL) are no-ops; view(NULL) is NULL.
 * Retained INFO anchors source identity; it survives terminal release/slot reuse. */
LV_API const lv_event_view *LV_CALL lv_event_get_view(const lv_event *event);
LV_API void LV_CALL lv_event_release(lv_event *event);
LV_API const lv_pixel_view *LV_CALL lv_pixels_get_view(const lv_pixels *pixels);
LV_API void LV_CALL lv_pixels_retain(lv_pixels *pixels);
LV_API void LV_CALL lv_pixels_release(lv_pixels *pixels);

/* Closing is asynchronous and discards UNPOLLED events. Normal per-request
 * terminal-delivery guarantees end here; borrowed objects stay valid.
 * join(0) polls; a nonzero timeout is a bounded wait, never a forced thread kill.
 * join does not wait for application upload threads; no callbacks occur after OK.
 * destroy returns BUSY until joined and all borrowed objects have been released.
 * The caller must finish all external object accesses before final destroy. */
LV_API lv_status LV_CALL lv_core_begin_shutdown(lv_core *core);
LV_API lv_status LV_CALL lv_core_join(lv_core *core, uint32_t timeout_ms);
LV_API lv_status LV_CALL lv_core_destroy(lv_core **inout_core);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LILTVIEW_CORE_H */
