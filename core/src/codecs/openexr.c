#include <math.h>
#include <openexr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "codec.h"

typedef struct openexr_source
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    lv_error_code failure;
} openexr_source;

static int64_t openexr_size(exr_const_context_t context, void *userdata)
{
    (void)context;
    openexr_source *source = userdata;
    return source->input->version.bytes > INT64_MAX ? -1 : (int64_t)source->input->version.bytes;
}

static int64_t openexr_read(exr_const_context_t context, void *userdata, void *buffer, uint64_t size, uint64_t offset,
                            exr_stream_error_func_ptr_t error_cb)
{
    (void)context;
    openexr_source *source = userdata;
    if (lv_stopped(source->core, source->request) || offset > source->input->version.bytes ||
        size > source->input->version.bytes - offset || size > SIZE_MAX)
        return -1;
    if (!lv_file_seek(source->input->file, offset))
    {
        source->failure = LV_ERROR_IO;
        if (error_cb)
            error_cb(context, EXR_ERR_READ_IO, "seek failed");
        return -1;
    }
    uint8_t *out = buffer;
    uint64_t remaining = size;
    while (remaining && !lv_stopped(source->core, source->request))
    {
        const size_t wanted = remaining < 16384 ? (size_t)remaining : 16384;
        const size_t got = fread(out, 1, wanted, source->input->file);
        out += got;
        remaining -= got;
        source->request->bytes_read += got;
        if (got != wanted)
            break;
    }
    if (remaining)
    {
        source->failure = ferror(source->input->file) ? LV_ERROR_IO : LV_ERROR_CORRUPT_DATA;
        return -1;
    }
    return size > INT64_MAX ? -1 : (int64_t)size;
}

static lv_error_code openexr_error(exr_result_t result, lv_error_code callback_failure)
{
    if (callback_failure)
        return callback_failure;
    switch (result)
    {
    case EXR_ERR_OUT_OF_MEMORY:
        return LV_ERROR_OUT_OF_MEMORY;
    case EXR_ERR_FILE_ACCESS:
    case EXR_ERR_READ_IO:
        return LV_ERROR_IO;
    case EXR_ERR_FEATURE_NOT_IMPLEMENTED:
        return LV_ERROR_FORMAT_UNSUPPORTED;
    case EXR_ERR_FILE_BAD_HEADER:
    case EXR_ERR_CORRUPT_CHUNK:
    case EXR_ERR_INCOMPLETE_CHUNK_TABLE:
    case EXR_ERR_DECOMPRESSION_FAILED:
        return LV_ERROR_CORRUPT_DATA;
    default:
        return LV_ERROR_CORRUPT_DATA;
    }
}

static bool openexr_channel(const exr_attr_chlist_entry_t *entry, const char *name)
{
    return entry && entry->name.str && !strcmp(entry->name.str, name) && entry->pixel_type == EXR_PIXEL_HALF &&
        entry->x_sampling == 1 && entry->y_sampling == 1;
}

static uint8_t openexr_sample(float value)
{
    if (isnan(value) || value <= 0)
        return 0;
    if (isinf(value) || value >= 1)
        return 255;
    return (uint8_t)(value * 255.0f + 0.5f);
}

static void openexr_fill_rgba(uint8_t *output, const float *red, const float *green, const float *blue,
                              const float *alpha, uint64_t pixels)
{
    for (uint64_t at = 0; at < pixels; ++at)
    {
        output[at * 4 + 0] = openexr_sample(red[at]);
        output[at * 4 + 1] = openexr_sample(green[at]);
        output[at * 4 + 2] = openexr_sample(blue[at]);
        output[at * 4 + 3] = alpha ? openexr_sample(alpha[at]) : 255;
    }
}

void lv_decode_openexr(lv_core *core, lv_request *request, lv_input *input, lv_error_info *error)
{
    openexr_source source = {.core = core, .request = request, .input = input};
    exr_context_t context = nullptr;
    lv_frame frame = {.core = core, .request = request, .input = input};
    float *planes[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_size_u32 size = {0, 0};
    lv_size_u32 canvas = {0, 0};
    uint64_t plane_bytes = 0;
    exr_result_t result;

    if (input->version.bytes > INT64_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }

    exr_context_initializer_t initializer = EXR_DEFAULT_CONTEXT_INITIALIZER;
    initializer.read_fn = openexr_read;
    initializer.size_fn = openexr_size;
    initializer.user_data = &source;
    initializer.flags |= EXR_CONTEXT_FLAG_SILENT_HEADER_PARSE;
    result = exr_start_read(&context, "liltview.exr", &initializer);
    if (result != EXR_ERR_SUCCESS)
    {
        error->code = openexr_error(result, source.failure);
        goto done;
    }

    int parts = 0;
    exr_storage_t storage = EXR_STORAGE_UNKNOWN;
    const exr_attr_chlist_t *channels = nullptr;
    exr_attr_box2i_t data_window = {0};
    exr_attr_box2i_t display_window = {0};
    if (exr_get_count(context, &parts) != EXR_ERR_SUCCESS || parts != 1 ||
        exr_get_storage(context, 0, &storage) != EXR_ERR_SUCCESS || storage != EXR_STORAGE_SCANLINE ||
        exr_get_channels(context, 0, &channels) != EXR_ERR_SUCCESS || !channels ||
        exr_get_data_window(context, 0, &data_window) != EXR_ERR_SUCCESS ||
        exr_get_display_window(context, 0, &display_window) != EXR_ERR_SUCCESS)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }

    if (data_window.max.x < data_window.min.x || data_window.max.y < data_window.min.y ||
        display_window.max.x < display_window.min.x || display_window.max.y < display_window.min.y ||
        data_window.min.x < display_window.min.x || data_window.min.y < display_window.min.y ||
        data_window.max.x > display_window.max.x || data_window.max.y > display_window.max.y)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    const int64_t width64 = (int64_t)data_window.max.x - data_window.min.x + 1;
    const int64_t height64 = (int64_t)data_window.max.y - data_window.min.y + 1;
    if (width64 <= 0 || height64 <= 0 || width64 > UINT32_MAX || height64 > UINT32_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }
    size = (lv_size_u32){(uint32_t)width64, (uint32_t)height64};
    if (!lv_bitmap_size(core, size.width, size.height, error))
        goto done;

    int red_index = -1, green_index = -1, blue_index = -1, alpha_index = -1;
    for (int i = 0; i < channels->num_channels; ++i)
    {
        const exr_attr_chlist_entry_t *entry = &channels->entries[i];
        if (!entry->name.str)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            goto done;
        }
        if (!strcmp(entry->name.str, "R"))
            red_index = i;
        else if (!strcmp(entry->name.str, "G"))
            green_index = i;
        else if (!strcmp(entry->name.str, "B"))
            blue_index = i;
        else if (!strcmp(entry->name.str, "A"))
            alpha_index = i;
        else
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            goto done;
        }
        if (!openexr_channel(entry, entry->name.str))
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            goto done;
        }
    }
    if ((channels->num_channels != 3 && channels->num_channels != 4) || red_index < 0 || green_index < 0 || blue_index < 0)
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        goto done;
    }

    plane_bytes = (uint64_t)size.width * size.height * sizeof(float);
    const uint64_t pixels = (uint64_t)size.width * size.height;
    const uint64_t native_bytes = pixels * 4;
    if (plane_bytes > UINT64_MAX / (alpha_index >= 0 ? 4 : 3) ||
        native_bytes > UINT64_MAX - plane_bytes * (alpha_index >= 0 ? 4 : 3))
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        goto done;
    }
    const uint64_t scratch = plane_bytes * (alpha_index >= 0 ? 4 : 3) + native_bytes;
    canvas = request->desc.target.kind == LV_TARGET_NATIVE ? size : lv_fit_size(size, request->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    if (!lv_frame_prepare(&frame, size, canvas, LV_FORMAT_OPENEXR, 0, scratch, error) || !lv_frame_info(&frame, error))
        goto done;
    for (int i = 0; i < 4; ++i)
    {
        if (i == alpha_index || i == red_index || i == green_index || i == blue_index)
        {
            planes[i] = lv_workspace_allocate(core, request, (size_t)plane_bytes);
            if (!planes[i])
            {
                error->code = LV_ERROR_OUT_OF_MEMORY;
                goto done;
            }
        }
    }

    int32_t lines = 0;
    if (exr_get_scanlines_per_chunk(context, 0, &lines) != EXR_ERR_SUCCESS || lines <= 0)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    for (int64_t y = data_window.min.y; y <= data_window.max.y;)
    {
        if (lv_stopped(core, request))
            goto done;
        exr_chunk_info_t chunk = {0};
        result = exr_read_scanline_chunk_info(context, 0, (int)y, &chunk);
        if (result != EXR_ERR_SUCCESS)
        {
            error->code = openexr_error(result, source.failure);
            goto done;
        }
        if (chunk.height <= 0 || chunk.start_y < y || (int64_t)chunk.start_y + chunk.height <= y)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            goto done;
        }
        y = (int64_t)chunk.start_y + chunk.height;
        exr_decode_pipeline_t decoder = {0};
        decoder.pipe_size = sizeof decoder;
        result = exr_decoding_initialize(context, 0, &chunk, &decoder);
        if (result == EXR_ERR_SUCCESS)
        {
            const int64_t row = (int64_t)chunk.start_y - data_window.min.y;
            if (row < 0 || row + chunk.height > size.height)
                result = EXR_ERR_CORRUPT_CHUNK;
            for (int c = 0; result == EXR_ERR_SUCCESS && c < decoder.channel_count; ++c)
            {
                int index = -1;
                if (!strcmp(decoder.channels[c].channel_name, "R")) index = red_index;
                else if (!strcmp(decoder.channels[c].channel_name, "G")) index = green_index;
                else if (!strcmp(decoder.channels[c].channel_name, "B")) index = blue_index;
                else if (!strcmp(decoder.channels[c].channel_name, "A")) index = alpha_index;
                if (index < 0 || decoder.channels[c].width != (int32_t)size.width || decoder.channels[c].y_samples != 1)
                {
                    result = EXR_ERR_INVALID_ATTR;
                    break;
                }
                exr_coding_channel_info_t *channel = &decoder.channels[c];
                channel->decode_to_ptr = (uint8_t *)planes[index] + (size_t)row * size.width * sizeof(float);
                channel->user_bytes_per_element = sizeof(float);
                channel->user_data_type = EXR_PIXEL_FLOAT;
                channel->user_pixel_stride = sizeof(float);
                channel->user_line_stride = (int32_t)((uint64_t)size.width * sizeof(float));
            }
            if (result == EXR_ERR_SUCCESS)
                result = exr_decoding_choose_default_routines(context, 0, &decoder);
            if (result == EXR_ERR_SUCCESS)
                result = exr_decoding_run(context, 0, &decoder);
            exr_decoding_destroy(context, &decoder);
        }
        if (result != EXR_ERR_SUCCESS)
        {
            error->code = openexr_error(result, source.failure);
            goto done;
        }
    }

    {
        const uint64_t pixels = (uint64_t)size.width * size.height;
        uint8_t *native = lv_workspace_allocate(core, request, (size_t)(pixels * 4));
        if (!native)
        {
            error->code = LV_ERROR_OUT_OF_MEMORY;
            goto done;
        }
        openexr_fill_rgba(native, planes[red_index], planes[green_index], planes[blue_index],
                          alpha_index >= 0 ? planes[alpha_index] : nullptr, pixels);
        if (!lv_resample_rgba(core, request, native, size, frame.data, canvas))
        {
            lv_workspace_release(core, request, native, (size_t)(pixels * 4));
            goto done;
        }
        lv_workspace_release(core, request, native, (size_t)(pixels * 4));
    }
    if (!lv_frame_finish(&frame, error))
        goto done;
    error->code = 0;

done:
    if (lv_stopped(core, request))
        error->code = 0;
    for (int i = 0; i < 4; ++i)
        if (planes[i])
            lv_workspace_release(core, request, planes[i], (size_t)plane_bytes);
    if (context)
        exr_finish(&context);
    lv_frame_release(&frame);
}
