#include <libraw/libraw.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "codec.h"

static lv_error_code raw_error(int result)
{
    switch (result)
    {
    case LIBRAW_SUCCESS:
        return 0;
    case LIBRAW_IO_ERROR:
        return LV_ERROR_IO;
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_MEMPOOL_OVERFLOW:
        return LV_ERROR_OUT_OF_MEMORY;
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
    case LIBRAW_NOT_IMPLEMENTED:
        return LV_ERROR_FORMAT_UNSUPPORTED;
    case LIBRAW_CANCELLED_BY_CALLBACK:
        return 0;
    case LIBRAW_DATA_ERROR:
    case LIBRAW_TOO_BIG:
    default:
        return LV_ERROR_CORRUPT_DATA;
    }
}

void lv_decode_raw(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    if (input->version.bytes > SIZE_MAX || input->version.bytes > UINT64_C(128) * 1024 * 1024)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    uint8_t *data = lv_input_read_all(core, request, input, error);
    if (!data)
        return;

    const size_t bytes = (size_t)input->version.bytes;
    libraw_data_t *raw = libraw_init(0);
    lv_frame frame = {.core = core, .request = request, .input = input};
    libraw_processed_image_t *image = nullptr;
    if (!raw)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    int result = libraw_open_buffer(raw, data, bytes);
    if (result != LIBRAW_SUCCESS)
    {
        error->code = raw_error(result);
        goto done;
    }
    if (lv_stopped(core, request))
        goto done;
    if (raw->idata.raw_count != 1 || (raw->idata.dng_version == 0 && strcmp(raw->idata.make, "Canon")))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }
    if (raw->sizes.iwidth <= 0 || raw->sizes.iheight <= 0 ||
        (uint64_t)raw->sizes.iwidth * raw->sizes.iheight > UINT32_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }
    const lv_size_u32 source = {(uint32_t)raw->sizes.iwidth, (uint32_t)raw->sizes.iheight};
    if (!lv_bitmap_size(core, source.width, source.height, error))
        goto done;
    const uint64_t pixels = (uint64_t)source.width * source.height;
    const uint64_t native_bytes = pixels * 4;
    if (native_bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }
    const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
        source : lv_fit_size(source, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    if (!lv_frame_prepare(&frame, source, canvas, LV_FORMAT_RAW, 0, native_bytes + bytes, error) ||
        !lv_frame_info(&frame, error))
        goto done;
    if (lv_stopped(core, request))
        goto done;
    result = libraw_unpack(raw);
    if (result != LIBRAW_SUCCESS)
    {
        error->code = raw_error(result);
        goto done;
    }
    if (lv_stopped(core, request))
        goto done;
    raw->params.output_bps = 8;
    raw->params.output_color = 1;
    raw->params.use_camera_wb = 1;
    raw->params.no_auto_bright = 1;
    result = libraw_dcraw_process(raw);
    if (result != LIBRAW_SUCCESS)
    {
        error->code = raw_error(result);
        goto done;
    }
    int image_error = 0;
    image = libraw_dcraw_make_mem_image(raw, &image_error);
    if (!image)
    {
        error->code = raw_error(image_error);
        goto done;
    }
    if (image->type != LIBRAW_IMAGE_BITMAP || image->bits != 8 || image->colors != 3 ||
        image->width != source.width || image->height != source.height || image->data_size < pixels * 3)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }
    uint8_t *native = lv_workspace_allocate(core, request, (size_t)native_bytes);
    if (!native)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    const uint8_t *rgb = image->data;
    for (uint64_t at = 0; at < pixels; ++at)
    {
        native[at * 4 + 0] = rgb[at * 3 + 0];
        native[at * 4 + 1] = rgb[at * 3 + 1];
        native[at * 4 + 2] = rgb[at * 3 + 2];
        native[at * 4 + 3] = 255;
    }
    if (!lv_resample_rgba(core, request, native, source, frame.data, canvas))
    {
        lv_workspace_release(core, request, native, (size_t)native_bytes);
        goto done;
    }
    lv_workspace_release(core, request, native, (size_t)native_bytes);
    if (!lv_frame_finish(&frame, error))
        goto done;
    error->code = 0;
done:
    if (image)
        libraw_dcraw_clear_mem(image);
    if (raw)
        libraw_close(raw);
    lv_frame_release(&frame);
    lv_workspace_release(core, request, data, bytes + 1);
}
