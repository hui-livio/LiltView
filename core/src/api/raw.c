#include <limits.h>
#include <string.h>
#include "../internal.h"

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b)
        return false;
    *out = a + b;
    return true;
}

static bool mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

static uint64_t raw_hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

static bool raw_layout_valid(const lv_raw_layout_desc *layout)
{
    if (layout->struct_size != sizeof *layout || !layout->size.width || !layout->size.height ||
        layout->orientation != LV_ORIENTATION_NORMAL ||
        (layout->row_order != LV_RAW_TOP_DOWN && layout->row_order != LV_RAW_BOTTOM_UP) ||
        (layout->byte_order != LV_RAW_BYTE_NATIVE && layout->byte_order != LV_RAW_BYTE_LITTLE &&
         layout->byte_order != LV_RAW_BYTE_BIG) || !layout->plane_count || layout->plane_count > LV_RAW_MAX_PLANES)
        return false;
    uint64_t minimum_row = 0;
    uint32_t expected_planes = 1;
    switch (layout->layout)
    {
    case LV_RAW_LAYOUT_RGB24:
    case LV_RAW_LAYOUT_BGR24:
        if (!mul_u64(layout->size.width, 3, &minimum_row)) return false;
        break;
    case LV_RAW_LAYOUT_RGBA8:
    case LV_RAW_LAYOUT_BGRA8:
        if (!mul_u64(layout->size.width, 4, &minimum_row)) return false;
        break;
    case LV_RAW_LAYOUT_GRAY8:
    case LV_RAW_LAYOUT_BAYER8:
        minimum_row = layout->size.width;
        break;
    case LV_RAW_LAYOUT_BAYER16:
        if (!mul_u64(layout->size.width, 2, &minimum_row)) return false;
        break;
    case LV_RAW_LAYOUT_I420:
    case LV_RAW_LAYOUT_YV12:
        if ((layout->size.width & 1u) || (layout->size.height & 1u) || !mul_u64(layout->size.width, 1, &minimum_row))
            return false;
        expected_planes = 3;
        break;
    case LV_RAW_LAYOUT_NV12:
    case LV_RAW_LAYOUT_NV21:
        if ((layout->size.width & 1u) || (layout->size.height & 1u) || !mul_u64(layout->size.width, 1, &minimum_row))
            return false;
        expected_planes = 2;
        break;
    default:
        return false;
    }
    if (layout->plane_count != expected_planes)
        return false;
    for (uint32_t i = 0; i < layout->plane_count; ++i)
    {
        const lv_raw_plane *plane = &layout->planes[i];
        const uint32_t expected_height = (layout->layout == LV_RAW_LAYOUT_I420 || layout->layout == LV_RAW_LAYOUT_YV12 ||
                                          ((layout->layout == LV_RAW_LAYOUT_NV12 || layout->layout == LV_RAW_LAYOUT_NV21) && i == 1))
            ? layout->size.height / 2 : layout->size.height;
        const uint64_t expected_row = (layout->layout == LV_RAW_LAYOUT_I420 || layout->layout == LV_RAW_LAYOUT_YV12) && i > 0
            ? layout->size.width / 2 : minimum_row;
        if (plane->height != expected_height || plane->row_bytes < expected_row || plane->stride < plane->row_bytes)
            return false;
        uint64_t rows = plane->height ? plane->height - 1 : 0;
        uint64_t end = 0, span = 0;
        if (!mul_u64(rows, plane->stride, &span) || !add_u64(plane->offset, span, &end) ||
            !add_u64(end, plane->row_bytes, &end))
            return false;
    }
    return true;
}

static bool raw_processing_valid(const lv_raw_processing *processing)
{
    if (processing->struct_size != sizeof *processing || (processing->demosaic != LV_RAW_DEMOSAIC_NEAREST &&
                                                           processing->demosaic != LV_RAW_DEMOSAIC_BILINEAR))
        return false;
    if (processing->matrix > LV_RAW_MATRIX_BT2020 || processing->range > LV_RAW_RANGE_LIMITED ||
        processing->chroma_siting < LV_RAW_CHROMA_CENTER || processing->chroma_siting > LV_RAW_CHROMA_COSITED ||
        processing->primaries > LV_RAW_PRIMARIES_BT709 || processing->transfer > LV_RAW_TRANSFER_SRGB ||
        processing->cfa > LV_RAW_CFA_GBRG)
        return false;
    if (processing->white_level <= processing->black_level)
        return false;
    return processing->red_gain_q16 && processing->green_red_gain_q16 && processing->green_blue_gain_q16 &&
        processing->blue_gain_q16;
}

static bool raw_output_valid(const lv_raw_output *output)
{
    return output->struct_size == sizeof *output && output->pixel_format == LV_PIXEL_RGBA8 && output->allow_conversion == 1 &&
        output->color_space <= LV_COLOR_SRGB;
}

uint64_t lv_raw_identity(const lv_raw_image_desc *image)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = raw_hash_bytes(hash, &image->layout, sizeof image->layout);
    hash = raw_hash_bytes(hash, &image->processing, sizeof image->processing);
    return raw_hash_bytes(hash, &image->output, sizeof image->output);
}

lv_status lv_validate_raw_request(const lv_raw_request_desc *request)
{
    if (!request || request->struct_size != sizeof *request || request->abi_version != LV_RAW_ABI_VERSION)
        return LV_ABI_MISMATCH;
    if (!raw_layout_valid(&request->image.layout) || !raw_processing_valid(&request->image.processing) ||
        !raw_output_valid(&request->image.output))
        return LV_INVALID_ARGUMENT;
    if (request->image.struct_size != sizeof request->image)
        return LV_ABI_MISMATCH;
    lv_request_desc base;
    lv_request_desc_init(&base);
    base.kind = request->kind;
    base.priority = request->priority;
    base.user_tag = request->user_tag;
    base.path = request->path;
    base.target = request->target;
    return lv_validate_request(&base);
}

uint32_t LV_CALL lv_raw_abi_version(void)
{
    return LV_RAW_ABI_VERSION;
}

void LV_CALL lv_raw_layout_desc_init(lv_raw_layout_desc *out)
{
    if (out)
    {
        *out = (lv_raw_layout_desc){0};
        out->struct_size = sizeof *out;
        out->orientation = LV_ORIENTATION_NORMAL;
        out->row_order = LV_RAW_TOP_DOWN;
        out->byte_order = LV_RAW_BYTE_NATIVE;
        out->alpha = LV_RAW_ALPHA_OPAQUE;
    }
}

void LV_CALL lv_raw_processing_init(lv_raw_processing *out)
{
    if (out)
        *out = (lv_raw_processing){.struct_size = sizeof *out,
                                   .chroma_siting = LV_RAW_CHROMA_CENTER,
                                   .demosaic = LV_RAW_DEMOSAIC_BILINEAR,
                                   .red_gain_q16 = 65536,
                                   .green_red_gain_q16 = 65536,
                                   .green_blue_gain_q16 = 65536,
                                   .blue_gain_q16 = 65536,
                                   .white_level = 255};
}

void LV_CALL lv_raw_output_init(lv_raw_output *out)
{
    if (out)
        *out = (lv_raw_output){.struct_size = sizeof *out,
                               .pixel_format = LV_PIXEL_RGBA8,
                               .color_space = LV_COLOR_UNKNOWN,
                               .allow_conversion = 1};
}

void LV_CALL lv_raw_image_desc_init(lv_raw_image_desc *out)
{
    if (!out) return;
    *out = (lv_raw_image_desc){0};
    out->struct_size = sizeof *out;
    lv_raw_layout_desc_init(&out->layout);
    lv_raw_processing_init(&out->processing);
    lv_raw_output_init(&out->output);
}

void LV_CALL lv_raw_request_desc_init(lv_raw_request_desc *out)
{
    if (!out) return;
    *out = (lv_raw_request_desc){0};
    out->struct_size = sizeof *out;
    out->abi_version = LV_RAW_ABI_VERSION;
    out->kind = LV_REQUEST_DISPLAY;
    out->priority = LV_PRIORITY_CURRENT;
    out->target.kind = LV_TARGET_FIT;
    lv_raw_image_desc_init(&out->image);
}

lv_status LV_CALL lv_core_submit_raw(lv_core *core, const lv_raw_request_desc *request, lv_request_id *out_request_id)
{
    if (out_request_id) *out_request_id = 0;
    if (!out_request_id || !core || !request)
        return LV_INVALID_ARGUMENT;
    const lv_status status = lv_validate_raw_request(request);
    if (status != LV_OK)
        return status;
    lv_request_desc base;
    lv_request_desc_init(&base);
    base.kind = request->kind;
    base.priority = request->priority;
    base.user_tag = request->user_tag;
    base.path = request->path;
    base.target = request->target;
    mtx_lock(&core->mutex);
    if (core->closing)
    {
        mtx_unlock(&core->mutex);
        return LV_CLOSING;
    }
    lv_status accepted = lv_accept_locked(core, &base, nullptr, 0, out_request_id);
    if (accepted == LV_OK)
    {
        lv_request *stored = nullptr;
        for (uint32_t i = 0; i < core->config.max_requests; ++i)
            if (core->requests[i].id == *out_request_id)
                stored = &core->requests[i];
        if (stored)
        {
            stored->raw_input = true;
            stored->raw_identity = lv_raw_identity(&request->image);
            stored->raw_image = request->image;
        }
    }
    mtx_unlock(&core->mutex);
    return accepted;
}
