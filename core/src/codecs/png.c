#include <png.h>
#include <stdlib.h>
#include <string.h>
#include "codec.h"

typedef struct png_state
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    lv_frame frame;
    png_structp decoder;
    png_infop metadata;
    uint8_t *image;
    size_t image_bytes;
    uint64_t remaining;
    lv_error_code failure;
} png_state;

static void png_failure(png_structp decoder, png_const_charp message)
{
    (void)message;
    png_longjmp(decoder, 1);
}
static void png_warning_ignore(png_structp decoder, png_const_charp message)
{
    (void)decoder;
    (void)message;
}
static void png_input(png_structp decoder, png_bytep out, size_t count)
{
    png_state *s = png_get_io_ptr(decoder);
    if (lv_stopped(s->core, s->request))
        png_error(decoder, "Cancelled");
    // Never consume bytes appended after the recorded file version.
    if (count > s->remaining)
        png_error(decoder, "Truncated PNG");
    size_t got              = fread(out, 1, count, s->input->file);
    s->remaining           -= got;
    s->request->bytes_read += got;
    if (got != count)
    {
        if (ferror(s->input->file))
            s->failure = LV_ERROR_IO;
        png_error(decoder, "PNG read failure");
    }
}

// APNG's default image is not necessarily its first animation frame. Reject it
// explicitly before invoking libpng, which otherwise ignores animation chunks.
static bool png_static_source(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    uint64_t at = 8;
    uint8_t chunk[8];
    while (at <= input->version.bytes && input->version.bytes - at >= 12)
    {
        if (lv_stopped(core, r))
            return false;
        if (fseek(input->file, (long)at, SEEK_SET))
        {
            error->code = LV_ERROR_IO;
            return false;
        }
        const size_t got  = fread(chunk, 1, 8, input->file);
        r->bytes_read    += got;
        if (got != 8)
        {
            error->code = ferror(input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
            return false;
        }
        const uint64_t length = (uint32_t)chunk[0] << 24 | (uint32_t)chunk[1] << 16 | (uint32_t)chunk[2] << 8 | chunk[3];
        if (!memcmp(chunk + 4, "acTL", 4))
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
        if (length > input->version.bytes - at - 12)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            return false;
        }
        at += length + 12;
        if (!memcmp(chunk + 4, "IEND", 4))
            break;
    }
    if (fseek(input->file, 0, SEEK_SET))
    {
        error->code = LV_ERROR_IO;
        return false;
    }
    return true;
}

static bool png_header(png_state *s, lv_error_info *error, lv_size_u32 *size)
{
    png_structp d  = s->decoder;
    png_infop info = s->metadata;
    png_set_read_fn(d, s, png_input);
    png_set_user_limits(d, 67108864, 67108864);
    png_set_chunk_cache_max(d, 128);
    png_set_chunk_malloc_max(d, 1024 * 1024);
    // Do not retain arbitrary metadata or compressed ICC/text chunks.
    png_set_keep_unknown_chunks(d, PNG_HANDLE_CHUNK_NEVER, nullptr, -1);
    png_set_crc_action(d, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_read_info(d, info);
    *size = (lv_size_u32){png_get_image_width(d, info), png_get_image_height(d, info)};
    if (!lv_bitmap_size(s->core, size->width, size->height, error))
        return false;
    const int depth = png_get_bit_depth(d, info), color = png_get_color_type(d, info);
    const bool transparency = png_get_valid(d, info, PNG_INFO_tRNS) != 0;
    if (depth == 16)
        png_set_scale_16(d);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(d);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(d);
    if (transparency)
        png_set_tRNS_to_alpha(d);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(d);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !transparency)
        png_set_add_alpha(d, 255, PNG_FILLER_AFTER);
    return true;
}

static void png_run(png_state *s, lv_error_info *error)
{
    lv_size_u32 size;
    if (!png_header(s, error, &size))
        return;
    const lv_size_u32 canvas = lv_fit_size(size, s->request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    s->image_bytes           = (size_t)size.width * size.height * 4;
    if (!lv_frame_prepare(&s->frame, size, canvas, LV_FORMAT_PNG, 0, s->image_bytes, error))
        return;
    s->image = lv_workspace_allocate(s->core, s->request, s->image_bytes);
    if (!s->image)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    const int passes = png_set_interlace_handling(s->decoder);
    png_read_update_info(s->decoder, s->metadata);
    if (png_get_rowbytes(s->decoder, s->metadata) != (size_t)size.width * 4)
        png_error(s->decoder, "Unexpected PNG row layout");
    if (!lv_frame_info(&s->frame, error))
        return;
    // Adam7 combines each pass into these initialized full-size rows.
    memset(s->image, 0, s->image_bytes);
    for (int pass = 0; pass < passes; ++pass)
        for (uint32_t y = 0; y < size.height; ++y)
        {
            if (lv_stopped(s->core, s->request))
                return;
            png_read_row(s->decoder, s->image + (size_t)y * size.width * 4, nullptr);
        }
    png_read_end(s->decoder, nullptr);
    if (!lv_resample_rgba(s->core, s->request, s->image, size, s->frame.data, canvas))
        return;
    png_destroy_read_struct(&s->decoder, &s->metadata, nullptr);
    lv_workspace_release(s->core, s->request, s->image, s->image_bytes);
    s->image = nullptr;
    lv_frame_finish(&s->frame, error);
}

void lv_decode_png(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    if (!png_static_source(core, r, input, error))
        return;
    const uint64_t allowance = sizeof(png_state) + UINT64_C(4) * 1024 * 1024 + input->version.bytes;
    if (!lv_reserve(core, r, allowance))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    png_state *s = lv_workspace_allocate(core, r, sizeof *s);
    if (!s)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return;
    }
    *s         = (png_state){.core      = core,
                             .request   = r,
                             .input     = input,
                             .remaining = input->version.bytes,
                             .frame     = {.core = core, .request = r, .input = input}};
    s->decoder = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, png_failure, png_warning_ignore);
    if (!s->decoder)
        error->code = LV_ERROR_OUT_OF_MEMORY;
    else if (setjmp(png_jmpbuf(s->decoder)))
        error->code = s->failure ? s->failure : LV_ERROR_CORRUPT_DATA;
    else
    {
        s->metadata = png_create_info_struct(s->decoder);
        if (!s->metadata)
            error->code = LV_ERROR_OUT_OF_MEMORY;
        else
            png_run(s, error);
    }
    if (s->decoder)
        png_destroy_read_struct(&s->decoder, &s->metadata, nullptr);
    lv_workspace_release(core, r, s->image, s->image_bytes);
    lv_frame_release(&s->frame);
    lv_workspace_release(core, r, s, sizeof *s);
}
