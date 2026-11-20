#include <string.h>
#include "codec.h"

typedef struct raw_row_context
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    const lv_raw_layout_desc *layout;
} raw_row_context;

static bool raw_read_row(void *userdata, uint32_t y, uint8_t *rgba, lv_error_info *error)
{
    raw_row_context *context = userdata;
    const lv_raw_plane *plane = &context->layout->planes[0];
    const uint32_t physical_y = context->layout->row_order == LV_RAW_TOP_DOWN ? y : plane->height - 1 - y;
    const uint64_t offset = plane->offset + (uint64_t)physical_y * plane->stride;
    if (!lv_file_seek(context->input->file, offset))
    {
        error->code = LV_ERROR_IO;
        return false;
    }
    const size_t row_bytes = (size_t)plane->row_bytes;
    uint8_t *row = lv_workspace_allocate(context->core, context->request, row_bytes);
    if (!row)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    const size_t count = fread(row, 1, row_bytes, context->input->file);
    context->request->bytes_read += count;
    if (count != row_bytes)
    {
        lv_workspace_release(context->core, context->request, row, row_bytes);
        error->code = ferror(context->input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        return false;
    }
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
    const lv_raw_plane *plane = &layout->planes[0];
    uint64_t span = plane->height ? (uint64_t)(plane->height - 1) * plane->stride : 0;
    if (plane->offset > UINT64_MAX - span || plane->offset + span > UINT64_MAX - plane->row_bytes ||
        plane->offset + span + plane->row_bytes > input->version.bytes)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return;
    }
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
    const uint64_t row_workspace = width_workspace + plane->row_bytes;
    if (row_workspace > SIZE_MAX ||
        !lv_frame_prepare(&frame, source, canvas, LV_FORMAT_RAW_LAYOUT, 0, row_workspace, error) ||
        !lv_frame_info(&frame, error))
        goto done;
    raw_row_context context = {.core = core, .request = request, .input = input, .layout = layout};
    if (!lv_raster_read(&frame, source, raw_read_row, &context, error))
        goto done;
    if (!lv_frame_finish(&frame, error))
        goto done;
    error->code = 0;
done:
    lv_frame_release(&frame);
}
