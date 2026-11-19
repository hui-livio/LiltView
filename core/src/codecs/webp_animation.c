#include <webp/demux.h>
#include <string.h>
#include "codec.h"

struct lv_webp_animation
{
    lv_core *core;
    lv_request *request;
    lv_animation *session;
    lv_input input;
    uint8_t *data;
    size_t data_bytes, image_bytes;
    WebPAnimDecoder *decoder;
    WebPAnimInfo info;
    uint8_t *canvas;
    uint32_t current;
    uint64_t pts;
};

static bool unchanged(lv_webp_animation *s, lv_error_info *error)
{
    if (lv_file_unchanged(s->input.file, s->session->path, &s->input.version))
        return true;
    error->code = LV_ERROR_SOURCE_CHANGED;
    return false;
}

static void open_animation(lv_core *core, lv_request *request, lv_error_info *error)
{
    lv_animation *session = request->animation;
    uint32_t code = 0;
    lv_file_version version;
    FILE *file = lv_file_open(session->path, &version, &code);
    if (!file)
    {
        *error = (lv_error_info){.code = LV_ERROR_IO, .system_domain = LV_FILE_SYSTEM_DOMAIN, .system_code = code};
        return;
    }
    if (version.bytes > UINT64_C(64) * 1024 * 1024)
    {
        lv_file_close(file);
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    if (!lv_animation_reserve(core, request, sizeof(lv_webp_animation) + version.bytes + UINT64_C(4) * 1024 * 1024, error))
    {
        lv_file_close(file);
        return;
    }
    lv_webp_animation *s = lv_animation_allocate(core, session, sizeof *s);
    if (!s)
    {
        lv_file_close(file);
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    *s = (lv_webp_animation){.core = core, .request = request, .session = session, .input = {.file = file, .version = version}};
    session->decoder = s;
    session->decoder_kind = 3;
    s->data_bytes = (size_t)version.bytes;
    s->data       = lv_animation_allocate(core, session, s->data_bytes);
    if (!s->data)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    const size_t got = fread(s->data, 1, s->data_bytes, file);
    request->bytes_read += got;
    if (got != s->data_bytes)
    {
        error->code = ferror(file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        return;
    }
    WebPData bitstream = {s->data, s->data_bytes};
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options))
    {
        error->code = LV_ERROR_INTERNAL;
        return;
    }
    options.color_mode = MODE_RGBA;
    s->decoder = WebPAnimDecoderNew(&bitstream, &options);
    if (!s->decoder || !WebPAnimDecoderGetInfo(s->decoder, &s->info))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    if (s->info.frame_count <= 1 || !s->info.canvas_width || !s->info.canvas_height)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return;
    }
    const lv_size_u32 size = {s->info.canvas_width, s->info.canvas_height};
    if (!lv_bitmap_size(core, size.width, size.height, error))
        return;
    s->image_bytes = (size_t)((uint64_t)size.width * size.height * 4);
    if (!lv_animation_reserve(core, request, s->image_bytes, error))
        return;
    s->canvas = lv_animation_allocate(core, session, s->image_bytes);
    if (!s->canvas)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
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
    const lv_size_u32 canvas = lv_fit_size(size, session->target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    lv_animation_info info = {.animation_id = session->id,
                              .epoch = 1,
                              .image = {.format = LV_FORMAT_WEBP,
                                        .flags = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                                        .source_size = size,
                                        .file_bytes = version.bytes,
                                        .source_revision = session->source->revision,
                                        .orientation = LV_ORIENTATION_NORMAL,
                                        .capabilities = LV_CAP_ANIMATION,
                                        .load_mode = s->image_bytes <= core->config.small_pixel_bytes &&
                                                version.bytes <= core->config.small_file_bytes ? LV_LOAD_SMALL : LV_LOAD_INCREMENTAL,
                                        .canvas = canvas,
                                        .region = {0, 0, canvas.width, canvas.height},
                                        .tile_edge = core->config.tile_edge}};
    lv_animation_metadata(core, request, LV_EVENT_ANIMATION_INFO, &info);
}

static void end_animation(lv_webp_animation *s, lv_request *request, lv_error_info *error)
{
    if (!unchanged(s, error))
        return;
    lv_animation_info info = s->session->info;
    info.flags = LV_ANIMATION_FRAME_COUNT_KNOWN | LV_ANIMATION_PLAY_COUNT_KNOWN | LV_ANIMATION_DURATION_KNOWN;
    info.frame_count = s->info.frame_count;
    info.total_plays = s->info.loop_count ? (uint64_t)s->info.loop_count + 1 : 0;
    info.duration_ns = s->pts;
    request->animation_end = true;
    lv_animation_metadata(s->core, request, LV_EVENT_ANIMATION_END, &info);
}

static void decode_frame(lv_webp_animation *s, lv_request *request, lv_error_info *error)
{
    if (!WebPAnimDecoderHasMoreFrames(s->decoder))
    {
        end_animation(s, request, error);
        return;
    }
    uint8_t *pixels = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(s->decoder, &pixels, &timestamp) || timestamp < 0)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    const uint64_t pts = (uint64_t)timestamp * UINT64_C(1000000);
    if (pts < s->pts)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    memcpy(s->canvas, pixels, s->image_bytes);
    const uint64_t duration = pts - s->pts;
    lv_frame output = {.core = s->core, .request = request, .input = &s->input};
    const lv_animation_info *info = &s->session->info;
    if (!lv_frame_prepare(&output, info->image.source_size, info->image.canvas, LV_FORMAT_WEBP, LV_CAP_ANIMATION, 0, error))
        return;
    lv_animation_frame_info timing = {.key = {s->session->id, info->epoch, s->current},
                                      .source_duration_ns = duration,
                                      .source_pts_ns = s->pts,
                                      .flags = LV_ANIMATION_FRAME_DURATION_KNOWN | LV_ANIMATION_FRAME_PTS_KNOWN};
    if (lv_resample_rgba(s->core, request, s->canvas, info->image.source_size, output.data, info->image.canvas) &&
        unchanged(s, error) && lv_animation_frame(s->core, request, &timing))
        lv_frame_finish(&output, error);
    lv_frame_release(&output);
    s->pts = pts;
    ++s->current;
}

void lv_webp_animation_run(lv_core *core, lv_request *request, lv_error_info *error)
{
    if (!request->animation_action)
    {
        open_animation(core, request, error);
        return;
    }
    lv_webp_animation *s = request->animation->decoder;
    if (!s)
    {
        error->code = LV_ERROR_INTERNAL;
        return;
    }
    s->request = request;
    if (!unchanged(s, error))
        return;
    if (request->animation_action == LV_ANIMATION_RESET)
    {
        WebPAnimDecoderReset(s->decoder);
        s->current = 0;
        s->pts = 0;
        lv_animation_info info = request->animation->info;
        ++info.epoch;
        lv_animation_metadata(core, request, LV_EVENT_ANIMATION_INFO, &info);
    }
    else
        decode_frame(s, request, error);
}

void lv_webp_animation_destroy(lv_core *core, lv_animation *session)
{
    lv_webp_animation *s = session->decoder;
    if (!s)
        return;
    WebPAnimDecoderDelete(s->decoder);
    lv_file_close(s->input.file);
    lv_animation_deallocate(core, session, s->data, s->data_bytes);
    lv_animation_deallocate(core, session, s->canvas, s->image_bytes);
    lv_animation_deallocate(core, session, s, sizeof *s);
    session->decoder = nullptr;
    session->decoder_kind = 0;
}
