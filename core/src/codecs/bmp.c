#include <string.h>
#include "codec.h"

typedef struct bmp_header
{
    lv_size_u32 size;
    uint32_t offset, compression, colors, masks[4];
    uint16_t depth;
    size_t palette, palette_stride, row_bytes;
    bool top_down;
} bmp_header;

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}
static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool valid_masks(const bmp_header *h)
{
    uint32_t used = 0;
    for (unsigned i = 0; i < 4; ++i)
    {
        uint32_t mask = h->masks[i];
        if (!mask && i < 3)
            return false;
        if (used & mask || (h->depth == 16 && (mask >> 16)))
            return false;
        used |= mask;
        if (mask)
        {
            while (!(mask & 1u))
                mask >>= 1;
            if (mask & (mask + 1u))
                return false;
        }
    }
    return true;
}

static bool bmp_header_read(const uint8_t *data, size_t bytes, bmp_header *h, lv_error_info *error)
{
    if (bytes < 26)
        return false;
    const uint32_t dib = u32(data + 14);
    h->offset          = u32(data + 10);
    if (dib != 12 && dib != 40 && dib != 52 && dib != 56 && dib != 108 && dib != 124)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if (dib > bytes - 14 || h->offset > bytes || h->offset < 14 + dib)
        return false;
    const bool core           = dib == 12;
    const uint32_t width      = core ? u16(data + 18) : u32(data + 18);
    const uint32_t raw_height = core ? u16(data + 20) : u32(data + 22);
    h->top_down               = !core && (raw_height & UINT32_C(0x80000000));
    if (!width || width > INT32_MAX || !raw_height || raw_height == UINT32_C(0x80000000))
        return false;
    h->size = (lv_size_u32){width, h->top_down ? 0u - raw_height : raw_height};
    if (u16(data + (core ? 22 : 26)) != 1)
        return false;
    h->depth       = u16(data + (core ? 24 : 28));
    h->compression = core ? 0 : u32(data + 30);
    h->colors      = core ? 0 : u32(data + 46);
    if (h->depth != 1 && h->depth != 4 && h->depth != 8 && h->depth != 16 && h->depth != 24 && h->depth != 32)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if (h->compression != 0 && h->compression != 1 && h->compression != 2 && h->compression != 3 && h->compression != 6)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if ((h->compression == 1 && h->depth != 8) || (h->compression == 2 && h->depth != 4) ||
        ((h->compression == 3 || h->compression == 6) && h->depth != 16 && h->depth != 32) ||
        (h->top_down && (h->compression == 1 || h->compression == 2)))
        return false;
    h->palette        = 14 + dib;
    h->palette_stride = core ? 3 : 4;
    if (h->compression == 3 || h->compression == 6)
    {
        const unsigned count = h->compression == 6 ? 4 : 3;
        const size_t at      = 14 + 40;
        if ((dib == 40 && count * 4 > h->offset - at) || (dib != 40 && dib < 40 + count * 4))
            return false;
        for (unsigned i = 0; i < count; ++i)
            h->masks[i] = u32(data + at + i * 4);
        if (dib >= 56)
            h->masks[3] = u32(data + at + 12);
        if (dib == 40)
            h->palette += count * 4;
    }
    else if (h->depth == 16)
    {
        h->masks[0] = 0x7c00;
        h->masks[1] = 0x03e0;
        h->masks[2] = 0x001f;
    }
    else if (h->depth == 32)
    {
        h->masks[0] = 0xff0000;
        h->masks[1] = 0x00ff00;
        h->masks[2] = 0x0000ff;
        // BI_RGB's fourth byte is reserved, including when every byte is zero.
    }
    if ((h->depth == 16 || h->depth == 32) && !valid_masks(h))
        return false;
    if (h->depth <= 8)
    {
        const uint32_t maximum = 1u << h->depth;
        if (!h->colors)
            h->colors = maximum;
        if (h->colors > maximum || h->palette > h->offset || h->colors > (h->offset - h->palette) / h->palette_stride)
            return false;
    }
    const uint64_t row = (((uint64_t)width * h->depth + 31) / 32) * 4;
    if (row > SIZE_MAX)
        return false;
    h->row_bytes = (size_t)row;
    if (h->compression != 1 && h->compression != 2 && (uint64_t)h->size.height > (bytes - h->offset) / row)
        return false;
    return true;
}

static uint8_t channel(uint32_t pixel, uint32_t mask)
{
    if (!mask)
        return 255;
    while (!(mask & 1u))
    {
        mask  >>= 1;
        pixel >>= 1;
    }
    return (uint8_t)(((uint64_t)(pixel & mask) * 255 + mask / 2) / mask);
}
static bool palette_pixel(const uint8_t *data, const bmp_header *h, uint32_t index, uint8_t *out)
{
    if (index >= h->colors)
        return false;
    const uint8_t *p = data + h->palette + index * h->palette_stride;
    out[0]           = p[2];
    out[1]           = p[1];
    out[2]           = p[0];
    out[3]           = 255;
    return true;
}

static bool bmp_rows(lv_core *core, lv_request *r, const uint8_t *data, const bmp_header *h, uint8_t *out)
{
    for (uint32_t y = 0; y < h->size.height; ++y)
    {
        if (lv_stopped(core, r))
            return false;
        const uint32_t row = h->top_down ? y : h->size.height - y - 1;
        const uint8_t *p   = data + h->offset + (size_t)row * h->row_bytes;
        for (uint32_t x = 0; x < h->size.width; ++x, out += 4)
        {
            if (h->depth <= 8)
            {
                const uint32_t bit   = x * h->depth;
                const uint32_t index = ((uint32_t)p[bit / 8] >> (8 - h->depth - bit % 8)) & ((1u << h->depth) - 1);
                if (!palette_pixel(data, h, index, out))
                    return false;
            }
            else if (h->depth == 24)
            {
                out[0] = p[x * 3 + 2];
                out[1] = p[x * 3 + 1];
                out[2] = p[x * 3];
                out[3] = 255;
            }
            else
            {
                const uint32_t pixel = h->depth == 16 ? u16(p + x * 2) : u32(p + x * 4);
                for (unsigned c = 0; c < 4; ++c)
                    out[c] = channel(pixel, h->masks[c]);
            }
        }
    }
    return true;
}

static bool rle_put(const uint8_t *data, const bmp_header *h, uint8_t *image, uint32_t x, uint32_t y, unsigned index)
{
    if (x >= h->size.width || y >= h->size.height)
        return false;
    return palette_pixel(data, h, index, image + ((size_t)(h->size.height - y - 1) * h->size.width + x) * 4);
}

static bool bmp_rle(lv_core *core, lv_request *r, const uint8_t *data, size_t bytes, const bmp_header *h, uint8_t *image)
{
    // Skipped pixels in RLE are palette entry zero, not transparent uninitialized memory.
    uint8_t background[4];
    if (!palette_pixel(data, h, 0, background))
        return false;
    for (size_t i = 0, count = (size_t)h->size.width * h->size.height; i < count; ++i)
    {
        if (!(i & 16383u) && lv_stopped(core, r))
            return false;
        memcpy(image + i * 4, background, 4);
    }
    uint32_t x = 0, y = 0;
    size_t at = h->offset;
    while (bytes - at >= 2)
    {
        if (lv_stopped(core, r))
            return false;
        const unsigned count = data[at++], value = data[at++];
        if (count)
        {
            if (x > h->size.width || count > h->size.width - x)
                return false;
            for (unsigned i = 0; i < count; ++i)
            {
                unsigned index = h->depth == 8 ? value : (i & 1u) ? value & 15u : value >> 4;
                if (!rle_put(data, h, image, x++, y, index))
                    return false;
            }
        }
        else if (value == 0)
        {
            x = 0;
            if (y >= h->size.height)
                return false;
            ++y;
        }
        else if (value == 1)
            return true;
        else if (value == 2)
        {
            if (bytes - at < 2 || x > h->size.width || y >= h->size.height)
                return false;
            const uint32_t dx = data[at++], dy = data[at++];
            if (dx > h->size.width - x || dy >= h->size.height - y)
                return false;
            x += dx;
            y += dy;
        }
        else
        {
            if (x > h->size.width || value > h->size.width - x)
                return false;
            size_t packed       = h->depth == 8 ? value : (value + 1u) / 2;
            const size_t padded = (packed + 1) & ~(size_t)1;
            if (padded > bytes - at)
                return false;
            for (unsigned i = 0; i < value; ++i)
            {
                unsigned index = h->depth == 8 ? data[at + i] : (i & 1u) ? data[at + i / 2] & 15u : data[at + i / 2] >> 4;
                if (!rle_put(data, h, image, x++, y, index))
                    return false;
            }
            at += padded;
        }
    }
    return false; // End-of-bitmap escape is mandatory for an RLE stream.
}

void lv_decode_bmp(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    uint8_t *data = lv_input_read_all(core, r, input, error);
    if (!data)
        return;
    lv_frame f         = {.core = core, .request = r, .input = input};
    bmp_header h       = {0};
    uint8_t *image     = nullptr;
    size_t image_bytes = 0;
    error->code        = LV_ERROR_CORRUPT_DATA;
    if (!bmp_header_read(data, (size_t)input->version.bytes, &h, error))
        goto done;
    error->code = 0;
    if (!lv_bitmap_size(core, h.size.width, h.size.height, error))
        goto done;
    image_bytes        = (size_t)h.size.width * h.size.height * 4;
    lv_size_u32 canvas = lv_fit_size(h.size, r->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    if (!lv_frame_prepare(&f, h.size, canvas, LV_FORMAT_BMP, 0, image_bytes, error))
        goto done;
    image = lv_workspace_allocate(core, r, image_bytes);
    if (!image)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (!lv_frame_info(&f, error))
        goto done;
    bool decoded = h.compression == 1 || h.compression == 2 ? bmp_rle(core, r, data, (size_t)input->version.bytes, &h, image) :
                                                              bmp_rows(core, r, data, &h, image);
    if (!decoded)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    if (!lv_resample_rgba(core, r, image, h.size, f.data, canvas))
        goto done;
    lv_workspace_release(core, r, image, image_bytes);
    image = nullptr;
    lv_workspace_release(core, r, data, (size_t)input->version.bytes + 1);
    data = nullptr;
    lv_frame_finish(&f, error);
done:
    lv_workspace_release(core, r, image, image_bytes);
    lv_workspace_release(core, r, data, (size_t)input->version.bytes + 1);
    lv_frame_release(&f);
}
