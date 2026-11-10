#include "../fixtures/path.hpp"
#include "../fixtures/sparse_file.hpp"
#include "support.hpp"

#include <array>
#include <zlib.h>

namespace
{
using namespace liltview::test;
using Bytes = std::vector<unsigned char>;

void little(Bytes &bytes, std::uint64_t value, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        bytes.push_back(static_cast<unsigned char>(value >> (i * 8)));
}

void big(Bytes &bytes, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<unsigned char>(value >> ((3 - i) * 8)));
}

Decoded decode(const std::filesystem::path &path)
{
    Engine engine;
    engine.submit(fixture_path_bytes(path), {1, 1});
    return collect(engine);
}
} // namespace

TEST_CASE("TIFF reads IFD and pixel data beyond 32-bit file offsets", "[formats][tiff][file_offset]")
{
    for (const bool big_tiff : {false, true})
    {
        CAPTURE(big_tiff);
        Files files;
        const auto path = files.directory / "offset.tif";
        SparseFile file(path);
        // Classic TIFF: directory beyond signed 32-bit. BigTIFF: strip beyond
        // unsigned 32-bit, with a low directory to exercise both kinds of seek.
        const std::uint64_t directory = big_tiff ? 16 : 0x80001000ULL;
        const std::uint64_t strip     = big_tiff ? 0x100001000ULL : 8;
        Bytes header{'I', 'I'};
        little(header, big_tiff ? 43 : 42, 2);
        if (big_tiff)
        {
            little(header, 8, 2);
            little(header, 0, 2);
        }
        little(header, directory, big_tiff ? 8 : 4);
        file.write(0, header);
        file.write(strip, {127});
        Bytes ifd;
        little(ifd, 9, big_tiff ? 8 : 2);
        const auto entry = [&](unsigned tag, unsigned type, std::uint64_t value)
        {
            little(ifd, tag, 2);
            little(ifd, type, 2);
            little(ifd, 1, big_tiff ? 8 : 4);
            little(ifd, value, big_tiff ? 8 : 4);
        };
        entry(256, 4, 1); // Width.
        entry(257, 4, 1); // Height.
        entry(258, 3, 8); // Bits per sample.
        entry(259, 3, 1); // Uncompressed.
        entry(262, 3, 1); // Black is zero.
        entry(273, big_tiff ? 16 : 4, strip);
        entry(277, 3, 1); // Samples per pixel.
        entry(278, 4, 1); // Rows per strip.
        entry(279, big_tiff ? 16 : 4, 1);
        little(ifd, 0, big_tiff ? 8 : 4);
        file.write(directory, ifd);
        file.close();
        const auto result = decode(path);
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        CHECK(result.info.file_bytes == std::filesystem::file_size(path));
        CHECK(result.pixels == Bytes{127, 127, 127, 255});
    }
}

TEST_CASE("BMP reads pixel rows beyond signed 32-bit file offsets", "[formats][bmp][file_offset]")
{
    Files files;
    const auto path = files.directory / "offset.bmp";
    SparseFile file(path);
    constexpr std::uint32_t offset = 0x80001000U;
    Bytes header{'B', 'M'};
    little(header, static_cast<std::uint64_t>(offset) + 4, 4);
    little(header, 0, 4);
    little(header, offset, 4);
    little(header, 40, 4);
    little(header, 1, 4); // Width.
    little(header, 1, 4); // Height.
    little(header, 1, 2); // Planes.
    little(header, 24, 2);
    little(header, 0, 4); // BI_RGB.
    little(header, 4, 4);
    header.resize(54);
    file.write(0, header);
    file.write(offset, {0x12, 0x34, 0x56, 0});
    file.close();
    const auto result = decode(path);
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.pixels == Bytes{0x56, 0x34, 0x12, 255});
}

TEST_CASE("PNG animation preflight seeks past 2 GiB without narrowing offsets", "[formats][png][file_offset]")
{
    Files files;
    const auto path = files.directory / "offset.png";
    SparseFile file(path);
    // Preflight skips ancillary payloads without decoding them, and must still
    // find acTL after a large sparse chunk (rather than fail a narrowed seek).
    constexpr std::uint32_t padding = 0x7fffffffU;
    Bytes header{137, 80, 78, 71, 13, 10, 26, 10};
    big(header, padding);
    header.insert(header.end(), {'n', 'p', 'A', 'D'});
    file.write(0, header);
    const std::uint64_t next = 8ULL + 12 + padding;
    Bytes control;
    big(control, 8);
    control.insert(control.end(), {'a', 'c', 'T', 'L'});
    big(control, 1);
    big(control, 0);
    big(control, static_cast<std::uint32_t>(crc32(0, control.data() + 4, 12)));
    file.write(next, control);
    file.close();
    const auto result = decode(path);
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
}
