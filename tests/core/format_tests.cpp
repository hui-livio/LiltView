#include "support.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <png.h>
#include <string>
#include <tuple>
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

void put32be(Bytes &out, std::size_t at, std::uint32_t value)
{
    out.at(at)     = static_cast<unsigned char>(value >> 24);
    out.at(at + 1) = static_cast<unsigned char>(value >> 16);
    out.at(at + 2) = static_cast<unsigned char>(value >> 8);
    out.at(at + 3) = static_cast<unsigned char>(value);
}

Bytes qoi(std::uint32_t width, std::uint32_t height, unsigned channels, unsigned colorspace, Bytes operations)
{
    Bytes output(14, 0);
    output[0] = 'q';
    output[1] = 'o';
    output[2] = 'i';
    output[3] = 'f';
    put32be(output, 4, width);
    put32be(output, 8, height);
    output[12] = static_cast<unsigned char>(channels);
    output[13] = static_cast<unsigned char>(colorspace);
    output.insert(output.end(), operations.begin(), operations.end());
    output.insert(output.end(), {0, 0, 0, 0, 0, 0, 0, 1});
    return output;
}

Bytes qoi_solid(std::uint32_t width, std::uint32_t height, std::array<unsigned char, 4> pixel)
{
    Bytes operations{0xff, pixel[0], pixel[1], pixel[2], pixel[3]};
    std::size_t remaining = static_cast<std::size_t>(width) * height - 1;
    while (remaining)
    {
        const auto count = std::min<std::size_t>(remaining, 62);
        operations.push_back(static_cast<unsigned char>(0xc0u | (count - 1)));
        remaining -= count;
    }
    return qoi(width, height, 4, 0, std::move(operations));
}

Bytes fixture_bytes(const char *name)
{
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / name;
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

Bytes ico_dib(std::array<unsigned char, 4> pixel, unsigned width = 2, unsigned height = 2)
{
    const unsigned xor_stride  = width * 4;
    const unsigned mask_stride = ((width + 31) / 32) * 4;
    Bytes output(40 + xor_stride * height + mask_stride * height, 0);
    put32(output, 0, 40);
    put32(output, 4, width);
    put32(output, 8, height * 2);
    put16(output, 12, 1);
    put16(output, 14, 32);
    put32(output, 20, xor_stride * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
        {
            const auto at = 40 + y * xor_stride + x * 4;
            output[at]     = pixel[2];
            output[at + 1] = pixel[1];
            output[at + 2] = pixel[0];
            output[at + 3] = pixel[3];
        }
    return output;
}

Bytes ico(unsigned type, std::vector<std::tuple<unsigned, unsigned, Bytes, unsigned, unsigned>> entries)
{
    Bytes output(6 + entries.size() * 16, 0);
    put16(output, 2, type);
    put16(output, 4, static_cast<unsigned>(entries.size()));
    unsigned offset = static_cast<unsigned>(output.size());
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        const auto &[width, height, payload, hotspot_x, hotspot_y] = entries[i];
        const auto at = 6 + i * 16;
        output[at]     = width == 256 ? 0 : static_cast<unsigned char>(width);
        output[at + 1] = height == 256 ? 0 : static_cast<unsigned char>(height);
        if (type == 2)
        {
            put16(output, at + 4, hotspot_x);
            put16(output, at + 6, hotspot_y);
        }
        put32(output, at + 8, static_cast<std::uint32_t>(payload.size()));
        put32(output, at + 12, offset);
        output.insert(output.end(), payload.begin(), payload.end());
        offset += static_cast<unsigned>(payload.size());
    }
    return output;
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

TEST_CASE("QOI RGB and RGBA decode all pixel opcodes", "[formats][qoi]")
{
    const auto image = qoi(7, 1, 4, 0,
                           {0xfe, 10, 20, 30, // RGB
                            0x7e,             // DIFF: +1, +1, 0
                            0xa4, 0x77,      // LUMA: dg +4, dr-dg -1, db-dg -1
                            0xff, 100, 110, 120, 64, // RGBA
                            0x1a,             // INDEX for RGBA(100, 110, 120, 64)
                            0xc1});           // RUN for two more pixels
    const auto result = decode(image);
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.info.format == LV_FORMAT_QOI);
    CHECK(result.info.source_size.width == 7);
    CHECK(result.info.source_size.height == 1);
    const std::array<std::array<unsigned char, 4>, 7> expected{{
        {10, 20, 30, 255}, {11, 21, 30, 255}, {14, 25, 33, 255}, {100, 110, 120, 64},
        {100, 110, 120, 64}, {100, 110, 120, 64}, {100, 110, 120, 64}}};
    for (std::size_t i = 0; i < expected.size(); ++i)
        for (std::size_t channel = 0; channel < 4; ++channel)
            CHECK(result.pixels[i * 4 + channel] == expected[i][channel]);

    const auto native = decode_native(image);
    REQUIRE(native.terminal == LV_EVENT_COMPLETE);
    CHECK(native.info.canvas.width == 7);
    CHECK(native.info.canvas.height == 1);
    CHECK(native.pixels == result.pixels);

    auto rgb = decode(qoi(2, 1, 3, 1, {0xfe, 255, 0, 0, 0xc0}));
    REQUIRE(rgb.terminal == LV_EVENT_COMPLETE);
    CHECK(rgb.info.format == LV_FORMAT_QOI);
    CHECK(rgb.pixels == Bytes{255, 0, 0, 255, 255, 0, 0, 255});
}

TEST_CASE("QOI FIT preserves dimensions and rejects malformed sources", "[formats][qoi][errors]")
{
    auto fitted = decode(qoi_solid(7, 5, {255, 0, 0, 128}), {3, 3});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.canvas.width == 3);
    CHECK(fitted.info.canvas.height == 2);
    red_alpha(fitted);

    auto invalid_channels = qoi(1, 1, 2, 0, {0xfe, 1, 2, 3});
    auto invalid_colorspace = qoi(1, 1, 4, 2, {0xff, 1, 2, 3, 4});
    auto truncated = qoi(1, 1, 4, 0, {0xff, 1, 2, 3, 4});
    truncated.pop_back();
    auto trailing = qoi(1, 1, 4, 0, {0xff, 1, 2, 3, 4});
    trailing.push_back(0);
    auto overrun = qoi(1, 1, 4, 0, {0xc1});
    auto zero = qoi(0, 1, 4, 0, {0xff, 1, 2, 3, 4});
    for (const auto &source : {invalid_channels, invalid_colorspace, truncated, trailing, overrun})
    {
        const auto result = decode(source);
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == (source == invalid_channels || source == invalid_colorspace ?
                                        LV_ERROR_FORMAT_UNSUPPORTED :
                                        LV_ERROR_CORRUPT_DATA));
    }
    const auto zero_result = decode(zero);
    REQUIRE(zero_result.terminal == LV_EVENT_ERROR);
    CHECK(zero_result.error.code == LV_ERROR_DIMENSION_LIMIT);
}

TEST_CASE("ICO and CUR select entries and decode PNG and DIB payloads", "[formats][ico][cur]")
{
    const auto png_entry = png();
    auto icon = ico(1, {{7, 5, png_entry, 0, 0}});
    auto result = decode(icon);
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.info.format == LV_FORMAT_ICO);
    CHECK(result.info.source_size.width == 7);
    CHECK(result.info.source_size.height == 5);
    red_alpha(result);

    const auto dib_entry = ico_dib({255, 0, 0, 128});
    auto cursor = ico(2, {{2, 2, dib_entry, 1, 1}});
    auto cur_result = decode(cursor);
    REQUIRE(cur_result.terminal == LV_EVENT_COMPLETE);
    CHECK(cur_result.info.format == LV_FORMAT_CUR);
    CHECK(cur_result.info.source_size.width == 2);
    CHECK(cur_result.info.source_size.height == 2);
    red_alpha(cur_result);

    auto multiple = ico(1, {{2, 2, dib_entry, 0, 0}, {7, 5, png_entry, 0, 0}});
    auto large = decode(multiple, {3, 3});
    REQUIRE(large.terminal == LV_EVENT_COMPLETE);
    CHECK(large.info.source_size.width == 7);
    CHECK(large.info.source_size.height == 5);
    auto small = decode(multiple, {1, 1});
    REQUIRE(small.terminal == LV_EVENT_COMPLETE);
    CHECK(small.info.source_size.width == 2);
    CHECK(small.info.source_size.height == 2);
}

TEST_CASE("ICO and CUR malformed directories and unsupported entries fail cleanly", "[formats][ico][cur][errors]")
{
    auto bad_count = Bytes{0, 0, 1, 0, 1, 0};
    bad_count.resize(6 + 16, 0);
    auto bad_payload = ico(1, {{2, 2, Bytes{'x', 'x'}, 0, 0}});
    auto bad_cursor = ico(2, {{2, 2, ico_dib({255, 0, 0, 255}), 2, 2}});
    for (const auto &source : {bad_count, bad_payload, bad_cursor})
    {
        const auto result = decode(source);
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK((result.error.code == LV_ERROR_CORRUPT_DATA || result.error.code == LV_ERROR_FORMAT_UNSUPPORTED));
    }
}

TEST_CASE("JPEG 2000 JP2 and raw J2K decode 8-bit RGB", "[formats][jpeg2000]")
{
    for (const auto *name : {"red.jp2", "red.j2k"})
    {
        const auto result = decode(fixture_bytes(name));
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.format == LV_FORMAT_JPEG2000);
        CHECK(result.info.source_size.width == 7);
        CHECK(result.info.source_size.height == 5);
        REQUIRE_FALSE(result.pixels.empty());
        CHECK(result.pixels[0] >= 245);
        CHECK(result.pixels[1] <= 10);
        CHECK(result.pixels[2] <= 10);
        CHECK(result.pixels[3] >= 245);
    }
    auto truncated = fixture_bytes("red.jp2");
    truncated.resize(truncated.size() / 2);
    const auto bad = decode(truncated);
    REQUIRE(bad.terminal == LV_EVENT_ERROR);
    CHECK(bad.error.code == LV_ERROR_CORRUPT_DATA);
}

TEST_CASE("AVIF static lossless and lossy images decode to RGBA8", "[formats][avif]")
{
    for (const auto *name : {"avif-red.avif", "avif-red-lossy.avif"})
    {
        const auto result = decode(fixture_bytes(name));
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.format == LV_FORMAT_AVIF);
        CHECK(result.info.source_size.width == 96);
        CHECK(result.info.source_size.height == 64);
        CHECK(result.info.canvas.width == 96);
        CHECK(result.info.canvas.height == 64);
        REQUIRE(result.pixels.size() == 96 * 64 * 4);
        for (std::size_t i = 0; i < result.pixels.size(); i += 4)
        {
            CHECK(result.pixels[i] >= 245);
            CHECK(result.pixels[i + 1] <= 10);
            CHECK(result.pixels[i + 2] <= 10);
            CHECK(result.pixels[i + 3] >= 120);
            CHECK(result.pixels[i + 3] <= 136);
        }
    }
    const auto fitted = decode(fixture_bytes("avif-red.avif"), {24, 24});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.canvas.width == 24);
    CHECK(fitted.info.canvas.height == 16);
    CHECK(fitted.pixels.size() == 24 * 16 * 4);
}

TEST_CASE("AVIF rejects animation and truncation", "[formats][avif]")
{
    const auto animated = decode(fixture_bytes("avif-animated.avif"));
    REQUIRE(animated.terminal == LV_EVENT_ERROR);
    CHECK(animated.error.code == LV_ERROR_FORMAT_UNSUPPORTED);

    auto truncated = fixture_bytes("avif-red.avif");
    truncated.resize(truncated.size() / 2);
    const auto damaged = decode(truncated);
    REQUIRE(damaged.terminal == LV_EVENT_ERROR);
    CHECK(damaged.error.code == LV_ERROR_CORRUPT_DATA);
}

TEST_CASE("JPEG XL static lossless and lossy images decode to RGBA8", "[formats][jpegxl]")
{
    for (const auto *name : {"red-alpha-lossless.jxl", "red-alpha-lossy.jxl"})
    {
        const auto result = decode(fixture_bytes(name));
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.format == LV_FORMAT_JPEGXL);
        CHECK(result.info.source_size.width == 96);
        CHECK(result.info.source_size.height == 64);
        REQUIRE(result.pixels.size() == 96 * 64 * 4);
        for (std::size_t i = 0; i < result.pixels.size(); i += 4)
        {
            CHECK(result.pixels[i] >= 240);
            CHECK(result.pixels[i + 1] <= 15);
            CHECK(result.pixels[i + 2] <= 30);
            CHECK(result.pixels[i + 3] >= 120);
            CHECK(result.pixels[i + 3] <= 136);
        }
    }
    auto truncated = fixture_bytes("red-alpha-lossless.jxl");
    truncated.resize(truncated.size() / 2);
    const auto damaged = decode(truncated);
    REQUIRE(damaged.terminal == LV_EVENT_ERROR);
    CHECK(damaged.error.code == LV_ERROR_CORRUPT_DATA);
}

TEST_CASE("OpenEXR scanline HALF RGB(A) decodes with windows, FIT, and SDR mapping", "[formats][openexr]")
{
    const auto result = decode(fixture_bytes("red.exr"));
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.info.format == LV_FORMAT_OPENEXR);
    CHECK(result.info.source_size.width == 7);
    CHECK(result.info.source_size.height == 5);
    CHECK(result.info.canvas.width == 7);
    CHECK(result.info.canvas.height == 5);
    REQUIRE(result.pixels.size() == 7 * 5 * 4);
    CHECK(result.pixels[0] == 255);
    CHECK(result.pixels[1] >= 63);
    CHECK(result.pixels[1] <= 65);
    CHECK(result.pixels[2] == 0);
    CHECK(result.pixels[3] == 255);

    const auto negative = decode(fixture_bytes("red-negative.exr"));
    REQUIRE(negative.terminal == LV_EVENT_COMPLETE);
    CHECK(negative.info.source_size.width == 7);
    CHECK(negative.info.source_size.height == 4);
    CHECK(negative.pixels[0] == 255);

    const auto decreasing = decode(fixture_bytes("red-decreasing.exr"));
    REQUIRE(decreasing.terminal == LV_EVENT_COMPLETE);
    CHECK(decreasing.info.source_size.width == 7);
    CHECK(decreasing.info.source_size.height == 5);
    CHECK(decreasing.pixels[0] == 255);

    const auto fitted = decode(fixture_bytes("red.exr"), {3, 3});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.canvas.width == 3);
    CHECK(fitted.info.canvas.height == 2);
    CHECK(fitted.pixels[0] == 255);
}

TEST_CASE("OpenEXR NaN and infinities use fixed SDR boundary mapping", "[formats][openexr]")
{
    const auto result = decode(fixture_bytes("red-special.exr"));
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    REQUIRE(result.pixels.size() == 2 * 4);
    CHECK(result.pixels[0] == 255);
    CHECK(result.pixels[1] == 0);
    CHECK(result.pixels[2] == 0);
    CHECK(result.pixels[3] == 255);
    CHECK(result.pixels[4] == 255);
    CHECK(result.pixels[5] >= 127);
    CHECK(result.pixels[5] <= 128);
    CHECK(result.pixels[6] == 0);
    CHECK(result.pixels[7] >= 127);
    CHECK(result.pixels[7] <= 128);
}

TEST_CASE("OpenEXR rejects unsupported storage, channels, sample types, and truncation", "[formats][openexr][errors]")
{
    for (const auto *name : {"red-extra.exr", "red-float.exr", "red-tiled.exr", "red-multipart.exr"})
    {
        const auto result = decode(fixture_bytes(name));
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
    }
    auto truncated = fixture_bytes("red.exr");
    truncated.resize(truncated.size() / 2);
    const auto damaged = decode(truncated);
    REQUIRE(damaged.terminal == LV_EVENT_ERROR);
    CHECK(damaged.error.code == LV_ERROR_CORRUPT_DATA);
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
    for (lv_format format = LV_FORMAT_JPEG; format <= LV_FORMAT_OPENEXR; ++format)
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
    const std::vector<Bytes> images{png(), webp(false), larger_bmp, red_svg(), qoi_solid(128, 128, {255, 0, 0, 255})};
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
    for (const auto &image : std::vector<Bytes>{png(), webp(false), bmp(), red_svg(), qoi_solid(7, 5, {255, 0, 0, 255})})
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

TEST_CASE("NATIVE ignores FIT bounds for complete-output WebP RLE BMP and SVG", "[native][formats]")
{
    for (const auto &bytes : {webp(false), webp(true), bmp_rle(8, false), bmp_rle(4, true), red_svg()})
    {
        const auto result = decode_native(bytes);
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.canvas.width == result.info.source_size.width);
        CHECK(result.info.canvas.height == result.info.source_size.height);
        CHECK(result.info.canvas.width == 7);
        CHECK(result.info.canvas.height == 5);
    }
}
