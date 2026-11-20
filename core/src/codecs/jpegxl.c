#include <jxl/decode.h>
#include <string.h>
#include "codec.h"

static lv_error_code jxl_error(JxlDecoderStatus status)
{
    return status == JXL_DEC_ERROR ? LV_ERROR_CORRUPT_DATA : LV_ERROR_OUT_OF_MEMORY;
}

void lv_decode_jpegxl(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    if (input->version.bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    uint8_t *data = lv_input_read_all(core, request, input, error);
    if (!data)
        return;
    JxlDecoder *decoder = JxlDecoderCreate(nullptr);
    lv_frame frame       = {.core = core, .request = request, .input = input};
    const size_t bytes   = (size_t)input->version.bytes;
    JxlBasicInfo info    = {0};
    uint8_t *output      = nullptr;
    size_t output_bytes  = 0;
    JxlDecoderStatus status = JXL_DEC_ERROR;
    if (!decoder)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    const JxlDecoderStatus subscribe = JxlDecoderSubscribeEvents(decoder, JXL_DEC_BASIC_INFO | JXL_DEC_FRAME | JXL_DEC_FULL_IMAGE);
    const JxlDecoderStatus set_input = JxlDecoderSetInput(decoder, data, bytes);
    if (subscribe != JXL_DEC_SUCCESS || set_input != JXL_DEC_SUCCESS)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    JxlDecoderCloseInput(decoder);
    while (!lv_stopped(core, request))
    {
        status = JxlDecoderProcessInput(decoder);
        if (status == JXL_DEC_BASIC_INFO)
        {
            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS)
                break;
            if (info.have_animation || info.bits_per_sample != 8 || info.exponent_bits_per_sample ||
                (info.num_color_channels != 1 && info.num_color_channels != 3) || info.num_extra_channels > 1 ||
                (info.alpha_bits && info.alpha_bits != 8))
            {
                error->code = LV_ERROR_FORMAT_UNSUPPORTED;
                goto done;
            }
            const lv_size_u32 source = {info.xsize, info.ysize};
            if (!lv_bitmap_size(core, source.width, source.height, error))
                goto done;
            const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
                source : lv_fit_size(source, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
            const uint64_t scratch = UINT64_C(8) * 1024 * 1024 + (uint64_t)source.width * source.height * 8 + bytes;
            if (!lv_frame_prepare(&frame, source, canvas, LV_FORMAT_JPEGXL, 0, scratch, error) || !lv_frame_info(&frame, error))
                goto done;
            JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
            if (JxlDecoderImageOutBufferSize(decoder, &format, &output_bytes) != JXL_DEC_SUCCESS ||
                output_bytes != (size_t)canvas.width * canvas.height * 4)
            {
                error->code = LV_ERROR_FORMAT_UNSUPPORTED;
                goto done;
            }
            output = frame.data;
        }
        else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER)
        {
            JxlPixelFormat format = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
            if (!output || JxlDecoderSetImageOutBuffer(decoder, &format, output, output_bytes) != JXL_DEC_SUCCESS)
                break;
        }
        else if (status == JXL_DEC_FULL_IMAGE)
        {
            if (!lv_frame_finish(&frame, error))
                goto done;
            error->code = 0;
            goto done;
        }
        else if (status == JXL_DEC_SUCCESS)
            break;
        else if (status != JXL_DEC_FRAME)
            break;
    }
    if (!error->code)
    {
        error->code = lv_stopped(core, request) ? 0 : jxl_error(status);
    }
done:
    if (decoder)
        JxlDecoderDestroy(decoder);
    lv_frame_release(&frame);
    lv_workspace_release(core, request, data, bytes + 1);
}
