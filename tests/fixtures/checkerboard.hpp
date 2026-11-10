#pragma once
#include "file_io.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <jpeglib.h>
#include <png.h>
#include <stdexcept>
#include <string>
#include <tiffio.h>
#include <vector>
#include <zlib.h>

namespace liltview::test
{
inline void fixture_check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
inline void checker_png(const std::filesystem::path &path, unsigned width, unsigned height, unsigned cell)
{
    FILE *file = fixture_file(path);
    fixture_check(file, "PNG fixture open");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info  = png_create_info_struct(png);
    fixture_check(png && info, "PNG fixture allocation");
    if (setjmp(png_jmpbuf(png)))
        throw std::runtime_error("PNG fixture encode");
    png_init_io(png, file);
    png_set_IHDR(png, info, width, height, 1, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_color palette[2]{{255, 0, 0}, {255, 255, 255}};
    png_set_PLTE(png, info, palette, 2);
    png_set_compression_level(png, 1);
    png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    png_write_info(png, info);
    std::vector<unsigned char> row((static_cast<std::size_t>(width) + 7) / 8, 0);
    for (unsigned y = 0; y < height; ++y)
    {
        if (y % cell == 0)
        {
            std::fill(row.begin(), row.end(), 0);
            for (unsigned x = 0; x < width; ++x)
                if (((x / cell + y / cell) & 1u) != 0)
                    row[x / 8] |= static_cast<unsigned char>(1u << (7 - x % 8));
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    fixture_check(std::fclose(file) == 0, "PNG fixture close");
}
inline void checker_jpeg(const std::filesystem::path &path, unsigned width, unsigned height, unsigned cell)
{
    FILE *file = fixture_file(path);
    fixture_check(file, "JPEG fixture open");
    jpeg_compress_struct encoder{};
    jpeg_error_mgr error{};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    jpeg_stdio_dest(&encoder, file);
    encoder.image_width      = width;
    encoder.image_height     = height;
    encoder.input_components = 3;
    encoder.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 95, TRUE);
    jpeg_start_compress(&encoder, TRUE);
    std::vector<unsigned char> row(static_cast<std::size_t>(width) * 3);
    for (unsigned y = 0; y < height; ++y)
    {
        if (y % cell == 0)
            for (unsigned x = 0; x < width; ++x)
            {
                const bool white = ((x / cell + y / cell) & 1u) != 0;
                row[x * 3]       = 255;
                row[x * 3 + 1] = row[x * 3 + 2] = white ? 255 : 0;
            }
        JSAMPROW data = row.data();
        jpeg_write_scanlines(&encoder, &data, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    fixture_check(std::fclose(file) == 0, "JPEG fixture close");
}
inline void checker_bmp(const std::filesystem::path &path, unsigned width, unsigned height, unsigned cell)
{
    const std::uint64_t stride = (static_cast<std::uint64_t>(width) + 3) / 4 * 4;
    fixture_check(stride * height + 62 <= UINT32_MAX, "Classic BMP file size limit");
    std::array<unsigned char, 62> header{};
    auto put = [&](unsigned at, std::uint32_t value, unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
            header.at(at + i) = static_cast<unsigned char>(value >> (8 * i));
    };
    header[0] = 'B';
    header[1] = 'M';
    put(2, static_cast<std::uint32_t>(stride * height + 62), 4);
    put(10, 62, 4);
    put(14, 40, 4);
    put(18, width, 4);
    put(22, height, 4);
    put(26, 1, 2);
    put(28, 8, 2);
    put(46, 2, 4);
    header[56] = 255;
    header[58] = header[59] = header[60] = 255;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(header.data()), header.size());
    std::vector<unsigned char> row(static_cast<std::size_t>(stride));
    for (unsigned fy = 0; fy < height; ++fy)
    {
        const unsigned y = height - 1 - fy;
        if (fy == 0 || (y + 1) % cell == 0)
            for (unsigned x = 0; x < width; ++x)
                row[x] = static_cast<unsigned char>((x / cell + y / cell) & 1u);
        file.write(reinterpret_cast<const char *>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    fixture_check(static_cast<bool>(file), "BMP fixture write");
}
inline void checker_tiff(const std::filesystem::path &path, unsigned width, unsigned height, unsigned cell)
{
    constexpr unsigned edge = 256;
    TIFF *tif               = fixture_tiff(path, "w8");
    fixture_check(tif, "TIFF fixture open");
    TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, edge);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, edge);
    // Uniform native blocks are compressed once; no multi-gigabyte raw fixture
    // allocation or repeated compression is needed for 16K/32K square samples.
    std::array<std::vector<unsigned char>, 2> compressed;
    const bool uniform = cell % edge == 0;
    std::vector<unsigned char> pixels(edge * edge * 3);
    auto encode = [&](unsigned x, unsigned y)
    {
        for (unsigned row = 0; row < edge; ++row)
            for (unsigned col = 0; col < edge; ++col)
            {
                const bool white = (((x + col) / cell + (y + row) / cell) & 1u) != 0;
                const auto at    = (row * edge + col) * 3;
                pixels[at]       = 255;
                pixels[at + 1] = pixels[at + 2] = white ? 255 : 0;
            }
        uLongf size = compressBound(static_cast<uLong>(pixels.size()));
        std::vector<unsigned char> bytes(size);
        fixture_check(compress2(bytes.data(), &size, pixels.data(), static_cast<uLong>(pixels.size()), 1) == Z_OK,
                      "TIFF fixture compress");
        bytes.resize(size);
        return bytes;
    };
    if (uniform)
    {
        compressed[0] = encode(0, 0);
        compressed[1] = encode(cell, 0);
    }
    for (unsigned y = 0; y < height; y += edge)
        for (unsigned x = 0; x < width; x += edge)
        {
            auto bytes      = uniform ? compressed[(x / cell + y / cell) & 1u] : encode(x, y);
            const auto tile = TIFFComputeTile(tif, x, y, 0, 0);
            fixture_check(TIFFWriteRawTile(tif, tile, bytes.data(), static_cast<tmsize_t>(bytes.size())) >= 0,
                          "TIFF fixture write");
        }
    TIFFClose(tif);
}
inline void checkerboard(const std::filesystem::path &path, unsigned width, unsigned height, unsigned cell = 1024)
{
    fixture_check(width && height && cell, "Invalid checkerboard geometry");
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    const auto suffix = path.extension().string();
    if (suffix == ".png")
        checker_png(path, width, height, cell);
    else if (suffix == ".jpg")
        checker_jpeg(path, width, height, cell);
    else if (suffix == ".bmp")
        checker_bmp(path, width, height, cell);
    else if (suffix == ".tif" || suffix == ".tiff")
        checker_tiff(path, width, height, cell);
    else
        throw std::runtime_error("Use .png/.jpg/.bmp/.tiff output");
}
} // namespace liltview::test
