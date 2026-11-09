#include <gif_lib.h>
#include <string.h>
#include "codec.h"

typedef struct gif_input
{
    lv_core *core;
    lv_request *request;
    lv_input *input;
    uint64_t remaining;
    bool io_error;
} gif_input;

static int gif_read(GifFileType *gif, GifByteType *out, int count)
{
    gif_input *s = gif->UserData;
    if (count <= 0 || lv_stopped(s->core, s->request))
        return 0;
    const size_t wanted     = (uint64_t)count < s->remaining ? (size_t)count : (size_t)s->remaining;
    const size_t got        = fread(out, 1, wanted, s->input->file);
    s->remaining           -= got;
    s->request->bytes_read += got;
    s->io_error             = ferror(s->input->file) != 0;
    return (int)got;
}

// Stop at the first image: never retain every animation frame with DGifSlurp.
static bool gif_first_image(lv_core *core, lv_request *r, GifFileType *gif, GraphicsControlBlock *control, lv_error_info *error)
{
    while (!lv_stopped(core, r))
    {
        GifRecordType type;
        if (DGifGetRecordType(gif, &type) == GIF_ERROR)
            return false;
        if (type == IMAGE_DESC_RECORD_TYPE)
            return DGifGetImageHeader(gif) == GIF_OK;
        if (type != EXTENSION_RECORD_TYPE)
            return false;
        int code;
        GifByteType *block = nullptr;
        if (DGifGetExtension(gif, &code, &block) == GIF_ERROR)
            return false;
        if (code == PLAINTEXT_EXT_FUNC_CODE)
        {
            error->code = LV_ERROR_FORMAT_UNSUPPORTED;
            return false;
        }
        if (code == GRAPHICS_EXT_FUNC_CODE &&
            (!block || block[0] != 4 || DGifExtensionToGCB(block[0], block + 1, control) == GIF_ERROR))
            return false;
        while (block)
        {
            if (lv_stopped(core, r) || DGifGetExtensionNext(gif, &block) == GIF_ERROR)
                return false;
            if (code == GRAPHICS_EXT_FUNC_CODE && block)
                return false;
        }
    }
    return false;
}

static bool gif_canvas(GifFileType *gif, const GraphicsControlBlock *control, uint8_t *image, lv_size_u32 size)
{
    uint8_t color[4] = {0};
    if (control->TransparentColor == NO_TRANSPARENT_COLOR && gif->SColorMap)
    {
        if (gif->SBackGroundColor < 0 || gif->SBackGroundColor >= gif->SColorMap->ColorCount)
            return false;
        const GifColorType bg = gif->SColorMap->Colors[gif->SBackGroundColor];
        color[0]              = bg.Red;
        color[1]              = bg.Green;
        color[2]              = bg.Blue;
        color[3]              = 255;
    }
    for (uint32_t y = 0; y < size.height; ++y)
    {
        gif_input *s = gif->UserData;
        if (lv_stopped(s->core, s->request))
            return false;
        for (uint32_t x = 0; x < size.width; ++x)
            memcpy(image + ((size_t)y * size.width + x) * 4, color, 4);
    }
    return true;
}

static bool gif_rows(GifFileType *gif, const GraphicsControlBlock *control, uint8_t *image, GifPixelType *row)
{
    const GifImageDesc *desc  = &gif->Image;
    const ColorMapObject *map = desc->ColorMap ? desc->ColorMap : gif->SColorMap;
    if (!map)
        return false;
    static const uint32_t starts[] = {0, 4, 2, 1}, steps[] = {8, 8, 4, 2};
    const unsigned passes = desc->Interlace ? 4 : 1;
    gif_input *s          = gif->UserData;
    for (unsigned pass = 0; pass < passes; ++pass)
        for (uint32_t y = desc->Interlace ? starts[pass] : 0; y < (uint32_t)desc->Height; y += desc->Interlace ? steps[pass] : 1)
        {
            if (lv_stopped(s->core, s->request) || DGifGetLine(gif, row, desc->Width) == GIF_ERROR)
                return false;
            uint8_t *out = image + (((size_t)desc->Top + y) * (uint32_t)gif->SWidth + (uint32_t)desc->Left) * 4;
            for (int x = 0; x < desc->Width; ++x, out += 4)
            {
                if (row[x] == control->TransparentColor)
                    continue;
                if (row[x] >= map->ColorCount)
                    return false;
                const GifColorType color = map->Colors[row[x]];
                out[0]                   = color.Red;
                out[1]                   = color.Green;
                out[2]                   = color.Blue;
                out[3]                   = 255;
            }
        }
    return true;
}

void lv_decode_gif(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    if (!lv_reserve(core, r, UINT64_C(1) * 1024 * 1024))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        return;
    }
    gif_input source   = {core, r, input, input->version.bytes, false};
    int code           = 0;
    GifFileType *gif   = DGifOpen(&source, gif_read, &code);
    lv_frame f         = {.core = core, .request = r, .input = input};
    uint8_t *image     = nullptr;
    GifPixelType *row  = nullptr;
    size_t image_bytes = 0, row_bytes = 0;
    bool finished = false;
    if (!gif)
        goto done;
    const lv_size_u32 size = {(uint32_t)gif->SWidth, (uint32_t)gif->SHeight};
    if (!lv_bitmap_size(core, size.width, size.height, error))
        goto done;
    GraphicsControlBlock control = {.TransparentColor = NO_TRANSPARENT_COLOR};
    if (!gif_first_image(core, r, gif, &control, error))
        goto done;
    const GifImageDesc *desc = &gif->Image;
    if (desc->Left < 0 || desc->Top < 0 || desc->Width <= 0 || desc->Height <= 0 || (uint32_t)desc->Left > size.width ||
        (uint32_t)desc->Top > size.height || (uint32_t)desc->Width > size.width - (uint32_t)desc->Left ||
        (uint32_t)desc->Height > size.height - (uint32_t)desc->Top)
        goto done;
    image_bytes = (size_t)size.width * size.height * 4;
    row_bytes   = (size_t)desc->Width;
    const lv_size_u32 canvas =
        r->desc.target.kind == LV_TARGET_NATIVE ? size : lv_fit_size(size, r->desc.target.as.fit_bounds, LV_ORIENTATION_NORMAL);
    const bool native = canvas.width == size.width && canvas.height == size.height;
    if (!lv_frame_prepare(&f, size, canvas, LV_FORMAT_GIF, 0, (native ? 0 : image_bytes) + row_bytes, error))
        goto done;
    image = native ? f.data : lv_workspace_allocate(core, r, image_bytes);
    row   = lv_workspace_allocate(core, r, row_bytes);
    if (!image || !row)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (!lv_frame_info(&f, error) || !gif_canvas(gif, &control, image, size) || !gif_rows(gif, &control, image, row))
        goto done;
    if (!native && !lv_resample_rgba(core, r, image, size, f.data, canvas))
        goto done;
    DGifCloseFile(gif, &code);
    gif = nullptr;
    if (image != f.data)
        lv_workspace_release(core, r, image, image_bytes);
    image = nullptr;
    lv_workspace_release(core, r, row, row_bytes);
    row      = nullptr;
    finished = lv_frame_finish(&f, error);
done:
    if (gif)
    {
        code = gif->Error;
        DGifCloseFile(gif, nullptr);
    }
    if (!error->code && !finished && !lv_stopped(core, r))
        error->code = source.io_error        ? LV_ERROR_IO :
            code == D_GIF_ERR_NOT_ENOUGH_MEM ? LV_ERROR_OUT_OF_MEMORY :
                                               LV_ERROR_CORRUPT_DATA;
    if (image != f.data)
        lv_workspace_release(core, r, image, image_bytes);
    lv_workspace_release(core, r, row, row_bytes);
    lv_frame_release(&f);
}
