#include "../internal.h"

static lv_animation *find_animation(lv_core *core, lv_animation_id id)
{
    for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
        if (core->animations[i].id == id)
            return &core->animations[i];
    return nullptr;
}

lv_status LV_CALL lv_core_open_animation(lv_core *core, const lv_animation_open_desc *desc, lv_animation_id *out_session,
                                         lv_request_id *out_request)
{
    if (out_session)
        *out_session = 0;
    if (out_request)
        *out_request = 0;
    if (!core || !desc || !out_session || !out_request || out_session == out_request)
        return LV_INVALID_ARGUMENT;
    if (desc->struct_size != sizeof *desc)
        return LV_ABI_MISMATCH;
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path     = desc->path;
    request.target   = desc->target;
    request.priority = desc->priority;
    request.user_tag = desc->user_tag;
    if (request.priority > LV_PRIORITY_VISIBLE_DETAIL)
        return LV_INVALID_ARGUMENT;
    lv_status status = lv_validate_request(&request);
    if (status != LV_OK)
        return status;
    mtx_lock(&core->mutex);
    if (core->closing)
        status = LV_CLOSING;
    else if (!core->config.animation.max_sessions)
        status = LV_UNSUPPORTED;
    else if (!core->next_animation)
        status = LV_LIMIT_REACHED;
    else
    {
        lv_animation *s = nullptr;
        for (uint32_t i = 0; i < core->config.animation.max_sessions; ++i)
            if (!core->animations[i].id)
            {
                s = &core->animations[i];
                break;
            }
        if (!s)
            status = LV_BUSY;
        else
        {
            *s     = (lv_animation){.id = core->next_animation, .target = desc->target, .state = LV_ANIM_OPENING};
            status = lv_accept_locked(core, &request, s, 0, out_request);
            if (status == LV_OK)
            {
                *out_session = core->next_animation++;
            }
            else
                *s = (lv_animation){0};
        }
    }
    mtx_unlock(&core->mutex);
    return status;
}

lv_status LV_CALL lv_core_submit_animation(lv_core *core, const lv_animation_request_desc *desc, lv_request_id *out)
{
    if (!out)
        return LV_INVALID_ARGUMENT;
    *out = 0;
    if (!core || !desc)
        return LV_INVALID_ARGUMENT;
    if (desc->struct_size != sizeof *desc)
        return LV_ABI_MISMATCH;
    if (!desc->animation_id || desc->reserved || desc->priority > LV_PRIORITY_VISIBLE_DETAIL ||
        (desc->action != LV_ANIMATION_NEXT && desc->action != LV_ANIMATION_RESET))
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    lv_animation *s  = find_animation(core, desc->animation_id);
    lv_status status = LV_OK;
    if (core->closing)
        status = LV_CLOSING;
    else if (!s || s->closing)
        status = LV_NOT_FOUND;
    else if (s->busy)
        status = LV_BUSY;
    else if (s->state == LV_ANIM_FAILED || s->state == LV_ANIM_INVALID ||
             (desc->action == LV_ANIMATION_NEXT && s->state != LV_ANIM_READY))
        status = LV_INVALID_STATE;
    else if (desc->action == LV_ANIMATION_RESET && s->info.epoch == UINT64_MAX)
        status = LV_LIMIT_REACHED;
    else
    {
        lv_request_desc request;
        lv_request_desc_init(&request);
        request.path     = (lv_path){LV_PATH_POSIX_BYTES, s->path, s->path_bytes - 1};
        request.target   = s->target;
        request.priority = desc->priority;
        request.user_tag = desc->user_tag;
        status           = lv_accept_locked(core, &request, s, desc->action, out);
    }
    mtx_unlock(&core->mutex);
    return status;
}

lv_status LV_CALL lv_core_close_animation(lv_core *core, lv_animation_id id)
{
    if (!core || !id)
        return LV_INVALID_ARGUMENT;
    mtx_lock(&core->mutex);
    lv_animation *s  = find_animation(core, id);
    lv_status status = core->closing ? LV_CLOSING : s ? LV_OK : LV_NOT_FOUND;
    if (status == LV_OK)
    {
        s->closing = true;
        if (s->busy && s->busy->state != LV_TERMINAL)
        {
            atomic_store(&s->busy->cancelled, true);
            lv_discard_tiles_locked(core, s->busy->id);
        }
        cnd_broadcast(&core->condition);
    }
    mtx_unlock(&core->mutex);
    return status;
}
