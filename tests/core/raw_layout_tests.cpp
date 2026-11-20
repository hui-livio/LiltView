#include "support.hpp"

#include <liltview/raw.h>

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
