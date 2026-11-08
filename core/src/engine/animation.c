#include <stdlib.h>
#include "../internal.h"

bool lv_animation_reserve(lv_core *core, lv_request *r, uint64_t bytes, lv_error_info *error)
{
    mtx_lock(&core->mutex);
    const bool fits = bytes <= core->config.animation.resident_budget_bytes - core->resident;
    mtx_unlock(&core->mutex);
    if (!fits || !lv_reserve(core, r, bytes))
    {
        *error =
            (lv_error_info){.code           = LV_ERROR_MEMORY_BUDGET,
                            .required_bytes = bytes,
                            .limit_bytes = fits ? core->config.cpu_budget_bytes : core->config.animation.resident_budget_bytes};
        return false;
    }
    mtx_lock(&core->mutex);
    r->reservation            -= bytes;
    r->animation->reservation += bytes;
    r->animation->resident    += bytes;
    core->resident            += bytes;
    mtx_unlock(&core->mutex);
    return true;
}

void *lv_animation_allocate(lv_core *core, lv_animation *s, size_t bytes)
{
    mtx_lock(&core->mutex);
    void *data = bytes <= s->reservation ? malloc(bytes) : nullptr;
    if (data)
    {
        s->reservation -= bytes;
        core->reserved -= bytes;
        core->used     += bytes;
    }
    mtx_unlock(&core->mutex);
    return data;
}
void lv_animation_deallocate(lv_core *core, lv_animation *s, void *data, size_t bytes)
{
    if (!data)
        return;
    mtx_lock(&core->mutex);
    free(data);
    core->used     -= bytes;
    s->resident    -= bytes;
    core->resident -= bytes;
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
}

void lv_animation_metadata(lv_core *core, lv_request *r, lv_event_type type, const lv_animation_info *info)
{
    mtx_lock(&core->mutex);
    r->deferred_info   = true;
    r->info->view.type = type;
    if (type == LV_EVENT_ANIMATION_END)
        r->info->view.as.animation_end.final_info = *info;
    else
        r->info->view.as.animation = *info;
    r->info->source = r->animation->source;
    if (r->info->source)
        ++r->info->source->refs;
    mtx_unlock(&core->mutex);
}

bool lv_animation_frame(lv_core *core, lv_request *r, const lv_animation_frame_info *info)
{
    mtx_lock(&core->mutex);
    if (lv_stopped(core, r))
    {
        mtx_unlock(&core->mutex);
        return false;
    }
    r->frame               = *info;
    r->info->view.as.frame = *info;
    lv_enqueue_locked(core, r->info, r, LV_EVENT_ANIMATION_FRAME_INFO);
    r->info = nullptr;
    mtx_unlock(&core->mutex);
    lv_notify(core);
    return true;
}

void lv_animation_commit_locked(lv_core *core, lv_request *r, lv_event_type outcome)
{
    lv_animation *s = r->animation;
    if (outcome == LV_EVENT_COMPLETE)
    {
        if (r->deferred_info)
        {
            const lv_event_type type = r->info->view.type;
            s->info = type == LV_EVENT_ANIMATION_END ? r->info->view.as.animation_end.final_info : r->info->view.as.animation;
            lv_enqueue_locked(core, r->info, r, type);
            r->info = nullptr;
        }
        s->state = r->animation_end ? LV_ANIM_END : LV_ANIM_READY;
    }
    else if (s->state != LV_ANIM_INVALID)
        s->state = !r->animation_action ? LV_ANIM_FAILED : LV_ANIM_NEEDS_RESET;
}

void lv_animation_decode(lv_core *core, lv_request *r, lv_error_info *error)
{
    lv_gif_animation_run(core, r, error);
}

// Returns true if cleanup made progress. Never waits for externally borrowed
// objects to let join finish; the session slot can outlive the cleaned decoder.
bool lv_animation_cleanup(lv_core *core)
{
    mtx_lock(&core->mutex);
    lv_animation *s = nullptr;
    for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
    {
        lv_animation *candidate = &core->animations[i];
        if (candidate->id && candidate->closing && !candidate->cleaned && !candidate->cleaning &&
            (!candidate->busy || candidate->busy->state == LV_TERMINAL))
        {
            s = candidate;
            break;
        }
    }
    if (!s)
    {
        mtx_unlock(&core->mutex);
        return false;
    }
    s->cleaning = true;
    mtx_unlock(&core->mutex);
    lv_gif_animation_destroy(core, s);
    mtx_lock(&core->mutex);
    if (s->source)
    {
        --s->source->refs;
        s->source = nullptr;
    }
    core->used -= s->path_bytes;
    free(s->path);
    s->path         = nullptr;
    s->path_bytes   = 0;
    core->reserved -= s->reservation;
    core->resident -= s->resident;
    s->reservation = s->resident = 0;
    s->cleaned                   = true;
    s->cleaning                  = false;
    if (!s->busy)
        *s = (lv_animation){0};
    core->resource_notify = true;
    cnd_broadcast(&core->condition);
    mtx_unlock(&core->mutex);
    return true;
}
