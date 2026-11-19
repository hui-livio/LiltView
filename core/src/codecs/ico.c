#include <png.h>
#include <string.h>
#include "codec.h"

typedef struct ico_entry
{
    uint32_t width, height;
    uint32_t bytes, offset;
    uint16_t bits;
    uint16_t hotspot_x, hotspot_y;
    bool png;
} ico_entry;

typedef struct ico_png_input
{
    const uint8_t *data;
    size_t bytes, offset;
} ico_png_input;

static uint16_t ico_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

static uint32_t ico_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint32_t ico_abs32(const uint8_t *data)
{
    const int32_t value = (int32_t)ico_le32(data);
    return value < 0 ? (uint32_t)(-(int64_t)value) : (uint32_t)value;
}

static void ico_png_read(png_structp decoder, png_bytep output, size_t count)
{
    ico_png_input *input = png_get_io_ptr(decoder);
    if (count > input->bytes - input->offset)
        png_error(decoder, "truncated embedded PNG");
    memcpy(output, input->data + input->offset, count);
    input->offset += count;
}

static bool ico_png_static(const uint8_t *data, size_t bytes, lv_error_info *error)
{
    if (bytes < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    size_t at = 8;
    while (at <= bytes && bytes - at >= 12)
    {
        const uint32_t length = (uint32_t)data[at] << 24 | (uint32_t)data[at + 1] << 16 | (uint32_t)data[at + 2] << 8 |
            data[at + 3];
        if ((uint64_t)length > bytes - at - 12)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            return false;
        }
        if (!memcmp(data + at + 4, "acTL", 4))
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
        const bool end = !memcmp(data + at + 4, "IEND", 4);
        at += (size_t)length + 12;
        if (end)
            return at == bytes;
    }
    error->code = LV_ERROR_CORRUPT_DATA;
    return false;
}

static bool ico_png_decode(lv_core *core, lv_request *request, const uint8_t *data, size_t bytes, uint8_t **output,
                           lv_size_u32 *size, lv_error_info *error)
{
    if (!ico_png_static(data, bytes, error))
        return false;
    png_structp decoder = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info      = decoder ? png_create_info_struct(decoder) : nullptr;
    ico_png_input input = {data, bytes, 0};
    if (!decoder || !info)
    {
        if (decoder)
            png_destroy_read_struct(&decoder, &info, nullptr);
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    if (setjmp(png_jmpbuf(decoder)))
    {
        png_destroy_read_struct(&decoder, &info, nullptr);
        if (*output)
        {
            lv_workspace_release(core, request, *output, (size_t)((uint64_t)size->width * size->height * 4));
            *output = nullptr;
        }
        error->code = lv_stopped(core, request) ? 0 : LV_ERROR_CORRUPT_DATA;
        return false;
    }
    png_set_read_fn(decoder, &input, ico_png_read);
    png_set_user_limits(decoder, PNG_UINT_31_MAX, PNG_UINT_31_MAX);
    png_set_crc_action(decoder, PNG_CRC_ERROR_QUIT, PNG_CRC_ERROR_QUIT);
    png_read_info(decoder, info);
    *size = (lv_size_u32){png_get_image_width(decoder, info), png_get_image_height(decoder, info)};
    if (!lv_bitmap_size(core, size->width, size->height, error))
    {
        png_destroy_read_struct(&decoder, &info, nullptr);
        return false;
    }
    const int depth = png_get_bit_depth(decoder, info), color = png_get_color_type(decoder, info);
    const bool transparency = png_get_valid(decoder, info, PNG_INFO_tRNS) != 0;
    if (depth == 16)
        png_set_scale_16(decoder);
    if (color == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(decoder);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8)
        png_set_expand_gray_1_2_4_to_8(decoder);
    if (transparency)
        png_set_tRNS_to_alpha(decoder);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(decoder);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !transparency)
        png_set_add_alpha(decoder, 255, PNG_FILLER_AFTER);
    png_set_interlace_handling(decoder);
    png_read_update_info(decoder, info);
    if (png_get_rowbytes(decoder, info) != (size_t)size->width * 4)
        png_error(decoder, "unexpected embedded PNG row layout");
    const size_t image_bytes = (size_t)((uint64_t)size->width * size->height * 4);
    if (!lv_reserve(core, request, image_bytes))
    {
        png_destroy_read_struct(&decoder, &info, nullptr);
        error->code = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    *output = lv_workspace_allocate(core, request, image_bytes);
    if (!*output)
    {
        png_destroy_read_struct(&decoder, &info, nullptr);
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    for (uint32_t y = 0; y < size->height; ++y)
    {
        if (lv_stopped(core, request))
        {
            png_destroy_read_struct(&decoder, &info, nullptr);
            return false;
        }
        png_read_row(decoder, *output + (size_t)y * size->width * 4, nullptr);
    }
    png_read_end(decoder, nullptr);
    png_destroy_read_struct(&decoder, &info, nullptr);
    return true;
}

static bool ico_dib_decode(lv_core *core, lv_request *request, const uint8_t *data, size_t bytes, const ico_entry *entry,
                           uint8_t **output, lv_size_u32 *size, lv_error_info *error)
{
    if (bytes < 40)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    const uint32_t header = ico_le32(data), width = ico_abs32(data + 4), total_height = ico_abs32(data + 8);
    const uint16_t planes = ico_le16(data + 12), bits = ico_le16(data + 14);
    const uint32_t compression = ico_le32(data + 16), colors = ico_le32(data + 32);
    if (header < 40 || width != entry->width || total_height < 2 || total_height & 1u || planes != 1 || !width)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    if (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if (compression != 0)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    size->width  = width;
    size->height = total_height / 2;
    if (!lv_bitmap_size(core, size->width, size->height, error))
        return false;
    const uint32_t palette_count = bits <= 8 ? (colors ? colors : 1u << bits) : 0;
    const uint64_t palette_bytes = (uint64_t)palette_count * 4;
    const uint64_t header_bytes  = header + palette_bytes;
    const uint64_t xor_stride    = ((uint64_t)width * bits + 31) / 32 * 4;
    const uint64_t mask_stride   = ((uint64_t)width + 31) / 32 * 4;
    const uint64_t required      = header_bytes + xor_stride * size->height + mask_stride * size->height;
    if (header_bytes > bytes || required > bytes)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    const size_t image_bytes = (size_t)((uint64_t)size->width * size->height * 4);
    if (!lv_reserve(core, request, image_bytes))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    *output = lv_workspace_allocate(core, request, image_bytes);
    if (!*output)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    const uint8_t *palette = data + header;
    const uint8_t *xor_data = data + header_bytes, *mask_data = xor_data + xor_stride * size->height;
    bool any_alpha = false;
    for (uint32_t y = 0; y < size->height; ++y)
    {
        if (lv_stopped(core, request))
            return false;
        const uint32_t source_y = size->height - y - 1;
        const uint8_t *row      = xor_data + (size_t)source_y * xor_stride;
        const uint8_t *mask     = mask_data + (size_t)source_y * mask_stride;
        for (uint32_t x = 0; x < size->width; ++x)
        {
            uint8_t *pixel = *output + ((size_t)y * size->width + x) * 4;
            if (bits <= 8)
            {
                const unsigned bit = x * bits;
                const unsigned index = bits == 8 ? row[x] : (row[bit / 8] >> (8 - bits - bit % 8)) & ((1u << bits) - 1);
                if (index >= palette_count)
                {
                    error->code = LV_ERROR_CORRUPT_DATA;
                    return false;
                }
                pixel[0] = palette[index * 4 + 2];
                pixel[1] = palette[index * 4 + 1];
                pixel[2] = palette[index * 4];
                pixel[3] = 255;
            }
            else if (bits == 24)
            {
                const uint8_t *source = row + x * 3;
                pixel[0]               = source[2];
                pixel[1]               = source[1];
                pixel[2]               = source[0];
                pixel[3]               = 255;
            }
            else
            {
                const uint8_t *source = row + x * 4;
                pixel[0]               = source[2];
                pixel[1]               = source[1];
                pixel[2]               = source[0];
                pixel[3]               = source[3];
                any_alpha |= source[3] != 0;
            }
            if (mask[x / 8] & (0x80u >> (x % 8)))
                pixel[3] = 0;
        }
    }
    if (bits == 32 && !any_alpha)
        for (uint32_t y = 0; y < size->height; ++y)
            for (uint32_t x = 0; x < size->width; ++x)
                if (!(mask_data[(size->height - y - 1) * mask_stride + x / 8] & (0x80u >> (x % 8))))
                    (*output + ((size_t)y * size->width + x) * 4)[3] = 255;
    return true;
}

static bool ico_entry_read(const uint8_t *data, size_t bytes, uint16_t type, size_t at, ico_entry *entry)
{
    const uint32_t width = data[at] ? data[at] : 256, height = data[at + 1] ? data[at + 1] : 256;
    const uint32_t length = ico_le32(data + at + 8), offset = ico_le32(data + at + 12);
    if (!width || !height || !length || offset > bytes || length > bytes - offset)
        return false;
    *entry = (ico_entry){.width = width,
                         .height = height,
                         .bytes = length,
                         .offset = offset,
                         .bits = type == 2 ? 32 : ico_le16(data + at + 6),
                         .hotspot_x = type == 2 ? ico_le16(data + at + 4) : 0,
                         .hotspot_y = type == 2 ? ico_le16(data + at + 6) : 0,
                         .png = length >= 8 && !memcmp(data + offset, "\x89PNG\r\n\x1a\n", 8)};
    if (type == 2 && (entry->hotspot_x >= width || entry->hotspot_y >= height))
        return false;
    return true;
}

static bool ico_better(const ico_entry *candidate, const ico_entry *best, lv_request *request)
{
    const uint32_t bound = request->desc.target.kind == LV_TARGET_NATIVE ? UINT32_MAX :
        request->desc.target.as.fit_bounds.width > request->desc.target.as.fit_bounds.height ?
        request->desc.target.as.fit_bounds.width : request->desc.target.as.fit_bounds.height;
    const uint32_t candidate_edge = candidate->width > candidate->height ? candidate->width : candidate->height;
    const uint32_t best_edge = best->width > best->height ? best->width : best->height;
    const bool candidate_cover = candidate_edge >= bound, best_cover = best_edge >= bound;
    if (candidate_cover != best_cover)
        return candidate_cover;
    if (candidate_cover && candidate_edge != best_edge)
        return candidate_edge < best_edge;
    if (!candidate_cover && candidate_edge != best_edge)
        return candidate_edge > best_edge;
    const uint64_t candidate_area = (uint64_t)candidate->width * candidate->height;
    const uint64_t best_area      = (uint64_t)best->width * best->height;
    return candidate_area > best_area || (candidate_area == best_area && candidate->bits > best->bits);
}

void lv_decode_ico(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    if (input->version.bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    uint8_t *data = lv_input_read_all(core, request, input, error);
    if (!data)
        return;
    const size_t bytes = (size_t)input->version.bytes;
    const uint16_t type = bytes >= 4 ? ico_le16(data + 2) : 0;
    const uint16_t count = bytes >= 6 ? ico_le16(data + 4) : 0;
    lv_frame frame       = {.core = core, .request = request, .input = input};
    ico_entry selected   = {0};
    uint8_t *source      = nullptr;
    lv_size_u32 size     = {0, 0};
    if (bytes < 6 || (type != 1 && type != 2) || !count || count > 1024 || bytes < 6 + (size_t)count * 16)
        error->code = LV_ERROR_CORRUPT_DATA;
    else
    {
        for (uint16_t i = 0; i < count; ++i)
        {
            ico_entry candidate;
            if (ico_entry_read(data, bytes, type, 6 + (size_t)i * 16, &candidate) &&
                (!selected.bytes || ico_better(&candidate, &selected, request)))
                selected = candidate;
        }
        if (!selected.bytes)
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        else if (selected.png)
            ico_png_decode(core, request, data + selected.offset, selected.bytes, &source, &size, error);
        else
            ico_dib_decode(core, request, data + selected.offset, selected.bytes, &selected, &source, &size, error);
        if (source && !error->code && !lv_stopped(core, request))
        {
            const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
                size : lv_fit_size(size, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
            const lv_format format = type == 2 ? LV_FORMAT_CUR : LV_FORMAT_ICO;
            if (lv_frame_prepare(&frame, size, canvas, format, 0, 0, error) && lv_frame_info(&frame, error) &&
                lv_resample_rgba(core, request, source, size, frame.data, canvas))
                lv_frame_finish(&frame, error);
        }
    }
    if (source)
        lv_workspace_release(core, request, source, (size_t)((uint64_t)size.width * size.height * 4));
    lv_frame_release(&frame);
    lv_workspace_release(core, request, data, bytes + 1);
}
