#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "../internal.h"

static bool config_valid(const lv_core_config *c)
{
    const uint64_t tile = (uint64_t)c->tile_edge * c->tile_edge * 4;
    return c->cpu_budget_bytes && c->queue_pixel_bytes >= tile && c->queue_pixel_bytes <= c->cpu_budget_bytes &&
        c->cache_budget_bytes <= c->cpu_budget_bytes && c->max_requests && c->max_requests <= 1024 && c->tile_edge >= 16 &&
        c->tile_edge <= 1024 && !(c->tile_edge & (c->tile_edge - 1)) && c->foreground_workers &&
        c->small_pixel_bytes <= c->large_pixel_bytes && c->log_level <= LV_LOG_OFF && c->animation.max_sessions <= 64 &&
        !c->animation.reserved && c->animation.resident_budget_bytes <= c->cpu_budget_bytes;
}

lv_status LV_CALL lv_core_create(const lv_core_config *config, lv_core **out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = nullptr;
    if (!config)
        return LV_INVALID_ARGUMENT;
    if (config->abi_version != LV_CORE_ABI_VERSION || config->struct_size != sizeof *config)
        return LV_ABI_MISMATCH;
    if (!config_valid(config))
        return LV_INVALID_ARGUMENT;
    if (config->foreground_workers != 1 || config->prefetch_workers > 1 || config->disk_cache_budget_bytes)
        return LV_UNSUPPORTED;
    const uint32_t events = config->max_requests * 2 + config->animation.max_sessions * 2 + 1;
    const uint64_t base   = sizeof(lv_core) + (uint64_t)config->max_requests * sizeof(lv_request) +
        (uint64_t)events * (sizeof(lv_event) + sizeof(lv_source)) +
        (uint64_t)config->animation.max_sessions * sizeof(lv_animation);
    if (base + (uint64_t)config->tile_edge * config->tile_edge * 4 + sizeof(lv_pixels) + sizeof(lv_event) >
        config->cpu_budget_bytes)
        return LV_INVALID_ARGUMENT;
    lv_core *core = calloc(1, sizeof *core);
    if (!core)
        return LV_OUT_OF_MEMORY;
    core->config         = *config;
    core->control_count  = events;
    core->used           = base;
    core->base           = base;
    core->next_animation = 1;
    if (!core->config.animation.resident_budget_bytes)
        core->config.animation.resident_budget_bytes = config->cpu_budget_bytes / 2 < UINT64_C(128) * 1024 * 1024 ?
            config->cpu_budget_bytes / 2 :
            UINT64_C(128) * 1024 * 1024;
    core->next_id       = 1;
    core->next_revision = 1;
    core->requests      = calloc(config->max_requests, sizeof(lv_request));
    core->controls      = calloc(events, sizeof(lv_event));
    core->sources       = calloc(events, sizeof(lv_source));
    core->animations    = config->animation.max_sessions ? calloc(config->animation.max_sessions, sizeof(lv_animation)) : nullptr;
    if (!core->requests || !core->controls || !core->sources || (config->animation.max_sessions && !core->animations))
        goto allocation_failed;
    if (mtx_init(&core->mutex, mtx_plain) != thrd_success)
        goto allocation_failed;
    if (cnd_init(&core->condition) != thrd_success)
    {
        mtx_destroy(&core->mutex);
        goto allocation_failed;
    }
    for (uint32_t i = 0; i < events; ++i)
        core->controls[i] = (lv_event){.core = core, .pooled = true};
    if (thrd_create(&core->worker, lv_worker_main, core) != thrd_success)
    {
        cnd_destroy(&core->condition);
        mtx_destroy(&core->mutex);
        goto allocation_failed;
    }
    *out = core;
    return LV_OK;
allocation_failed:
    free(core->requests);
    free(core->controls);
    free(core->sources);
    free(core->animations);
    free(core);
    return LV_OUT_OF_MEMORY;
}

static bool valid_utf8(const unsigned char *p, size_t length)
{
    for (size_t i = 0; i < length;)
    {
        unsigned char c = p[i++];
        if (c < 0x80)
            continue;
        unsigned count;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf)
        {
            count   = 1;
            value   = c & 31u;
            minimum = 0x80;
        }
        else if (c >= 0xe0 && c <= 0xef)
        {
            count   = 2;
            value   = c & 15u;
            minimum = 0x800;
        }
        else if (c >= 0xf0 && c <= 0xf4)
        {
            count   = 3;
            value   = c & 7u;
            minimum = 0x10000;
        }
        else
            return false;
        if (count > length - i)
            return false;
        while (count--)
        {
            c = p[i++];
            if ((c & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (c & 63u);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
lv_status lv_validate_request(const lv_request_desc *r)
{
    if (r->struct_size != sizeof *r)
        return LV_ABI_MISMATCH;
    if (r->kind != LV_REQUEST_DISPLAY && r->kind != LV_REQUEST_PREFETCH)
        return LV_INVALID_ARGUMENT;
    if (r->priority > LV_PRIORITY_PREFETCH || (r->kind == LV_REQUEST_DISPLAY && r->priority == LV_PRIORITY_PREFETCH) ||
        (r->kind == LV_REQUEST_PREFETCH && r->priority != LV_PRIORITY_PREFETCH))
        return LV_INVALID_ARGUMENT;
    if (r->target.kind != LV_TARGET_FIT && r->target.kind != LV_TARGET_REGION && r->target.kind != LV_TARGET_NATIVE)
        return LV_INVALID_ARGUMENT;
    if (r->target.kind == LV_TARGET_FIT && (!r->target.as.fit_bounds.width || !r->target.as.fit_bounds.height))
        return LV_INVALID_ARGUMENT;
    if (r->target.kind == LV_TARGET_REGION)
    {
        lv_rect_u32 rect = r->target.as.region.rect;
        lv_size_u32 size = r->target.as.region.canvas;
        if (!rect.width || !rect.height || rect.x > size.width || rect.y > size.height || rect.width > size.width - rect.x ||
            rect.height > size.height - rect.y)
            return LV_INVALID_ARGUMENT;
    }
    if (r->path.encoding == LV_PATH_WINDOWS_UTF16)
        return LV_UNSUPPORTED;
    if (r->path.encoding != LV_PATH_UTF8 && r->path.encoding != LV_PATH_POSIX_BYTES)
        return LV_INVALID_ARGUMENT;
    if (!r->path.data || !r->path.byte_count)
        return LV_INVALID_ARGUMENT;
    if (r->path.byte_count == SIZE_MAX)
        return LV_LIMIT_REACHED;
    const char *p = r->path.data;
    if (p[0] != '/' || memchr(p, 0, r->path.byte_count))
        return LV_INVALID_ARGUMENT;
    if (r->path.encoding == LV_PATH_UTF8 && !valid_utf8(r->path.data, r->path.byte_count))
        return LV_INVALID_ARGUMENT;
    return LV_OK;
}

lv_status LV_CALL lv_core_submit(lv_core *core, const lv_request_desc *desc, lv_request_id *out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = 0;
    if (!core || !desc)
        return LV_INVALID_ARGUMENT;
    lv_status status = lv_validate_request(desc);
    if (status != LV_OK)
        return status;
    mtx_lock(&core->mutex);
    if (core->closing)
    {
        mtx_unlock(&core->mutex);
        return LV_CLOSING;
    }
    if (desc->kind == LV_REQUEST_PREFETCH && (!core->config.prefetch_workers || !core->config.cache_budget_bytes))
    {
        mtx_unlock(&core->mutex);
        return LV_UNSUPPORTED;
    }
    status = lv_accept_locked(core, desc, nullptr, 0, out);
    mtx_unlock(&core->mutex);
    return status;
}

lv_status lv_accept_locked(lv_core *core, const lv_request_desc *desc, lv_animation *animation, uint32_t action,
                           lv_request_id *out)
{
    lv_request *r       = nullptr;
    lv_event *events[2] = {0};
    unsigned found      = 0;
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
        if (core->requests[i].state == LV_FREE)
        {
            r = &core->requests[i];
            break;
        }
    for (uint32_t i = 0; i < core->control_count && found < 2; ++i)
        if (!core->controls[i].occupied)
            events[found++] = &core->controls[i];
    if (!r || found < 2)
    {
        return LV_BUSY;
    }
    if (!core->next_id)
    {
        return LV_LIMIT_REACHED;
    }
    const bool copy     = !animation || action == 0;
    const size_t length = copy ? desc->path.byte_count + 1 : 0;
    const uint64_t base = core->base;
    if ((uint64_t)length > core->config.cpu_budget_bytes - base)
    {
        return LV_LIMIT_REACHED;
    }
    lv_cache_trim_locked(core, nullptr, length);
    if ((uint64_t)length > core->config.cpu_budget_bytes - core->used - core->reserved)
    {
        return LV_BUSY;
    }
    char *path = copy ? malloc(length) : animation->path;
    if (!path)
        return LV_OUT_OF_MEMORY;
    if (copy)
    {
        memcpy(path, desc->path.data, length - 1);
        path[length - 1] = 0;
    }
    *r = (lv_request){.id               = core->next_id++,
                      .desc             = *desc,
                      .path             = path,
                      .path_bytes       = animation ? 0 : length,
                      .animation        = animation,
                      .animation_action = action,
                      .state            = LV_PENDING,
                      .info             = events[0],
                      .terminal         = events[1],
                      .started_ns       = lv_clock_ns()};
    if (animation)
    {
        animation->busy = r;
        if (!action)
        {
            animation->path       = path;
            animation->path_bytes = length;
        }
    }
    r->desc.path.data = path;
    atomic_init(&r->cancelled, false);
    events[0]->occupied  = true;
    events[1]->occupied  = true;
    core->used          += length;
    ++core->accepted;
    *out = r->id;
    if (desc->kind == LV_REQUEST_DISPLAY)
        for (uint32_t i = 0; i < core->config.max_requests; ++i)
        {
            lv_request *background = &core->requests[i];
            if (background->state == LV_RUNNING && background->desc.kind == LV_REQUEST_PREFETCH)
                atomic_store(&background->cancelled, true);
        }
    cnd_broadcast(&core->condition);
    return LV_OK;
}

lv_status LV_CALL lv_core_cancel(lv_core *core, lv_request_id id)
{
    if (!core)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    if (core->closing)
    {
        mtx_unlock(&core->mutex);
        return LV_CLOSING;
    }
    lv_status result = LV_NOT_FOUND;
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
    {
        lv_request *r = &core->requests[i];
        if (r->state != LV_FREE && r->id == id)
        {
            result = LV_OK;
            if (r->state != LV_TERMINAL)
            {
                atomic_store(&r->cancelled, true);
                lv_discard_tiles_locked(core, id);
            }
            break;
        }
    }
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
    return result;
}
lv_status LV_CALL lv_core_poll_event(lv_core *core, lv_event **out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = nullptr;
    if (!core)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    if (core->closing)
    {
        mtx_unlock(&core->mutex);
        return LV_CLOSING;
    }
    lv_event *event = core->head;
    if (!event)
    {
        mtx_unlock(&core->mutex);
        return LV_EMPTY;
    }
    core->head = event->next;
    if (!core->head)
        core->tail = nullptr;
    event->next     = nullptr;
    event->borrowed = true;
    ++core->borrowed;
    if (lv_event_pixels(event))
    {
        lv_pixels *p        = lv_event_pixels(event);
        core->queued_bytes -= lv_pixels_payload(p);
        if (p->outside_refs++ == 0)
            core->pinned_bytes += lv_pixels_payload(p);
    }
    *out = event;
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
    return LV_OK;
}
lv_status LV_CALL lv_core_query_format(lv_core *core, lv_format format, lv_format_support *out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = (lv_format_support){0};
    if (!core)
        return LV_INVALID_ARGUMENT;
    if (core->closing)
        return LV_CLOSING;
    if (format < LV_FORMAT_JPEG || format > LV_FORMAT_TIFF)
        return LV_INVALID_ARGUMENT;
    const lv_capabilities capabilities = format == LV_FORMAT_SVG ? LV_CAP_VECTOR_RASTERIZE :
        (format == LV_FORMAT_JPEG || format == LV_FORMAT_WEBP)   ? LV_CAP_SCALED_DECODE :
                                                                   0;
    const bool region =
        format == LV_FORMAT_JPEG || format == LV_FORMAT_PNG || format == LV_FORMAT_BMP || format == LV_FORMAT_TIFF;
    *out = (lv_format_support){
        1, capabilities | (format == LV_FORMAT_GIF ? LV_CAP_ANIMATION : 0) | (region ? LV_CAP_REGION_DECODE : 0)};
    return LV_OK;
}
lv_status LV_CALL lv_core_get_stats(lv_core *core, lv_core_stats *out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = (lv_core_stats){0};
    if (!core)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    *out = (lv_core_stats){.accounted_cpu_bytes = core->used,
                           .cache_bytes         = core->cache_bytes,
                           .queued_pixel_bytes  = core->queued_bytes,
                           .pinned_pixel_bytes  = core->pinned_bytes,
                           .accepted_requests   = core->accepted,
                           .borrowed_events     = core->borrowed,
                           .external_pixel_refs = core->pixel_refs};
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
    {
        if (core->requests[i].state == LV_RUNNING)
            ++out->running_requests;
        if (core->requests[i].state == LV_PENDING)
            ++out->queued_requests;
    }
    mtx_unlock(&core->mutex);
    return LV_OK;
}
lv_status LV_CALL lv_core_begin_shutdown(lv_core *core)
{
    if (!core)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    core->closing = true;
    lv_cache_clear_locked(core);
    lv_pixels_trim_locked(core, 0);
    for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
        if (core->animations[i].id)
            core->animations[i].closing = true;
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
        if (core->requests[i].state != LV_FREE)
            atomic_store(&core->requests[i].cancelled, true);
    while (core->head)
    {
        lv_event *event = core->head;
        core->head      = event->next;
        if (lv_event_pixels(event))
            core->queued_bytes -= lv_pixels_payload(lv_event_pixels(event));
        lv_event_drop_locked(event);
    }
    core->tail = nullptr;
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
    return LV_OK;
}
lv_status LV_CALL lv_core_join(lv_core *core, uint32_t timeout)
{
    if (!core)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    if (!core->closing)
    {
        mtx_unlock(&core->mutex);
        return LV_INVALID_STATE;
    }
    if (core->joined)
    {
        mtx_unlock(&core->mutex);
        return LV_OK;
    }
    struct timespec deadline;
    timespec_get(&deadline, TIME_UTC);
    deadline.tv_sec  += timeout / 1000;
    deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    while (!core->exited)
    {
        if (!timeout || cnd_timedwait(&core->condition, &core->mutex, &deadline) == thrd_timedout)
        {
            mtx_unlock(&core->mutex);
            return LV_TIMEOUT;
        }
    }
    mtx_unlock(&core->mutex);
    thrd_join(core->worker, nullptr);
    core->joined = true;
    return LV_OK;
}
lv_status LV_CALL lv_core_destroy(lv_core **inout)
{
    if (!inout)
        return LV_INVALID_ARGUMENT;
    lv_core *core = *inout;
    if (!core)
        return LV_OK;
    mtx_lock(&core->mutex);
    const bool busy = !core->joined || core->borrowed || core->live_pixels;
    mtx_unlock(&core->mutex);
    if (busy)
        return LV_BUSY;
    cnd_destroy(&core->condition);
    mtx_destroy(&core->mutex);
    free(core->requests);
    free(core->controls);
    free(core->sources);
    free(core->animations);
    free(core);
    *inout = nullptr;
    return LV_OK;
}
