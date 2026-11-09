#include <math.h>
#include <plutosvg.h>
#include <string.h>
#include "codec.h"

// Restrict the initial vector profile explicitly: PlutoSVG otherwise silently
// drops unsupported elements. This is admission checking, not an XML parser.
static bool svg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool svg_element(const char *name, size_t length)
{
    static const char *const allowed[] = {"svg",     "g",     "defs",     "path",    "rect",           "circle",
                                          "ellipse", "line",  "polyline", "polygon", "linearGradient", "radialGradient",
                                          "stop",    "title", "desc"};
    for (size_t i = 0; i < sizeof allowed / sizeof *allowed; ++i)
        if (strlen(allowed[i]) == length && !memcmp(name, allowed[i], length))
            return true;
    return false;
}

static const char *svg_tag_end(const char *p)
{
    char quote = 0;
    for (; *p; ++p)
    {
        if (quote)
        {
            if (*p == quote)
                quote = 0;
        }
        else if (*p == '\'' || *p == '"')
            quote = *p;
        else if (*p == '>')
            return p;
    }
    return nullptr;
}

static bool svg_effects(const char *begin, const char *end)
{
    // This conservative profile rejects CSS classes and effects even in style
    // attributes; none of them is implemented by this renderer. Scan once.
    static const char *const effects[] = {"clip-path", "filter", "mask", "class"};
    for (const char *p = begin; p != end; ++p)
        for (size_t i = 0; i < sizeof effects / sizeof *effects; ++i)
        {
            const size_t length = strlen(effects[i]);
            if ((size_t)(end - p) >= length && !memcmp(p, effects[i], length))
                return false;
        }
    return true;
}

static bool svg_profile(lv_core *core, lv_request *r, const char *data, size_t bytes, lv_error_info *error)
{
    if (memchr(data, 0, bytes))
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    uint32_t elements = 0, depth = 0;
    for (const char *p = data; (p = strchr(p, '<'));)
    {
        if (lv_stopped(core, r))
            return false;
        if (!strncmp(p, "<!--", 4) || !strncmp(p, "<?xml", 5))
        {
            const bool comment = p[1] == '!';
            p                  = strstr(p + (comment ? 4 : 5), comment ? "-->" : "?>");
            if (!p)
                goto corrupt;
            p += comment ? 3 : 2;
            continue;
        }
        const bool closing = p[1] == '/';
        const char *name = p + (closing ? 2 : 1), *end = name;
        while ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z'))
            ++end;
        if (!svg_element(name, (size_t)(end - name)) || (!svg_space(*end) && *end != '/' && *end != '>'))
            goto unsupported;
        const char *tag_end = svg_tag_end(end);
        if (!tag_end || (closing && !depth))
            goto corrupt;
        if (!svg_effects(end, tag_end))
            goto unsupported;
        if (closing)
            --depth;
        else
        {
            ++elements;
            if (tag_end[-1] != '/')
                ++depth;
        }
        if (depth > 128 || elements > 8192)
        {
            error->code = LV_ERROR_DIMENSION_LIMIT;
            return false;
        }
        p = tag_end + 1;
    }
    if (depth || !elements)
        goto corrupt;
    return true;
unsupported:
    error->code = LV_ERROR_FORMAT_UNSUPPORTED;
    return false;
corrupt:
    error->code = LV_ERROR_CORRUPT_DATA;
    return false;
}

static bool svg_canvas(lv_request *r, plutosvg_document_t *document, lv_size_u32 *source, lv_size_u32 *canvas,
                       lv_error_info *error)
{
    const double w = plutosvg_document_get_width(document), h = plutosvg_document_get_height(document);
    if (!isfinite(w) || !isfinite(h) || w <= 0 || h <= 0 || w > UINT32_MAX || h > UINT32_MAX)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return false;
    }
    *source = (lv_size_u32){(uint32_t)ceil(w), (uint32_t)ceil(h)};
    if (r->desc.target.kind == LV_TARGET_NATIVE)
    {
        *canvas = *source;
        return true;
    }
    const lv_size_u32 bounds = r->desc.target.as.fit_bounds;
    const double scale       = fmin(bounds.width / w, bounds.height / h);
    const double out_w = floor(w * scale), out_h = floor(h * scale);
    *canvas = (lv_size_u32){out_w >= 1 ? (uint32_t)fmin(out_w, bounds.width) : 1,
                            out_h >= 1 ? (uint32_t)fmin(out_h, bounds.height) : 1};
    return true;
}

static bool svg_render(lv_frame *f, plutosvg_document_t *document, lv_error_info *error)
{
    if (lv_stopped(f->core, f->request))
        return false;
    const lv_size_u32 size = f->info.canvas;
    memset(f->data, 0, f->bytes);
    plutovg_surface_t *surface =
        plutovg_surface_create_for_data(f->data, (int)size.width, (int)size.height, (int)(size.width * 4));
    if (!surface)
    {
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    plutovg_canvas_t *canvas = plutovg_canvas_create(surface);
    if (!canvas)
    {
        plutovg_surface_destroy(surface);
        error->code = LV_ERROR_OUT_OF_MEMORY;
        return false;
    }
    plutovg_canvas_scale(canvas, (float)size.width / plutosvg_document_get_width(document),
                         (float)size.height / plutosvg_document_get_height(document));
    const bool ok = plutosvg_document_render(document, nullptr, canvas, nullptr, nullptr, nullptr);
    plutovg_canvas_destroy(canvas);
    plutovg_surface_destroy(surface);
    if (!ok)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        return false;
    }
    // Renderer contract is straight RGBA; PlutoVG writes native premultiplied ARGB.
    plutovg_convert_argb_to_rgba(f->data, f->data, (int)size.width, (int)size.height, (int)(size.width * 4));
    return !lv_stopped(f->core, f->request);
}

void lv_decode_svg(lv_core *core, lv_request *r, lv_input *input, lv_error_info *error)
{
    if (input->version.bytes > UINT64_C(1) * 1024 * 1024)
    {
        error->code = LV_ERROR_DIMENSION_LIMIT;
        return;
    }
    uint8_t *data = lv_input_read_all(core, r, input, error);
    if (!data)
        return;
    lv_frame f                    = {.core = core, .request = r, .input = input};
    plutosvg_document_t *document = nullptr;
    if (!svg_profile(core, r, (const char *)data, (size_t)input->version.bytes, error))
        goto done;
    // Bound document complexity and conservatively reserve foreign parse memory.
    if (!lv_reserve(core, r, UINT64_C(4) * 1024 * 1024 + input->version.bytes * 96))
    {
        error->code = LV_ERROR_MEMORY_BUDGET;
        goto done;
    }
    document = plutosvg_document_load_from_data((const char *)data, (int)input->version.bytes, -1, -1, nullptr, nullptr);
    if (!document)
    {
        error->code = LV_ERROR_CORRUPT_DATA;
        goto done;
    }
    lv_size_u32 source, canvas;
    if (!svg_canvas(r, document, &source, &canvas, error))
        goto done;
    if (!lv_bitmap_size(core, canvas.width, canvas.height, error))
        goto done;
    const uint64_t scratch = UINT64_C(4) * 1024 * 1024 + (uint64_t)canvas.width * canvas.height * 8;
    if (lv_frame_prepare(&f, source, canvas, LV_FORMAT_SVG, LV_CAP_VECTOR_RASTERIZE, scratch, error) &&
        lv_frame_info(&f, error) && svg_render(&f, document, error))
    {
        plutosvg_document_destroy(document);
        document = nullptr;
        lv_workspace_release(core, r, data, (size_t)input->version.bytes + 1);
        data = nullptr;
        lv_frame_finish(&f, error);
    }
done:
    plutosvg_document_destroy(document);
    lv_workspace_release(core, r, data, (size_t)input->version.bytes + 1);
    lv_frame_release(&f);
}
