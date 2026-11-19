#include <avif/avif.h>
#include <string.h>
#include "codec.h"

static lv_error_code avif_error(avifResult result)
{
    switch (result)
    {
    case AVIF_RESULT_OUT_OF_MEMORY:
        return LV_ERROR_OUT_OF_MEMORY;
    case AVIF_RESULT_NO_CODEC_AVAILABLE:
    case AVIF_RESULT_NOT_IMPLEMENTED:
    case AVIF_RESULT_UNSUPPORTED_DEPTH:
        return LV_ERROR_FORMAT_UNSUPPORTED;
    case AVIF_RESULT_IO_ERROR:
        return LV_ERROR_IO;
    default:
        return LV_ERROR_CORRUPT_DATA;
    }
}

static bool avif_has_unsupported_transform(const avifImage *image)
{
    return image->transformFlags != AVIF_TRANSFORM_NONE;
}

static void avif_zero_transparent(uint8_t *pixels, lv_size_u32 size)
{
    for (uint64_t at = 0, count = (uint64_t)size.width * size.height; at < count; ++at)
    {
        uint8_t *pixel = pixels + at * 4;
        if (!pixel[3])
            pixel[0] = pixel[1] = pixel[2] = 0;
    }
}

void lv_decode_avif(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    if (input->version.bytes > SIZE_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    uint8_t *data = lv_input_read_all(core, request, input, error);
    if (!data)
        return;

    avifDecoder *decoder = avifDecoderCreate();
    avifImage *image     = nullptr;
    lv_frame frame       = {.core = core, .request = request, .input = input};
    const size_t bytes   = (size_t)input->version.bytes;
    if (!decoder)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    decoder->maxThreads      = 1;
    decoder->imageCountLimit = 1;
    decoder->imageContentToDecode = AVIF_IMAGE_CONTENT_COLOR_AND_ALPHA;
    avifResult result = avifDecoderSetIOMemory(decoder, data, bytes);
    if (result != AVIF_RESULT_OK)
    {
        error->code = avif_error(result);
        goto done;
    }
    result = avifDecoderParse(decoder);
    if (result != AVIF_RESULT_OK)
    {
        error->code = avif_error(result);
        goto done;
    }
    if (decoder->imageSequenceTrackPresent)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }
    if (lv_stopped(core, request))
        goto done;
    result = avifDecoderNextImage(decoder);
    if (result != AVIF_RESULT_OK)
    {
        error->code = avif_error(result);
        goto done;
    }
    image = decoder->image;
    if (!image || decoder->imageCount != 1 || !image->width || !image->height || image->depth != 8 ||
        image->yuvFormat == AVIF_PIXEL_FORMAT_NONE || avif_has_unsupported_transform(image))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }
    const lv_size_u32 source = {image->width, image->height};
    if (!lv_bitmap_size(core, source.width, source.height, error))
        goto done;
    const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
        source : lv_fit_size(source, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    const uint64_t pixels  = (uint64_t)source.width * source.height;
    const uint64_t scratch = UINT64_C(8) * 1024 * 1024 + pixels * 16 + bytes * 2;
    if (!lv_frame_prepare(&frame, source, canvas, LV_FORMAT_AVIF, 0, scratch, error) || !lv_frame_info(&frame, error))
        goto done;
    if (lv_stopped(core, request))
        goto done;
    if (canvas.width != source.width || canvas.height != source.height)
    {
        result = avifImageScale(image, canvas.width, canvas.height, &decoder->diag);
        if (result != AVIF_RESULT_OK)
        {
            error->code = avif_error(result);
            goto done;
        }
        image = decoder->image;
    }
    if (!image || image->depth != 8 || image->width != canvas.width || image->height != canvas.height ||
        avif_has_unsupported_transform(image))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }
    {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image);
        rgb.format     = AVIF_RGB_FORMAT_RGBA;
        rgb.depth      = 8;
        rgb.pixels     = frame.data;
        rgb.rowBytes   = canvas.width * 4;
        rgb.maxThreads = 1;
        result         = avifImageYUVToRGB(image, &rgb);
    }
    if (result != AVIF_RESULT_OK)
    {
        error->code = avif_error(result);
        goto done;
    }
    avif_zero_transparent(frame.data, canvas);
    if (!lv_stopped(core, request))
        lv_frame_finish(&frame, error);
done:
    if (decoder)
        avifDecoderDestroy(decoder);
    lv_frame_release(&frame);
    lv_workspace_release(core, request, data, bytes + 1);
}
