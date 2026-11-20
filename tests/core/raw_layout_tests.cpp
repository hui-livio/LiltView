#include "support.hpp"

#include <liltview/raw.h>
#include <utility>

namespace
{
using namespace liltview::test;
using Bytes = std::vector<unsigned char>;

Decoded decode_raw(const Bytes &bytes, lv_raw_layout layout, lv_size_u32 size, lv_raw_plane plane,
                   lv_raw_row_order order = LV_RAW_TOP_DOWN, bool native = true,
                   lv_size_u32 bounds = {100, 100}, lv_raw_alpha_mode alpha = LV_RAW_ALPHA_OPAQUE,
                   std::uint64_t memory = 16ULL * 1024 * 1024)
{
    Files files;
    Engine engine(1, memory);
    const auto path = files.write("raw-layout.data", bytes);
    lv_raw_request_desc request;
    lv_raw_request_desc_init(&request);
    request.path = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.kind = native ? LV_TARGET_NATIVE : LV_TARGET_FIT;
    request.target.as.fit_bounds = bounds;
    request.image.layout.layout = layout;
    request.image.layout.size = size;
    request.image.layout.row_order = order;
    request.image.layout.alpha = alpha;
    request.image.layout.plane_count = 1;
    request.image.layout.planes[0] = plane;
    request.image.processing.cfa = LV_RAW_CFA_NONE;
    request.image.processing.black_level = 0;
    request.image.processing.white_level = 255;
    lv_request_id id = 0;
    REQUIRE(lv_core_submit_raw(engine.core, &request, &id) == LV_OK);
    REQUIRE(id != 0);
    return collect(engine);
}

Decoded decode_yuv(const Bytes &bytes, lv_raw_layout layout, lv_size_u32 size, const std::array<lv_raw_plane, 3> &planes,
                   std::uint32_t plane_count, lv_raw_color_range range = LV_RAW_RANGE_FULL,
                   bool native = true, lv_size_u32 bounds = {100, 100},
                   lv_raw_color_matrix matrix = LV_RAW_MATRIX_BT601,
                   lv_raw_chroma_siting siting = LV_RAW_CHROMA_CENTER,
                   lv_raw_row_order row_order = LV_RAW_TOP_DOWN)
{
    Files files;
    Engine engine;
    const auto path = files.write("raw-layout.yuv", bytes);
    lv_raw_request_desc request;
    lv_raw_request_desc_init(&request);
    request.path = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.kind = native ? LV_TARGET_NATIVE : LV_TARGET_FIT;
    request.target.as.fit_bounds = bounds;
    request.image.layout.layout = layout;
    request.image.layout.size = size;
    request.image.layout.row_order = row_order;
    request.image.layout.plane_count = plane_count;
    for (std::uint32_t i = 0; i < plane_count; ++i)
        request.image.layout.planes[i] = planes[i];
    request.image.processing.matrix = matrix;
    request.image.processing.range = range;
    request.image.processing.chroma_siting = siting;
    request.image.processing.cfa = LV_RAW_CFA_NONE;
    request.image.processing.black_level = 0;
    request.image.processing.white_level = 255;
    lv_request_id id = 0;
    const auto submit_status = lv_core_submit_raw(engine.core, &request, &id);
    REQUIRE(submit_status == LV_OK);
    REQUIRE(id != 0);
    return collect(engine);
}

Decoded decode_bayer(const Bytes &bytes, lv_raw_cfa_pattern cfa, lv_size_u32 size, lv_raw_plane plane,
                     uint32_t cfa_x_origin = 0, uint32_t cfa_y_origin = 0, uint32_t black_level = 0,
                     uint32_t white_level = 255, uint32_t red_gain = 65536, uint32_t green_red_gain = 65536,
                     uint32_t green_blue_gain = 65536, uint32_t blue_gain = 65536,
                     lv_raw_row_order row_order = LV_RAW_TOP_DOWN, bool native = true,
                     lv_size_u32 bounds = {100, 100}, std::uint64_t memory = 16ULL * 1024 * 1024,
                     lv_raw_demosaic_quality demosaic = LV_RAW_DEMOSAIC_BILINEAR)
{
    Files files;
    Engine engine(1, memory);
    const auto path = files.write("raw-layout.bayer", bytes);
    lv_raw_request_desc request;
    lv_raw_request_desc_init(&request);
    request.path = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.kind = native ? LV_TARGET_NATIVE : LV_TARGET_FIT;
    request.target.as.fit_bounds = bounds;
    request.image.layout.layout = LV_RAW_LAYOUT_BAYER8;
    request.image.layout.size = size;
    request.image.layout.row_order = row_order;
    request.image.layout.plane_count = 1;
    request.image.layout.planes[0] = plane;
    request.image.processing.cfa = cfa;
    request.image.processing.cfa_x_origin = cfa_x_origin;
    request.image.processing.cfa_y_origin = cfa_y_origin;
    request.image.processing.black_level = black_level;
    request.image.processing.white_level = white_level;
    request.image.processing.red_gain_q16 = red_gain;
    request.image.processing.green_red_gain_q16 = green_red_gain;
    request.image.processing.green_blue_gain_q16 = green_blue_gain;
    request.image.processing.blue_gain_q16 = blue_gain;
    request.image.processing.demosaic = demosaic;
    lv_request_id id = 0;
    const auto submit_status = lv_core_submit_raw(engine.core, &request, &id);
    REQUIRE(submit_status == LV_OK);
    REQUIRE(id != 0);
    return collect(engine);
}

unsigned bayer_test_color(lv_raw_cfa_pattern cfa, uint32_t x, uint32_t y)
{
    const bool odd_x = (x & 1u) != 0;
    const bool odd_y = (y & 1u) != 0;
    switch (cfa)
    {
    case LV_RAW_CFA_RGGB: return odd_y ? (odd_x ? 2u : 1u) : (odd_x ? 1u : 0u);
    case LV_RAW_CFA_BGGR: return odd_y ? (odd_x ? 0u : 1u) : (odd_x ? 1u : 2u);
    case LV_RAW_CFA_GRBG: return odd_y ? (odd_x ? 1u : 2u) : (odd_x ? 0u : 1u);
    case LV_RAW_CFA_GBRG: return odd_y ? (odd_x ? 1u : 0u) : (odd_x ? 2u : 1u);
    default: return 1u;
    }
}

void check_pixel(const Decoded &result, std::uint32_t x, std::uint32_t y, std::array<unsigned, 3> expected,
                 unsigned tolerance = 0)
{
    const std::size_t offset = (static_cast<std::size_t>(y) * result.info.canvas.width + x) * 4;
    for (unsigned channel = 0; channel < 3; ++channel)
    {
        CAPTURE(x, y, channel);
        CHECK(std::abs(static_cast<int>(result.pixels[offset + channel]) - static_cast<int>(expected[channel])) <=
              static_cast<int>(tolerance));
    }
    CHECK(result.pixels[offset + 3] == 255);
}

void require_native(const char *label, const Decoded &result, lv_size_u32 size, const Bytes &expected)
{
    CAPTURE(label);
    REQUIRE(result.terminal == LV_EVENT_COMPLETE);
    CHECK(result.info.format == LV_FORMAT_RAW_LAYOUT);
    CHECK(result.info.source_size.width == size.width);
    CHECK(result.info.source_size.height == size.height);
    CHECK(result.info.canvas.width == size.width);
    CHECK(result.info.canvas.height == size.height);
    REQUIRE(result.pixels.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CAPTURE(i);
        INFO("actual=" << static_cast<unsigned>(result.pixels[i]) << " expected=" << static_cast<unsigned>(expected[i]));
        CHECK(result.pixels[i] == expected[i]);
    }
}

lv_raw_request_desc valid_request(const std::string &path)
{
    lv_raw_request_desc request;
    lv_raw_request_desc_init(&request);
    request.path = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.as.fit_bounds = {16, 16};
    request.image.layout.layout = LV_RAW_LAYOUT_RGB24;
    request.image.layout.size = {3, 2};
    request.image.layout.plane_count = 1;
    request.image.layout.planes[0] = {.offset = 0, .stride = 9, .row_bytes = 9, .height = 2};
    request.image.processing.white_level = 255;
    request.image.processing.cfa = LV_RAW_CFA_NONE;
    return request;
}
}

TEST_CASE("raw layout ABI initializes independently from encoded image ABI", "[raw-layout][abi]")
{
    lv_raw_request_desc request;
    lv_raw_request_desc_init(&request);
    CHECK(request.abi_version == LV_RAW_ABI_VERSION);
    CHECK(request.struct_size == sizeof request);
    CHECK(request.image.struct_size == sizeof request.image);
    CHECK(request.image.layout.struct_size == sizeof request.image.layout);
    CHECK(request.image.processing.struct_size == sizeof request.image.processing);
    CHECK(request.image.output.struct_size == sizeof request.image.output);
    CHECK(request.target.kind == LV_TARGET_FIT);
    CHECK(request.image.output.pixel_format == LV_PIXEL_RGBA8);
}

TEST_CASE("raw layout validation rejects ambiguous and unsafe descriptions", "[raw-layout][errors]")
{
    Files files;
    const auto path = files.write("raw.rgb", std::vector<unsigned char>(18, 0));
    Engine engine;

    auto request = valid_request(path);
    lv_request_id id = 0;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_OK);
    REQUIRE(id != 0);
    CHECK(collect(engine).terminal == LV_EVENT_COMPLETE);
    id = 0;

    request.image.layout.planes[0].stride = 8;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);

    request = valid_request(path);
    request.image.layout.layout = LV_RAW_LAYOUT_NV12;
    request.image.layout.plane_count = 1;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);

    request = valid_request(path);
    request.image.layout.planes[0].offset = UINT64_MAX;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);

    request = valid_request(path);
    request.image.layout.struct_size -= 1;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_ABI_MISMATCH);
    CHECK(id == 0);

    request = valid_request(path);
    request.image.layout.planes[0].reserved = 1;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);

    request = valid_request(path);
    request.image.layout.layout = LV_RAW_LAYOUT_I420;
    request.image.layout.size = {2, 2};
    request.image.layout.plane_count = 3;
    request.image.layout.planes[0] = {.offset = 0, .stride = 2, .row_bytes = 2, .height = 2};
    request.image.layout.planes[1] = {.offset = 3, .stride = 1, .row_bytes = 1, .height = 1};
    request.image.layout.planes[2] = {.offset = 4, .stride = 1, .row_bytes = 1, .height = 1};
    request.image.processing.matrix = LV_RAW_MATRIX_BT601;
    request.image.processing.range = LV_RAW_RANGE_FULL;
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);

    request = valid_request(path);
    request.target.kind = LV_TARGET_REGION;
    request.target.as.region.canvas = {3, 2};
    request.target.as.region.rect = {0, 0, 3, 2};
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_UNSUPPORTED);
    CHECK(id == 0);
}

TEST_CASE("raw RGB and grayscale layouts decode to RGBA8", "[raw-layout][rgb]")
{
    const lv_size_u32 size{3, 2};
    const Bytes rgb{255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 255, 255, 0};
    const Bytes bgr{0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 255};
    const Bytes rgba{255, 0, 0, 10, 0, 255, 0, 20, 0, 0, 255, 30, 255, 255, 255, 40, 0, 0, 0, 50, 255, 255, 0, 60};
    const Bytes bgra{0, 0, 255, 10, 0, 255, 0, 20, 255, 0, 0, 30, 255, 255, 255, 40, 0, 0, 0, 50, 0, 255, 255, 60};
    const Bytes gray{10, 20, 30, 40, 50, 60};
    const Bytes expected_rgb{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255,
                             255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 0, 255};
    const Bytes expected_rgba{255, 0, 0, 10, 0, 255, 0, 20, 0, 0, 255, 30, 255, 255, 255, 40,
                             0, 0, 0, 50, 255, 255, 0, 60};
    const Bytes expected_gray{10, 10, 10, 255, 20, 20, 20, 255, 30, 30, 30, 255,
                              40, 40, 40, 255, 50, 50, 50, 255, 60, 60, 60, 255};

    require_native("rgb", decode_raw(rgb, LV_RAW_LAYOUT_RGB24, size, {0, 9, 9, 2}), size, expected_rgb);
    require_native("bgr", decode_raw(bgr, LV_RAW_LAYOUT_BGR24, size, {0, 9, 9, 2}), size, expected_rgb);
    require_native("rgba", decode_raw(rgba, LV_RAW_LAYOUT_RGBA8, size, {0, 12, 12, 2}, LV_RAW_TOP_DOWN, true,
                                    {100, 100}, LV_RAW_ALPHA_STRAIGHT),
                   size, expected_rgba);
    require_native("bgra", decode_raw(bgra, LV_RAW_LAYOUT_BGRA8, size, {0, 12, 12, 2}, LV_RAW_TOP_DOWN, true,
                                    {100, 100}, LV_RAW_ALPHA_STRAIGHT),
                   size, expected_rgba);
    require_native("gray", decode_raw(gray, LV_RAW_LAYOUT_GRAY8, size, {0, 3, 3, 2}), size, expected_gray);
}

TEST_CASE("raw RGB layouts honor offset, stride, bottom-up rows, and FIT", "[raw-layout][rgb][boundaries]")
{
    const lv_size_u32 size{2, 2};
    Bytes storage(19, 0xaa);
    const Bytes offset_row0{255, 0, 0, 0, 255, 0};
    const Bytes offset_row1{0, 0, 255, 255, 255, 255};
    std::copy(offset_row0.begin(), offset_row0.end(), storage.begin() + 3);
    std::copy(offset_row1.begin(), offset_row1.end(), storage.begin() + 11);
    const Bytes expected{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    require_native("offset", decode_raw(storage, LV_RAW_LAYOUT_RGB24, size, {3, 8, 6, 2}), size, expected);

    Bytes bottom_up(16, 0);
    const Bytes bottom_row{0, 0, 255, 255, 255, 255};
    const Bytes top_row{255, 0, 0, 0, 255, 0};
    std::copy(bottom_row.begin(), bottom_row.end(), bottom_up.begin());
    std::copy(top_row.begin(), top_row.end(), bottom_up.begin() + 8);
    const Bytes top_down_expected{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    require_native("bottom-up", decode_raw(bottom_up, LV_RAW_LAYOUT_RGB24, size, {0, 8, 6, 2}, LV_RAW_BOTTOM_UP), size,
                   top_down_expected);

    const auto fitted = decode_raw(storage, LV_RAW_LAYOUT_RGB24, size, {3, 8, 6, 2}, LV_RAW_TOP_DOWN, false, {1, 1});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.format == LV_FORMAT_RAW_LAYOUT);
    CHECK(fitted.info.canvas.width == 1);
    CHECK(fitted.info.canvas.height == 1);
    CHECK(fitted.pixels.size() == 4);
}

TEST_CASE("raw RGB reports truncation and advertises its explicit format", "[raw-layout][rgb][errors]")
{
    Engine engine;
    lv_format_support support{};
    REQUIRE(lv_core_query_format(engine.core, LV_FORMAT_RAW_LAYOUT, &support) == LV_OK);
    CHECK(support.available == 1);

    Files files;
    const auto path = files.write("truncated.rgb", {255, 0, 0, 0});
    lv_raw_request_desc request = valid_request(path);
    lv_request_id id = 0;
    REQUIRE(lv_core_submit_raw(engine.core, &request, &id) == LV_OK);
    const auto result = collect(engine);
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_CORRUPT_DATA);
}

TEST_CASE("raw RGB layouts reject truncation and report budget failures", "[raw-layout][errors]")
{
    const lv_size_u32 size{3, 2};
    const Bytes complete(18, 0x7f);
    const auto truncated = decode_raw(complete, LV_RAW_LAYOUT_RGB24, size, {0, 9, 9, 2});
    REQUIRE(truncated.terminal == LV_EVENT_COMPLETE);

    const auto damaged = decode_raw(Bytes(8, 0x7f), LV_RAW_LAYOUT_RGB24, size, {0, 9, 9, 2});
    REQUIRE(damaged.terminal == LV_EVENT_ERROR);
    CHECK((damaged.error.code == LV_ERROR_CORRUPT_DATA || damaged.error.code == LV_ERROR_IO));

    const lv_size_u32 budget_size{128, 128};
    const Bytes budget_bytes(static_cast<std::size_t>(budget_size.width) * budget_size.height * 3, 0x7f);
    const auto budget = decode_raw(budget_bytes, LV_RAW_LAYOUT_RGB24, budget_size,
                                   {0, budget_size.width * 3, budget_size.width * 3, budget_size.height},
                                   LV_RAW_TOP_DOWN, true, {100, 100}, LV_RAW_ALPHA_OPAQUE, 64ULL * 1024);
    REQUIRE(budget.terminal == LV_EVENT_ERROR);
    CHECK((budget.error.code == LV_ERROR_MEMORY_BUDGET || budget.error.code == LV_ERROR_OUT_OF_MEMORY));
}

TEST_CASE("raw RGB detects cancellation and source changes after metadata", "[raw-layout][source][cancel]")
{
    for (const bool cancel : {false, true})
    {
        Files files;
        Engine engine;
        engine.gate = true;
        const lv_size_u32 source_size{3, 2};
        const Bytes bytes(static_cast<std::size_t>(source_size.width) * source_size.height * 3, 0x7f);
        const auto path = files.write("gated.rgb", bytes);
        auto request = valid_request(path);
        request.target.as.fit_bounds = source_size;
        request.image.layout.size = source_size;
        request.image.layout.planes[0] = {0, source_size.width * 3, source_size.width * 3, source_size.height, 0};
        lv_request_id id = 0;
        REQUIRE(lv_core_submit_raw(engine.core, &request, &id) == LV_OK);
        {
            std::unique_lock lock(engine.mutex);
            REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
        }
        if (cancel)
            REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
        else
        {
            std::ofstream output(path, std::ios::binary | std::ios::app);
            output.put('\0');
        }
        {
            std::lock_guard lock(engine.mutex);
            engine.proceed = true;
            engine.changed.notify_all();
        }
        const auto result = collect(engine);
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

TEST_CASE("raw YUV420 layouts decode I420 YV12 NV12 and NV21", "[raw-layout][yuv]")
{
    const lv_size_u32 size{4, 2};
    const std::array<unsigned char, 8> y{128, 128, 128, 128, 128, 128, 128, 128};
    const std::array<unsigned char, 2> u{128, 128};
    const std::array<unsigned char, 2> v{128, 128};
    const std::array<lv_raw_plane, 3> planes{{{0, 4, 4, 2, 0}, {8, 2, 2, 1, 0}, {10, 2, 2, 1, 0}}};
    Bytes i420;
    i420.insert(i420.end(), y.begin(), y.end());
    i420.insert(i420.end(), u.begin(), u.end());
    i420.insert(i420.end(), v.begin(), v.end());
    const auto i420_result = decode_yuv(i420, LV_RAW_LAYOUT_I420, size, planes, 3);
    REQUIRE(i420_result.terminal == LV_EVENT_COMPLETE);
    check_pixel(i420_result, 0, 0, {128, 128, 128});
    check_pixel(i420_result, 3, 1, {128, 128, 128});

    Bytes yv12;
    yv12.insert(yv12.end(), y.begin(), y.end());
    yv12.insert(yv12.end(), v.begin(), v.end());
    yv12.insert(yv12.end(), u.begin(), u.end());
    const auto yv12_result = decode_yuv(yv12, LV_RAW_LAYOUT_YV12, size, planes, 3);
    REQUIRE(yv12_result.terminal == LV_EVENT_COMPLETE);
    check_pixel(yv12_result, 1, 0, {128, 128, 128});

    const std::array<lv_raw_plane, 3> nv_planes{{{0, 4, 4, 2, 0}, {8, 4, 4, 1, 0}, {0, 0, 0, 0, 0}}};
    Bytes nv12;
    nv12.insert(nv12.end(), y.begin(), y.end());
    nv12.insert(nv12.end(), {128, 128, 128, 128});
    const auto nv12_result = decode_yuv(nv12, LV_RAW_LAYOUT_NV12, size, nv_planes, 2);
    REQUIRE(nv12_result.terminal == LV_EVENT_COMPLETE);
    check_pixel(nv12_result, 2, 1, {128, 128, 128});

    Bytes nv21;
    nv21.insert(nv21.end(), y.begin(), y.end());
    nv21.insert(nv21.end(), {128, 128, 128, 128});
    const auto nv21_result = decode_yuv(nv21, LV_RAW_LAYOUT_NV21, size, nv_planes, 2);
    REQUIRE(nv21_result.terminal == LV_EVENT_COMPLETE);
    check_pixel(nv21_result, 0, 1, {128, 128, 128});
}

TEST_CASE("raw YUV420 applies BT.601 full and limited range", "[raw-layout][yuv][color]")
{
    const lv_size_u32 size{2, 2};
    const std::array<lv_raw_plane, 3> planes{{{0, 2, 2, 2, 0}, {4, 1, 1, 1, 0}, {5, 1, 1, 1, 0}}};
    const Bytes full{128, 128, 128, 128, 128, 128};
    const auto full_result = decode_yuv(full, LV_RAW_LAYOUT_I420, size, planes, 3, LV_RAW_RANGE_FULL);
    REQUIRE(full_result.terminal == LV_EVENT_COMPLETE);
    check_pixel(full_result, 0, 0, {128, 128, 128});

    const Bytes limited_black{16, 16, 16, 16, 128, 128};
    const auto black = decode_yuv(limited_black, LV_RAW_LAYOUT_I420, size, planes, 3, LV_RAW_RANGE_LIMITED);
    REQUIRE(black.terminal == LV_EVENT_COMPLETE);
    check_pixel(black, 0, 0, {0, 0, 0});

    const Bytes limited_white{235, 235, 235, 235, 128, 128};
    const auto white = decode_yuv(limited_white, LV_RAW_LAYOUT_I420, size, planes, 3, LV_RAW_RANGE_LIMITED);
    REQUIRE(white.terminal == LV_EVENT_COMPLETE);
    check_pixel(white, 1, 1, {255, 255, 255});
}

TEST_CASE("raw YUV420 honors plane offsets strides FIT and bottom-up rows", "[raw-layout][yuv][boundaries]")
{
    const lv_size_u32 size{4, 2};
    Bytes storage(32, 0xaa);
    const std::array<unsigned char, 8> y{128, 128, 128, 128, 128, 128, 128, 128};
    std::copy(y.begin(), y.end(), storage.begin() + 3);
    storage[15] = 128;
    storage[16] = 128;
    storage[21] = 128;
    storage[22] = 128;
    const std::array<lv_raw_plane, 3> planes{{{3, 6, 4, 2, 0}, {15, 3, 2, 1, 0}, {21, 3, 2, 1, 0}}};
    const auto native = decode_yuv(storage, LV_RAW_LAYOUT_I420, size, planes, 3);
    REQUIRE(native.terminal == LV_EVENT_COMPLETE);
    CHECK(native.info.canvas.width == 4);
    CHECK(native.info.canvas.height == 2);
    const auto fitted = decode_yuv(storage, LV_RAW_LAYOUT_I420, size, planes, 3, LV_RAW_RANGE_FULL, false, {2, 1});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.canvas.width == 2);
    CHECK(fitted.info.canvas.height == 1);

    Bytes bottom_up(32, 0xaa);
    const std::array<unsigned char, 4> bottom_row{16, 16, 16, 16};
    const std::array<unsigned char, 4> top_row{235, 235, 235, 235};
    std::copy(bottom_row.begin(), bottom_row.end(), bottom_up.begin());
    std::copy(top_row.begin(), top_row.end(), bottom_up.begin() + 6);
    bottom_up[15] = bottom_up[16] = bottom_up[21] = bottom_up[22] = 128;
    const std::array<lv_raw_plane, 3> bottom_planes{{{0, 6, 4, 2, 0}, {15, 2, 2, 1, 0}, {21, 2, 2, 1, 0}}};
    const auto bottom = decode_yuv(bottom_up, LV_RAW_LAYOUT_I420, size, bottom_planes, 3, LV_RAW_RANGE_LIMITED,
                                   true, {100, 100}, LV_RAW_MATRIX_BT601, LV_RAW_CHROMA_CENTER, LV_RAW_BOTTOM_UP);
    REQUIRE(bottom.terminal == LV_EVENT_COMPLETE);
    check_pixel(bottom, 0, 0, {255, 255, 255});
    check_pixel(bottom, 0, 1, {0, 0, 0});
}

TEST_CASE("raw YUV420 rejects unsupported color semantics and truncation", "[raw-layout][yuv][errors]")
{
    const lv_size_u32 size{2, 2};
    const std::array<lv_raw_plane, 3> planes{{{0, 2, 2, 2, 0}, {4, 1, 1, 1, 0}, {5, 1, 1, 1, 0}}};
    const Bytes bytes{128, 128, 128, 128, 128, 128};

    for (const auto &semantics : {
             std::pair{LV_RAW_MATRIX_BT709, LV_RAW_RANGE_FULL},
             std::pair{LV_RAW_MATRIX_BT601, LV_RAW_RANGE_UNKNOWN},
         })
    {
        const auto result = decode_yuv(bytes, LV_RAW_LAYOUT_I420, size, planes, 3, semantics.second, true, {100, 100},
                                       semantics.first);
        REQUIRE(result.terminal == LV_EVENT_ERROR);
        CHECK(result.error.code == LV_ERROR_FORMAT_UNSUPPORTED);
    }

    const auto cosited = decode_yuv(bytes, LV_RAW_LAYOUT_I420, size, planes, 3, LV_RAW_RANGE_FULL, true, {100, 100},
                                    LV_RAW_MATRIX_BT601, LV_RAW_CHROMA_COSITED);
    REQUIRE(cosited.terminal == LV_EVENT_ERROR);
    CHECK(cosited.error.code == LV_ERROR_FORMAT_UNSUPPORTED);

    const auto truncated = decode_yuv(Bytes{128, 128, 128, 128}, LV_RAW_LAYOUT_I420, size, planes, 3);
    REQUIRE(truncated.terminal == LV_EVENT_ERROR);
    CHECK(truncated.error.code == LV_ERROR_CORRUPT_DATA);
}

TEST_CASE("raw Bayer8 decodes all CFA patterns with calibrated channels", "[raw-layout][bayer]")
{
    const lv_size_u32 size{4, 4};
    for (const auto cfa : {LV_RAW_CFA_RGGB, LV_RAW_CFA_BGGR, LV_RAW_CFA_GRBG, LV_RAW_CFA_GBRG})
    {
        Bytes raw(static_cast<std::size_t>(size.width) * size.height, 0);
        for (uint32_t y = 0; y < size.height; ++y)
            for (uint32_t x = 0; x < size.width; ++x)
            {
                const unsigned channel = bayer_test_color(cfa, x, y);
                raw[static_cast<std::size_t>(y) * size.width + x] = channel == 0 ? 220 : channel == 1 ? 128 : 36;
            }
        const auto result = decode_bayer(raw, cfa, size, {0, 4, 4, 4});
        REQUIRE(result.terminal == LV_EVENT_COMPLETE);
        check_pixel(result, 1, 1, {220, 128, 36}, 2);
        check_pixel(result, 2, 2, {220, 128, 36}, 2);
    }
}

TEST_CASE("raw Bayer8 applies black white levels gains origin and FIT", "[raw-layout][bayer][boundaries]")
{
    const lv_size_u32 size{4, 4};
    Bytes raw(16, 100);
    const auto calibrated = decode_bayer(raw, LV_RAW_CFA_RGGB, size, {0, 4, 4, 4}, 0, 0, 0, 255, 32768,
                                                                       65536, 98304, 131072);
    REQUIRE(calibrated.terminal == LV_EVENT_COMPLETE);
    check_pixel(calibrated, 0, 0, {50, 125, 200}, 1);
    check_pixel(calibrated, 1, 1, {50, 125, 200}, 1);

    const auto shifted = decode_bayer(raw, LV_RAW_CFA_RGGB, size, {0, 4, 4, 4}, 1, 1, 0, 255);
    REQUIRE(shifted.terminal == LV_EVENT_COMPLETE);
    CHECK(shifted.info.canvas.width == 4);
    CHECK(shifted.info.canvas.height == 4);

    const auto fitted = decode_bayer(raw, LV_RAW_CFA_RGGB, size, {0, 4, 4, 4}, 0, 0, 0, 255, 65536, 65536,
                                                                    65536, 65536, LV_RAW_TOP_DOWN, false, {2, 2});
    REQUIRE(fitted.terminal == LV_EVENT_COMPLETE);
    CHECK(fitted.info.canvas.width == 2);
    CHECK(fitted.info.canvas.height == 2);

    const auto wide_level = decode_bayer(raw, LV_RAW_CFA_RGGB, size, {0, 4, 4, 4}, 0, 0, 0, UINT32_MAX);
    REQUIRE(wide_level.terminal == LV_EVENT_COMPLETE);
    CHECK(wide_level.pixels[0] == 0);

    const auto gained = decode_bayer(raw, LV_RAW_CFA_RGGB, size, {0, 4, 4, 4}, 0, 0, 0, 255, 2 * 65536,
                                                                  65536, 3 * 65536, 4 * 65536);
    REQUIRE(gained.terminal == LV_EVENT_COMPLETE);
    CHECK(gained.pixels[0] == 200);
    CHECK(gained.pixels[1] >= 150);
    CHECK(gained.pixels[2] == 255);
}

TEST_CASE("raw Bayer8 honors offset stride bottom-up and rejects unsupported or damaged input", "[raw-layout][bayer][errors]")
{
    const lv_size_u32 size{3, 3};
    Bytes storage(24, 0xaa);
    for (uint32_t y = 0; y < size.height; ++y)
        for (uint32_t x = 0; x < size.width; ++x)
            storage[2 + static_cast<std::size_t>(y) * 6 + x] = 128;
    const auto offset = decode_bayer(storage, LV_RAW_CFA_RGGB, size, {2, 6, 3, 3});
    REQUIRE(offset.terminal == LV_EVENT_COMPLETE);
    CHECK(offset.info.source_size.width == 3);
    CHECK(offset.info.source_size.height == 3);

    Bytes bottom_up(18, 0);
    std::fill_n(bottom_up.begin(), 3, static_cast<unsigned char>(224));
    std::fill_n(bottom_up.begin() + 6, 3, static_cast<unsigned char>(128));
    std::fill_n(bottom_up.begin() + 12, 3, static_cast<unsigned char>(32));
    const auto bottom = decode_bayer(bottom_up, LV_RAW_CFA_RGGB, size, {0, 6, 3, 3}, 0, 0, 0, 255, 65536,
                                                                        65536, 65536, 65536, LV_RAW_BOTTOM_UP);
    REQUIRE(bottom.terminal == LV_EVENT_COMPLETE);
    CHECK(bottom.pixels[0] < bottom.pixels[24]);

    const auto unsupported = decode_bayer(Bytes(16, 128), LV_RAW_CFA_NONE, {4, 4}, {0, 4, 4, 4});
    REQUIRE(unsupported.terminal == LV_EVENT_ERROR);
    CHECK(unsupported.error.code == LV_ERROR_FORMAT_UNSUPPORTED);

    const auto damaged = decode_bayer(Bytes(8, 128), LV_RAW_CFA_RGGB, {4, 4}, {0, 4, 4, 4});
    REQUIRE(damaged.terminal == LV_EVENT_ERROR);
    CHECK(damaged.error.code == LV_ERROR_CORRUPT_DATA);

    const lv_size_u32 large{128, 128};
    const auto budget = decode_bayer(Bytes(static_cast<std::size_t>(large.width) * large.height, 128), LV_RAW_CFA_RGGB,
                                     large, {0, large.width, large.width, large.height}, 0, 0, 0, 255, 65536, 65536,
                                     65536, 65536, LV_RAW_TOP_DOWN, true, {100, 100}, 64ULL * 1024);
    REQUIRE(budget.terminal == LV_EVENT_ERROR);
    CHECK((budget.error.code == LV_ERROR_MEMORY_BUDGET || budget.error.code == LV_ERROR_OUT_OF_MEMORY));
}
