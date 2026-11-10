#include <gif_lib.h>
#include <string.h>
#include "codec.h"

struct lv_gif_animation
{
    lv_core *core;
    lv_request *request; // Borrowed only during a worker job; never used while idle.
    lv_animation *session;
    lv_input input;
    GifFileType *gif;
    uint64_t remaining, count, pts;
    lv_error_code failure;
    uint8_t *canvas, *backup;
    GifPixelType *row;
    size_t image_bytes, row_bytes;
    GraphicsControlBlock previous;
    lv_rect_u32 previous_rect;
    bool has_previous, loop_known, duration_known;
    uint64_t total_plays;
};

static int read_gif(GifFileType *gif, GifByteType *data, int count)
{
    lv_gif_animation *s = gif->UserData;
    if (count <= 0 || lv_stopped(s->core, s->request))
        return 0;
    const size_t wanted     = (uint64_t)count < s->remaining ? (size_t)count : (size_t)s->remaining;
    const size_t got        = fread(data, 1, wanted, s->input.file);
    s->remaining           -= got;
    s->request->bytes_read += got;
    if (ferror(s->input.file))
        s->failure = LV_ERROR_IO;
    return (int)got;
}

static void background(lv_gif_animation *s, uint8_t out[4], bool transparent)
{
    memset(out, 0, 4);
    const GifFileType *gif = s->gif;
    if (!transparent && gif->SColorMap && gif->SBackGroundColor >= 0 && gif->SBackGroundColor < gif->SColorMap->ColorCount)
    {
        const GifColorType color = gif->SColorMap->Colors[gif->SBackGroundColor];
        out[0]                   = color.Red;
        out[1]                   = color.Green;
        out[2]                   = color.Blue;
        out[3]                   = 255;
    }
}
static bool fill_rect(lv_gif_animation *s, lv_rect_u32 rect, bool transparent)
{
    uint8_t color[4];
    background(s, color, transparent);
    for (uint32_t y = rect.y; y < rect.y + rect.height; ++y)
    {
        if (lv_stopped(s->core, s->request))
            return false;
        uint8_t *out = s->canvas + ((size_t)y * (uint32_t)s->gif->SWidth + rect.x) * 4;
        for (uint32_t x = 0; x < rect.width; ++x, out += 4)
            memcpy(out, color, 4);
    }
    return true;
}

static bool restart(lv_gif_animation *s)
{
    if (s->gif)
        DGifCloseFile(s->gif, nullptr);
    s->gif = nullptr;
    if (!lv_file_seek(s->input.file, 0))
    {
        s->failure = LV_ERROR_IO;
        return false;
    }
    s->remaining = s->input.version.bytes;
    s->count = s->pts = 0;
    s->has_previous = s->loop_known = false;
    s->duration_known               = true;
    s->total_plays                  = 1;
    s->failure                      = 0;
    int code                        = 0;
    s->gif                          = DGifOpen(s, read_gif, &code);
    if (!s->gif && code == D_GIF_ERR_NOT_ENOUGH_MEM)
        s->failure = LV_ERROR_OUT_OF_MEMORY;
    return s->gif != nullptr;
}

static bool extension(lv_gif_animation *s, GraphicsControlBlock *control)
{
    int code;
    GifByteType *block = nullptr;
    if (DGifGetExtension(s->gif, &code, &block) == GIF_ERROR)
        return false;
    bool loop = false;
    if (code == PLAINTEXT_EXT_FUNC_CODE)
    {
        s->failure = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if (code == GRAPHICS_EXT_FUNC_CODE)
    {
        if (!block || block[0] != 4 || DGifExtensionToGCB(4, block + 1, control) == GIF_ERROR)
            return false;
        if (control->UserInputFlag || control->DisposalMode > DISPOSE_PREVIOUS)
        {
            s->failure = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
    }
    if (code == APPLICATION_EXT_FUNC_CODE && block && block[0] == 11)
        loop = !memcmp(block + 1, "NETSCAPE2.0", 11) || !memcmp(block + 1, "ANIMEXTS1.0", 11);
    bool first = true;
    while (block)
    {
        if (lv_stopped(s->core, s->request) || DGifGetExtensionNext(s->gif, &block) == GIF_ERROR)
            return false;
        if (code == GRAPHICS_EXT_FUNC_CODE && block)
            return false;
        if (loop && first)
        {
            if (!block || block[0] != 3 || block[1] != 1)
                return false;
            const uint32_t repeat = (uint32_t)block[2] | (uint32_t)block[3] << 8;
            const uint64_t plays  = repeat ? (uint64_t)repeat + 1 : 0;
            if (s->loop_known && s->total_plays != plays)
                return false;
            s->loop_known  = true;
            s->total_plays = plays;
        }
        first = false;
    }
    return true;
}

// Return 1 for an image descriptor, 0 for validated trailer, -1 for failure.
static int next_image(lv_gif_animation *s, GraphicsControlBlock *control)
{
    while (!lv_stopped(s->core, s->request))
    {
        GifRecordType record;
        if (DGifGetRecordType(s->gif, &record) == GIF_ERROR)
            return -1;
        if (record == TERMINATE_RECORD_TYPE)
            return 0;
        if (record == IMAGE_DESC_RECORD_TYPE)
            return DGifGetImageHeader(s->gif) == GIF_OK ? 1 : -1;
        if (record != EXTENSION_RECORD_TYPE || !extension(s, control))
            return -1;
    }
    return -1;
}

static bool compose(lv_gif_animation *s, const GraphicsControlBlock *control)
{
    const GifImageDesc *d = &s->gif->Image;
    const uint32_t width = (uint32_t)s->gif->SWidth, height = (uint32_t)s->gif->SHeight;
    if (d->Left < 0 || d->Top < 0 || d->Width <= 0 || d->Height <= 0 || (uint32_t)d->Left > width || (uint32_t)d->Top > height ||
        (uint32_t)d->Width > width - (uint32_t)d->Left || (uint32_t)d->Height > height - (uint32_t)d->Top)
        return false;
    if (s->has_previous)
    {
        if (s->previous.DisposalMode == DISPOSE_BACKGROUND &&
            !fill_rect(s, s->previous_rect, s->previous.TransparentColor != NO_TRANSPARENT_COLOR))
            return false;
        if (s->previous.DisposalMode == DISPOSE_PREVIOUS)
            memcpy(s->canvas, s->backup, s->image_bytes);
    }
    else if (!fill_rect(s, (lv_rect_u32){0, 0, width, height}, control->TransparentColor != NO_TRANSPARENT_COLOR))
        return false;
    if (control->DisposalMode == DISPOSE_PREVIOUS)
        memcpy(s->backup, s->canvas, s->image_bytes);
    const ColorMapObject *map = d->ColorMap ? d->ColorMap : s->gif->SColorMap;
    if (!map)
        return false;
    const uint32_t starts[4] = {0, 4, 2, 1}, steps[4] = {8, 8, 4, 2};
    for (unsigned pass = 0; pass < (d->Interlace ? 4u : 1u); ++pass)
        for (uint32_t y = d->Interlace ? starts[pass] : 0; y < (uint32_t)d->Height; y += d->Interlace ? steps[pass] : 1)
        {
            if (lv_stopped(s->core, s->request) || DGifGetLine(s->gif, s->row, d->Width) == GIF_ERROR)
                return false;
            uint8_t *out = s->canvas + (((size_t)d->Top + y) * width + (uint32_t)d->Left) * 4;
            for (int x = 0; x < d->Width; ++x, out += 4)
            {
                const unsigned index = s->row[x];
                if ((int)index == control->TransparentColor)
                    continue;
                if (index >= (unsigned)map->ColorCount)
                    return false;
                const GifColorType c = map->Colors[index];
                out[0]               = c.Red;
                out[1]               = c.Green;
                out[2]               = c.Blue;
                out[3]               = 255;
            }
        }
    s->previous      = *control;
    s->previous_rect = (lv_rect_u32){(uint32_t)d->Left, (uint32_t)d->Top, (uint32_t)d->Width, (uint32_t)d->Height};
    s->has_previous  = true;
    return true;
}

static bool unchanged(lv_gif_animation *s, lv_error_info *error)
{
    if (lv_file_unchanged(s->input.file, s->session->path, &s->input.version))
        return true;
    error->code = LV_ERROR_SOURCE_CHANGED;
    return false;
}

static void open_animation(lv_core *core, lv_request *r, lv_error_info *error)
{
    if (!lv_animation_reserve(core, r, sizeof(lv_gif_animation) + UINT64_C(1) * 1024 * 1024, error))
        return;
    lv_animation *session = r->animation;
    lv_gif_animation *s   = lv_animation_allocate(core, session, sizeof *s);
    if (!s)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    *s               = (lv_gif_animation){.core = core, .request = r, .session = session};
    session->decoder = s;
    uint32_t code    = 0;
    s->input.file    = lv_file_open(session->path, &s->input.version, &code);
    if (!s->input.file)
    {
        *error = (lv_error_info){.code = LV_ERROR_IO, .system_domain = LV_FILE_SYSTEM_DOMAIN, .system_code = code};
        return;
    }
    uint8_t signature[6];
    const size_t got  = fread(signature, 1, 6, s->input.file);
    r->bytes_read    += got;
    if (got < 2)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    if (got != 6 || (memcmp(signature, "GIF87a", 6) && memcmp(signature, "GIF89a", 6)))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return;
    }
    if (s->input.version.bytes > UINT64_C(64) * 1024 * 1024)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    if (!restart(s))
    {
        error->code = s->failure ? s->failure : LV_ERROR_CORRUPT_DATA;
        return;
    }
    const lv_size_u32 size = {(uint32_t)s->gif->SWidth, (uint32_t)s->gif->SHeight};
    if (!lv_bitmap_size(core, size.width, size.height, error))
        return;
    s->image_bytes = (size_t)size.width * size.height * 4;
    s->row_bytes   = size.width;
    if (!lv_animation_reserve(core, r, (uint64_t)s->image_bytes * 2 + s->row_bytes, error))
        return;
    s->canvas = lv_animation_allocate(core, session, s->image_bytes);
    s->backup = lv_animation_allocate(core, session, s->image_bytes);
    s->row    = lv_animation_allocate(core, session, s->row_bytes);
    if (!s->canvas || !s->backup || !s->row)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    const lv_size_u32 canvas = lv_fit_size(size, session->target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    if (!unchanged(s, error))
        return;
    mtx_lock(&core->mutex);
    session->source = lv_source_for_locked(core, &s->input.version);
    mtx_unlock(&core->mutex);
    if (!session->source)
    {
        error->code = LV_ERROR_INTERNAL;
        return;
    }
    lv_animation_info info = {.animation_id = session->id,
                              .epoch        = 1,
                              .image        = {.format          = LV_FORMAT_GIF,
                                               .flags           = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                                               .source_size     = size,
                                               .file_bytes      = s->input.version.bytes,
                                               .source_revision = session->source->revision,
                                               .orientation     = LV_ORIENTATION_NORMAL,
                                               .capabilities    = LV_CAP_ANIMATION,
                                               .load_mode       = s->image_bytes <= core->config.small_pixel_bytes &&
                                                       s->input.version.bytes <= core->config.small_file_bytes ?
                                                   LV_LOAD_SMALL :
                                                   LV_LOAD_INCREMENTAL,
                                               .canvas          = canvas,
                                               .region          = {0, 0, canvas.width, canvas.height},
                                               .tile_edge       = core->config.tile_edge}};
    lv_animation_metadata(core, r, LV_EVENT_ANIMATION_INFO, &info);
}

static void end_animation(lv_gif_animation *s, lv_request *r, lv_error_info *error)
{
    if (!s->count)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    lv_animation_info info = s->session->info;
    const uint64_t plays   = s->loop_known ? s->total_plays : 1;
    if (((info.flags & LV_ANIMATION_FRAME_COUNT_KNOWN) && info.frame_count != s->count) ||
        ((info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN) && info.total_plays != plays) ||
        ((info.flags & LV_ANIMATION_DURATION_KNOWN) && (!s->duration_known || info.duration_ns != s->pts)))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    info.flags =
        LV_ANIMATION_FRAME_COUNT_KNOWN | LV_ANIMATION_PLAY_COUNT_KNOWN | (s->duration_known ? LV_ANIMATION_DURATION_KNOWN : 0);
    info.frame_count = s->count;
    info.total_plays = plays;
    info.duration_ns = s->duration_known ? s->pts : 0;
    if (!unchanged(s, error))
        return;
    r->animation_end = true;
    lv_animation_metadata(s->core, r, LV_EVENT_ANIMATION_END, &info);
}

static void decode_frame(lv_gif_animation *s, lv_request *r, lv_error_info *error)
{
    GraphicsControlBlock control = {.TransparentColor = NO_TRANSPARENT_COLOR};
    const int result             = next_image(s, &control);
    if (!result)
    {
        end_animation(s, r, error);
        return;
    }
    if (result < 0 || !compose(s, &control))
    {
        error->code = s->failure                      ? s->failure :
            s->gif->Error == D_GIF_ERR_NOT_ENOUGH_MEM ? LV_ERROR_OUT_OF_MEMORY :
                                                        LV_ERROR_CORRUPT_DATA;
        return;
    }
    if (s->count == UINT64_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    const uint64_t duration       = (uint64_t)control.DelayTime * UINT64_C(10000000);
    const lv_animation_info *info = &s->session->info;
    lv_frame frame                = {.core = s->core, .request = r, .input = &s->input};
    if (!lv_frame_prepare(&frame, info->image.source_size, info->image.canvas, LV_FORMAT_GIF, LV_CAP_ANIMATION, 0, error))
        return;
    lv_animation_frame_info timing = {.key                = {s->session->id, info->epoch, s->count},
                                      .source_duration_ns = duration,
                                      .source_pts_ns      = s->duration_known ? s->pts : 0,
                                      .flags              = LV_ANIMATION_FRAME_DURATION_KNOWN |
                                          (s->duration_known ? LV_ANIMATION_FRAME_PTS_KNOWN : 0)};
    if (lv_resample_rgba(s->core, r, s->canvas, info->image.source_size, frame.data, info->image.canvas) && unchanged(s, error) &&
        lv_animation_frame(s->core, r, &timing))
        lv_frame_finish(&frame, error);
    lv_frame_release(&frame);
    ++s->count;
    if (UINT64_MAX - s->pts < duration)
        s->duration_known = false;
    else
        s->pts += duration;
}

void lv_gif_animation_run(lv_core *core, lv_request *r, lv_error_info *error)
{
    if (!r->animation_action)
    {
        open_animation(core, r, error);
        return;
    }
    lv_gif_animation *s = r->animation->decoder;
    if (!s)
    {
        error->code = LV_ERROR_INTERNAL;
        return;
    }
    s->request = r;
    if (!unchanged(s, error))
        return;
    if (r->animation_action == LV_ANIMATION_RESET)
    {
        if (!restart(s))
        {
            error->code = s->failure ? s->failure : LV_ERROR_CORRUPT_DATA;
            return;
        }
        if (!unchanged(s, error))
            return;
        lv_animation_info info = r->animation->info;
        ++info.epoch;
        lv_animation_metadata(core, r, LV_EVENT_ANIMATION_INFO, &info);
    }
    else
        decode_frame(s, r, error);
}

void lv_gif_animation_destroy(lv_core *core, lv_animation *session)
{
    lv_gif_animation *s = session->decoder;
    if (!s)
        return;
    if (s->gif)
        DGifCloseFile(s->gif, nullptr);
    lv_file_close(s->input.file);
    lv_animation_deallocate(core, session, s->canvas, s->image_bytes);
    lv_animation_deallocate(core, session, s->backup, s->image_bytes);
    lv_animation_deallocate(core, session, s->row, s->row_bytes);
    lv_animation_deallocate(core, session, s, sizeof *s);
    session->decoder = nullptr;
}
