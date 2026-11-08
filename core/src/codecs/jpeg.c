#include "codec.h"

#include <jerror.h>
#include <jpeglib.h>
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
    lv_frame frame;
    lv_rows rows;
    bool reading_header;
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
    if (s->reading_header && s->request->bytes_read + bytes > UINT64_C(2) * 1024 * 1024)
    {
        s->failure = LV_ERROR_DIMENSION_LIMIT;
        longjmp(s->escape, 1);
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

static bool decode_rows(lv_core *core, lv_request *request, jpeg_state *s)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    while (d->output_scanline < d->output_height)
    {
        if (lv_stopped(core, request))
            return false;
        const uint32_t y = d->output_scanline;
        JSAMPROW row     = lv_rows_buffer(&s->rows, y);
        if (jpeg_read_scanlines(d, &row, 1) != 1)
            return false;
        lv_rows_push(&s->rows, y);
    }
    return s->rows.next_y == s->frame.info.region.height;
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
    s->remaining      = version->bytes;
    s->input          = (struct jpeg_source_mgr){.init_source       = input_init,
                                                 .fill_input_buffer = input_fill,
                                                 .skip_input_data   = input_skip,
                                                 .resync_to_restart = jpeg_resync_to_restart,
                                                 .term_source       = input_term};
    d->src            = &s->input;
    s->reading_header = true;
    jpeg_read_header(d, TRUE);
    s->reading_header = false;
    exif_marker(d);
}

static bool prepare_output(lv_request *r, jpeg_state *s, lv_error_info *error)
{
    struct jpeg_decompress_struct *d = &s->decoder;
    if (!d->image_width || !d->image_height || d->data_precision != 8)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    if (d->jpeg_color_space == JCS_CMYK || d->jpeg_color_space == JCS_YCCK)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    const lv_size_u32 source = {d->image_width, d->image_height};
    lv_size_u32 canvas;
    if (!lv_target_canvas(r, source, s->orientation, &canvas, error))
        return false;
    d->out_color_space = JCS_EXT_RGBA;
    for (unsigned n = 1; n <= 8; ++n)
    {
        d->scale_num   = n;
        d->scale_denom = 8;
        jpeg_calc_output_dimensions(d);
        if (d->output_width >= canvas.width && d->output_height >= canvas.height)
            break;
    }
    const uint64_t original = (uint64_t)source.width * source.height * 4;
    const uint64_t codec    = d->progressive_mode ? original * 2 : (uint64_t)d->image_width * 4 * 128;
    if (!lv_frame_prepare(&s->frame, source, canvas, LV_FORMAT_JPEG, LV_CAP_SCALED_DECODE | LV_CAP_REGION_DECODE,
                          codec + (uint64_t)d->output_width * 8, error))
        return false;
    s->frame.info.orientation = s->orientation;
    return lv_rows_init(&s->rows, &s->frame, (lv_size_u32){d->output_width, d->output_height}, error);
}

static void run_decode(lv_core *core, lv_request *r, jpeg_state *s, lv_input *input, lv_error_info *error)
{
    s->file  = input->file;
    s->frame = (lv_frame){.core = core, .request = r, .input = input};
    // Saved EXIF markers are bounded by the 2 MiB header-input limit.
    s->header_allowance = UINT64_C(8) * 1024 * 1024;
    if (!lv_reserve(core, r, s->header_allowance))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    read_header(s, &input->version);
    if (!prepare_output(r, s, error) || !lv_frame_info(&s->frame, error) || lv_stopped(core, r))
        return;
    jpeg_start_decompress(&s->decoder);
    if (!decode_rows(core, r, s) || !jpeg_finish_decompress(&s->decoder) || s->warning)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
    jpeg_destroy_decompress(&s->decoder);
    s->created = false;
    lv_rows_release(&s->rows);
    lv_frame_finish(&s->frame, error);
}

void lv_decode_jpeg(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
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
        run_decode(core, r, s, input, error);
    if (s->created)
        jpeg_destroy_decompress(&s->decoder);
    lv_rows_release(&s->rows);
    if (s->frame.core)
        lv_frame_release(&s->frame);
    lv_workspace_release(core, r, s, sizeof *s);
}
