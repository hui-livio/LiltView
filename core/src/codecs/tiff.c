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
    if (!lv_bitmap_size(core, size->width, size->height, error))
        return false;
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

static bool tiff_rgba(tiff_input *source, TIFF *tif, uint32_t *raster, lv_size_u32 size, lv_error_info *error)
{
    uint16_t extras = 0, *types = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &extras, &types);
    const bool alpha = extras > 0 && types && (types[0] == EXTRASAMPLE_ASSOCALPHA || types[0] == EXTRASAMPLE_UNASSALPHA);
    char message[1024];
    TIFFRGBAImage image;
    if (!TIFFRGBAImageBegin(&image, tif, 1, message))
    {
        error->code = source->failure ? source->failure : LV_ERROR_FORMAT_UNSUPPORTED;
        return false;
    }
    // Keep raw source coordinates. The existing viewer applies all 8 orientations.
    image.orientation     = ORIENTATION_TOPLEFT;
    image.req_orientation = ORIENTATION_TOPLEFT;
    const bool ok         = TIFFRGBAImageGet(&image, raster, size.width, size.height) != 0;
    TIFFRGBAImageEnd(&image);
    if (!ok || source->failure)
        return false;
    for (uint32_t y = 0; y < size.height; ++y)
    {
        if (lv_stopped(source->core, source->request))
            return false;
        for (uint32_t x = 0; x < size.width; ++x)
        {
            uint32_t *pixel      = raster + (size_t)y * size.width + x;
            const uint32_t value = *pixel;
            const uint32_t a     = alpha ? TIFFGetA(value) : 255;
            uint8_t *out         = (uint8_t *)pixel;
            // libtiff returns premultiplied numeric ABGR, independent of endian.
            const uint32_t colors[3] = {TIFFGetR(value), TIFFGetG(value), TIFFGetB(value)};
            for (unsigned c = 0; c < 3; ++c)
            {
                uint32_t v = a ? (colors[c] * 255 + a / 2) / a : 0;
                out[c]     = (uint8_t)(v > 255 ? 255 : v);
            }
            out[3] = (uint8_t)a;
        }
    }
    return true;
}

void lv_decode_tiff(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    // The per-handle cumulative allocator limit also bounds malicious IFD tables.
    if (!lv_reserve(core, r, UINT64_C(16) * 1024 * 1024))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    tiff_input source = {.core = core, .request = r, .input = input};
    TIFF *tif         = tiff_open(&source, error);
    lv_frame f        = {.core = core, .request = r, .input = input};
    uint8_t *raster = nullptr, *raw = nullptr;
    size_t bytes = 0, raw_bytes = 0;
    if (!tif || source.failure)
        goto done;
    lv_size_u32 size     = {0};
    uint16_t orientation = 1;
    if (!tiff_header(core, tif, &size, &orientation, error))
        goto done;
    uint16_t compression = 0;
    TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
    if (TIFFIsTiled(tif) && compression == COMPRESSION_NONE)
    {
        const tmsize_t tile_bytes = TIFFTileSize(tif);
        if (tile_bytes <= 0)
        {
            error->code = LV_ERROR_CORRUPT_DATA;
            goto done;
        }
        if (tile_bytes > 8 * 1024 * 1024)
        {
            error->code = LV_ERROR_MEMORY_BUDGET;
            goto done;
        }
        raw_bytes = (size_t)tile_bytes;
    }
    const lv_size_u32 canvas = lv_fit_size(size, r->desc.target.as.fit_bounds, orientation);
    bytes                    = (size_t)size.width * size.height * 4;
    // Raw RGBA plus conservative JPEG/zlib workspace outside TIFF's allocator.
    if (!lv_frame_prepare(&f, size, canvas, LV_FORMAT_TIFF, 0, (uint64_t)bytes * 2 + raw_bytes, error))
        goto done;
    if (raw_bytes)
    {
        raw = lv_workspace_allocate(core, r, raw_bytes);
        if (!raw)
        {
            error->code = LV_ERROR_OUT_OF_MEMORY;
            goto done;
        }
        // LibTIFF 4.7.2 validates uncompressed tile size against the read-buffer
        // capacity. Its default rounding breaks small tiles; supply exact storage.
        if (!TIFFReadBufferSetup(tif, raw, (tmsize_t)raw_bytes))
            goto done;
    }
    f.info.orientation = orientation;
    raster             = lv_workspace_allocate(core, r, bytes);
    if (!raster)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (!lv_frame_info(&f, error) || !tiff_rgba(&source, tif, (uint32_t *)raster, size, error) ||
        !lv_resample_rgba(core, r, raster, size, f.data, canvas))
        goto done;
    TIFFClose(tif);
    tif = nullptr;
    lv_workspace_release(core, r, raw, raw_bytes);
    raw = nullptr;
    lv_workspace_release(core, r, raster, bytes);
    raster = nullptr;
    lv_frame_finish(&f, error);
done:
    if (tif)
        TIFFClose(tif);
    lv_workspace_release(core, r, raw, raw_bytes);
    if (!error->code && !r->final_tiles && !lv_stopped(core, r))
        error->code = source.failure ? source.failure : LV_ERROR_CORRUPT_DATA;
    lv_workspace_release(core, r, raster, bytes);
    lv_frame_release(&f);
}
