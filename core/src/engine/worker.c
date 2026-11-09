#include <stdlib.h>
#include "../internal.h"

void lv_discard_tiles_locked(lv_core *core, lv_request_id id)
{
    lv_event **link = &core->head;
    core->tail      = nullptr;
    while (*link)
    {
        lv_event *event = *link;
        if (lv_event_pixels(event) && (!id || event->view.request_id == id))
        {
            *link               = event->next;
            core->queued_bytes -= lv_pixels_payload(lv_event_pixels(event));
            lv_event_drop_locked(event);
        }
        else
        {
            core->tail = event;
            link       = &event->next;
        }
    }
}

static void finish_request(lv_core *core, lv_request *r, lv_error_info error)
{
    mtx_lock(&core->mutex);
    if (r->cache_failure)
        error.code = r->cache_failure;
    core->reserved -= r->reservation;
    r->reservation  = 0;
    core->used     -= r->path_bytes;
    if (!r->animation)
        free(r->path);
    r->path = nullptr;
    if (r->source)
    {
        --r->source->refs;
        r->source = nullptr;
    }
    lv_event_type outcome = atomic_load(&r->cancelled) ? LV_EVENT_CANCELLED : error.code ? LV_EVENT_ERROR : LV_EVENT_COMPLETE;
    const lv_error_code cache_error = lv_cache_finish_locked(core, r, outcome == LV_EVENT_COMPLETE);
    if (cache_error && outcome == LV_EVENT_COMPLETE)
    {
        error.code = cache_error;
        outcome    = LV_EVENT_ERROR;
    }
    if (r->animation)
    {
        if (outcome == LV_EVENT_ERROR && error.code == LV_ERROR_SOURCE_CHANGED)
            r->animation->state = LV_ANIM_INVALID;
        lv_animation_commit_locked(core, r, outcome);
    }
    if (r->info)
    {
        lv_event_drop_locked(r->info);
        r->info = nullptr;
    }
    lv_event *terminal = r->terminal;
    if (core->closing)
    {
        terminal->terminal_request = r;
        lv_event_drop_locked(terminal);
    }
    else
    {
        lv_event_type type = outcome;
        if (type == LV_EVENT_ERROR)
        {
            error.has_partial_output = r->partial;
            terminal->view.as.error  = error;
        }
        if (type == LV_EVENT_COMPLETE)
            terminal->view.as.completion = (lv_completion_info){r->final_tiles, lv_clock_ns() - r->started_ns, r->bytes_read};
        r->state                   = LV_TERMINAL;
        terminal->terminal_request = r;
        lv_enqueue_locked(core, terminal, r, type);
    }
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
    lv_notify(core);
}

static lv_request *next_request(lv_core *core)
{
    lv_request *best = nullptr;
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
    {
        lv_request *r = &core->requests[i];
        if (r->state == LV_PENDING &&
            (!best || r->desc.priority < best->desc.priority || (r->desc.priority == best->desc.priority && r->id < best->id)))
            best = r;
    }
    return best;
}

int lv_worker_main(void *data)
{
    lv_core *core = data;
    for (;;)
    {
        // Cleanup is a worker operation, prioritized before new budget admission.
        if (lv_animation_cleanup(core))
            continue;
        mtx_lock(&core->mutex);
        lv_request *r;
        if (core->resource_notify && !core->closing)
        {
            core->resource_notify = false;
            mtx_unlock(&core->mutex);
            lv_notify(core);
            continue;
        }
        if (!(r = next_request(core)) && !core->closing)
        {
            cnd_wait(&core->condition, &core->mutex);
            mtx_unlock(&core->mutex);
            continue;
        }
        if (!r)
        {
            // Shutdown can start after the cleanup pass but before this lock.
            // Recheck under the same mutex as begin_shutdown before exiting.
            bool cleanup_pending = false;
            for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
                if (core->animations[i].id && core->animations[i].closing && !core->animations[i].cleaned)
                    cleanup_pending = true;
            if (cleanup_pending)
            {
                mtx_unlock(&core->mutex);
                continue;
            }
            mtx_unlock(&core->mutex);
            lv_notify(core);
            mtx_lock(&core->mutex);
            core->exited = true;
            cnd_broadcast(&core->condition);
            mtx_unlock(&core->mutex);
            return 0;
        }
        r->state = LV_RUNNING;
        mtx_unlock(&core->mutex);
        lv_error_info error = {0};
        if (!lv_stopped(core, r))
        {
            if (r->animation)
                lv_animation_decode(core, r, &error);
            else
                lv_decode_image(core, r, &error);
        }
        finish_request(core, r, error);
    }
}
