#include <webp/decode.h>
#include "codec.h"

static uint32_t little32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool webp_container(lv_core *core, lv_request *r, const uint8_t *data, size_t bytes)
{
    if (bytes < 12 || (uint64_t)little32(data + 4) + 8 != bytes)
        return false;
    size_t at = 12;
    while (bytes - at >= 8)
    {
        if (lv_stopped(core, r))
            return false;
        const uint64_t length = little32(data + at + 4);
        const uint64_t padded = length + (length & 1u);
        if (padded > bytes - at - 8)
            return false;
        at += (size_t)padded + 8;
    }
    return at == bytes;
}

static lv_error_code webp_error(VP8StatusCode status)
{
    return status == VP8_STATUS_OUT_OF_MEMORY    ? LV_ERROR_OUT_OF_MEMORY :
        status == VP8_STATUS_UNSUPPORTED_FEATURE ? LV_ERROR_FORMAT_UNSUPPORTED :
                                                   LV_ERROR_CORRUPT_DATA;
}

static bool webp_pixels(lv_frame *f, const uint8_t *data, size_t bytes, lv_error_info *error)
{
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
    {
        error->code = LV_ERROR_INTERNAL;
        return false;
    }
    config.output.colorspace         = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba        = f->data;
    config.output.u.RGBA.stride      = (int)(f->info.canvas.width * 4);
    config.output.u.RGBA.size        = f->bytes;
    config.options.use_scaling       = 1;
    config.options.scaled_width      = (int)f->info.canvas.width;
    config.options.scaled_height     = (int)f->info.canvas.height;
    // Incremental decoder keeps cancellation checks between bounded input chunks.
    WebPIDecoder *decoder = WebPIDecode(nullptr, 0, &config);
    if (!decoder)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    VP8StatusCode status = VP8_STATUS_SUSPENDED;
    for (size_t offset = 0; offset < bytes && status == VP8_STATUS_SUSPENDED;)
    {
        if (lv_stopped(f->core, f->request))
            break;
        offset += bytes - offset < 16384 ? bytes - offset : 16384;
        // Update references stable input storage; it does not copy the whole input.
        status = WebPIUpdate(decoder, data, offset);
    }
    WebPIDelete(decoder);
    WebPFreeDecBuffer(&config.output);
    if (status != VP8_STATUS_OK)
    {
        error->code = webp_error(status);
        return false;
    }
    return true;
}

void lv_decode_webp(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    uint8_t *data = lv_input_read_all(core, r, input, error);
    if (!data)
        return;
    const size_t bytes = (size_t)input->version.bytes;
    lv_frame f         = {.core = core, .request = r, .input = input};
    WebPBitstreamFeatures features;
    VP8StatusCode status = WebPGetFeatures(data, bytes, &features);
    if (!webp_container(core, r, data, bytes))
        error->code = LV_ERROR_CORRUPT_DATA;
    else if (status != VP8_STATUS_OK)
        error->code = webp_error(status);
    else if (features.has_animation)
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
    else if (lv_bitmap_size(core, (uint32_t)features.width, (uint32_t)features.height, error))
    {
        const lv_size_u32 size   = {(uint32_t)features.width, (uint32_t)features.height};
        const lv_size_u32 canvas = r->desc.target.kind == LV_TARGET_NATIVE ?
            size :
            lv_fit_size(size, r->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
        // Foreign codec memory has no allocator hook; reserve a conservative plan.
        const uint64_t scratch = UINT64_C(4) * 1024 * 1024 + (uint64_t)size.width * size.height * 16 + bytes * 2;
        if (lv_frame_prepare(&f, size, canvas, LV_FORMAT_WEBP, LV_CAP_SCALED_DECODE, scratch, error) &&
            lv_frame_info(&f, error) && webp_pixels(&f, data, bytes, error))
        {
            lv_workspace_release(core, r, data, bytes + 1);
            data = nullptr;
            lv_frame_finish(&f, error);
        }
    }
    lv_frame_release(&f);
    lv_workspace_release(core, r, data, bytes + 1);
}
