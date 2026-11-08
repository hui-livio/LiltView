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
        *error = (lv_error_info){.code = LV_ERROR_IO, .system_domain = LV_SYSTEM_ERRNO, .system_code = code};
        return;
    }
    if (input.version.bytes > UINT64_C(64) * 1024 * 1024)
        error->code = LV_ERROR_DIMENSION_LIMIT;
    else
    {
        uint8_t magic[512];
        size_t count        = fread(magic, 1, sizeof magic, input.file);
        r->bytes_read      += count;
        decode_func decode  = detect_format(magic, count);
        if (ferror(input.file))
            error->code = LV_ERROR_IO;
        else if (count < 2)
            error->code = LV_ERROR_CORRUPT_DATA;
        else if (!decode)
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        else if (fseek(input.file, 0, SEEK_SET))
            error->code = LV_ERROR_IO;
        else if (!lv_stopped(core, r))
            decode(core, r, &input, error);
    }
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

bool lv_frame_prepare(lv_frame *f, lv_size_u32 source, lv_size_u32 canvas, lv_format format, lv_capabilities caps,
                      uint64_t scratch, lv_error_info *error)
{
    if (!lv_bitmap_size(f->core, canvas.width, canvas.height, error))
        return false;
    const uint64_t bytes = (uint64_t)canvas.width * canvas.height * 4;
    const uint64_t edge  = f->core->config.tile_edge;
    const uint64_t count = ((canvas.width + edge - 1) / edge) * ((canvas.height + edge - 1) / edge);
    f->bytes             = (size_t)bytes;
    f->tile_reserve      = bytes + count * (sizeof(lv_pixels) + sizeof(lv_event));
    const uint64_t plan  = bytes + f->tile_reserve + scratch;
    if (!lv_reserve(f->core, f->request, plan))
    {
        *error = (lv_error_info){
            .code = LV_ERROR_MEMORY_BUDGET, .required_bytes = plan, .limit_bytes = f->core->config.cpu_budget_bytes};
        return false;
    }
    f->data = lv_workspace_allocate(f->core, f->request, f->bytes);
    if (!f->data)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    const uint64_t original = format == LV_FORMAT_SVG ? bytes : (uint64_t)source.width * source.height * 4;
    f->info                 = (lv_image_info){.format       = format,
                                              .flags        = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                                              .source_size  = source,
                                              .file_bytes   = f->input->version.bytes,
                                              .orientation  = LV_ORIENTATION_NORMAL,
                                              .capabilities = caps,
                                              .load_mode    = original <= f->core->config.small_pixel_bytes &&
                                                      f->input->version.bytes <= f->core->config.small_file_bytes ?
                                                  LV_LOAD_SMALL :
                                                  LV_LOAD_INCREMENTAL,
                                              .canvas       = canvas,
                                              .region       = {0, 0, canvas.width, canvas.height},
                                              .tile_edge    = (uint32_t)edge};
    return true;
}

bool lv_frame_info(lv_frame *f, lv_error_info *error)
{
    if (lv_publish_info(f->core, f->request, &f->info, &f->input->version))
        return true;
    error->code = LV_ERROR_INTERNAL;
    return false;
}

bool lv_publish_rgba(lv_core *core, lv_request *r, const uint8_t *data, lv_size_u32 size, lv_error_info *error)
{
    const uint32_t edge    = core->config.tile_edge;
    const uint64_t columns = ((uint64_t)size.width + edge - 1) / edge;
    for (uint32_t y = 0; y < size.height; y += edge)
        for (uint32_t x = 0; x < size.width; x += edge)
        {
            if (lv_stopped(core, r))
                return false;
            const uint32_t w = size.width - x < edge ? size.width - x : edge;
            const uint32_t h = size.height - y < edge ? size.height - y : edge;
            if (r->reservation < sizeof(lv_pixels) + (uint64_t)w * h * 4 + sizeof(lv_event))
            {
                error->code = LV_ERROR_MEMORY_BUDGET;
                return false;
            }
            lv_pixels *p = lv_pixels_new(core, r, w, h);
            if (!p)
            {
                error->code = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
            for (uint32_t row = 0; row < h; ++row)
                memcpy(p->data + (size_t)row * w * 4, data + ((size_t)(y + row) * size.width + x) * 4, (size_t)w * 4);
            if (!lv_publish_tile(core, r, p, (lv_rect_u32){x, y, w, h}, (uint64_t)(y / edge) * columns + x / edge))
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

bool lv_frame_finish(lv_frame *f, lv_error_info *error)
{
    if (lv_stopped(f->core, f->request))
        return false;
    if (!lv_file_unchanged(f->input->file, f->request->path, &f->input->version))
    {
        error->code = LV_ERROR_SOURCE_CHANGED;
        return false;
    }
    // Call only after freeing codec scratch. Queue waits retain just the FIT image.
    mtx_lock(&f->core->mutex);
    f->core->reserved       -= f->request->reservation - f->tile_reserve;
    f->request->reservation  = f->tile_reserve;
    mtx_unlock(&f->core->mutex);
    return lv_publish_rgba(f->core, f->request, f->data, f->info.canvas, error);
}

void lv_frame_release(lv_frame *f)
{
    lv_workspace_release(f->core, f->request, f->data, f->bytes);
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
