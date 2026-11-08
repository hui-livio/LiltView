#include <gif_lib.h>
#include <iterator>
#include <memory>
#include <tiffio.h>
#include "support.hpp"

namespace
{
using namespace liltview::test;
using Bytes = std::vector<unsigned char>;

Bytes gif(bool interlaced = false, bool local = false, bool transparent = false, bool offset = false, bool animated = false)
{
    Bytes bytes;
    int code         = 0;
    GifFileType *out = EGifOpen(
        &bytes,
        [](GifFileType *f, const GifByteType *p, int count)
        {
            auto &data = *static_cast<Bytes *>(f->UserData);
            data.insert(data.end(), p, p + count);
            return count;
        },
        &code);
    REQUIRE(out);
    EGifSetGifVersion(out, transparent || animated);
    GifColorType colors[4]{{255, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 255}};
    ColorMapObject *map = GifMakeMapObject(4, colors);
    REQUIRE(map);
    REQUIRE(EGifPutScreenDesc(out, 7, 9, 8, 2, local ? nullptr : map) == GIF_OK);
    if (animated)
    {
        const char loop[3]{1, 0, 0};
        REQUIRE(EGifPutExtensionLeader(out, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(out, 11, "NETSCAPE2.0") == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(out, 3, loop) == GIF_OK);
        REQUIRE(EGifPutExtensionTrailer(out) == GIF_OK);
    }
    if (transparent)
    {
        GraphicsControlBlock control{};
        control.DisposalMode     = DISPOSE_BACKGROUND;
        control.TransparentColor = 1;
        control.DelayTime        = 7;
        GifByteType extension[4];
        REQUIRE(EGifGCBToExtension(&control, extension) == 4);
        REQUIRE(EGifPutExtension(out, GRAPHICS_EXT_FUNC_CODE, 4, extension) == GIF_OK);
    }
    const int width = offset ? 3 : 7, height = offset ? 5 : 9;
    REQUIRE(EGifPutImageDesc(out, offset ? 2 : 0, offset ? 1 : 0, width, height, interlaced, local ? map : nullptr) == GIF_OK);
    static constexpr unsigned starts[]{0, 4, 2, 1}, steps[]{8, 8, 4, 2};
    for (unsigned pass = 0; pass < (interlaced ? 4u : 1u); ++pass)
        for (unsigned y = interlaced ? starts[pass] : 0; y < static_cast<unsigned>(height); y += interlaced ? steps[pass] : 1)
        {
            std::vector<GifPixelType> row(static_cast<std::size_t>(width), y < 3 ? 0 : 1);
            REQUIRE(EGifPutLine(out, row.data(), width) == GIF_OK);
        }
    if (animated)
    {
        REQUIRE(EGifPutImageDesc(out, 0, 0, 7, 9, false, local ? map : nullptr) == GIF_OK);
        std::array<GifPixelType, 7> blue;
        blue.fill(1);
        for (unsigned y = 0; y < 9; ++y)
            REQUIRE(EGifPutLine(out, blue.data(), 7) == GIF_OK);
    }
    REQUIRE(EGifCloseFile(out, &code) == GIF_OK);
    GifFreeMapObject(map);
    return bytes;
}

struct TiffFixture
{
    std::uint16_t compression = COMPRESSION_NONE, orientation = ORIENTATION_TOPLEFT;
    bool big_endian = false, big_tiff = false, tiled = false, planar = false, multipage = false;
    unsigned bits  = 8;
    unsigned alpha = 0; // 0 RGB, 1 associated, 2 unassociated, 3 unspecified.
};
Bytes tiff_fixture(TiffFixture fixture = {})
{
    Files files;
    const auto filename  = files.directory / "fixture.tiff";
    std::string mode     = fixture.big_tiff ? "w8" : "w";
    mode                += fixture.big_endian ? "b" : "l";
    std::unique_ptr<TIFF, decltype(&TIFFClose)> out(TIFFOpen(filename.c_str(), mode.c_str()), TIFFClose);
    REQUIRE(out);
    const std::uint32_t width = fixture.tiled ? 17 : 7, height = fixture.tiled ? 19 : 9;
    const unsigned samples = fixture.alpha ? 4 : 3;
    for (unsigned page = 0; page < (fixture.multipage ? 2u : 1u); ++page)
    {
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_IMAGEWIDTH, width));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_IMAGELENGTH, height));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_SAMPLESPERPIXEL, samples));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_BITSPERSAMPLE, fixture.bits));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_PLANARCONFIG, fixture.planar ? PLANARCONFIG_SEPARATE : PLANARCONFIG_CONTIG));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_ORIENTATION, fixture.orientation));
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_COMPRESSION, fixture.compression));
        if (fixture.alpha)
        {
            std::uint16_t extra = fixture.alpha == 1 ? EXTRASAMPLE_ASSOCALPHA :
                fixture.alpha == 2                   ? EXTRASAMPLE_UNASSALPHA :
                                                       EXTRASAMPLE_UNSPECIFIED;
            REQUIRE(TIFFSetField(out.get(), TIFFTAG_EXTRASAMPLES, 1, &extra));
        }
        if (fixture.tiled)
        {
            REQUIRE(TIFFSetField(out.get(), TIFFTAG_TILEWIDTH, 16));
            REQUIRE(TIFFSetField(out.get(), TIFFTAG_TILELENGTH, 16));
        }
        else
            REQUIRE(TIFFSetField(out.get(), TIFFTAG_ROWSPERSTRIP, fixture.compression == COMPRESSION_JPEG ? 16u : 2u));
        const unsigned channels = fixture.planar ? 1 : samples;
        const unsigned size     = fixture.tiled ? 16 : width;
        std::vector<std::uint16_t> storage(static_cast<std::size_t>(size) * (fixture.tiled ? 16 : 1) * channels);
        auto *pixels = reinterpret_cast<unsigned char *>(storage.data());
        for (unsigned plane = 0; plane < (fixture.planar ? samples : 1u); ++plane)
            for (unsigned y = 0; y < height; y += fixture.tiled ? 16u : 1u)
                for (unsigned x = 0; x < width; x += fixture.tiled ? 16u : width)
                {
                    for (unsigned row = 0; row < (fixture.tiled ? 16u : 1u); ++row)
                        for (unsigned column = 0; column < size; ++column)
                            for (unsigned c = 0; c < channels; ++c)
                            {
                                const unsigned channel = fixture.planar ? plane : c;
                                const bool red         = page == 0 && y + row < 3;
                                unsigned value         = channel == 3 ? (fixture.alpha == 3 ? 0u : 128u) :
                                    channel == (red ? 0u : 2u)        ? (fixture.alpha == 1 ? 128u : 255u) :
                                                                        0u;
                                const auto at          = (static_cast<std::size_t>(row) * size + column) * channels + c;
                                if (fixture.bits == 16)
                                    storage[at] = static_cast<std::uint16_t>(value * 257);
                                else
                                    pixels[at] = static_cast<unsigned char>(value);
                            }
                    if (fixture.tiled)
                        REQUIRE(TIFFWriteTile(out.get(), pixels, x, y, 0, static_cast<std::uint16_t>(plane)) >= 0);
                    else
                        REQUIRE(TIFFWriteScanline(out.get(), pixels, y, static_cast<std::uint16_t>(plane)) == 1);
                }
        if (fixture.multipage)
            REQUIRE(TIFFWriteDirectory(out.get()));
    }
    out.reset();
    std::ifstream file(filename, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), {});
}
Bytes tiff_gray(unsigned bits, bool palette, std::uint16_t compression)
{
    Files files;
    const auto filename = files.directory / "gray.tif";
    std::unique_ptr<TIFF, decltype(&TIFFClose)> out(TIFFOpen(filename.c_str(), "w"), TIFFClose);
    REQUIRE(out);
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_IMAGEWIDTH, 13));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_IMAGELENGTH, 7));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_BITSPERSAMPLE, bits));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_SAMPLESPERPIXEL, 1));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_PHOTOMETRIC, palette ? PHOTOMETRIC_PALETTE : PHOTOMETRIC_MINISBLACK));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_COMPRESSION, compression));
    REQUIRE(TIFFSetField(out.get(), TIFFTAG_ROWSPERSTRIP, 2));
    if (palette)
    {
        std::vector<std::uint16_t> red(1u << bits, 0), other(1u << bits, 0);
        red[0] = 65535;
        REQUIRE(TIFFSetField(out.get(), TIFFTAG_COLORMAP, red.data(), other.data(), other.data()));
    }
    Bytes row(static_cast<std::size_t>(TIFFScanlineSize(out.get())), palette ? 0 : 255);
    for (unsigned y = 0; y < 7; ++y)
        REQUIRE(TIFFWriteScanline(out.get(), row.data(), y, 0) == 1);
    out.reset();
    std::ifstream input(filename, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

void change_tiff_tag(Bytes &bytes, unsigned tag, std::uint32_t value)
{
    const auto read32 = [&](std::size_t at)
    {
        return static_cast<std::uint32_t>(bytes.at(at)) | static_cast<std::uint32_t>(bytes.at(at + 1)) << 8 |
            static_cast<std::uint32_t>(bytes.at(at + 2)) << 16 | static_cast<std::uint32_t>(bytes.at(at + 3)) << 24;
    };
    const std::size_t ifd = read32(4);
    const unsigned count  = bytes.at(ifd) | static_cast<unsigned>(bytes.at(ifd + 1)) << 8;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto at = ifd + 2 + i * 12;
        if ((bytes[at] | static_cast<unsigned>(bytes[at + 1]) << 8) != tag)
            continue;
        bytes[at + 2] = 4;
        bytes[at + 3] = 0; // LONG, count remains one
        for (unsigned c = 0; c < 4; ++c)
            bytes[at + 8 + c] = static_cast<unsigned char>(value >> (c * 8));
        return;
    }
    FAIL("TIFF test tag not found");
}
} // namespace

TEST_CASE("GIF87a and GIF89a first image supports local palettes interlace and offsets", "[formats][gif]")
{
    for (bool interlace : {false, true})
        for (bool local : {false, true})
            for (bool offset : {false, true})
            {
                CAPTURE(interlace, local, offset);
                const auto result = decode(gif(interlace, local, false, offset));
                REQUIRE(result.terminal == LV_EVENT_COMPLETE);
                CHECK(result.info.format == LV_FORMAT_GIF);
                CHECK(result.info.canvas.width == 7);
                CHECK(result.info.canvas.height == 9);
                const auto at = (offset ? 1u * 7 + 2 : 0) * 4;
                CHECK(result.pixels[at] == 255);
                CHECK(result.pixels[at + 2] == 0);
                CHECK(result.pixels[at + 3] == 255);
                CHECK(result.pixels[((offset ? 5u : 8u) * 7 + (offset ? 2u : 0u)) * 4 + 2] == 255);
                if (offset)
                    CHECK(result.pixels[3] == (local ? 0 : 255));
            }
}

TEST_CASE("GIF transparency and animation return the first composed frame only", "[formats][gif][alpha]")
{
    for (bool local : {false, true})
    {
        auto result = decode(gif(true, local, true, true, true));
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.pixels[3] == 0);
        CHECK(result.pixels[(1 * 7 + 2) * 4] == 255);
        CHECK(result.pixels[(1 * 7 + 2) * 4 + 3] == 255);
        CHECK(result.pixels[(5 * 7 + 2) * 4 + 3] == 0);
    }
    auto scaled = decode(gif(), {3, 3});
    REQUIRE(scaled.terminal == LV_EVENT_COMPLETE);
    CHECK(scaled.info.canvas.width == 2);
    CHECK(scaled.info.canvas.height == 3);
}

TEST_CASE("TIFF byte orders BigTIFF and common compressions decode first page", "[formats][tiff]")
{
    for (bool big_endian : {false, true})
        for (bool big_tiff : {false, true})
            for (auto compression :
                 {COMPRESSION_NONE, COMPRESSION_PACKBITS, COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_JPEG})
            {
                CAPTURE(big_endian, big_tiff, compression);
                TiffFixture options;
                options.big_endian  = big_endian;
                options.big_tiff    = big_tiff;
                options.compression = static_cast<std::uint16_t>(compression);
                options.multipage   = true;
                const auto result   = decode(tiff_fixture(options));
                REQUIRE(result.terminal == LV_EVENT_COMPLETE);
                CHECK(result.info.format == LV_FORMAT_TIFF);
                CHECK(result.info.canvas.width == 7);
                CHECK(result.info.canvas.height == 9);
                CHECK(result.pixels[0] > 200);
                CHECK(result.pixels[2] < 60);
                CHECK(result.pixels[result.pixels.size() - 2] > 200);
            }
}

TEST_CASE("TIFF tile edges separate planes 16-bit and explicit alpha become straight RGBA", "[formats][tiff][alpha]")
{
    for (bool tiled : {false, true})
        for (bool planar : {false, true})
            for (unsigned bits : {8u, 16u})
                for (unsigned alpha : {0u, 1u, 2u, 3u})
                {
                    CAPTURE(tiled, planar, bits, alpha);
                    TiffFixture options;
                    options.tiled  = tiled;
                    options.planar = planar;
                    options.bits   = bits;
                    options.alpha  = alpha;
                    auto result    = decode(tiff_fixture(options));
                    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
                    CHECK(result.pixels[0] >= 250);
                    CHECK(result.pixels[2] == 0);
                    CHECK(result.pixels[3] == (alpha == 1 || alpha == 2 ? 128 : 255));
                    CHECK(result.pixels[result.pixels.size() - 2] >= 250);
                }
}

TEST_CASE("TIFF orientation stays in metadata and FIT uses oriented bounds", "[formats][tiff][orientation]")
{
    for (std::uint16_t orientation = 1; orientation <= 8; ++orientation)
    {
        TiffFixture options;
        options.orientation = orientation;
        auto result         = decode(tiff_fixture(options), {9, 7});
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.orientation == orientation);
        CHECK(result.info.canvas.width == (orientation >= 5 ? 7 : 5));
        CHECK(result.info.canvas.height == (orientation >= 5 ? 9 : 7));
        CHECK(result.pixels[0] > 200); // raw rows are not flipped twice
        CHECK(result.pixels[result.pixels.size() - 2] > 200);
    }
}

TEST_CASE("TIFF grayscale palette low bit depths and CCITT are supported", "[formats][tiff]")
{
    for (unsigned bits : {1u, 2u, 4u, 8u, 16u})
        for (bool palette : {false, true})
        {
            CAPTURE(bits, palette);
            auto result = decode(tiff_gray(bits, palette, COMPRESSION_LZW));
            if (palette && bits == 16)
            {
                REQUIRE(result.terminal == LV_EVENT_ERROR);
                CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
                continue;
            }
            REQUIRE(result.terminal == LV_EVENT_COMPLETE);
            CHECK(result.pixels[0] == 255);
            CHECK(result.pixels[1] == (palette ? 0 : 255));
            CHECK(result.pixels[3] == 255);
        }
    for (auto compression : {COMPRESSION_CCITTRLE, COMPRESSION_CCITTFAX3, COMPRESSION_CCITTFAX4})
    {
        auto result = decode(tiff_gray(1, false, static_cast<std::uint16_t>(compression)));
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.pixels[0] == 255);
    }
}

TEST_CASE("GIF and TIFF oversized dimensions and unsupported TIFF codecs reject early", "[formats][gif][tiff][limits]")
{
    auto large_gif = gif();
    std::fill(large_gif.begin() + 6, large_gif.begin() + 10, 255);
    auto result = decode(large_gif, {1, 1});
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_DIMENSION_LIMIT);
    auto large_tiff = tiff_fixture();
    change_tiff_tag(large_tiff, TIFFTAG_IMAGEWIDTH, 1u << 30);
    result = decode(large_tiff, {1, 1});
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_DIMENSION_LIMIT);
    auto unsupported = tiff_fixture();
    change_tiff_tag(unsupported, TIFFTAG_COMPRESSION, COMPRESSION_ZSTD);
    result = decode(unsupported);
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
}

TEST_CASE("GIF and TIFF corrupt data do not publish partial output and leave a reusable worker", "[formats][gif][tiff][errors]")
{
    auto truncated_gif = gif();
    truncated_gif.resize(truncated_gif.size() / 2);
    auto bad_rect         = gif();
    const auto descriptor = std::find(bad_rect.begin() + 25, bad_rect.end(), static_cast<unsigned char>(0x2c));
    REQUIRE(descriptor != bad_rect.end());
    *(descriptor + 1)   = 255;
    auto truncated_tiff = tiff_fixture();
    truncated_tiff.resize(16);
    Files files;
    Engine engine;
    for (const auto &bytes : std::vector<Bytes>{truncated_gif, bad_rect, truncated_tiff, Bytes{'G', 'I', 'F', '8', '9', 'a'},
                                                Bytes{'I', 'I', 42, 0, 255, 255, 255, 127}})
    {
        engine.submit(files.write("bad.other", bytes));
        auto result = collect(engine);
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == LV_ERROR_CORRUPT_DATA);
        CHECK(result.error.has_partial_output == 0);
    }
    engine.submit(files.write("next", gif()));
    CHECK(collect(engine).terminal == LV_EVENT_COMPLETE);
}

TEST_CASE("GIF TIFF capabilities memory budgets and cancelled metadata gates", "[formats][gif][tiff][budget][cancel]")
{
    for (const auto &data : std::vector<Bytes>{gif(), tiff_fixture()})
    {
        Files files;
        {
            Engine limited(1, 12 * 1024);
            limited.submit(files.write("limited", data));
            auto result = collect(limited);
            REQUIRE(result.terminal == LV_EVENT_ERROR);
            CHECK(result.error.code == LV_ERROR_MEMORY_BUDGET);
        }
        for (bool cancel : {false, true})
        {
            Engine engine;
            engine.gate     = true;
            const auto path = files.write("gated", data);
            const auto id   = engine.submit(path);
            {
                std::unique_lock lock(engine.mutex);
                REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
            }
            if (cancel)
                REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
            else
            {
                std::ofstream out(path, std::ios::app | std::ios::binary);
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
    Engine engine;
    for (lv_format format : {LV_FORMAT_GIF, LV_FORMAT_TIFF})
    {
        lv_format_support support{};
        REQUIRE(lv_core_query_format(engine.core, format, &support) == LV_OK);
        CHECK(support.available == 1);
        CHECK(support.capabilities == 0);
    }
}
