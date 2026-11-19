#include <png.h>
#include <string.h>
#include <zlib.h>
#include "codec.h"

typedef struct apng_frame_desc
{
    uint32_t width, height, x, y;
    uint16_t delay_num, delay_den;
    uint8_t dispose, blend;
} apng_frame_desc;

struct lv_apng_animation
{
    lv_core *core;
    lv_request *request;
    lv_animation *session;
    lv_input input;
    uint8_t *data;
    size_t data_bytes;
    uint8_t ihdr[13];
    uint32_t width, height, frame_count;
    uint32_t plays, current;
    apng_frame_desc *frames;
    uint8_t *canvas, *backup, *temp;
    size_t image_bytes;
    apng_frame_desc previous;
    bool has_previous;
    bool duration_known;
    uint64_t pts;
};

static uint32_t be32(const uint8_t *data)
{
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static void put_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static bool chunk_valid(const uint8_t *data, size_t bytes, size_t at, uint32_t length)
{
    if ((uint64_t)length > bytes - at - 12)
        return false;
    const uLong expected = crc32(0, data + at + 4, length + 4);
    return expected == be32(data + at + 8 + length);
}

static bool parse_control(lv_apng_animation *s, const uint8_t *data, apng_frame_desc *out)
{
    const uint32_t width = be32(data), height = be32(data + 4), x = be32(data + 8), y = be32(data + 12);
    if (!width || !height || x > s->width || y > s->height || width > s->width - x || height > s->height - y)
        return false;
    const uint16_t delay_num = (uint16_t)data[16] << 8 | data[17];
    const uint16_t delay_den = (uint16_t)data[18] << 8 | data[19];
    const uint8_t dispose   = data[20], blend = data[21];
    if (dispose > 2 || blend > 1)
        return false;
    *out = (apng_frame_desc){width, height, x, y, delay_num, delay_den ? delay_den : 100, dispose, blend};
    return true;
}

static bool parse_frames(lv_apng_animation *s, lv_error_info *error)
{
    if (s->data_bytes < 8 || memcmp(s->data, "\x89PNG\r\n\x1a\n", 8))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    size_t at = 8;
    bool seen_ihdr = false, seen_actl = false, seen_iend = false;
    uint32_t declared = 0;
    while (at <= s->data_bytes && s->data_bytes - at >= 12)
    {
        const uint32_t length = be32(s->data + at);
        if (!chunk_valid(s->data, s->data_bytes, at, length))
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            return false;
        }
        const uint8_t *type = s->data + at + 4, *payload = type + 4;
        if (!memcmp(type, "IHDR", 4))
        {
            if (seen_ihdr || length != 13)
            {
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
            memcpy(s->ihdr, payload, 13);
            s->width  = be32(payload);
            s->height = be32(payload + 4);
            if (!s->width || !s->height || payload[8] == 0 ||
                !(payload[9] == 0 || payload[9] == 2 || payload[9] == 3 || payload[9] == 4 || payload[9] == 6))
            {
                error->code = LV_ERROR_FORMAT_UNSUPPORTED;
                return false;
            }
            seen_ihdr = true;
        }
        else if (!memcmp(type, "acTL", 4))
        {
            if (!seen_ihdr || seen_actl || length != 8)
            {
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
            declared = be32(payload);
            s->plays = be32(payload + 4);
            if (!declared || declared > 4096)
            {
                error->code = LV_ERROR_DIMENSION_LIMIT;
                return false;
            }
            seen_actl = true;
        }
        at += (size_t)length + 12;
        if (!memcmp(type, "IEND", 4))
        {
            if (at != s->data_bytes)
            {
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
            seen_iend = true;
            break;
        }
    }
    if (!seen_ihdr || !seen_iend)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    if (!seen_actl || !declared)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    const uint64_t frame_bytes = (uint64_t)declared * sizeof(apng_frame_desc);
    if (!lv_animation_reserve(s->core, s->request, frame_bytes + (uint64_t)s->width * s->height * 12, error))
        return false;
    s->frame_count = declared;
    s->frames = lv_animation_allocate(s->core, s->session, (size_t)frame_bytes);
    s->image_bytes = (size_t)((uint64_t)s->width * s->height * 4);
    s->canvas = lv_animation_allocate(s->core, s->session, s->image_bytes);
    s->backup = lv_animation_allocate(s->core, s->session, s->image_bytes);
    s->temp   = lv_animation_allocate(s->core, s->session, s->image_bytes);
    if (!s->frames || !s->canvas || !s->backup || !s->temp)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    size_t frame = SIZE_MAX;
    at = 8;
    bool saw_idat = false;
    while (at <= s->data_bytes && s->data_bytes - at >= 12)
    {
        const uint32_t length = be32(s->data + at);
        const uint8_t *type   = s->data + at + 4, *payload = type + 4;
        if (!chunk_valid(s->data, s->data_bytes, at, length))
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            return false;
        }
        if (!memcmp(type, "fcTL", 4))
        {
            if (length != 26 || frame + 1 >= declared || !parse_control(s, payload + 4, &s->frames[++frame]))
            {
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
        }
        else if (!memcmp(type, "IDAT", 4))
        {
            if (frame == SIZE_MAX || frame != 0)
            {
                error->code = LV_ERROR_FORMAT_UNSUPPORTED;
                return false;
            }
            saw_idat = true;
        }
        else if (!memcmp(type, "fdAT", 4))
        {
            if (length < 4 || frame == SIZE_MAX || frame == 0)
            {
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
        }
        at += (size_t)length + 12;
        if (!memcmp(type, "IEND", 4))
            break;
    }
    if (!saw_idat || frame + 1 != declared)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    s->frame_count = declared;
    return true;
}

static bool append_chunk(uint8_t *output, size_t capacity, size_t *at, const char type[4], const uint8_t *payload,
                         uint32_t length)
{
    if ((uint64_t)*at + length + 12 > capacity)
        return false;
    put_be32(output + *at, length);
    memcpy(output + *at + 4, type, 4);
    if (length)
        memcpy(output + *at + 8, payload, length);
    const uLong crc = crc32(0, output + *at + 4, length + 4);
    put_be32(output + *at + 8 + length, (uint32_t)crc);
    *at += (size_t)length + 12;
    return true;
}

static bool build_frame_png(lv_apng_animation *s, lv_request *request, uint32_t wanted, uint8_t **output, size_t *bytes,
                            lv_error_info *error)
{
    const size_t capacity = s->data_bytes + 64;
    if (!lv_reserve(s->core, request, capacity))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    uint8_t *data = lv_workspace_allocate(s->core, request, capacity);
    if (!data)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    size_t at = 0;
    memcpy(data, "\x89PNG\r\n\x1a\n", 8);
    at = 8;
    uint8_t ihdr[13];
    memcpy(ihdr, s->ihdr, sizeof ihdr);
    put_be32(ihdr, s->frames[wanted].width);
    put_be32(ihdr + 4, s->frames[wanted].height);
    if (!append_chunk(data, capacity, &at, "IHDR", ihdr, sizeof ihdr))
        goto failed;
    size_t scan = 8;
    size_t frame = SIZE_MAX;
    bool wrote = false;
    while (scan <= s->data_bytes && s->data_bytes - scan >= 12)
    {
        const uint32_t length = be32(s->data + scan);
        const uint8_t *type   = s->data + scan + 4, *payload = type + 4;
        if (!memcmp(type, "fcTL", 4))
            ++frame;
        else if (!memcmp(type, "IDAT", 4) && frame == wanted)
        {
            if (!append_chunk(data, capacity, &at, "IDAT", payload, length))
                goto failed;
            wrote = true;
        }
        else if (!memcmp(type, "fdAT", 4) && frame == wanted)
        {
            if (length < 4 || !append_chunk(data, capacity, &at, "IDAT", payload + 4, length - 4))
                goto failed;
            wrote = true;
        }
        scan += (size_t)length + 12;
        if (!memcmp(type, "IEND", 4))
        {
            if (scan != s->data_bytes)
            {
                lv_workspace_release(s->core, request, data, capacity);
                error->code = LV_ERROR_CORRUPT_DATA;
                return false;
            }
            break;
        }
    }
    if (!wrote || !append_chunk(data, capacity, &at, "IEND", nullptr, 0))
        goto failed;
    *output = data;
    *bytes  = at;
    return true;
failed:
    lv_workspace_release(s->core, request, data, capacity);
    error->code = LV_ERROR_CORRUPT_DATA;
    return false;
}

static bool decode_frame_image(lv_apng_animation *s, lv_request *request, const uint8_t *data, size_t bytes,
                               const apng_frame_desc *desc, lv_error_info *error)
{
    (void)request;
    png_image image = {0};
    image.version   = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, data, bytes))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    if (image.width != desc->width || image.height != desc->height ||
        !png_image_finish_read(&image, nullptr, s->temp, (png_int_32)desc->width * 4, nullptr))
    {
        png_image_free(&image);
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    png_image_free(&image);
    return true;
}

static void clear_rect(lv_apng_animation *s, const apng_frame_desc *rect)
{
    for (uint32_t y = rect->y; y < rect->y + rect->height; ++y)
        memset(s->canvas + ((size_t)y * s->width + rect->x) * 4, 0, (size_t)rect->width * 4);
}

static void blend_rect(lv_apng_animation *s, const apng_frame_desc *rect)
{
    for (uint32_t y = 0; y < rect->height; ++y)
        for (uint32_t x = 0; x < rect->width; ++x)
        {
            uint8_t *dst = s->canvas + ((size_t)(rect->y + y) * s->width + rect->x + x) * 4;
            const uint8_t *src = s->temp + ((size_t)y * rect->width + x) * 4;
            const uint32_t sa = src[3], da = dst[3], inv = 255 - sa;
            const uint32_t out_a = sa + (da * inv + 127) / 255;
            if (!out_a)
                memset(dst, 0, 4);
            else
            {
                for (unsigned c = 0; c < 3; ++c)
                    dst[c] = (uint8_t)((src[c] * sa + dst[c] * da * inv / 255 + out_a / 2) / out_a);
                dst[3] = (uint8_t)out_a;
            }
        }
}

static bool unchanged(lv_apng_animation *s, lv_error_info *error)
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
    if (!lv_animation_reserve(core, request, sizeof(lv_apng_animation) + version.bytes + 4096, error))
    {
        lv_file_close(file);
        return;
    }
    lv_apng_animation *s = lv_animation_allocate(core, session, sizeof *s);
    if (!s)
    {
        lv_file_close(file);
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    *s = (lv_apng_animation){.core = core, .request = request, .session = session, .input = {.file = file, .version = version}};
    session->decoder = s;
    session->decoder_kind = 2;
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
    if (!parse_frames(s, error) || !lv_bitmap_size(core, s->width, s->height, error) || !unchanged(s, error))
        return;
    memset(s->canvas, 0, s->image_bytes);
    s->duration_known = true;
    mtx_lock(&core->mutex);
    session->source = lv_source_for_locked(core, &s->input.version);
    mtx_unlock(&core->mutex);
    if (!session->source)
    {
        error->code = LV_ERROR_INTERNAL;
        return;
    }
    const lv_size_u32 source = {s->width, s->height};
    const lv_size_u32 canvas = lv_fit_size(source, session->target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    lv_animation_info info = {.animation_id = session->id,
                              .epoch = 1,
                              .image = {.format = LV_FORMAT_PNG,
                                        .flags = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                                        .source_size = source,
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

static void end_animation(lv_apng_animation *s, lv_request *request, lv_error_info *error)
{
    if (!unchanged(s, error))
        return;
    lv_animation_info info = s->session->info;
    info.flags = LV_ANIMATION_FRAME_COUNT_KNOWN | LV_ANIMATION_PLAY_COUNT_KNOWN | LV_ANIMATION_DURATION_KNOWN;
    info.frame_count = s->frame_count;
    info.total_plays = s->plays;
    info.duration_ns = s->duration_known ? s->pts : 0;
    request->animation_end = true;
    lv_animation_metadata(s->core, request, LV_EVENT_ANIMATION_END, &info);
}

static void decode_frame(lv_apng_animation *s, lv_request *request, lv_error_info *error)
{
    if (s->current >= s->frame_count)
    {
        end_animation(s, request, error);
        return;
    }
    const apng_frame_desc *frame_desc = &s->frames[s->current];
    if (s->has_previous)
    {
        if (s->previous.dispose == 1)
            clear_rect(s, &s->previous);
        else if (s->previous.dispose == 2)
            memcpy(s->canvas, s->backup, s->image_bytes);
    }
    uint8_t *png_data = nullptr;
    size_t png_bytes = 0;
    if (!build_frame_png(s, request, s->current, &png_data, &png_bytes, error))
        return;
    const bool decoded = decode_frame_image(s, request, png_data, png_bytes, frame_desc, error);
    lv_workspace_release(s->core, request, png_data, s->data_bytes + 64);
    if (!decoded)
        return;
    if (frame_desc->dispose == 2)
        memcpy(s->backup, s->canvas, s->image_bytes);
    if (frame_desc->blend == 0)
        for (uint32_t y = 0; y < frame_desc->height; ++y)
            memcpy(s->canvas + ((size_t)(frame_desc->y + y) * s->width + frame_desc->x) * 4,
                   s->temp + (size_t)y * frame_desc->width * 4, (size_t)frame_desc->width * 4);
    else
        blend_rect(s, frame_desc);
    const uint64_t duration = (uint64_t)frame_desc->delay_num * UINT64_C(1000000000) / frame_desc->delay_den;
    lv_frame output = {.core = s->core, .request = request, .input = &s->input};
    const lv_animation_info *info = &s->session->info;
    if (!lv_frame_prepare(&output, info->image.source_size, info->image.canvas, LV_FORMAT_PNG, LV_CAP_ANIMATION, 0, error))
        return;
    lv_animation_frame_info timing = {.key = {s->session->id, info->epoch, s->current},
                                      .source_duration_ns = duration,
                                      .source_pts_ns = s->duration_known ? s->pts : 0,
                                      .flags = LV_ANIMATION_FRAME_DURATION_KNOWN |
                                          (s->duration_known ? LV_ANIMATION_FRAME_PTS_KNOWN : 0)};
    if (lv_resample_rgba(s->core, request, s->canvas, info->image.source_size, output.data, info->image.canvas) &&
        unchanged(s, error) && lv_animation_frame(s->core, request, &timing))
        lv_frame_finish(&output, error);
    lv_frame_release(&output);
    s->previous = *frame_desc;
    s->has_previous = true;
    ++s->current;
    if (UINT64_MAX - s->pts < duration)
        s->duration_known = false;
    else
        s->pts += duration;
}

void lv_apng_animation_run(lv_core *core, lv_request *request, lv_error_info *error)
{
    if (!request->animation_action)
    {
        open_animation(core, request, error);
        return;
    }
    lv_apng_animation *s = request->animation->decoder;
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
        s->current = 0;
        s->pts = 0;
        s->has_previous = false;
        s->duration_known = true;
        memset(s->canvas, 0, s->image_bytes);
        lv_animation_info info = request->animation->info;
        ++info.epoch;
        lv_animation_metadata(core, request, LV_EVENT_ANIMATION_INFO, &info);
    }
    else
        decode_frame(s, request, error);
}

void lv_apng_animation_destroy(lv_core *core, lv_animation *session)
{
    lv_apng_animation *s = session->decoder;
    if (!s)
        return;
    lv_file_close(s->input.file);
    lv_animation_deallocate(core, session, s->data, s->data_bytes);
    lv_animation_deallocate(core, session, s->frames, (size_t)s->frame_count * sizeof *s->frames);
    lv_animation_deallocate(core, session, s->canvas, s->image_bytes);
    lv_animation_deallocate(core, session, s->backup, s->image_bytes);
    lv_animation_deallocate(core, session, s->temp, s->image_bytes);
    lv_animation_deallocate(core, session, s, sizeof *s);
    session->decoder = nullptr;
    session->decoder_kind = 0;
}
