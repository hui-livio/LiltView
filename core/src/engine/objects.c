#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"

bool lv_stopped(lv_core *core, lv_request *request)
{
    (void)core;
    return atomic_load(&request->cancelled);
}

static bool plan_fits(lv_core *core, lv_request *request, uint64_t bytes)
{
    uint64_t fixed = core->base + core->resident;
    for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
        fixed += core->animations[i].path_bytes;
    bool impossible    = fixed > core->config.cpu_budget_bytes;
    uint64_t available = impossible ? 0 : core->config.cpu_budget_bytes - fixed;
    // A single worker cannot free another queued request's copied path while
    // blocked here. Treat those bytes as fixed instead of waiting on itself.
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
    {
        if (!core->requests[i].path)
            continue;
        const uint64_t bytes_in_path = core->requests[i].path_bytes;
        if (bytes_in_path > available)
            impossible = true;
        else
            available -= bytes_in_path;
    }
    if (request->workspace > available)
        impossible = true;
    else
        available -= request->workspace;
    if (request->reservation > available)
        impossible = true;
    else
        available -= request->reservation;
    return !impossible && bytes <= available;
}

bool lv_reserve(lv_core *core, lv_request *request, uint64_t bytes)
{
    mtx_lock(&core->mutex);
    while (!lv_stopped(core, request))
    {
        // New queued path copies can arrive during this wait. Recheck fixed
        // internal demand on every wake so the sole worker never waits on itself.
        if (!plan_fits(core, request, bytes))
        {
            mtx_unlock(&core->mutex);
            return false;
        }
        lv_pixels_trim_locked(core, bytes);
        if (bytes <= core->config.cpu_budget_bytes - core->used - core->reserved)
            break;
        cnd_wait(&core->condition, &core->mutex);
    }
    const bool ok = !lv_stopped(core, request);
    if (ok)
    {
        request->reservation += bytes;
        core->reserved       += bytes;
    }
    mtx_unlock(&core->mutex);
    return ok;
}

void *lv_allocate(lv_core *core, lv_request *request, size_t bytes)
{
    mtx_lock(&core->mutex);
    if ((uint64_t)bytes > request->reservation)
    {
        mtx_unlock(&core->mutex);
        return nullptr;
    }
    void *data = malloc(bytes);
    if (data)
    {
        core->reserved       -= bytes;
        request->reservation -= bytes;
        core->used           += bytes;
    }
    mtx_unlock(&core->mutex);
    return data;
}
void lv_deallocate(lv_core *core, void *data, size_t bytes)
{
    if (!data)
        return;
    mtx_lock(&core->mutex);
    free(data);
    core->used -= bytes;
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
}
void *lv_workspace_allocate(lv_core *core, lv_request *request, size_t bytes)
{
    void *data = lv_allocate(core, request, bytes);
    if (data)
    {
        mtx_lock(&core->mutex);
        request->workspace += bytes;
        mtx_unlock(&core->mutex);
    }
    return data;
}
void lv_workspace_release(lv_core *core, lv_request *request, void *data, size_t bytes)
{
    if (!data)
        return;
    mtx_lock(&core->mutex);
    request->workspace -= bytes;
    mtx_unlock(&core->mutex);
    lv_deallocate(core, data, bytes);
}

void lv_pixels_trim_locked(lv_core *core, uint64_t required)
{
    while (core->free_pixels && (!required || required > core->config.cpu_budget_bytes - core->used - core->reserved))
    {
        lv_pixels *pixels       = core->free_pixels;
        core->free_pixels       = pixels->next_free;
        core->free_pixel_bytes -= pixels->allocation;
        --core->free_pixel_count;
        core->used -= pixels->allocation;
        free(pixels);
    }
}

lv_pixels *lv_pixels_new(lv_core *core, lv_request *request, uint32_t width, uint32_t height)
{
    const size_t bytes = (size_t)width * height * 4, allocation = sizeof(lv_pixels) + bytes;
    mtx_lock(&core->mutex);
    if (allocation > request->reservation)
    {
        mtx_unlock(&core->mutex);
        return nullptr;
    }
    // Best fit with bounded internal fragmentation. A pinned/borrowed allocation
    // is never on this list and cannot be mutated by a subsequent decode.
    lv_pixels **best = nullptr;
    for (lv_pixels **link = &core->free_pixels; *link; link = &(*link)->next_free)
        if ((*link)->allocation >= allocation && (*link)->allocation - allocation <= allocation / 4 &&
            (!best || (*link)->allocation < (*best)->allocation))
            best = link;
    lv_pixels *pixels;
    if (best)
    {
        pixels                  = *best;
        *best                   = pixels->next_free;
        core->free_pixel_bytes -= pixels->allocation;
        --core->free_pixel_count;
    }
    else
    {
        pixels = malloc(allocation);
        if (!pixels)
        {
            mtx_unlock(&core->mutex);
            return nullptr;
        }
        pixels->allocation  = allocation;
        core->used         += allocation;
    }
    const uint64_t capacity  = pixels->allocation;
    core->reserved          -= allocation;
    request->reservation    -= allocation;
    *pixels = (lv_pixels){.core = core,
                          .view = {pixels->data, bytes, (uint64_t)width * 4, {width, height}, LV_PIXEL_RGBA8, LV_COLOR_UNKNOWN},
                          .refs = 1,
                          .allocation = capacity};
    ++core->live_pixels;
    mtx_unlock(&core->mutex);
    return pixels;
}

lv_pixels *lv_pixels_slice(lv_core *core, lv_request *r, lv_pixels *owner, lv_rect_u32 rect)
{
    lv_pixels *slice = lv_allocate(core, r, sizeof *slice);
    if (!slice)
        return nullptr;
    const uint64_t stride = owner->view.stride_bytes;
    *slice                = (lv_pixels){.core       = core,
                                        .refs       = 1,
                                        .allocation = sizeof *slice,
                                        .owner      = owner,
                                        .view       = {owner->view.data + (size_t)rect.y * stride + (size_t)rect.x * 4,
                                                       (uint64_t)(rect.height - 1) * stride + (uint64_t)rect.width * 4,
                                                       stride,
                                                       {rect.width, rect.height},
                                                       LV_PIXEL_RGBA8,
                                                       LV_COLOR_UNKNOWN}};
    mtx_lock(&core->mutex);
    ++owner->refs;
    ++core->live_pixels;
    mtx_unlock(&core->mutex);
    return slice;
}

void lv_pixels_drop_locked(lv_pixels *pixels, bool outside)
{
    lv_core *core = pixels->core;
    if (outside && --pixels->outside_refs == 0)
        core->pinned_bytes -= lv_pixels_payload(pixels);
    if (--pixels->refs == 0)
    {
        --core->live_pixels;
        lv_pixels *owner     = pixels->owner;
        const uint64_t limit = core->config.cpu_budget_bytes / 16 < UINT64_C(8) * 1024 * 1024 ?
            core->config.cpu_budget_bytes / 16 :
            UINT64_C(8) * 1024 * 1024;
        if (!owner && !core->closing && core->free_pixel_count < 128 && pixels->allocation <= limit - core->free_pixel_bytes)
        {
            pixels->next_free       = core->free_pixels;
            core->free_pixels       = pixels;
            core->free_pixel_bytes += pixels->allocation;
            ++core->free_pixel_count;
        }
        else
        {
            core->used -= pixels->allocation;
            free(pixels);
        }
        if (owner)
            lv_pixels_drop_locked(owner, false);
    }
    cnd_broadcast(&core->condition);
    core->resource_notify = true;
}

const lv_pixel_view *LV_CALL lv_pixels_get_view(const lv_pixels *pixels)
{
    return pixels ? &pixels->view : nullptr;
}
void LV_CALL lv_pixels_retain(lv_pixels *pixels)
{
    if (!pixels)
        return;
    lv_core *core = pixels->core;
    mtx_lock(&core->mutex);
    ++pixels->refs;
    ++core->pixel_refs;
    if (pixels->outside_refs++ == 0)
        core->pinned_bytes += lv_pixels_payload(pixels);
    mtx_unlock(&core->mutex);
}
void LV_CALL lv_pixels_release(lv_pixels *pixels)
{
    if (!pixels)
        return;
    lv_core *core = pixels->core;
    mtx_lock(&core->mutex);
    --core->pixel_refs;
    lv_pixels_drop_locked(pixels, true);
    mtx_unlock(&core->mutex);
}
const lv_event_view *LV_CALL lv_event_get_view(const lv_event *event)
{
    return event ? &event->view : nullptr;
}

void lv_event_drop_locked(lv_event *event)
{
    lv_core *core = event->core;
    if (event->source)
    {
        --event->source->refs;
        event->source = nullptr;
    }
    if (lv_event_pixels(event))
        lv_pixels_drop_locked(lv_event_pixels(event), event->borrowed);
    if (event->borrowed)
        --core->borrowed;
    if (event->terminal_request)
    {
        lv_animation *s = event->terminal_request->animation;
        if (s)
        {
            s->busy = nullptr;
            if (s->closing && s->cleaned)
                *s = (lv_animation){0};
        }
        event->terminal_request->state = LV_FREE;
        --core->accepted;
    }
    if (event->pooled)
        *event = (lv_event){.core = core, .pooled = true, .occupied = false};
    else
    {
        core->used -= sizeof(lv_event);
        free(event);
    }
    cnd_broadcast(&core->condition);
    core->resource_notify = true;
}
void LV_CALL lv_event_release(lv_event *event)
{
    if (!event)
        return;
    lv_core *core = event->core;
    mtx_lock(&core->mutex);
    lv_event_drop_locked(event);
    mtx_unlock(&core->mutex);
}

void lv_enqueue_locked(lv_core *core, lv_event *event, lv_request *request, lv_event_type type)
{
    event->view.type       = type;
    event->view.request_id = request->id;
    event->view.user_tag   = request->desc.user_tag;
    event->view.sequence   = ++request->sequence;
    if (core->tail)
        core->tail->next = event;
    else
        core->head = event;
    core->tail = event;
}
void lv_notify(lv_core *core)
{
    if (core->config.wakeup)
        core->config.wakeup(core->config.wakeup_userdata);
}

lv_source *lv_source_for_locked(lv_core *core, const lv_file_version *version)
{
    lv_source *empty = nullptr;
    for (uint32_t i = 0; i < core->control_count; ++i)
    {
        lv_source *s = &core->sources[i];
        if (s->refs && !memcmp(&s->version, version, sizeof *version))
        {
            ++s->refs;
            return s;
        }
        if (!s->refs)
            empty = s;
    }
    if (!empty || !core->next_revision)
        return nullptr;
    *empty = (lv_source){.version = *version, .revision = core->next_revision++, .refs = 1};
    return empty;
}
bool lv_publish_info(lv_core *core, lv_request *request, lv_image_info *info, const lv_file_version *version)
{
    mtx_lock(&core->mutex);
    if (lv_stopped(core, request))
    {
        mtx_unlock(&core->mutex);
        return false;
    }
    lv_source *source = lv_source_for_locked(core, version);
    if (!source)
    {
        mtx_unlock(&core->mutex);
        return false;
    }
    info->source_revision = source->revision;
    request->source       = source;
    ++source->refs; // The request and the INFO independently pin this identity.
    request->info->source        = source;
    request->info->view.as.image = *info;
    lv_enqueue_locked(core, request->info, request, LV_EVENT_IMAGE_INFO);
    request->info = nullptr;
    mtx_unlock(&core->mutex);
    lv_notify(core);
    return true;
}
bool lv_publish_tile(lv_core *core, lv_request *request, lv_pixels *pixels, lv_rect_u32 rect, uint64_t tile)
{
    lv_event *event = lv_allocate(core, request, sizeof(lv_event));
    if (!event)
        return false;
    *event = (lv_event){.core = core};
    mtx_lock(&core->mutex);
    while (!lv_stopped(core, request) && lv_pixels_payload(pixels) > core->config.queue_pixel_bytes - core->queued_bytes)
        cnd_wait(&core->condition, &core->mutex);
    if (lv_stopped(core, request))
    {
        core->used -= sizeof(lv_event);
        free(event);
        mtx_unlock(&core->mutex);
        return false;
    }
    const lv_region_info region = {tile,
                                   1,
                                   rect,
                                   LV_REGION_FINAL,
                                   request->desc.target.kind == LV_TARGET_NATIVE ? LV_PRESENT_IMMEDIATE : LV_PRESENT_AT_COMPLETE,
                                   pixels};
    const bool animated         = request->animation != nullptr;
    if (animated)
        event->view.as.frame_region = (lv_animation_region_info){request->frame.key, region};
    else
        event->view.as.region = region;
    core->queued_bytes += lv_pixels_payload(pixels);
    request->partial    = true;
    ++request->final_tiles;
    lv_enqueue_locked(core, event, request, animated ? LV_EVENT_ANIMATION_REGION_READY : LV_EVENT_REGION_READY);
    mtx_unlock(&core->mutex);
    lv_notify(core);
    return true;
}

lv_pixels *lv_event_pixels(const lv_event *event)
{
    if (event->view.type == LV_EVENT_REGION_READY)
        return event->view.as.region.pixels;
    if (event->view.type == LV_EVENT_ANIMATION_REGION_READY)
        return event->view.as.frame_region.region.pixels;
    return nullptr;
}
