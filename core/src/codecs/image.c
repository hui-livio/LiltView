#include <string.h>
#include "codec.h"

typedef void (*decode_func)(lv_core *, lv_request *, lv_input *, lv_error_info *);

static decode_func detect_format(const uint8_t *data, size_t size)
{
    if (size >= 2 && data[0] == 0xff && data[1] == 0xd8)
        return lv_decode_jpeg;
    if (size >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8))
        return lv_decode_png;
    if (size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4))
        return lv_decode_webp;
    if (size >= 2 && !memcmp(data, "BM", 2))
        return lv_decode_bmp;
    if (size >= 6 && (!memcmp(data, "GIF87a", 6) || !memcmp(data, "GIF89a", 6)))
        return lv_decode_gif;
    if (size >= 4 &&
        ((!memcmp(data, "II", 2) && (data[2] == 42 || data[2] == 43) && !data[3]) ||
         (!memcmp(data, "MM", 2) && !data[2] && (data[3] == 42 || data[3] == 43))))
        return lv_decode_tiff;
    size_t at = size >= 3 && !memcmp(data, "\xef\xbb\xbf", 3) ? 3 : 0;
    while (at < size && (data[at] == ' ' || data[at] == '\t' || data[at] == '\n' || data[at] == '\r'))
        ++at;
    if (at < size && data[at] == '<')
        return lv_decode_svg;
    return nullptr;
}

void lv_decode_image(lv_core *core, lv_request *r, lv_error_info *error)
{
    lv_input input = {0};
    uint32_t code  = 0;
    input.file     = lv_file_open(r->path, &input.version, &code);
    if (!input.file)
    {
        *error = (lv_error_info){.code = LV_ERROR_IO, .system_domain = LV_FILE_SYSTEM_DOMAIN, .system_code = code};
        return;
    }
    if (core->config.cache_budget_bytes && lv_cache_replay(core, r, input.file, &input.version, error))
    {
        lv_file_close(input.file);
        return;
    }
    uint8_t magic[512];
    const size_t count  = fread(magic, 1, sizeof magic, input.file);
    r->bytes_read      += count;
    decode_func decode  = detect_format(magic, count);
    const bool regional =
        decode == lv_decode_jpeg || decode == lv_decode_png || decode == lv_decode_bmp || decode == lv_decode_tiff;
    if (ferror(input.file))
        error->code = LV_ERROR_IO;
    else if (count < 2)
        error->code = LV_ERROR_CORRUPT_DATA;
    else if (!decode)
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
    else if (r->desc.target.kind == LV_TARGET_REGION && !regional)
        error->code = LV_ERROR_TARGET_UNSUPPORTED;
    else if (!regional && input.version.bytes > UINT64_C(64) * 1024 * 1024)
        error->code = LV_ERROR_DIMENSION_LIMIT;
    else if (fseek(input.file, 0, SEEK_SET))
        error->code = LV_ERROR_IO;
    else if (!lv_stopped(core, r))
        decode(core, r, &input, error);
    lv_file_close(input.file);
}

lv_size_u32 lv_fit_size(lv_size_u32 original, lv_size_u32 bounds, lv_orientation orientation)
{
    if (orientation >= 5)
    {
        uint32_t t    = bounds.width;
        bounds.width  = bounds.height;
        bounds.height = t;
    }
    lv_size_u32 size = original;
    if (original.width > bounds.width || original.height > bounds.height)
    {
        // Select the limiting axis before rounding either dimension.
        if ((uint64_t)original.width * bounds.height >= (uint64_t)original.height * bounds.width)
        {
            size.width  = bounds.width;
            size.height = (uint32_t)((uint64_t)original.height * bounds.width / original.width);
        }
        else
        {
            size.height = bounds.height;
            size.width  = (uint32_t)((uint64_t)original.width * bounds.height / original.height);
        }
    }
    if (!size.width)
        size.width = 1;
    if (!size.height)
        size.height = 1;
    return size;
}

bool lv_bitmap_size(lv_core *core, uint32_t width, uint32_t height, lv_error_info *error)
{
    const uint64_t limit = core->config.large_pixel_bytes < UINT64_C(256) * 1024 * 1024 ? core->config.large_pixel_bytes :
                                                                                          UINT64_C(256) * 1024 * 1024;
    // Divide before multiplying: hostile 32-bit dimensions must not wrap uint64.
    if (!width || !height || (uint64_t)width * height > limit / 4 || (uint64_t)width * height > SIZE_MAX / 4)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    return true;
}

static bool prepare_tiles(lv_frame *f, lv_error_info *error)
{
    const lv_rect_u32 region = f->info.region;
    const uint32_t edge      = f->info.tile_edge;
    const uint64_t columns   = ((uint64_t)f->info.canvas.width + edge - 1) / edge;
    f->tiles                 = lv_workspace_allocate(f->core, f->request, f->tile_count * sizeof *f->tiles);
    if (!f->tiles)
        goto failed;
    memset(f->tiles, 0, f->tile_count * sizeof *f->tiles);
    size_t at = 0;
    for (uint64_t gy = (uint64_t)(region.y / edge) * edge; gy < (uint64_t)region.y + region.height; gy += edge)
        for (uint64_t gx = (uint64_t)(region.x / edge) * edge; gx < (uint64_t)region.x + region.width; gx += edge)
        {
            const uint32_t x = (uint32_t)(gx < region.x ? region.x : gx), y = (uint32_t)(gy < region.y ? region.y : gy);
            const uint64_t right = gx + edge < (uint64_t)region.x + region.width ? gx + edge : (uint64_t)region.x + region.width;
            const uint64_t bottom =
                gy + edge < (uint64_t)region.y + region.height ? gy + edge : (uint64_t)region.y + region.height;
            lv_frame_tile *tile = &f->tiles[at++];
            tile->rect          = (lv_rect_u32){x, y, (uint32_t)(right - x), (uint32_t)(bottom - y)};
            tile->id            = (gy / edge) * columns + gx / edge;
            tile->pixels = f->streaming ? nullptr : lv_pixels_new(f->core, f->request, tile->rect.width, tile->rect.height);
            if (!f->streaming && !tile->pixels)
                goto failed;
        }
    return true;
failed:
    error->code = LV_ERROR_OUT_OF_MEMORY;
    lv_frame_release(f); // Includes partial allocation; callers may release again.
    return false;
}

bool lv_frame_prepare(lv_frame *f, lv_size_u32 source, lv_size_u32 canvas, lv_format format, lv_capabilities caps,
                      uint64_t scratch, lv_error_info *error)
{
    const lv_rect_u32 region = f->request->desc.target.kind == LV_TARGET_REGION ?
        f->request->desc.target.as.region.rect :
        (lv_rect_u32){0, 0, canvas.width, canvas.height};
    f->streaming             = f->request->desc.target.kind == LV_TARGET_NATIVE && (caps & LV_CAP_REGION_DECODE);
    if (!region.width || !region.height || (uint64_t)region.width * region.height > UINT64_MAX / 4)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    if (!f->streaming && !lv_bitmap_size(f->core, region.width, region.height, error))
        return false;
    const uint64_t bytes   = (uint64_t)region.width * region.height * 4;
    const uint64_t edge    = f->core->config.tile_edge;
    const uint64_t columns = ((uint64_t)region.x + region.width - 1) / edge - region.x / edge + 1;
    const uint64_t count   = columns * (((uint64_t)region.y + region.height - 1) / edge - region.y / edge + 1);
    const bool tiled       = (caps & LV_CAP_REGION_DECODE) && !f->request->animation;
    f->bytes               = (size_t)bytes;
    f->tile_reserve        = count * (sizeof(lv_event) + (tiled ? 0 : sizeof(lv_pixels)));
    const uint64_t plan    = f->streaming ? count * (sizeof(lv_frame_tile) + sizeof(lv_event)) + scratch :
        tiled ? bytes + count * (sizeof(lv_frame_tile) + sizeof(lv_pixels) + sizeof(lv_event)) + scratch :
                bytes + sizeof(lv_pixels) + f->tile_reserve + scratch;
    if (!lv_reserve(f->core, f->request, plan))
    {
        *error = (lv_error_info){
            .code = LV_ERROR_MEMORY_BUDGET, .required_bytes = plan, .limit_bytes = f->core->config.cpu_budget_bytes};
        return false;
    }
    const uint64_t pixels   = (uint64_t)source.width * source.height;
    const bool large        = format != LV_FORMAT_SVG && pixels > f->core->config.large_pixel_bytes / 4;
    const uint64_t original = format == LV_FORMAT_SVG ? bytes : pixels > UINT64_MAX / 4 ? UINT64_MAX : pixels * 4;
    f->info                 = (lv_image_info){.format       = format,
                                              .flags        = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                                              .source_size  = source,
                                              .file_bytes   = f->input->version.bytes,
                                              .orientation  = LV_ORIENTATION_NORMAL,
                                              .capabilities = caps,
                                              .load_mode    = large ? LV_LOAD_LARGE :
                                                  original <= f->core->config.small_pixel_bytes &&
                                                      f->input->version.bytes <= f->core->config.small_file_bytes ?
                                                                   LV_LOAD_SMALL :
                                                                   LV_LOAD_INCREMENTAL,
                                              .canvas       = canvas,
                                              .region       = region,
                                              .tile_edge    = (uint32_t)edge};
    if (tiled)
    {
        f->tile_count   = (size_t)count;
        f->tile_columns = (size_t)columns;
        return prepare_tiles(f, error);
    }
    f->output = lv_pixels_new(f->core, f->request, region.width, region.height);
    if (f->output)
    {
        f->data = f->output->data;
        return true;
    }
    error->code = LV_ERROR_OUT_OF_MEMORY;
    return false;
}

uint8_t *lv_frame_span(lv_frame *f, uint32_t x, uint32_t y, uint32_t *width)
{
    const lv_rect_u32 region = f->info.region;
    if (!f->tiles)
    {
        *width = region.width - x;
        return f->data + ((size_t)y * region.width + x) * 4;
    }
    const uint32_t gx = region.x + x, gy = region.y + y, edge = f->info.tile_edge;
    const size_t col = gx / edge - region.x / edge, row = gy / edge - region.y / edge;
    lv_frame_tile *tile = &f->tiles[row * f->tile_columns + col];
    if (f->streaming && !tile->pixels)
    {
        const uint64_t bytes = sizeof(lv_pixels) + (uint64_t)tile->rect.width * tile->rect.height * 4;
        if (!lv_reserve(f->core, f->request, bytes))
            f->failure = LV_ERROR_MEMORY_BUDGET;
        else if (!(tile->pixels = lv_pixels_new(f->core, f->request, tile->rect.width, tile->rect.height)))
            f->failure = LV_ERROR_OUT_OF_MEMORY;
        if (f->failure)
        {
            *width = 0;
            return nullptr;
        }
    }
    *width = tile->rect.width - (gx - tile->rect.x);
    return tile->pixels->data + ((size_t)(gy - tile->rect.y) * tile->rect.width + gx - tile->rect.x) * 4;
}

bool lv_frame_info(lv_frame *f, lv_error_info *error)
{
    if (lv_publish_info(f->core, f->request, &f->info, &f->input->version))
        return true;
    error->code = f->request->cache_failure ? f->request->cache_failure : LV_ERROR_INTERNAL;
    return false;
}

static bool publish_slices(lv_frame *f, lv_error_info *error)
{
    lv_core *core            = f->core;
    lv_request *r            = f->request;
    const lv_size_u32 canvas = f->info.canvas;
    const lv_rect_u32 region = f->info.region;
    const uint32_t edge      = core->config.tile_edge;
    const uint64_t columns   = ((uint64_t)canvas.width + edge - 1) / edge;
    for (uint64_t grid_y = (uint64_t)(region.y / edge) * edge; grid_y < (uint64_t)region.y + region.height; grid_y += edge)
        for (uint64_t grid_x = (uint64_t)(region.x / edge) * edge; grid_x < (uint64_t)region.x + region.width; grid_x += edge)
        {
            if (lv_stopped(core, r))
                return false;
            const uint32_t x = (uint32_t)(grid_x < region.x ? region.x : grid_x);
            const uint32_t y = (uint32_t)(grid_y < region.y ? region.y : grid_y);
            const uint64_t end_x =
                grid_x + edge < (uint64_t)region.x + region.width ? grid_x + edge : (uint64_t)region.x + region.width;
            const uint64_t end_y =
                grid_y + edge < (uint64_t)region.y + region.height ? grid_y + edge : (uint64_t)region.y + region.height;
            const uint32_t w = (uint32_t)(end_x - x), h = (uint32_t)(end_y - y);
            if (r->reservation < sizeof(lv_pixels) + sizeof(lv_event))
            {
                error->code = LV_ERROR_MEMORY_BUDGET;
                return false;
            }
            lv_pixels *p = lv_pixels_slice(core, r, f->output, (lv_rect_u32){x - region.x, y - region.y, w, h});
            if (!p)
            {
                error->code = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
            if (!lv_publish_tile(core, r, p, (lv_rect_u32){x, y, w, h}, (grid_y / edge) * columns + grid_x / edge))
            {
                mtx_lock(&core->mutex);
                lv_pixels_drop_locked(p, false);
                mtx_unlock(&core->mutex);
                error->code = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
        }
    return true;
}
bool lv_frame_flush_rows(lv_frame *f, uint32_t completed_rows, lv_error_info *error)
{
    if (!f->streaming || f->next_publish == f->tile_count ||
        (uint64_t)f->tiles[f->next_publish].rect.y + f->tiles[f->next_publish].rect.height >
            (uint64_t)f->info.region.y + completed_rows)
        return true;
    if (!lv_file_unchanged(f->input->file, f->request->path, &f->input->version))
    {
        error->code = LV_ERROR_SOURCE_CHANGED;
        return false;
    }
    while (f->next_publish < f->tile_count)
    {
        lv_frame_tile *tile = &f->tiles[f->next_publish];
        if ((uint64_t)tile->rect.y + tile->rect.height > (uint64_t)f->info.region.y + completed_rows)
            break;
        if (!tile->pixels || !lv_publish_tile(f->core, f->request, tile->pixels, tile->rect, tile->id))
        {
            if (!lv_stopped(f->core, f->request))
                error->code = LV_ERROR_OUT_OF_MEMORY;
            return false;
        }
        tile->pixels     = nullptr; // Immutable published row band; never touched again.
        f->tile_reserve -= sizeof(lv_event);
        ++f->next_publish;
    }
    return true;
}

bool lv_frame_finish(lv_frame *f, lv_error_info *error)
{
    if (lv_stopped(f->core, f->request))
        return false;
    if (!lv_file_unchanged(f->input->file, f->request->path, &f->input->version))
    {
        error->code = LV_ERROR_SOURCE_CHANGED;
        return false;
    }
    if (f->streaming && !lv_frame_flush_rows(f, f->info.region.height, error))
        return false;
    // Only event records remain reserved. Decoder-owned tiles are already live.
    mtx_lock(&f->core->mutex);
    f->core->reserved       -= f->request->reservation - f->tile_reserve;
    f->request->reservation  = f->tile_reserve;
    mtx_unlock(&f->core->mutex);
    if (!f->tiles)
    {
        if (!publish_slices(f, error))
            return false;
    }
    else
        for (size_t i = f->next_publish; i < f->tile_count; ++i)
        {
            lv_frame_tile *tile = &f->tiles[i];
            // Success moves the sole reference into the event. Never write it again.
            // Failure leaves ownership here for lv_frame_release, including cancel.
            if (!lv_publish_tile(f->core, f->request, tile->pixels, tile->rect, tile->id))
            {
                if (!lv_stopped(f->core, f->request))
                    error->code = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
            tile->pixels = nullptr;
        }
    // Queue backpressure can outlive the initial version check. Never commit
    // cached content or a complete animation candidate from a changed source.
    if (!lv_file_unchanged(f->input->file, f->request->path, &f->input->version))
    {
        error->code = LV_ERROR_SOURCE_CHANGED;
        return false;
    }
    return true;
}

void lv_frame_release(lv_frame *f)
{
    if (f->tiles)
    {
        mtx_lock(&f->core->mutex);
        for (size_t i = 0; i < f->tile_count; ++i)
            if (f->tiles[i].pixels)
                lv_pixels_drop_locked(f->tiles[i].pixels, false);
        mtx_unlock(&f->core->mutex);
        lv_workspace_release(f->core, f->request, f->tiles, f->tile_count * sizeof *f->tiles);
    }
    if (f->output)
    {
        mtx_lock(&f->core->mutex);
        lv_pixels_drop_locked(f->output, false);
        mtx_unlock(&f->core->mutex);
        f->output = nullptr;
    }
    f->tiles      = nullptr;
    f->data       = nullptr;
    f->tile_count = f->tile_columns = 0;
}

uint8_t *lv_input_read_all(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    const size_t bytes = (size_t)input->version.bytes;
    if (!lv_reserve(core, r, bytes + 1))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return nullptr;
    }
    uint8_t *data = lv_workspace_allocate(core, r, bytes + 1);
    if (!data)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return nullptr;
    }
    size_t offset = 0;
    while (offset < bytes && !lv_stopped(core, r))
    {
        const size_t wanted  = bytes - offset < 16384 ? bytes - offset : 16384;
        const size_t count   = fread(data + offset, 1, wanted, input->file);
        offset              += count;
        r->bytes_read       += count;
        if (count != wanted)
            break;
    }
    if (offset != bytes)
    {
        error->code = ferror(input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        lv_workspace_release(core, r, data, bytes + 1);
        return nullptr;
    }
    data[bytes] = 0;
    return data;
}

// Alpha-weighted bilinear resampling avoids dark fringes around transparency.
bool lv_resample_rgba(lv_core *core, lv_request *r, const uint8_t *src, lv_size_u32 size, uint8_t *dst, lv_size_u32 out)
{
    if (size.width == out.width && size.height == out.height)
    {
        memcpy(dst, src, (size_t)out.width * out.height * 4);
        return !lv_stopped(core, r);
    }
    for (uint32_t y = 0; y < out.height; ++y)
    {
        if (lv_stopped(core, r))
            return false;
        uint64_t sy = ((uint64_t)y * 2 + 1) * size.height * 65536 / ((uint64_t)out.height * 2);
        sy          = sy >= 32768 ? sy - 32768 : 0;
        uint32_t y0 = (uint32_t)(sy >> 16), y1 = y0 + 1 < size.height ? y0 + 1 : y0;
        uint64_t fy = sy & 65535;
        for (uint32_t x = 0; x < out.width; ++x)
        {
            uint64_t sx = ((uint64_t)x * 2 + 1) * size.width * 65536 / ((uint64_t)out.width * 2);
            sx          = sx >= 32768 ? sx - 32768 : 0;
            uint32_t x0 = (uint32_t)(sx >> 16), x1 = x0 + 1 < size.width ? x0 + 1 : x0;
            uint64_t fx         = sx & 65535;
            const uint8_t *p[4] = {src + ((size_t)y0 * size.width + x0) * 4, src + ((size_t)y0 * size.width + x1) * 4,
                                   src + ((size_t)y1 * size.width + x0) * 4, src + ((size_t)y1 * size.width + x1) * 4};
            uint64_t weights[4] = {(65536 - fx) * (65536 - fy), fx * (65536 - fy), (65536 - fx) * fy, fx * fy};
            uint64_t alpha      = 0;
            for (unsigned i = 0; i < 4; ++i)
                alpha += weights[i] * p[i][3];
            for (unsigned c = 0; c < 3; ++c)
            {
                uint64_t color = 0;
                for (unsigned i = 0; i < 4; ++i)
                    color += weights[i] * p[i][3] * p[i][c];
                *dst++ = (uint8_t)(alpha ? (color + alpha / 2) / alpha : 0);
            }
            *dst++ = (uint8_t)((alpha + (UINT64_C(1) << 31)) >> 32);
        }
    }
    return true;
}
