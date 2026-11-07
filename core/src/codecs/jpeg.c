#include "../internal.h"

#include <jpeglib.h>
#include <jerror.h>
#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

// Keep longjmp state on the heap: modified automatic locals would be indeterminate.
typedef struct jpeg_state
{
    struct jpeg_decompress_struct decoder;
    struct jpeg_error_mgr errors;
    jmp_buf escape;
    FILE *file;
    bool created, warning;
    uint8_t *row, *image;
    size_t row_bytes, image_bytes;
    lv_error_code failure;
    lv_orientation orientation;
    struct jpeg_source_mgr input;
    uint8_t input_buffer[16384];
    uint64_t remaining, header_allowance;
    lv_core *core;
    lv_request *request;
} jpeg_state;

static void jpeg_failure(j_common_ptr common)
{
    jpeg_state *s = common->client_data;
    if (common->err->msg_code == JERR_OUT_OF_MEMORY)
        s->failure = LV_ERROR_OUT_OF_MEMORY;
    else if (common->err->msg_code == JERR_IMAGE_TOO_BIG || common->err->msg_code == JERR_WIDTH_OVERFLOW)
        s->failure = LV_ERROR_DIMENSION_LIMIT;
    longjmp(s->escape, 1);
}
static void jpeg_message(j_common_ptr common, int level)
{
    if (level < 0)
        ((jpeg_state *)common->client_data)->warning = true;
}
static uint16_t read16(const uint8_t *p, bool little)
{
    return (uint16_t)(little ? ((uint32_t)p[0] | (uint32_t)p[1] << 8) : ((uint32_t)p[0] << 8 | p[1]));
}
static uint32_t read32(const uint8_t *p, bool little)
{
    return little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 :
                    (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void input_init(j_decompress_ptr d)
{
    (void)d;
}
static void input_term(j_decompress_ptr d)
{
    (void)d;
}
static boolean input_fill(j_decompress_ptr d)
{
    jpeg_state *s = d->client_data;
    if (lv_stopped(s->core, s->request))
        longjmp(s->escape, 1);
    if (!s->remaining)
        ERREXIT(d, JERR_INPUT_EOF);
    const size_t wanted = s->remaining < sizeof s->input_buffer ? (size_t)s->remaining : sizeof s->input_buffer;
    const size_t bytes  = fread(s->input_buffer, 1, wanted, s->file);
    if (!bytes)
    {
        if (ferror(s->file))
            s->failure = LV_ERROR_IO;
        ERREXIT(d, JERR_INPUT_EOF);
    }
    s->remaining             -= bytes;
    s->request->bytes_read   += bytes;
    s->input.next_input_byte  = s->input_buffer;
    s->input.bytes_in_buffer  = bytes;
    return TRUE;
}
static void input_skip(j_decompress_ptr d, long count)
{
    if (count <= 0)
        return;
    size_t bytes = (size_t)count;
    while (bytes > d->src->bytes_in_buffer)
    {
        bytes                   -= d->src->bytes_in_buffer;
        d->src->bytes_in_buffer  = 0;
        input_fill(d);
    }
    d->src->next_input_byte += bytes;
    d->src->bytes_in_buffer -= bytes;
}

static void exif_marker(j_decompress_ptr decoder)
{
    jpeg_state *s = decoder->client_data;
    // Marker collection is bounded separately below; parse only the orientation tag.
    for (jpeg_saved_marker_ptr marker = decoder->marker_list; marker; marker = marker->next)
    {
        const uint8_t *p   = marker->data;
        const size_t bytes = marker->data_length;
        if (marker->marker != JPEG_APP0 + 1 || bytes < 14 || memcmp(p, "Exif\0\0", 6))
            continue;
        p                   += 6;
        const size_t length  = bytes - 6;
        bool little          = p[0] == 'I' && p[1] == 'I';
        if ((!little && !(p[0] == 'M' && p[1] == 'M')) || read16(p + 2, little) != 42)
            continue;
        const uint32_t offset = read32(p + 4, little);
        if (offset > length - 2)
            continue;
        const uint16_t count = read16(p + offset, little);
        if ((size_t)count > (length - offset - 2) / 12)
            continue;
        for (uint16_t i = 0; i < count; ++i)
        {
            const uint8_t *entry = p + offset + 2 + (size_t)i * 12;
            if (read16(entry, little) == 0x112 && read16(entry + 2, little) == 3 && read32(entry + 4, little) == 1)
            {
                uint16_t value = read16(entry + 8, little);
                if (value >= 1 && value <= 8)
                    s->orientation = value;
            }
        }
    }
}

static lv_size_u32 fit_size(lv_size_u32 original, lv_size_u32 bounds, lv_orientation orientation)
{
    if (orientation >= 5)
    {
        uint32_t t    = bounds.width;
        bounds.width  = bounds.height;
        bounds.height = t;
    }
    lv_size_u32 size = original;
    if (size.width > bounds.width)
    {
        size.height = (uint32_t)((uint64_t)original.height * bounds.width / original.width);
        size.width  = bounds.width;
    }
    if (size.height > bounds.height)
    {
        size.width  = (uint32_t)((uint64_t)original.width * bounds.height / original.height);
        size.height = bounds.height;
    }
    if (!size.width)
        size.width = 1;
    if (!size.height)
        size.height = 1;
    return size;
}

static bool decode_rows(lv_core *core, lv_request *request, jpeg_state *s, lv_size_u32 canvas)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    uint32_t output_y                = 0;
    while (d->output_scanline < d->output_height)
    {
        if (lv_stopped(core, request))
            return false;
        const uint32_t input_y = d->output_scanline;
        JSAMPROW row           = s->row + (size_t)(input_y & 1u) * d->output_width * 4;
        if (jpeg_read_scanlines(d, &row, 1) != 1)
            return false;
        while (output_y < canvas.height)
        {
            uint64_t sy = ((uint64_t)output_y * 2 + 1) * d->output_height * 65536 / ((uint64_t)canvas.height * 2);
            sy          = sy >= 32768 ? sy - 32768 : 0;
            uint32_t y0 = (uint32_t)(sy >> 16), y1 = y0 + 1 < d->output_height ? y0 + 1 : y0;
            if (y1 > input_y)
                break;
            const uint32_t fy     = (uint32_t)(sy & 65535);
            const uint8_t *top    = s->row + (size_t)(y0 & 1u) * d->output_width * 4;
            const uint8_t *bottom = s->row + (size_t)(y1 & 1u) * d->output_width * 4;
            uint8_t *out          = s->image + (size_t)output_y * canvas.width * 4;
            for (uint32_t x = 0; x < canvas.width; ++x)
            {
                uint64_t sx     = ((uint64_t)x * 2 + 1) * d->output_width * 65536 / ((uint64_t)canvas.width * 2);
                sx              = sx >= 32768 ? sx - 32768 : 0;
                const size_t x0 = (size_t)(sx >> 16), x1 = x0 + 1 < d->output_width ? x0 + 1 : x0;
                const uint32_t fx = (uint32_t)(sx & 65535);
                for (unsigned c = 0; c < 3; ++c)
                {
                    const uint64_t a       = (uint64_t)top[x0 * 4 + c] * (65536 - fx) + (uint64_t)top[x1 * 4 + c] * fx;
                    const uint64_t b       = (uint64_t)bottom[x0 * 4 + c] * (65536 - fx) + (uint64_t)bottom[x1 * 4 + c] * fx;
                    out[(size_t)x * 4 + c] = (uint8_t)((a * (65536 - fy) + b * fy + (UINT64_C(1) << 31)) >> 32);
                }
                out[(size_t)x * 4 + 3] = 255;
            }
            ++output_y;
        }
    }
    return output_y == canvas.height;
}

static bool publish_tiles(lv_core *core, lv_request *r, jpeg_state *s, lv_size_u32 canvas)
{
    const uint32_t edge    = core->config.tile_edge;
    const uint64_t columns = ((uint64_t)canvas.width + edge - 1) / edge;
    for (uint32_t y = 0; y < canvas.height; y += edge)
        for (uint32_t x = 0; x < canvas.width; x += edge)
        {
            if (lv_stopped(core, r))
                return false;
            const uint32_t w          = canvas.width - x < edge ? canvas.width - x : edge;
            const uint32_t h          = canvas.height - y < edge ? canvas.height - y : edge;
            const uint64_t allocation = sizeof(lv_pixels) + (uint64_t)w * h * 4 + sizeof(lv_event);
            if (r->reservation < allocation)
            {
                s->failure = LV_ERROR_MEMORY_BUDGET;
                return false;
            }
            lv_pixels *pixels = lv_pixels_new(core, r, w, h);
            if (!pixels)
            {
                s->failure = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
            for (uint32_t row = 0; row < h; ++row)
                memcpy(pixels->data + (size_t)row * w * 4, s->image + ((size_t)(y + row) * canvas.width + x) * 4, (size_t)w * 4);
            if (!lv_publish_tile(core, r, pixels, (lv_rect_u32){x, y, w, h}, (uint64_t)(y / edge) * columns + x / edge))
            {
                mtx_lock(&core->mutex);
                lv_pixels_drop_locked(pixels, false);
                mtx_unlock(&core->mutex);
                s->failure = LV_ERROR_OUT_OF_MEMORY;
                return false;
            }
        }
    return true;
}

// File open and header parsing stay on the worker; submit only copies the path.
static bool open_source(lv_core *core, lv_request *r, jpeg_state *s, lv_error_info *error, lv_file_version *version)
{
    uint32_t system_code = 0;
    s->file              = lv_file_open(r->path, version, &system_code);
    if (!s->file)
    {
        *error = (lv_error_info){.code = LV_ERROR_IO, .system_domain = LV_SYSTEM_ERRNO, .system_code = system_code};
        return false;
    }
    // First edition deliberately bounds compressed and original RGBA sizes.
    if (version->bytes > 64ULL * 1024 * 1024)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    unsigned char magic[2];
    if (fread(magic, 1, 2, s->file) != 2)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    if (magic[0] != 0xff || magic[1] != 0xd8)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    rewind(s->file);
    // Reserve bounded header/marker space before the codec allocates any metadata.
    s->header_allowance = 2ULL * 1024 * 1024 + version->bytes * 2;
    if (!lv_reserve(core, r, s->header_allowance))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    return true;
}

static void read_header(jpeg_state *s, const lv_file_version *version)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    d->err                           = jpeg_std_error(&s->errors);
    s->errors.error_exit             = jpeg_failure;
    s->errors.emit_message           = jpeg_message;
    d->client_data                   = s;
    s->created                       = true;
    jpeg_create_decompress(d);
    // Bounded header input and image limits keep marker/coefficient memory finite.
    jpeg_save_markers(d, JPEG_APP0 + 1, 65535);
    s->remaining = version->bytes;
    s->input     = (struct jpeg_source_mgr){.init_source       = input_init,
                                            .fill_input_buffer = input_fill,
                                            .skip_input_data   = input_skip,
                                            .resync_to_restart = jpeg_resync_to_restart,
                                            .term_source       = input_term};
    d->src       = &s->input;
    jpeg_read_header(d, TRUE);
    exif_marker(d);
}

// Reserve the whole ordinary-image plan before the decoder grows its working set.
static bool prepare_output(lv_core *core, lv_request *r, jpeg_state *s, lv_error_info *error, lv_size_u32 *canvas_out,
                           uint64_t *reserve_out)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    const uint64_t original          = (uint64_t)d->image_width * d->image_height * 4;
    if (!d->image_width || !d->image_height || original > core->config.large_pixel_bytes ||
        original > UINT64_C(256) * 1024 * 1024 || d->data_precision != 8)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    if (d->jpeg_color_space == JCS_CMYK || d->jpeg_color_space == JCS_YCCK)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    const lv_size_u32 canvas =
        fit_size((lv_size_u32){d->image_width, d->image_height}, r->desc.target.as.fit_bounds, s->orientation);
    d->out_color_space = JCS_EXT_RGBA;
    for (unsigned numerator = 1; numerator <= 8; ++numerator)
    {
        d->scale_num   = numerator;
        d->scale_denom = 8;
        jpeg_calc_output_dimensions(d);
        if (d->output_width >= canvas.width && d->output_height >= canvas.height)
            break;
    }
    const uint64_t image_bytes = (uint64_t)canvas.width * canvas.height * 4;
    const uint64_t row_bytes   = (uint64_t)d->output_width * 8;
    if (image_bytes > SIZE_MAX || row_bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    s->row_bytes   = (size_t)row_bytes; // Two rows for bilinear FIT resampling.
    s->image_bytes = (size_t)image_bytes;
    // Codec internal allocations cannot be intercepted: conservative allowance plus measured limits, not an RSS cap.
    const uint64_t allowance      = s->header_allowance + original * (d->progressive_mode ? 2 : 1);
    const uint64_t edge           = core->config.tile_edge;
    const uint64_t count          = ((canvas.width + edge - 1) / edge) * ((canvas.height + edge - 1) / edge);
    const uint64_t output_reserve = s->image_bytes + count * (sizeof(lv_pixels) + sizeof(lv_event));
    if (!lv_reserve(core, r, allowance + s->row_bytes + s->image_bytes + output_reserve - s->header_allowance))
    {
        error->code           = LV_ERROR_MEMORY_BUDGET;
        error->required_bytes = allowance + s->row_bytes + s->image_bytes + output_reserve;
        error->limit_bytes    = core->config.cpu_budget_bytes;
        return false;
    }
    s->row   = lv_workspace_allocate(core, r, s->row_bytes);
    s->image = lv_workspace_allocate(core, r, s->image_bytes);
    if (!s->row || !s->image)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    *canvas_out  = canvas;
    *reserve_out = output_reserve;
    return true;
}

static bool publish_metadata(lv_core *core, lv_request *r, jpeg_state *s, lv_error_info *error, const lv_file_version *version,
                             lv_size_u32 canvas)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    const uint64_t original          = (uint64_t)d->image_width * d->image_height * 4;
    lv_image_info info = {.format       = LV_FORMAT_JPEG,
                          .flags        = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN,
                          .source_size  = {d->image_width, d->image_height},
                          .file_bytes   = version->bytes,
                          .orientation  = s->orientation,
                          .capabilities = LV_CAP_SCALED_DECODE,
                          .load_mode =
                              original <= core->config.small_pixel_bytes && version->bytes <= core->config.small_file_bytes ?
                              LV_LOAD_SMALL :
                              LV_LOAD_INCREMENTAL,
                          .canvas    = canvas,
                          .region    = {0, 0, canvas.width, canvas.height},
                          .tile_edge = core->config.tile_edge};
    if (!lv_publish_info(core, r, &info, version))
    {
        error->code = LV_ERROR_INTERNAL;
        return false;
    }
    return true;
}

static void run_decode(lv_core *core, lv_request *r, jpeg_state *s, lv_error_info *error)
{
    lv_file_version version;
    lv_size_u32 canvas;
    uint64_t output_reserve = 0;
    if (!open_source(core, r, s, error, &version))
        return;
    read_header(s, &version);
    if (!prepare_output(core, r, s, error, &canvas, &output_reserve))
        return;
    if (!publish_metadata(core, r, s, error, &version, canvas))
        return;
    struct jpeg_decompress_struct *d = &s->decoder;
    if (lv_stopped(core, r))
        return;
    jpeg_start_decompress(d);
    if (!decode_rows(core, r, s, canvas))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    jpeg_finish_decompress(d);
    if (s->warning)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    if (!lv_file_unchanged(s->file, r->path, &version))
    {
        error->code = LV_ERROR_SOURCE_CHANGED;
        return;
    }
    // Codec has finished: release its allowance before queue backpressure can wait.
    jpeg_destroy_decompress(d);
    s->created = false;
    mtx_lock(&core->mutex);
    core->reserved -= r->reservation - output_reserve;
    r->reservation  = output_reserve;
    mtx_unlock(&core->mutex);
    if (!publish_tiles(core, r, s, canvas))
        error->code = s->failure ? s->failure : LV_ERROR_MEMORY_BUDGET;
}

void lv_decode_jpeg(lv_core *core, lv_request *r, lv_error_info *error)
{
    if (!lv_reserve(core, r, sizeof(jpeg_state)))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    jpeg_state *s = lv_workspace_allocate(core, r, sizeof *s);
    if (!s)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    *s = (jpeg_state){.orientation = LV_ORIENTATION_NORMAL, .core = core, .request = r};
    if (setjmp(s->escape))
        error->code = s->failure ? s->failure : LV_ERROR_CORRUPT_DATA;
    else
        run_decode(core, r, s, error);
    if (s->created)
        jpeg_destroy_decompress(&s->decoder);
    lv_file_close(s->file);
    lv_workspace_release(core, r, s->row, s->row_bytes);
    lv_workspace_release(core, r, s->image, s->image_bytes);
    lv_workspace_release(core, r, s, sizeof *s);
}
