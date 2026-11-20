#include <string.h>
#include "codec.h"

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return false;
    *out = a + b;
    return true;
}

typedef struct raw_row_context
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    const lv_raw_layout_desc *layout;
    const lv_raw_processing *processing;
} raw_row_context;

static uint8_t *raw_read_bytes(raw_row_context *context, const lv_raw_plane *plane, uint32_t row, size_t *size,
                               lv_error_info *error)
{
    const uint32_t physical_y = context->layout->row_order == LV_RAW_TOP_DOWN ? row : plane->height - 1 - row;
    const uint64_t row_offset = (uint64_t)physical_y * plane->stride;
    if (plane->offset > UINT64_MAX - row_offset)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return nullptr;
    }
    const uint64_t offset = plane->offset + row_offset;
    if (!lv_file_seek(context->input->file, offset))
    {
        error->code = LV_ERROR_IO;
        return nullptr;
    }
    *size = (size_t)plane->row_bytes;
    uint8_t *bytes = lv_workspace_allocate(context->core, context->request, *size);
    if (!bytes)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return nullptr;
    }
    const size_t count = fread(bytes, 1, *size, context->input->file);
    context->request->bytes_read += count;
    if (count != *size)
    {
        lv_workspace_release(context->core, context->request, bytes, *size);
        error->code = ferror(context->input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        return nullptr;
    }
    return bytes;
}

static uint8_t clamp_u8(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void yuv601_to_rgb(uint8_t y, uint8_t u, uint8_t v, lv_raw_color_range range, uint8_t *rgba)
{
    const int d = (int)u - 128;
    const int e = (int)v - 128;
    int r, g, b;
    if (range == LV_RAW_RANGE_LIMITED)
    {
        const int c = (int)y - 16;
        r = (298 * c + 409 * e + 128) >> 8;
        g = (298 * c - 100 * d - 208 * e + 128) >> 8;
        b = (298 * c + 516 * d + 128) >> 8;
    }
    else
    {
        r = (int)y + ((359 * e + 128) >> 8);
        g = (int)y - ((88 * d + 183 * e + 128) >> 8);
        b = (int)y + ((454 * d + 128) >> 8);
    }
    rgba[0] = clamp_u8(r);
    rgba[1] = clamp_u8(g);
    rgba[2] = clamp_u8(b);
    rgba[3] = 255;
}

static bool raw_read_row(void *userdata, uint32_t y, uint8_t *rgba, lv_error_info *error)
{
    raw_row_context *context = userdata;
    const lv_raw_plane *plane = &context->layout->planes[0];
    size_t row_bytes = 0;
    uint8_t *row = raw_read_bytes(context, plane, y, &row_bytes, error);
    if (!row)
        return false;
    const uint32_t width = context->layout->size.width;
    switch (context->layout->layout)
    {
    case LV_RAW_LAYOUT_RGB24:
    case LV_RAW_LAYOUT_BGR24:
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *pixel = row + (size_t)x * 3;
            const bool bgr = context->layout->layout == LV_RAW_LAYOUT_BGR24;
            rgba[x * 4 + 0] = pixel[bgr ? 2 : 0];
            rgba[x * 4 + 1] = pixel[1];
            rgba[x * 4 + 2] = pixel[bgr ? 0 : 2];
            rgba[x * 4 + 3] = 255;
        }
        break;
    case LV_RAW_LAYOUT_RGBA8:
    case LV_RAW_LAYOUT_BGRA8:
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *pixel = row + (size_t)x * 4;
            const bool bgr = context->layout->layout == LV_RAW_LAYOUT_BGRA8;
            rgba[x * 4 + 0] = pixel[bgr ? 2 : 0];
            rgba[x * 4 + 1] = pixel[1];
            rgba[x * 4 + 2] = pixel[bgr ? 0 : 2];
            rgba[x * 4 + 3] = context->layout->alpha == LV_RAW_ALPHA_STRAIGHT ? pixel[3] : 255;
        }
        break;
    case LV_RAW_LAYOUT_GRAY8:
        for (uint32_t x = 0; x < width; ++x)
        {
            rgba[x * 4 + 0] = row[x];
            rgba[x * 4 + 1] = row[x];
            rgba[x * 4 + 2] = row[x];
            rgba[x * 4 + 3] = 255;
        }
        break;
    case LV_RAW_LAYOUT_I420:
    case LV_RAW_LAYOUT_YV12:
    case LV_RAW_LAYOUT_NV12:
    case LV_RAW_LAYOUT_NV21:
    {
        if (context->processing->matrix != LV_RAW_MATRIX_BT601 ||
            (context->processing->range != LV_RAW_RANGE_FULL && context->processing->range != LV_RAW_RANGE_LIMITED) ||
            context->processing->chroma_siting != LV_RAW_CHROMA_CENTER)
        {
            lv_workspace_release(context->core, context->request, row, row_bytes);
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
        const uint32_t chroma_y = y / 2;
        const lv_raw_plane *chroma_u = nullptr;
        const lv_raw_plane *chroma_v = nullptr;
        const lv_raw_plane *chroma_uv = nullptr;
        if (context->layout->layout == LV_RAW_LAYOUT_I420 || context->layout->layout == LV_RAW_LAYOUT_YV12)
        {
            chroma_u = &context->layout->planes[context->layout->layout == LV_RAW_LAYOUT_I420 ? 1 : 2];
            chroma_v = &context->layout->planes[context->layout->layout == LV_RAW_LAYOUT_I420 ? 2 : 1];
        }
        else
            chroma_uv = &context->layout->planes[1];
        size_t u_bytes = 0, v_bytes = 0, uv_bytes = 0;
        uint8_t *u_row = chroma_u ? raw_read_bytes(context, chroma_u, chroma_y, &u_bytes, error) : nullptr;
        uint8_t *v_row = chroma_v ? raw_read_bytes(context, chroma_v, chroma_y, &v_bytes, error) : nullptr;
        uint8_t *uv_row = chroma_uv ? raw_read_bytes(context, chroma_uv, chroma_y, &uv_bytes, error) : nullptr;
        if ((chroma_u && (!u_row || !v_row)) || (chroma_uv && !uv_row))
        {
            if (u_row) lv_workspace_release(context->core, context->request, u_row, u_bytes);
            if (v_row) lv_workspace_release(context->core, context->request, v_row, v_bytes);
            if (uv_row) lv_workspace_release(context->core, context->request, uv_row, uv_bytes);
            lv_workspace_release(context->core, context->request, row, row_bytes);
            return false;
        }
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t chroma_x = x / 2;
            uint8_t u, v;
            if (chroma_uv)
            {
                const uint8_t *pair = uv_row + (size_t)chroma_x * 2;
                if (context->layout->layout == LV_RAW_LAYOUT_NV12)
                    u = pair[0], v = pair[1];
                else
                    v = pair[0], u = pair[1];
            }
            else
                u = u_row[chroma_x], v = v_row[chroma_x];
            yuv601_to_rgb(row[x], u, v, context->processing->range, rgba + (size_t)x * 4);
        }
        if (u_row) lv_workspace_release(context->core, context->request, u_row, u_bytes);
        if (v_row) lv_workspace_release(context->core, context->request, v_row, v_bytes);
        if (uv_row) lv_workspace_release(context->core, context->request, uv_row, uv_bytes);
        break;
    }
    default:
        lv_workspace_release(context->core, context->request, row, row_bytes);
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    lv_workspace_release(context->core, context->request, row, row_bytes);
    return !lv_stopped(context->core, context->request);
}

void lv_decode_raw_layout(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    const lv_raw_layout_desc *layout = &request->raw_image.layout;
    for (uint32_t i = 0; i < layout->plane_count; ++i)
    {
        const lv_raw_plane *plane = &layout->planes[i];
        uint64_t span = plane->height ? (uint64_t)(plane->height - 1) * plane->stride : 0;
        uint64_t end = 0;
        if (plane->offset > UINT64_MAX - span || plane->offset + span > UINT64_MAX - plane->row_bytes ||
            !add_u64(plane->offset + span, plane->row_bytes, &end) || end > input->version.bytes)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            return;
        }
    }
    const lv_raw_plane *plane = &layout->planes[0];
    lv_size_u32 source = layout->size;
    lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ? source :
        lv_fit_size(source, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    lv_frame frame = {.core = core, .request = request, .input = input};
    const uint64_t width_workspace = (uint64_t)source.width * 8;
    if (plane->row_bytes > UINT64_MAX - width_workspace)
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        goto done;
    }
    uint64_t row_bytes_total = plane->row_bytes;
    if (layout->layout == LV_RAW_LAYOUT_I420 || layout->layout == LV_RAW_LAYOUT_YV12)
    {
        if (UINT64_MAX - row_bytes_total < layout->planes[1].row_bytes ||
            UINT64_MAX - row_bytes_total - layout->planes[1].row_bytes < layout->planes[2].row_bytes)
        {
            error->code = LV_ERROR_MEMORY_BUDGET;
            goto done;
        }
        row_bytes_total += layout->planes[1].row_bytes + layout->planes[2].row_bytes;
    }
    else if (layout->layout == LV_RAW_LAYOUT_NV12 || layout->layout == LV_RAW_LAYOUT_NV21)
    {
        if (UINT64_MAX - row_bytes_total < layout->planes[1].row_bytes)
        {
            error->code = LV_ERROR_MEMORY_BUDGET;
            goto done;
        }
        row_bytes_total += layout->planes[1].row_bytes;
    }
    uint64_t row_workspace = 0;
    if (!add_u64(width_workspace, row_bytes_total, &row_workspace))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        goto done;
    }
    if (row_workspace > SIZE_MAX ||
        !lv_frame_prepare(&frame, source, canvas, LV_FORMAT_RAW_LAYOUT, 0, row_workspace, error) ||
        !lv_frame_info(&frame, error))
        goto done;
    raw_row_context context = {.core = core, .request = request, .input = input, .layout = layout,
                               .processing = &request->raw_image.processing};
    if (!lv_raster_read(&frame, source, raw_read_row, &context, error))
        goto done;
    if (!lv_frame_finish(&frame, error))
        goto done;
    error->code = 0;
done:
    lv_frame_release(&frame);
}
