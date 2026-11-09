#include <stdarg.h>
#include <string.h>
#include <tiffio.h>
#include "codec.h"

typedef struct tiff_input
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    uint64_t position;
    lv_error_code failure;
} tiff_input;

static tmsize_t tiff_read(thandle_t handle, void *data, tmsize_t count)
{
    tiff_input *s = handle;
    if (count < 0 || lv_stopped(s->core, s->request))
        return -1;
    uint64_t wanted = (uint64_t)count;
    if (wanted > s->input->version.bytes - s->position)
        wanted = s->input->version.bytes - s->position;
    size_t got = 0;
    while (got < wanted && !lv_stopped(s->core, s->request))
    {
        const size_t chunk  = wanted - got < 16384 ? (size_t)(wanted - got) : 16384;
        const size_t n      = fread((uint8_t *)data + got, 1, chunk, s->input->file);
        got                += n;
        if (n != chunk)
            break;
    }
    s->position            += got;
    s->request->bytes_read += got;
    if (ferror(s->input->file))
        s->failure = LV_ERROR_IO;
    return (tmsize_t)got;
}
static tmsize_t tiff_write(thandle_t handle, void *data, tmsize_t bytes)
{
    (void)handle;
    (void)data;
    (void)bytes;
    return -1;
}
static toff_t tiff_seek(thandle_t handle, toff_t offset, int whence)
{
    tiff_input *s = handle;
    if (lv_stopped(s->core, s->request))
        return (toff_t)-1;
    uint64_t target;
    if (whence == SEEK_SET)
        target = offset;
    else if (whence == SEEK_CUR || whence == SEEK_END)
    {
        const uint64_t base = whence == SEEK_CUR ? s->position : s->input->version.bytes;
        // TIFF uses unsigned offsets even for negative relative seeks.
        if (offset > INT64_MAX)
        {
            const uint64_t backwards = ~offset + 1;
            if (backwards > base)
                return (toff_t)-1;
            target = base - backwards;
        }
        else
        {
            if (offset > s->input->version.bytes - base)
                return (toff_t)-1;
            target = base + offset;
        }
    }
    else
        return (toff_t)-1;
    if (target > s->input->version.bytes)
        return (toff_t)-1;
    if (fseek(s->input->file, (long)target, SEEK_SET))
    {
        s->failure = LV_ERROR_IO;
        return (toff_t)-1;
    }
    s->position = target;
    return target;
}
static int tiff_close(thandle_t handle)
{
    (void)handle;
    return 0; // The shared input owns FILE, not libtiff.
}
static toff_t tiff_size(thandle_t handle)
{
    return ((tiff_input *)handle)->input->version.bytes;
}
static int tiff_map(thandle_t handle, void **base, toff_t *bytes)
{
    (void)handle;
    (void)base;
    (void)bytes;
    return 0; // Keep all I/O bounded and cancellable; do not mmap.
}
static void tiff_unmap(thandle_t handle, void *base, toff_t bytes)
{
    (void)handle;
    (void)base;
    (void)bytes;
}
static int tiff_error(TIFF *tif, void *userdata, const char *module, const char *format, va_list args)
{
    (void)tif;
    (void)args;
    tiff_input *s = userdata;
    if (strstr(format, "compression support is not configured"))
        s->failure = LV_ERROR_FORMAT_UNSUPPORTED;
    else if (strstr(format, "limit defined in open options"))
        s->failure = LV_ERROR_MEMORY_BUDGET;
    else if (module && strstr(module, "allocExt"))
        s->failure = LV_ERROR_OUT_OF_MEMORY;
    else if (!s->failure)
        s->failure = LV_ERROR_CORRUPT_DATA;
    return 1; // Per-handle handlers do not change process-global library state.
}
static int tiff_warning(TIFF *tif, void *userdata, const char *module, const char *format, va_list args)
{
    (void)tif;
    (void)userdata;
    (void)module;
    (void)format;
    (void)args;
    return 1;
}

static TIFF *tiff_open(tiff_input *source, lv_error_info *error)
{
    TIFFOpenOptions *options = TIFFOpenOptionsAlloc();
    if (!options)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return nullptr;
    }
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, 8 * 1024 * 1024);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(options, 16 * 1024 * 1024);
    TIFFOpenOptionsSetErrorHandlerExtR(options, tiff_error, source);
    TIFFOpenOptionsSetWarningHandlerExtR(options, tiff_warning, source);
    TIFF *tif = TIFFClientOpenExt("LiltView input", "r", source, tiff_read, tiff_write, tiff_seek, tiff_close, tiff_size,
                                  tiff_map, tiff_unmap, options);
    TIFFOpenOptionsFree(options);
    return tif;
}

static bool tiff_header(lv_core *core, TIFF *tif, lv_size_u32 *size, uint16_t *orientation, lv_error_info *error)
{
    if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &size->width) || !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &size->height))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    (void)core;
    if (!size->width || !size->height || size->width > INT32_MAX || size->height > INT32_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    uint16_t depth = 0, samples = 0, sample_format = 0, compression = 0, photo = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &depth);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samples);
    TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, orientation);
    if (!samples || samples > 4 || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) ||
        sample_format != SAMPLEFORMAT_UINT || !TIFFIsCODECConfigured(compression) ||
        (photo != PHOTOMETRIC_MINISWHITE && photo != PHOTOMETRIC_MINISBLACK && photo != PHOTOMETRIC_RGB &&
         photo != PHOTOMETRIC_PALETTE && photo != PHOTOMETRIC_YCBCR))
    {
        error->code = LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    if (*orientation < 1 || *orientation > 8)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    return true;
}

static bool tiff_rgba(tiff_input *source, TIFF *tif, uint32_t *raster, lv_size_u32 size, uint32_t origin_x, uint32_t origin_y,
                      lv_error_info *error)
{
    char message[1024];
    TIFFRGBAImage image;
    if (!TIFFRGBAImageBegin(&image, tif, 1, message))
    {
        error->code = source->failure ? source->failure : LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    // Keep raw source coordinates. The existing viewer applies all 8 orientations.
    image.col_offset      = (int32_t)origin_x;
    image.row_offset      = (int32_t)origin_y;
    image.orientation     = ORIENTATION_TOPLEFT;
    image.req_orientation = ORIENTATION_TOPLEFT;
    const bool ok         = TIFFRGBAImageGet(&image, raster, size.width, size.height) != 0;
    TIFFRGBAImageEnd(&image);
    if (!ok || source->failure)
        return false;

    return true;
}

static bool tiff_sample_blocks(tiff_input *source, TIFF *tif, lv_frame *f, uint32_t *raster, lv_size_u32 block,
                               lv_error_info *error)
{
    uint16_t extras = 0, *types = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extras, &types);
    const bool alpha         = extras > 0 && types && (types[0] == EXTRASAMPLE_ASSOCALPHA || types[0] == EXTRASAMPLE_UNASSALPHA);
    const lv_rect_u32 region = f->info.region;
    const lv_size_u32 size = f->info.source_size, canvas = f->info.canvas;
    for (uint32_t y = 0; y < region.height;)
    {
        const uint32_t sy = lv_sample_nearest(region.y + y, size.height, canvas.height);
        const uint32_t by = sy / block.height * block.height;
        uint32_t end_y    = y + 1;
        while (end_y < region.height &&
               (!f->streaming || block.width >= region.width ||
                (region.y + end_y) / f->info.tile_edge == (region.y + y) / f->info.tile_edge) &&
               lv_sample_nearest(region.y + end_y, size.height, canvas.height) / block.height == by / block.height)
            ++end_y;
        for (uint32_t x = 0; x < region.width;)
        {
            if (lv_stopped(source->core, source->request))
                return false;
            const uint32_t sx = lv_sample_nearest(region.x + x, size.width, canvas.width);
            const uint32_t bx = sx / block.width * block.width;
            uint32_t end_x    = x + 1;
            while (end_x < region.width &&
                   lv_sample_nearest(region.x + end_x, size.width, canvas.width) / block.width == bx / block.width)
                ++end_x;
            const uint32_t raster_y  = f->streaming ? region.y + y : by;
            const lv_size_u32 extent = {size.width - bx < block.width ? size.width - bx : block.width,
                                        f->streaming ? end_y - y :
                                                       (size.height - by < block.height ? size.height - by : block.height)};
            if (!tiff_rgba(source, tif, raster, extent, bx, raster_y, error))
                return false;

            for (uint32_t oy = y; oy < end_y; ++oy)
            {
                const uint32_t py = lv_sample_nearest(region.y + oy, size.height, canvas.height) - raster_y;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                if (extras == 0 && canvas.width == size.width && canvas.height == size.height)
                {
                    // TIFFRGBA's opaque packed words are already native RGBA8
                    // on little-endian machines: no per-pixel sampling/division.
                    for (uint32_t ox = x; ox < end_x;)
                    {
                        uint32_t span = 0;
                        uint8_t *out  = lv_frame_span(f, ox, oy, &span);
                        if (!out)
                        {
                            error->code = f->failure;
                            return false;
                        }
                        if (span > end_x - ox)
                            span = end_x - ox;
                        memcpy(out, raster + (size_t)py * extent.width + region.x + ox - bx, (size_t)span * 4);
                        ox += span;
                    }
                    if (x == 0 && end_x == region.width && !lv_frame_flush_rows(f, oy + 1, error))
                        return false;
                    continue;
                }
#endif
                uint8_t *out       = nullptr;
                uint32_t remaining = 0;
                for (uint32_t ox = x; ox < end_x; ++ox, out += 4, --remaining)
                {
                    if (!remaining)
                    {
                        out = lv_frame_span(f, ox, oy, &remaining);
                        if (!out)
                        {
                            error->code = f->failure;
                            return false;
                        }
                    }
                    const uint32_t px    = lv_sample_nearest(region.x + ox, size.width, canvas.width) - bx;
                    const uint32_t value = raster[(size_t)py * extent.width + px], a = alpha ? TIFFGetA(value) : 255;
                    const uint32_t channels[3] = {TIFFGetR(value), TIFFGetG(value), TIFFGetB(value)};
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const uint32_t v = a ? (channels[c] * 255 + a / 2) / a : 0;
                        out[c]           = (uint8_t)(v > 255 ? 255 : v);
                    }
                    out[3] = (uint8_t)a;
                }
                if (x == 0 && end_x == region.width && !lv_frame_flush_rows(f, oy + 1, error))
                    return false;
            }
            x = end_x;
        }
        if (!lv_frame_flush_rows(f, end_y, error))
            return false;
        y = end_y;
    }
    return true;
}

void lv_decode_tiff(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    if (!lv_reserve(core, r, UINT64_C(16) * 1024 * 1024))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    tiff_input source  = {.core = core, .request = r, .input = input};
    TIFF *tif          = tiff_open(&source, error);
    lv_frame frame     = {.core = core, .request = r, .input = input};
    uint32_t *raster   = nullptr;
    uint8_t *raw       = nullptr;
    size_t block_bytes = 0, raw_bytes = 0;
    bool finished = false;
    if (!tif || source.failure)
        goto done;
    lv_size_u32 size     = {0}, canvas;
    uint16_t orientation = 1, compression = 0;
    if (!tiff_header(core, tif, &size, &orientation, error) || !lv_target_canvas(r, size, orientation, &canvas, error))
        goto done;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    lv_size_u32 block = {256, 256};
    if (!TIFFIsTiled(tif))
    {
        uint32_t strip_height = 1;
        TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &strip_height);
        block.width  = size.width < 1024 ? size.width : 1024;
        block.height = strip_height < 64 ? strip_height : 64;
        if (!block.height)
            block.height = 1;
    }
    if (TIFFIsTiled(tif) && compression == COMPRESSION_NONE)
    {
        const tmsize_t tile = TIFFTileSize(tif);
        if (tile <= 0)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            goto done;
        }
        if (tile > 8 * 1024 * 1024)
        {
            error->code = LV_ERROR_MEMORY_BUDGET;
            goto done;
        }
        raw_bytes = (size_t)tile;
    }
    block_bytes = (size_t)block.width * block.height * 4;
    if (!lv_frame_prepare(&frame, size, canvas, LV_FORMAT_TIFF, LV_CAP_REGION_DECODE, block_bytes + raw_bytes, error))
        goto done;
    frame.info.orientation = orientation;
    raster                 = lv_workspace_allocate(core, r, block_bytes);
    if (!raster)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (raw_bytes)
    {
        raw = lv_workspace_allocate(core, r, raw_bytes);
        if (!raw)
        {
            error->code = LV_ERROR_OUT_OF_MEMORY;
            goto done;
        }
        if (!TIFFReadBufferSetup(tif, raw, (tmsize_t)raw_bytes))
            goto done;
    }
    if (lv_frame_info(&frame, error) && tiff_sample_blocks(&source, tif, &frame, raster, block, error))
    {
        TIFFClose(tif);
        tif = nullptr;
        lv_workspace_release(core, r, raster, block_bytes);
        raster = nullptr;
        lv_workspace_release(core, r, raw, raw_bytes);
        raw      = nullptr;
        finished = lv_frame_finish(&frame, error);
    }
done:
    if (tif)
        TIFFClose(tif);
    if (!error->code && !finished && !lv_stopped(core, r))
        error->code = source.failure ? source.failure : LV_ERROR_CORRUPT_DATA;
    lv_workspace_release(core, r, raster, block_bytes);
    lv_workspace_release(core, r, raw, raw_bytes);
    lv_frame_release(&frame);
}
