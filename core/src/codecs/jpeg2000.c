#include <openjpeg.h>
#include <string.h>
#include "codec.h"

typedef struct jpeg2000_source
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    uint64_t position;
    lv_error_code failure;
} jpeg2000_source;

static OPJ_SIZE_T j2k_read(void *buffer, OPJ_SIZE_T bytes, void *userdata)
{
    jpeg2000_source *source = userdata;
    if (lv_stopped(source->core, source->request))
        return (OPJ_SIZE_T)-1;
    if (!bytes)
        return (OPJ_SIZE_T)-1;
    const uint64_t remaining = source->input->version.bytes - source->position;
    const size_t wanted       = bytes < remaining ? (size_t)bytes : (size_t)remaining;
    const size_t got          = fread(buffer, 1, wanted, source->input->file);
    source->position += got;
    source->request->bytes_read += got;
    return got;
}

static OPJ_OFF_T j2k_skip(OPJ_OFF_T bytes, void *userdata)
{
    jpeg2000_source *source = userdata;
    if (lv_stopped(source->core, source->request))
        return (OPJ_OFF_T)-1;
    if (bytes < 0)
    {
        const uint64_t backward = (uint64_t)(-(bytes + 1)) + 1;
        if (backward > source->position)
            return (OPJ_OFF_T)-1;
        source->position -= backward;
    }
    else
    {
        if ((uint64_t)bytes > source->input->version.bytes - source->position)
            return (OPJ_OFF_T)-1;
        source->position += (size_t)bytes;
    }
    if (!lv_file_seek(source->input->file, source->position))
        return (OPJ_OFF_T)-1;
    return bytes;
}

static OPJ_BOOL j2k_seek(OPJ_OFF_T position, void *userdata)
{
    jpeg2000_source *source = userdata;
    if (position < 0 || (uint64_t)position > source->input->version.bytes || lv_stopped(source->core, source->request))
        return OPJ_FALSE;
    if (!lv_file_seek(source->input->file, (uint64_t)position))
        return OPJ_FALSE;
    source->position = (uint64_t)position;
    return OPJ_TRUE;
}

static void j2k_error(const char *message, void *userdata)
{
    (void)message;
    jpeg2000_source *source = userdata;
    if (!source->failure)
        source->failure = LV_ERROR_CORRUPT_DATA;
}

static void j2k_warning(const char *message, void *userdata)
{
    (void)message;
    (void)userdata;
}

static void j2k_info(const char *message, void *userdata)
{
    (void)message;
    (void)userdata;
}

static bool j2k_pixels(lv_core *core, lv_request *request, lv_input *input, opj_image_t *image, lv_size_u32 *size,
                       uint8_t **output, lv_error_info *error)
{
    (void)input;
    if (!image || (image->numcomps != 1 && image->numcomps != 3) || image->x1 <= image->x0 || image->y1 <= image->y0)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    const uint32_t width = image->x1 - image->x0, height = image->y1 - image->y0;
    if (!lv_bitmap_size(core, width, height, error))
        return false;
    for (OPJ_UINT32 i = 0; i < image->numcomps; ++i)
    {
        const opj_image_comp_t *component = &image->comps[i];
        if (component->prec != 8 || component->sgnd || component->dx != 1 || component->dy != 1 || component->w != width ||
            component->h != height)
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
    }
    const size_t bytes = (size_t)((uint64_t)width * height * 4);
    if (!lv_reserve(core, request, bytes))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return false;
    }
    *output = lv_workspace_allocate(core, request, bytes);
    if (!*output)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    *size = (lv_size_u32){width, height};
    return true;
}

void lv_decode_jpeg2000(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    jpeg2000_source source = {.core = core, .request = request, .input = input};
    opj_stream_t *stream   = nullptr;
    opj_codec_t *codec     = nullptr;
    opj_image_t *image     = nullptr;
    uint8_t *pixels        = nullptr;
    lv_frame frame         = {.core = core, .request = request, .input = input};
    lv_size_u32 size       = {0, 0};
    uint8_t magic[12] = {0};
    lv_file_seek(input->file, 0);
    const size_t magic_bytes = fread(magic, 1, sizeof magic, input->file);
    request->bytes_read += magic_bytes;
    lv_file_seek(input->file, 0);
    const bool jp2 = magic_bytes == sizeof magic && !memcmp(magic, "\0\0\0\x0cjP  \r\n\x87\n", sizeof magic);
    const OPJ_CODEC_FORMAT format = jp2 ? OPJ_CODEC_JP2 : OPJ_CODEC_J2K;
    stream = opj_stream_create_default_file_stream(request->path, OPJ_TRUE);
    codec  = opj_create_decompress(format);
    if (!stream || !codec)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    opj_dparameters_t parameters;
    opj_set_default_decoder_parameters(&parameters);
    opj_set_error_handler(codec, j2k_error, &source);
    opj_set_warning_handler(codec, j2k_warning, &source);
    opj_set_info_handler(codec, j2k_info, &source);
    if (!opj_setup_decoder(codec, &parameters) || !opj_read_header(stream, codec, &image))
    {
        error->code = source.failure ? source.failure : LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    if (!j2k_pixels(core, request, input, image, &size, &pixels, error))
        goto done;
    if (!opj_decode(codec, stream, image) || !opj_end_decompress(codec, stream))
    {
        error->code = source.failure ? source.failure : LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    for (OPJ_UINT32 i = 0; i < image->numcomps; ++i)
        if (!image->comps[i].data)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            goto done;
        }
    // The component buffers are populated by opj_decode. Rebuild the RGBA output now.
    for (uint32_t y = 0; y < size.height; ++y)
        for (uint32_t x = 0; x < size.width; ++x)
        {
            const size_t at = (size_t)y * size.width + x;
            uint8_t *pixel  = pixels + at * 4;
            if (image->numcomps == 1)
                pixel[0] = pixel[1] = pixel[2] = (uint8_t)image->comps[0].data[at];
            else
            {
                pixel[0] = (uint8_t)image->comps[0].data[at];
                pixel[1] = (uint8_t)image->comps[1].data[at];
                pixel[2] = (uint8_t)image->comps[2].data[at];
            }
            pixel[3] = 255;
        }
    {
        const lv_size_u32 canvas = request->desc.target.kind == LV_TARGET_NATIVE ?
            size : lv_fit_size(size, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
        if (lv_frame_prepare(&frame, size, canvas, LV_FORMAT_JPEG2000, 0, 0, error) && lv_frame_info(&frame, error) &&
            lv_resample_rgba(core, request, pixels, size, frame.data, canvas))
            lv_frame_finish(&frame, error);
    }
done:
    if (image)
        opj_image_destroy(image);
    if (codec)
        opj_destroy_codec(codec);
    if (stream)
        opj_stream_destroy(stream);
    if (pixels)
        lv_workspace_release(core, request, pixels, (size_t)((uint64_t)size.width * size.height * 4));
    lv_frame_release(&frame);
}
