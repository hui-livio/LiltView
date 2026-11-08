#ifndef LV_INTERNAL_H
#define LV_INTERNAL_H
#include <liltview/core.h>
#include <stdatomic.h>
#include <stdio.h>
#include <threads.h>
#include <time.h>

typedef struct lv_file_version
{
    uint64_t device, inode, bytes, modified_s, modified_ns, changed_s, changed_ns;
} lv_file_version;
typedef struct lv_source
{
    lv_file_version version;
    uint64_t revision;
    uint32_t refs;
} lv_source;
typedef struct lv_request lv_request;

struct lv_pixels
{
    lv_core *core;
    lv_pixel_view view;
    uint32_t refs, outside_refs;
    uint64_t allocation;
    uint8_t data[];
};
struct lv_event
{
    lv_core *core;
    lv_event_view view;
    lv_event *next;
    lv_request *terminal_request;
    lv_source *source;
    bool pooled, occupied, borrowed;
};
struct lv_request
{
    lv_request_id id;
    lv_request_desc desc;
    char *path;
    size_t path_bytes;
    atomic_bool cancelled;
    enum
    {
        LV_FREE,
        LV_PENDING,
        LV_RUNNING,
        LV_TERMINAL
    } state;
    lv_event *info, *terminal;
    lv_source *source;
    uint64_t sequence, started_ns, final_tiles, bytes_read, reservation;
    uint64_t workspace;
    bool partial;
};
struct lv_core
{
    lv_core_config config;
    mtx_t mutex;
    cnd_t condition;
    thrd_t worker;
    bool closing, exited, joined;
    lv_request *requests;
    lv_event *controls, *head, *tail;
    lv_source *sources;
    uint32_t control_count;
    uint64_t next_id, next_revision, used, reserved, queued_bytes, pinned_bytes;
    uint32_t accepted, borrowed, pixel_refs, live_pixels;
};

uint64_t lv_clock_ns(void);
bool lv_stopped(lv_core *, lv_request *);
bool lv_reserve(lv_core *, lv_request *, uint64_t bytes);
void *lv_allocate(lv_core *, lv_request *, size_t bytes);
void lv_deallocate(lv_core *, void *, size_t bytes);
void *lv_workspace_allocate(lv_core *, lv_request *, size_t bytes);
void lv_workspace_release(lv_core *, lv_request *, void *, size_t bytes);
lv_pixels *lv_pixels_new(lv_core *, lv_request *, uint32_t width, uint32_t height);
bool lv_publish_info(lv_core *, lv_request *, lv_image_info *, const lv_file_version *);
bool lv_publish_tile(lv_core *, lv_request *, lv_pixels *, lv_rect_u32, uint64_t tile);
void lv_decode_image(lv_core *, lv_request *, lv_error_info *);
void lv_file_close(FILE *);
FILE *lv_file_open(const char *, lv_file_version *, uint32_t *system_code);
bool lv_file_unchanged(FILE *, const char *, const lv_file_version *);
void lv_pixels_drop_locked(lv_pixels *, bool outside);
void lv_event_drop_locked(lv_event *);
void lv_enqueue_locked(lv_core *, lv_event *, lv_request *, lv_event_type);
void lv_notify(lv_core *);
#endif
