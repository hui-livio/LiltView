#include <limits.h>
#include <libheif/heif.h>
#include <libheif/heif_aux_images.h>
#include <libheif/heif_sequences.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "codec.h"

typedef struct heif_source
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    lv_error_code failure;
} heif_source;

static int64_t heif_position(void *userdata)
{
    heif_source *source = userdata;
    const long position = ftell(source->input->file);
    return position < 0 ? -1 : (int64_t)position;
}

static int heif_read(void *data, size_t size, void *userdata)
{
    heif_source *source = userdata;
    const long position = ftell(source->input->file);
    if (lv_stopped(source->core, source->request))
        return 1;
    if (position < 0 || (uint64_t)position > source->input->version.bytes ||
        (uint64_t)size > source->input->version.bytes - (uint64_t)position)
    {
        source->failure = LV_ERROR_CORRUPT_DATA;
        return 1;
    }
    const size_t got = fread(data, 1, size, source->input->file);
    source->request->bytes_read += got;
    if (got != size)
    {
        source->failure = ferror(source->input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        return 1;
    }
    return 0;
}

static int heif_seek(int64_t position, void *userdata)
{
    heif_source *source = userdata;
    if (lv_stopped(source->core, source->request))
        return 1;
    if (position < 0 || (uint64_t)position > source->input->version.bytes ||
        !lv_file_seek(source->input->file, (uint64_t)position))
    {
        source->failure = LV_ERROR_IO;
        return 1;
    }
    return 0;
}

static enum heif_reader_grow_status heif_wait_for_file_size(int64_t target, void *userdata)
{
    heif_source *source = userdata;
    if (target < 0 || (uint64_t)target > source->input->version.bytes)
        return heif_reader_grow_status_size_beyond_eof;
    return heif_reader_grow_status_size_reached;
}

static lv_error_code heif_map_error(heif_error result, lv_error_code callback_failure)
{
    if (callback_failure)
        return callback_failure;
    switch (result.code)
    {
    case heif_error_Memory_allocation_error:
        return LV_ERROR_OUT_OF_MEMORY;
    case heif_error_Unsupported_filetype:
    case heif_error_Unsupported_feature:
        return LV_ERROR_FORMAT_UNSUPPORTED;
    case heif_error_Input_does_not_exist:
        return LV_ERROR_IO;
    case heif_error_Canceled:
        return 0;
    case heif_error_Invalid_input:
    case heif_error_Decoder_plugin_error:
    default:
        return LV_ERROR_CORRUPT_DATA;
    }
}

static bool heif_has_unsupported_profile(const heif_image_handle *handle)
{
    const heif_color_profile_type profile = heif_image_handle_get_color_profile_type(handle);
    return profile == heif_color_profile_type_rICC || profile == heif_color_profile_type_prof;
}

static bool heif_has_unsupported_depth(const heif_image_handle *handle)
{
    const int luma_bits = heif_image_handle_get_luma_bits_per_pixel(handle);
    const int chroma_bits = heif_image_handle_get_chroma_bits_per_pixel(handle);
    return luma_bits > 8 || chroma_bits > 8;
}

void lv_decode_heif(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    heif_source source = {.core = core, .request = request, .input = input};
    heif_context *context = nullptr;
    heif_image_handle *handle = nullptr;
    heif_image *image = nullptr;
    lv_frame frame = {.core = core, .request = request, .input = input};
    uint8_t *native = nullptr;
    lv_size_u32 size = {0, 0};
    lv_size_u32 canvas = {0, 0};
    size_t native_bytes = 0;
    heif_error result;

    if (input->version.bytes > INT64_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }

    heif_reader reader = {0};
    reader.reader_api_version = 1;
    reader.get_position = heif_position;
    reader.read = heif_read;
    reader.seek = heif_seek;
    reader.wait_for_file_size = heif_wait_for_file_size;

    context = heif_context_alloc();
    if (!context)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    result = heif_context_read_from_reader(context, &reader, &source, nullptr);
    if (result.code)
    {
        error->code = heif_map_error(result, source.failure);
        goto done;
    }
    if (lv_stopped(core, request))
        goto done;
    if (heif_context_has_sequence(context) || heif_context_get_number_of_top_level_images(context) != 1)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }

    result = heif_context_get_primary_image_handle(context, &handle);
    if (result.code || !handle)
    {
        error->code = heif_map_error(result, source.failure);
        goto done;
    }
    if (!heif_image_handle_is_primary_image(handle) || heif_has_unsupported_depth(handle) ||
        heif_image_handle_get_number_of_thumbnails(handle) > 0 || heif_image_handle_has_depth_image(handle) ||
        heif_image_handle_get_number_of_auxiliary_images(handle, 0) > 0 || heif_has_unsupported_profile(handle))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }

    result = heif_decode_image(handle, &image, heif_colorspace_RGB, heif_chroma_interleaved_RGBA, nullptr);
    if (result.code || !image)
    {
        error->code = heif_map_error(result, source.failure);
        goto done;
    }
    if (heif_image_get_bits_per_pixel_range(image, heif_channel_interleaved) != 8)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }

    const int width = heif_image_get_width(image, heif_channel_interleaved);
    const int height = heif_image_get_height(image, heif_channel_interleaved);
    size_t stride = 0;
    const uint8_t *pixels = heif_image_get_plane_readonly2(image, heif_channel_interleaved, &stride);
    if (width <= 0 || height <= 0 || !pixels || (uint64_t)width > SIZE_MAX / 4 || stride < (size_t)width * 4)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    size = (lv_size_u32){(uint32_t)width, (uint32_t)height};
    if (!lv_bitmap_size(core, size.width, size.height, error))
        goto done;
    if ((uint64_t)size.width * size.height > SIZE_MAX / 4)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }
    native_bytes = (size_t)((uint64_t)size.width * size.height * 4);
    canvas = request->desc.target.kind == LV_TARGET_NATIVE ? size :
        lv_fit_size(size, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    if (!lv_frame_prepare(&frame, size, canvas, LV_FORMAT_HEIF, 0, native_bytes, error) ||
        !lv_frame_info(&frame, error))
        goto done;
    if (lv_stopped(core, request))
        goto done;

    native = lv_workspace_allocate(core, request, native_bytes);
    if (!native)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    for (uint32_t y = 0; y < size.height; ++y)
        memcpy(native + (size_t)y * size.width * 4, pixels + (size_t)y * stride, (size_t)size.width * 4);
    if (!lv_resample_rgba(core, request, native, size, frame.data, canvas))
        goto done;
    if (!lv_frame_finish(&frame, error))
        goto done;
    error->code = 0;

done:
    if (lv_stopped(core, request))
        error->code = 0;
    if (native)
        lv_workspace_release(core, request, native, native_bytes);
    if (image)
        heif_image_release(image);
    if (handle)
        heif_image_handle_release(handle);
    if (context)
        heif_context_free(context);
    lv_frame_release(&frame);
}
