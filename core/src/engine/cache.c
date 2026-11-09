#include <stdlib.h>
#include <string.h>
#include "../internal.h"

typedef struct lv_cached_tile
{
    lv_pixels *pixels;
    lv_rect_u32 rect;
    uint64_t id;
} lv_cached_tile;

struct lv_cache_entry
{
    lv_cache_entry *next;
    lv_source *source;
    lv_file_version version;
    lv_target target;
    lv_image_info info;
    uint64_t bytes, metadata;
    size_t count, captured;
    lv_pixels *last_owner;
    bool in_use;
    lv_cached_tile tiles[];
};

static bool same_target(const lv_target *a, const lv_target *b)
{
    if (a->kind != b->kind)
        return false;
    if (a->kind == LV_TARGET_NATIVE)
        return true;
    if (a->kind == LV_TARGET_FIT)
        return a->as.fit_bounds.width == b->as.fit_bounds.width && a->as.fit_bounds.height == b->as.fit_bounds.height;
    return a->as.region.canvas.width == b->as.region.canvas.width && a->as.region.canvas.height == b->as.region.canvas.height &&
        a->as.region.rect.x == b->as.region.rect.x && a->as.region.rect.y == b->as.region.rect.y &&
        a->as.region.rect.width == b->as.region.rect.width && a->as.region.rect.height == b->as.region.rect.height;
}

static void drop_entry(lv_core *core, lv_cache_entry *entry)
{
    for (size_t i = 0; i < entry->captured; ++i)
        lv_pixels_drop_locked(entry->tiles[i].pixels, false);
    --entry->source->refs;
    core->used -= entry->metadata;
    free(entry);
}

static void remove_entry(lv_core *core, lv_cache_entry **link)
{
    lv_cache_entry *entry  = *link;
    *link                  = entry->next;
    core->cache_bytes     -= entry->bytes;
    --core->cache_count;
    drop_entry(core, entry);
}

bool lv_cache_evict_locked(lv_core *core)
{
    lv_cache_entry **oldest = nullptr;
    for (lv_cache_entry **link = &core->cache; *link; link = &(*link)->next)
        if (!(*link)->in_use)
            oldest = link;
    if (!oldest)
        return false;
    remove_entry(core, oldest);
    return true;
}

void lv_cache_trim_locked(lv_core *core, lv_request *request, uint64_t required)
{
    lv_pixels_trim_locked(core, required);
    while (required > core->config.cpu_budget_bytes - core->used - core->reserved && lv_cache_evict_locked(core))
        lv_pixels_trim_locked(core, required);
    if (request && request->capture && required > core->config.cpu_budget_bytes - core->used - core->reserved)
    {
        // Retaining prior row bands must not turn streaming into a self-wait.
        drop_entry(core, request->capture);
        request->capture = nullptr;
        if (request->desc.kind == LV_REQUEST_PREFETCH)
            request->cache_failure = LV_ERROR_MEMORY_BUDGET;
        lv_pixels_trim_locked(core, required);
    }
}

void lv_cache_clear_locked(lv_core *core)
{
    lv_cache_entry **link = &core->cache;
    while (*link)
        if ((*link)->in_use)
            link = &(*link)->next;
        else
            remove_entry(core, link);
    for (uint32_t i = 0; i < core->config.max_requests; ++i)
        if (core->requests[i].capture)
        {
            drop_entry(core, core->requests[i].capture);
            core->requests[i].capture = nullptr;
        }
}

bool lv_cache_begin_locked(lv_core *core, lv_request *r, const lv_image_info *info, const lv_file_version *version,
                           lv_source *source)
{
    if (r->animation || r->replay || !core->config.cache_budget_bytes)
        return true;
    const uint64_t edge    = info->tile_edge;
    const lv_rect_u32 rect = info->region;
    const uint64_t columns = ((uint64_t)rect.x + rect.width - 1) / edge - rect.x / edge + 1;
    const uint64_t count   = columns * (((uint64_t)rect.y + rect.height - 1) / edge - rect.y / edge + 1);
    const uint64_t pixels  = (uint64_t)rect.width * rect.height * 4;
    if (count > (SIZE_MAX - sizeof(lv_cache_entry)) / sizeof(lv_cached_tile))
        goto unavailable;
    const uint64_t metadata = sizeof(lv_cache_entry) + count * sizeof(lv_cached_tile);
    if (pixels > UINT64_MAX - metadata || count > (UINT64_MAX - metadata - pixels) / sizeof(lv_pixels))
        goto unavailable;
    uint64_t estimate = metadata + count * sizeof(lv_pixels) + pixels;
    if (!(info->capabilities & LV_CAP_REGION_DECODE))
    {
        if (estimate > UINT64_MAX - sizeof(lv_pixels))
            goto unavailable;
        estimate += sizeof(lv_pixels); // Contiguous backing plus slice descriptors.
    }
    if (estimate > core->config.cache_budget_bytes)
        goto unavailable;
    lv_cache_trim_locked(core, nullptr, metadata);
    if (metadata > core->config.cpu_budget_bytes - core->used - core->reserved)
        goto unavailable;
    lv_cache_entry *entry = calloc(1, (size_t)metadata);
    if (!entry)
    {
        if (r->desc.kind == LV_REQUEST_PREFETCH)
            r->cache_failure = LV_ERROR_OUT_OF_MEMORY;
        return r->desc.kind != LV_REQUEST_PREFETCH;
    }
    *entry = (lv_cache_entry){.source   = source,
                              .version  = *version,
                              .target   = r->desc.target,
                              .info     = *info,
                              .bytes    = metadata,
                              .metadata = metadata,
                              .count    = (size_t)count};
    ++source->refs;
    core->used += metadata;
    r->capture  = entry;
    return true;
unavailable:
    if (r->desc.kind == LV_REQUEST_PREFETCH)
    {
        r->cache_failure = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    return true;
}

bool lv_cache_capture_locked(lv_core *core, lv_request *r, lv_pixels *pixels, lv_rect_u32 rect, uint64_t tile, bool take)
{
    lv_cache_entry *entry = r->capture;
    if (!entry)
        return !take;
    uint64_t added = pixels->allocation;
    if (pixels->owner && pixels->owner != entry->last_owner)
    {
        bool seen = false;
        for (size_t i = 0; i < entry->captured; ++i)
            if (entry->tiles[i].pixels->owner == pixels->owner)
                seen = true;
        if (!seen)
            added += pixels->owner->allocation;
    }
    while (added <= core->config.cache_budget_bytes && entry->bytes <= core->config.cache_budget_bytes - added &&
           core->cache_bytes > core->config.cache_budget_bytes - entry->bytes - added && lv_cache_evict_locked(core))
    {
    }
    if (entry->captured == entry->count || added > core->config.cache_budget_bytes ||
        entry->bytes > core->config.cache_budget_bytes - added ||
        core->cache_bytes > core->config.cache_budget_bytes - entry->bytes - added)
    {
        drop_entry(core, entry);
        r->capture = nullptr;
        if (take)
            r->cache_failure = LV_ERROR_MEMORY_BUDGET;
        return !take;
    }
    if (!take)
        ++pixels->refs;
    entry->tiles[entry->captured++]  = (lv_cached_tile){pixels, rect, tile};
    entry->bytes                    += added;
    entry->last_owner                = pixels->owner;
    return true;
}

lv_error_code lv_cache_finish_locked(lv_core *core, lv_request *r, bool success)
{
    lv_cache_entry *entry = r->capture;
    r->capture            = nullptr;
    if (!entry)
        return success && r->desc.kind == LV_REQUEST_PREFETCH && !r->cache_hit ? LV_ERROR_MEMORY_BUDGET : 0;
    if (!success || core->closing || entry->captured != entry->count)
    {
        drop_entry(core, entry);
        return success && !core->closing && r->desc.kind == LV_REQUEST_PREFETCH ? LV_ERROR_INTERNAL : 0;
    }
    while ((core->cache_count >= 64 || core->cache_bytes > core->config.cache_budget_bytes - entry->bytes) &&
           lv_cache_evict_locked(core))
    {
    }
    entry->next        = core->cache;
    core->cache        = entry;
    core->cache_bytes += entry->bytes;
    ++core->cache_count;
    return 0;
}

uint64_t lv_cache_fixed_locked(const lv_request *r)
{
    return r->replay ? r->replay->bytes : 0;
}

bool lv_cache_replay(lv_core *core, lv_request *r, FILE *file, const lv_file_version *version, lv_error_info *error)
{
    mtx_lock(&core->mutex);
    lv_cache_entry **link = &core->cache;
    lv_cache_entry *entry = nullptr;
    while (*link)
    {
        lv_cache_entry *candidate = *link;
        if (candidate->version.device == version->device && candidate->version.inode == version->inode &&
            memcmp(&candidate->version, version, sizeof *version))
        {
            remove_entry(core, link);
            continue;
        }
        if (!memcmp(&candidate->version, version, sizeof *version) && same_target(&candidate->target, &r->desc.target))
        {
            entry             = candidate;
            *link             = entry->next;
            entry->next       = core->cache;
            core->cache       = entry;
            entry->in_use     = true;
            r->replay         = entry;
            r->replay_file    = file;
            r->replay_version = *version;
            r->cache_hit      = true;
            break;
        }
        link = &candidate->next;
    }
    mtx_unlock(&core->mutex);
    if (!entry)
        return false;
    lv_image_info info = entry->info;
    if (!lv_file_unchanged(file, r->path, version))
        error->code = LV_ERROR_SOURCE_CHANGED;
    else if (!lv_publish_info(core, r, &info, version))
        error->code = LV_ERROR_INTERNAL;
    else if (r->desc.kind == LV_REQUEST_DISPLAY)
        for (size_t i = 0; i < entry->captured && !lv_stopped(core, r); ++i)
        {
            if (!lv_reserve(core, r, sizeof(lv_event)))
            {
                error->code = LV_ERROR_MEMORY_BUDGET;
                break;
            }
            lv_cached_tile *tile = &entry->tiles[i];
            mtx_lock(&core->mutex);
            ++tile->pixels->refs;
            mtx_unlock(&core->mutex);
            if (!lv_publish_tile(core, r, tile->pixels, tile->rect, tile->id))
            {
                mtx_lock(&core->mutex);
                lv_pixels_drop_locked(tile->pixels, false);
                mtx_unlock(&core->mutex);
                error->code = r->cache_failure ? r->cache_failure : LV_ERROR_OUT_OF_MEMORY;
                break;
            }
        }
    if (!error->code && !lv_stopped(core, r) && !lv_file_unchanged(file, r->path, version))
        error->code = LV_ERROR_SOURCE_CHANGED;
    mtx_lock(&core->mutex);
    entry->in_use  = false;
    r->replay      = nullptr;
    r->replay_file = nullptr;
    if (core->closing || error->code == LV_ERROR_SOURCE_CHANGED)
    {
        for (link = &core->cache; *link && *link != entry; link = &(*link)->next)
        {
        }
        if (*link)
            remove_entry(core, link);
    }
    mtx_unlock(&core->mutex);
    return true;
}
