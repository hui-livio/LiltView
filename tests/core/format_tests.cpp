#include "support.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <png.h>
#include <string>
#include <webp/encode.h>
#include <zlib.h>

namespace
{
using namespace liltview::test;
using Bytes = std::vector<unsigned char>;

void put16(Bytes &out, std::size_t at, unsigned value)
{
    out.at(at)     = static_cast<unsigned char>(value);
    out.at(at + 1) = static_cast<unsigned char>(value >> 8);
}
void put32(Bytes &out, std::size_t at, std::uint32_t value)
{
    put16(out, at, value & 0xffffu);
    put16(out, at + 2, value >> 16);
}

Bytes png(bool interlaced = false, int depth = 8, bool indexed = false, bool gray = false, bool alpha_edge = false)
{
    Bytes output;
    png_structp writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    REQUIRE(writer);
    png_infop info = png_create_info_struct(writer);
    REQUIRE(info);
    if (setjmp(png_jmpbuf(writer)))
    {
        png_destroy_write_struct(&writer, &info);
        throw std::runtime_error("PNG fixture encoding failed");
    }
    png_set_write_fn(
        writer, &output,
        [](png_structp encoder, png_bytep data, std::size_t count)
        {
            auto &result = *static_cast<Bytes *>(png_get_io_ptr(encoder));
            result.insert(result.end(), data, data + count);
        },
        [](png_structp) {});
    const int color = indexed ? PNG_COLOR_TYPE_PALETTE : gray ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGBA;
    png_set_IHDR(writer, info, 7, 5, depth, color, interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_color palette[2]{{255, 0, 0}, {0, 0, 255}};
    png_byte alpha[2]{128, 0};
    if (indexed)
    {
        png_set_PLTE(writer, info, palette, 2);
        png_set_tRNS(writer, info, alpha, 2, nullptr);
    }
    png_write_info(writer, info);
    const auto stride = static_cast<std::size_t>(png_get_rowbytes(writer, info));
    Bytes pixels(stride * 5, 0);
    if (!indexed)
    {
        if (gray)
            std::fill(pixels.begin(), pixels.end(), 0xff);
        else
            for (std::size_t y = 0; y < 5; ++y)
                for (std::size_t x = 0; x < 7; ++x)
                {
                    auto *p = pixels.data() + y * stride + x * (depth == 16 ? 8 : 4);
                    if (depth == 16)
                    {
                        p[0] = p[1] = 255;
                        p[6] = p[7] = 128;
                    }
                    else
                    {
                        p[0] = 255;
                        p[3] = 128;
                        if (alpha_edge)
                        {
                            p[0] = x < 3 ? 255 : 0;
                            p[2] = x < 3 ? 0 : 255;
                            p[3] = x < 3 ? 255 : 0;
                        }
                    }
                }
    }
    std::array<png_bytep, 5> rows{};
    for (std::size_t y = 0; y < rows.size(); ++y)
        rows[y] = pixels.data() + y * stride;
    png_write_image(writer, rows.data());
    png_write_end(writer, info);
    png_destroy_write_struct(&writer, &info);
    return output;
}

Bytes webp(bool lossless)
{
    std::array<std::uint8_t, 7 * 5 * 4> pixels{};
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i]     = 255;
        pixels[i + 3] = 128;
    }
    std::uint8_t *output    = nullptr;
    const std::size_t count = lossless ? WebPEncodeLosslessRGBA(pixels.data(), 7, 5, 7 * 4, &output) :
                                         WebPEncodeRGBA(pixels.data(), 7, 5, 7 * 4, 95, &output);
    REQUIRE(count);
    Bytes result(output, output + count);
    WebPFree(output);
    return result;
}

Bytes svg(const std::string &body, const std::string &attributes = "width='7' height='5'")
{
    const std::string text =
        "<?xml version='1.0'?>\n<svg xmlns='http://www.w3.org/2000/svg' " + attributes + ">" + body + "</svg>";
    return {text.begin(), text.end()};
}
Bytes red_svg()
{
    return svg("<rect width='7' height='5' fill='#ff0000' fill-opacity='0.5'/>");
}

Bytes bmp(unsigned depth = 24, bool top_down = false, unsigned compression = 0, bool alpha = false)
{
    const unsigned palette        = depth <= 8 ? 2 : 0;
    const unsigned masks          = compression == 3 ? (alpha ? 16u : 12u) : 0;
    const unsigned dib            = alpha ? 56 : 40;
    const unsigned external_masks = dib == 40 ? masks : 0;
    const unsigned offset         = 14 + dib + external_masks + palette * 4;
    const unsigned stride         = ((7 * depth + 31) / 32) * 4;
    Bytes data(offset + stride * 5, 0);
    data[0] = 'B';
    data[1] = 'M';
    put32(data, 2, static_cast<std::uint32_t>(data.size()));
    put32(data, 10, offset);
    put32(data, 14, dib);
    put32(data, 18, 7);
    put32(data, 22, top_down ? 0u - 5u : 5u);
    put16(data, 26, 1);
    put16(data, 28, depth);
    put32(data, 30, compression);
    put32(data, 46, palette);
    if (masks)
    {
        put32(data, 54, depth == 16 ? 0xf800 : 0xff0000);
        put32(data, 58, depth == 16 ? 0x07e0 : 0x00ff00);
        put32(data, 62, depth == 16 ? 0x001f : 0x0000ff);
        if (alpha)
            put32(data, 66, 0xff000000);
    }
    if (palette)
    {
        data[offset - 8 + 2] = 255; // index zero: red
        data[offset - 4]     = 255; // index one: blue
    }
    for (unsigned file_y = 0; file_y < 5; ++file_y)
        for (unsigned x = 0; x < 7; ++x)
        {
            const unsigned logical_y = top_down ? file_y : 4 - file_y;
            const bool red           = logical_y < 3;
            const auto at            = offset + file_y * stride;
            if (palette)
            {
                if (!red)
                    data[at + x * depth / 8] |= static_cast<unsigned char>(1u << (8 - depth - (x * depth) % 8));
            }
            else if (depth == 24)
                data[at + x * 3 + (red ? 2 : 0)] = 255;
            else if (depth == 16)
                put16(data, at + x * 2, red ? (compression ? 0xf800u : 0x7c00u) : 0x001fu);
            else
                put32(data, at + x * 4, (red ? 0xff0000u : 0xffu) | (alpha ? 0x80000000u : 0));
        }
    return data;
}

Bytes bmp_rle(unsigned depth, bool absolute)
{
    auto data             = bmp(depth);
    const unsigned offset = 62;
    data.resize(offset);
    put32(data, 30, depth == 8 ? 1 : 2);
    for (unsigned y = 0; y < 5; ++y)
    {
        const unsigned char index = y >= 2 ? 0 : depth == 8 ? 1 : 0x11;
        if (absolute)
        {
            data.insert(data.end(), {0, 7});
            data.insert(data.end(), depth == 8 ? 7 : 4, index);
            if (depth == 8)
                data.push_back(0);
        }
        else
            data.insert(data.end(), {7, index});
        data.insert(data.end(), {0, 0});
    }
    data.insert(data.end(), {0, 1});
    put32(data, 2, static_cast<std::uint32_t>(data.size()));
    return data;
}

struct Decoded
{
    lv_image_info info{};
    lv_event_type terminal = 0;
    lv_error_info error{};
    Bytes pixels;
};
Decoded collect(Engine &engine)
{
    Decoded result;
    std::uint64_t area = 0, sequence = 0;
    for (;;)
    {
        Event event(engine.next());
        const auto &v = event.view();
        CHECK(v.sequence > sequence);
        sequence = v.sequence;
        if (v.type == LV_EVENT_IMAGE_INFO)
        {
            result.info = v.as.image;
            result.pixels.resize(static_cast<std::size_t>(result.info.canvas.width) * result.info.canvas.height * 4);
        }
        else if (v.type == LV_EVENT_REGION_READY)
        {
            REQUIRE_FALSE(result.pixels.empty());
            const auto &region = v.as.region;
            const auto &p      = *lv_pixels_get_view(region.pixels);
            REQUIRE(region.rect.x + region.rect.width <= result.info.canvas.width);
            REQUIRE(region.rect.y + region.rect.height <= result.info.canvas.height);
            for (unsigned y = 0; y < region.rect.height; ++y)
                std::copy_n(p.data + y * p.stride_bytes, region.rect.width * 4,
                            result.pixels.data() +
                                (static_cast<std::size_t>(region.rect.y + y) * result.info.canvas.width + region.rect.x) * 4);
            area += static_cast<std::uint64_t>(region.rect.width) * region.rect.height;
        }
        else
        {
            result.terminal = v.type;
            if (v.type == LV_EVENT_ERROR)
                result.error = v.as.error;
            if (v.type == LV_EVENT_COMPLETE)
                CHECK(area == static_cast<std::uint64_t>(result.info.canvas.width) * result.info.canvas.height);
            return result;
        }
    }
}
Decoded decode(const Bytes &bytes, lv_size_u32 bounds = {100, 100})
{
    Files files;
    Engine engine;
    engine.submit(files.write("图片.wrong-extension", bytes), bounds);
    return collect(engine);
}
void red_alpha(const Decoded &result)
{
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    REQUIRE_FALSE(result.pixels.empty());
    for (std::size_t i = 0; i < result.pixels.size(); i += 4)
    {
        CHECK(result.pixels[i] >= 250);
        CHECK(result.pixels[i + 1] <= 5);
        CHECK(result.pixels[i + 2] <= 5);
        CHECK(result.pixels[i + 3] >= 127);
        CHECK(result.pixels[i + 3] <= 129);
    }
}
} // namespace

TEST_CASE("PNG normal Adam7 palette transparency and 16-bit decode to straight RGBA", "[formats][png]")
{
    for (bool interlaced : {false, true})
    {
        for (int depth : {8, 16})
        {
            auto result = decode(png(interlaced, depth));
            CHECK(result.info.format == LV_FORMAT_PNG);
            CHECK(result.info.canvas.width == 7);
            CHECK(result.info.canvas.height == 5);
            red_alpha(result);
        }
        for (int depth : {1, 2, 4, 8})
            red_alpha(decode(png(interlaced, depth, true)));
        for (int depth : {1, 2, 4, 8, 16})
        {
            auto result = decode(png(interlaced, depth, false, true));
            REQUIRE(result.terminal == LV_EVENT_COMPLETE);
            CHECK(std::all_of(result.pixels.begin(), result.pixels.end(), [](unsigned char c) { return c == 255; }));
        }
    }
}

TEST_CASE("WebP lossy lossless and alpha use decoder-scaled FIT", "[formats][webp]")
{
    for (bool lossless : {false, true})
    {
        const auto data = webp(lossless);
        red_alpha(decode(data));
        auto small = decode(data, {3, 3});
        REQUIRE(small.terminal == LV_EVENT_COMPLETE);
        CHECK(small.info.format == LV_FORMAT_WEBP);
        CHECK(small.info.source_size.width == 7);
        CHECK(small.info.canvas.width == 3);
        CHECK(small.info.canvas.height == 2);
        red_alpha(small);
    }
}

TEST_CASE("BMP palette truecolor bitfields row padding and top-down order", "[formats][bmp]")
{
    for (unsigned depth : {1u, 4u, 8u, 16u, 24u, 32u})
        for (bool top_down : {false, true})
            for (unsigned compression : {0u, 3u})
            {
                if (compression && depth != 16 && depth != 32)
                    continue;
                CAPTURE(depth, top_down, compression);
                const auto result = decode(bmp(depth, top_down, compression));
                REQUIRE(result.terminal == LV_EVENT_COMPLETE);
                CHECK(result.info.format == LV_FORMAT_BMP);
                CHECK(result.pixels[0] == 255);
                CHECK(result.pixels[2] == 0);
                CHECK(result.pixels[3] == 255);
                const auto last = result.pixels.size() - 4;
                CHECK(result.pixels[last] == 0);
                CHECK(result.pixels[last + 2] == 255);
                CHECK(result.pixels[last + 3] == 255);
            }
    auto alpha = decode(bmp(32, false, 3, true));
    REQUIRE(alpha.terminal == LV_EVENT_COMPLETE);
    CHECK(alpha.pixels[3] == 128);
}

TEST_CASE("BMP RLE4 and RLE8 encoded and absolute runs preserve row order", "[formats][bmp][rle]")
{
    for (unsigned depth : {4u, 8u})
        for (bool absolute : {false, true})
        {
            auto result = decode(bmp_rle(depth, absolute));
            REQUIRE(result.terminal == LV_EVENT_COMPLETE);
            CHECK(result.pixels[0] == 255);
            CHECK(result.pixels[result.pixels.size() - 2] == 255);
        }
    auto delta = bmp_rle(8, false);
    delta.resize(62);
    delta.insert(delta.end(), {0, 2, 2, 1, 3, 1, 0, 1});
    auto result = decode(delta);
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.pixels[(3 * 7 + 2) * 4 + 2] == 255);
    CHECK(result.pixels[0] == 255);
}

TEST_CASE("SVG rasterizes FIT with straight alpha viewBox and no intrinsic dimensions", "[formats][svg]")
{
    red_alpha(decode(red_svg(), {7, 5}));
    auto large = decode(red_svg(), {70, 50});
    REQUIRE(large.terminal == LV_EVENT_COMPLETE);
    CHECK(large.info.format == LV_FORMAT_SVG);
    CHECK(large.info.canvas.width == 70);
    CHECK(large.info.canvas.height == 50);
    auto viewbox = decode(svg("<rect width='7' height='5' fill='red'/>", "viewBox='0 0 7 5'"), {70, 70});
    REQUIRE(viewbox.terminal == LV_EVENT_COMPLETE);
    CHECK(viewbox.info.canvas.width == 70);
    CHECK(viewbox.info.canvas.height == 50);
    auto no_size = decode(svg("<rect width='300' height='150' fill='red'/>", ""), {100, 100});
    REQUIRE(no_size.terminal == LV_EVENT_COMPLETE);
    CHECK(no_size.info.canvas.width == 100);
    CHECK(no_size.info.canvas.height == 50);
    auto gradient = decode(svg("<defs><linearGradient id='g'><stop stop-color='red'/>"
                               "<stop offset='1' stop-color='blue'/></linearGradient></defs>"
                               "<g transform='translate(0,0)'><path d='M0 0H7V5H0Z' fill='url(#g)'/></g>"),
                           {70, 50});
    REQUIRE(gradient.terminal == LV_EVENT_COMPLETE);
    CHECK(gradient.pixels[0] > 200);
    CHECK(gradient.pixels[(70 - 1) * 4 + 2] > 200);
}

TEST_CASE("new format errors never publish partial pixels and workers remain reusable", "[formats][errors]")
{
    auto bad_png = png();
    bad_png.resize(bad_png.size() / 2);
    auto bad_webp = webp(false);
    bad_webp.resize(bad_webp.size() / 2);
    auto bad_bmp = bmp();
    bad_bmp.pop_back();
    auto bad_mask = bmp(16, false, 3);
    put32(bad_mask, 58, 0xf800);
    auto bad_rle = bmp_rle(8, false);
    bad_rle[62]  = 255;
    const std::vector<Bytes> cases{bad_png, bad_webp,      bad_bmp,         bad_mask,
                                   bad_rle, svg("<rect>"), Bytes{'B', 'M'}, Bytes{0x89, 'P', 'N', 'G'}};
    Files files;
    Engine engine;
    for (const auto &bytes : cases)
    {
        engine.submit(files.write("bad", bytes));
        auto result = collect(engine);
        CHECK(result.terminal == LV_EVENT_ERROR);
        CHECK((result.error.code == LV_ERROR_CORRUPT_DATA || result.error.code == LV_ERROR_FORMAT_UNSUPPORTED));
        CHECK(result.error.has_partial_output == 0);
    }
    engine.submit(files.write("valid.png", png()));
    red_alpha(collect(engine));
}

TEST_CASE("animation containers reject instead of showing a misleading default image", "[formats][animation]")
{
    // A complete PNG default image plus an APNG animation control chunk.
    auto apng = png();
    Bytes control{0, 0, 0, 8, 'a', 'c', 'T', 'L', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    auto checksum = crc32(0, control.data() + 4, 12);
    for (unsigned i = 0; i < 4; ++i)
        control[16 + i] = static_cast<unsigned char>(checksum >> (24 - i * 8));
    apng.insert(apng.begin() + 33, control.begin(), control.end());
    auto png_result = decode(apng);
    REQUIRE(png_result.terminal == LV_EVENT_ERROR);
    CHECK(png_result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
    // WebPGetFeatures can identify animation directly from VP8X.
    Bytes animated{'R', 'I', 'F', 'F', 22, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8',
                   'X', 10,  0,   0,   0,  2, 0, 0, 0,   6,   0,   0,   4,   0,   0};
    auto webp_result = decode(animated);
    REQUIRE(webp_result.terminal == LV_EVENT_ERROR);
    CHECK(webp_result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
}

TEST_CASE("SVG unsupported resources effects and complexity reject explicitly", "[formats][svg][limits]")
{
    for (const auto &body :
         {"<text>Hello</text>", "<image href='file:///tmp/private.png'/>", "<image href='https://example.invalid/p.png'/>",
          "<script/>", "<filter/>", "<clipPath/>", "<use href='#x'/>", "<rect filter='url(#x)'/>"})
    {
        auto result = decode(svg(body));
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
    }
    auto deep = decode(svg(std::string(130, ' ') +
                           []
                           {
                               std::string s;
                               for (unsigned i = 0; i < 130; ++i)
                                   s += "<g>";
                               for (unsigned i = 0; i < 130; ++i)
                                   s += "</g>";
                               return s;
                           }()));
    REQUIRE(deep.terminal == LV_EVENT_ERROR);
    CHECK(deep.error.code == LV_ERROR_DIMENSION_LIMIT);
}

TEST_CASE("all formats report capabilities and enforce output or original pixel limits", "[formats][budget]")
{
    Engine engine;
    for (lv_format format = LV_FORMAT_JPEG; format <= LV_FORMAT_BMP; ++format)
    {
        lv_format_support support{};
        REQUIRE(lv_core_query_format(engine.core, format, &support) == LV_OK);
        CHECK(support.available == 1);
        CHECK((support.capabilities & LV_CAP_INCREMENTAL) == 0);
        CHECK((support.capabilities & LV_CAP_VECTOR_RASTERIZE) == (format == LV_FORMAT_SVG ? LV_CAP_VECTOR_RASTERIZE : 0));
    }
    Files files;
    auto larger_bmp = bmp();
    larger_bmp.resize(54 + 128 * 128 * 3, 0);
    put32(larger_bmp, 18, 128);
    put32(larger_bmp, 22, 128);
    const std::vector<Bytes> images{png(), webp(false), larger_bmp, red_svg()};
    for (const auto &image : images)
    {
        Engine limited(1, 12 * 1024);
        limited.submit(files.write("memory", image));
        auto result = collect(limited);
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == LV_ERROR_MEMORY_BUDGET);
    }
    auto huge_svg = decode(red_svg(), {UINT32_MAX, UINT32_MAX});
    REQUIRE(huge_svg.terminal == LV_EVENT_ERROR);
    CHECK(huge_svg.error.code == LV_ERROR_DIMENSION_LIMIT);
}

TEST_CASE("new formats detect mutation and cancellation after metadata", "[formats][source][cancel]")
{
    for (const auto &image : std::vector<Bytes>{png(), webp(false), bmp(), red_svg()})
        for (bool cancel : {false, true})
        {
            Files files;
            Engine engine;
            engine.gate     = true;
            const auto path = files.write("gated", image);
            const auto id   = engine.submit(path, {7, 5});
            {
                std::unique_lock lock(engine.mutex);
                REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
            }
            if (cancel)
                REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
            else
            {
                std::ofstream out(path, std::ios::binary | std::ios::app);
                out.put('x');
            }
            {
                std::lock_guard lock(engine.mutex);
                engine.proceed = true;
                engine.changed.notify_all();
            }
            auto result = collect(engine);
            if (cancel)
                CHECK(result.terminal == LV_EVENT_CANCELLED);
            else
            {
                REQUIRE(result.terminal == LV_EVENT_ERROR);
                CHECK(result.error.code == LV_ERROR_SOURCE_CHANGED);
                CHECK(result.error.has_partial_output == 0);
            }
        }
}

TEST_CASE("transparent FIT filtering does not create dark fringes", "[formats][png][alpha]")
{
    // An opaque red / transparent blue edge must not mix invisible blue into red.
    auto result = decode(png(false, 8, false, false, true), {4, 4});
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.info.canvas.width == 4);
    CHECK(result.info.canvas.height == 2);
    CHECK(result.pixels[4] == 255);
    CHECK(result.pixels[5] == 0);
    CHECK(result.pixels[6] == 0);
    CHECK(result.pixels[7] > 0);
    CHECK(result.pixels[7] < 255);
    CHECK(result.pixels[11] == 0);
}
