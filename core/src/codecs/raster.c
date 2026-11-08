#include <math.h>
#include "codec.h"

bool lv_target_canvas(lv_request *r, lv_size_u32 source, lv_orientation orientation, lv_size_u32 *canvas, lv_error_info *error)
{
    if (!source.width || !source.height)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    if (r->desc.target.kind == LV_TARGET_FIT)
        *canvas = lv_fit_size(source, r->desc.target.as.fit_bounds, orientation);
    else
    {
        *canvas          = r->desc.target.as.region.canvas;
        const uint64_t a = (uint64_t)source.width * canvas->height, b = (uint64_t)source.height * canvas->width;
        const uint64_t diff = a > b ? a - b : b - a, tolerance = source.width > source.height ? source.width : source.height;
        if (diff > tolerance)
        {
            error->code = LV_ERROR_TARGET_UNSUPPORTED;
            return false;
        }
    }
    return true;
}
lv_axis_sample lv_sample_axis(uint32_t at, uint32_t source, uint32_t canvas)
{
    double p = ((double)at + 0.5) * (double)source / (double)canvas - 0.5;
    if (p < 0)
        p = 0;
    if (p > (double)source - 1)
        p = (double)source - 1;
    const uint32_t a = (uint32_t)p;
    return (lv_axis_sample){a, a + 1 < source ? a + 1 : a, (uint32_t)((p - a) * 65536)};
}
uint32_t lv_sample_nearest(uint32_t at, uint32_t source, uint32_t canvas)
{
    const double p = ((double)at + 0.5) * (double)source / (double)canvas;
    return p >= source ? source - 1 : (uint32_t)p;
}
static void filter_row(lv_rows *rows, uint32_t local_y, lv_axis_sample sy)
{
    lv_frame *frame        = rows->frame;
    const lv_rect_u32 rect = frame->info.region;
    const uint8_t *top     = rows->data + (size_t)(sy.first & 1u) * rows->source.width * 4;
    const uint8_t *bottom  = rows->data + (size_t)(sy.second & 1u) * rows->source.width * 4;
    uint8_t *out           = nullptr;
    uint32_t remaining     = 0;
    for (uint32_t x = 0; x < rect.width; ++x, out += 4, --remaining)
    {
        if (!remaining)
            out = lv_frame_span(frame, x, local_y, &remaining);
        const lv_axis_sample sx = lv_sample_axis(rect.x + x, rows->source.width, frame->info.canvas.width);
        const uint8_t *p[4]     = {top + (size_t)sx.first * 4, top + (size_t)sx.second * 4, bottom + (size_t)sx.first * 4,
                                   bottom + (size_t)sx.second * 4};
        const uint64_t fx = sx.fraction, fy = sy.fraction;
        const uint64_t weights[4] = {(65536 - fx) * (65536 - fy), fx * (65536 - fy), (65536 - fx) * fy, fx * fy};
        uint64_t alpha            = 0;
        for (unsigned i = 0; i < 4; ++i)
            alpha += weights[i] * p[i][3];
        for (unsigned c = 0; c < 3; ++c)
        {
            uint64_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value += weights[i] * p[i][3] * p[i][c];
            out[c] = (uint8_t)(alpha ? (value + alpha / 2) / alpha : 0);
        }
        out[3] = (uint8_t)((alpha + (UINT64_C(1) << 31)) >> 32);
    }
}
bool lv_rows_init(lv_rows *rows, lv_frame *frame, lv_size_u32 source, lv_error_info *error)
{
    const uint64_t bytes = (uint64_t)source.width * 8;
    if (bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    *rows      = (lv_rows){.frame = frame, .source = source, .bytes = (size_t)bytes};
    rows->data = lv_workspace_allocate(frame->core, frame->request, rows->bytes);
    if (!rows->data)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    return true;
}
uint8_t *lv_rows_buffer(lv_rows *rows, uint32_t y)
{
    return rows->data + (size_t)(y & 1u) * rows->source.width * 4;
}
void lv_rows_push(lv_rows *rows, uint32_t y)
{
    const lv_image_info *info = &rows->frame->info;
    while (rows->next_y < info->region.height)
    {
        lv_axis_sample sy = lv_sample_axis(info->region.y + rows->next_y, rows->source.height, info->canvas.height);
        if (sy.second > y)
            break;
        filter_row(rows, rows->next_y++, sy);
    }
}
void lv_rows_release(lv_rows *rows)
{
    if (rows->frame)
        lv_workspace_release(rows->frame->core, rows->frame->request, rows->data, rows->bytes);
    rows->data = nullptr;
}
bool lv_raster_read(lv_frame *frame, lv_size_u32 source, lv_read_row_fn read, void *userdata, lv_error_info *error)
{
    lv_rows rows;
    if (!lv_rows_init(&rows, frame, source, error))
        return false;
    uint32_t loaded[2] = {UINT32_MAX, UINT32_MAX};
    bool success       = true;
    for (uint32_t y = 0; y < frame->info.region.height; ++y)
    {
        if (lv_stopped(frame->core, frame->request))
        {
            success = false;
            break;
        }
        const lv_axis_sample sy = lv_sample_axis(frame->info.region.y + y, source.height, frame->info.canvas.height);
        const uint32_t lines[2] = {sy.first, sy.second};
        for (unsigned i = 0; i < 2; ++i)
        {
            const uint32_t line = lines[i], slot = line & 1u;
            if (loaded[slot] != line)
            {
                if (!read(userdata, line, lv_rows_buffer(&rows, line), error))
                {
                    success = false;
                    break;
                }
                loaded[slot] = line;
            }
        }
        if (!success)
            break;
        filter_row(&rows, y, sy);
    }
    lv_rows_release(&rows);
    return success;
}
