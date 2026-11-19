#include <string.h>
#include "codec.h"

static uint32_t qoi_be32(const uint8_t *data)
{
    return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3];
}

static unsigned qoi_hash(const uint8_t pixel[4])
{
    return (pixel[0] * 3u + pixel[1] * 5u + pixel[2] * 7u + pixel[3] * 11u) & 63u;
}

static bool qoi_decode_pixels(lv_core *core, lv_request *request, const uint8_t *data, size_t bytes, uint8_t *output,
                              uint32_t width, uint32_t height)
{
    static const uint8_t end_marker[] = {0, 0, 0, 0, 0, 0, 0, 1};
    const size_t end = bytes - sizeof end_marker;
    if (bytes < 14 + sizeof end_marker || memcmp(data + end, end_marker, sizeof end_marker))
        return false;

    uint8_t index[64][4] = {{0}};
    uint8_t pixel[4]      = {0, 0, 0, 255};
    const size_t count    = (size_t)width * height;
    size_t at             = 14;
    size_t written        = 0;
    unsigned run           = 0;
    while (written < count)
    {
        if ((written & 16383u) == 0 && lv_stopped(core, request))
            return false;
        if (run)
            --run;
        else
        {
            if (at >= end)
                return false;
            const uint8_t op = data[at++];
            if (op == 0xfe)
            {
                if (end - at < 3)
                    return false;
                pixel[0] = data[at++];
                pixel[1] = data[at++];
                pixel[2] = data[at++];
            }
            else if (op == 0xff)
            {
                if (end - at < 4)
                    return false;
                pixel[0] = data[at++];
                pixel[1] = data[at++];
                pixel[2] = data[at++];
                pixel[3] = data[at++];
            }
            else
            {
                switch (op & 0xc0u)
                {
                case 0x00u:
                    memcpy(pixel, index[op & 63u], sizeof pixel);
                    break;
                case 0x40u:
                    pixel[0] = (uint8_t)(pixel[0] + ((op >> 4) & 3u) - 2u);
                    pixel[1] = (uint8_t)(pixel[1] + ((op >> 2) & 3u) - 2u);
                    pixel[2] = (uint8_t)(pixel[2] + (op & 3u) - 2u);
                    break;
                case 0x80u:
                {
                    if (at >= end)
                        return false;
                    const uint8_t next = data[at++];
                    const int green    = (int)(op & 63u) - 32;
                    const int red_green = (int)(next >> 4) - 8;
                    const int blue_green = (int)(next & 15u) - 8;
                    pixel[0]             = (uint8_t)(pixel[0] + green + red_green);
                    pixel[1]             = (uint8_t)(pixel[1] + green);
                    pixel[2]             = (uint8_t)(pixel[2] + green + blue_green);
                    break;
                }
                default:
                    run = op & 63u;
                    if (run >= count - written)
                        return false;
                    break;
                }
            }
            if (op < 0xc0u || op >= 0xfeu)
                memcpy(index[qoi_hash(pixel)], pixel, sizeof pixel);
        }
        memcpy(output + written * 4, pixel, sizeof pixel);
        ++written;
    }
    return at == end;
}

void lv_decode_qoi(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
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
    lv_frame frame      = {.core = core, .request = request, .input = input};
    uint8_t *source     = nullptr;
    lv_size_u32 size    = {0, 0};
    if (bytes < 14 || memcmp(data, "qoif", 4))
        error->code = LV_ERROR_CORRUPT_DATA;
    else if (data[12] != 3 && data[12] != 4)
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
    else if (data[13] > 1)
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
    else
    {
        size = (lv_size_u32){qoi_be32(data + 4), qoi_be32(data + 8)};
        if (lv_bitmap_size(core, size.width, size.height, error))
        {
            const uint64_t source_bytes = (uint64_t)size.width * size.height * 4;
            if (!lv_reserve(core, request, source_bytes))
                error->code = LV_ERROR_MEMORY_BUDGET;
            else if (!(source = lv_workspace_allocate(core, request, (size_t)source_bytes)))
                error->code = LV_ERROR_OUT_OF_MEMORY;
            else if (!qoi_decode_pixels(core, request, data, bytes, source, size.width, size.height))
            {
                if (!lv_stopped(core, request))
                    error->code = LV_ERROR_CORRUPT_DATA;
            }
            else
            {
                const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
                    size : lv_fit_size(size, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
                if (lv_frame_prepare(&frame, size, canvas, LV_FORMAT_QOI, 0, 0, error) &&
                    lv_frame_info(&frame, error) && lv_resample_rgba(core, request, source, size, frame.data, canvas))
                    lv_frame_finish(&frame, error);
            }
        }
    }
    if (source)
        lv_workspace_release(core, request, source, (size_t)((uint64_t)size.width * size.height * 4));
    lv_frame_release(&frame);
    lv_workspace_release(core, request, data, bytes + 1);
}
