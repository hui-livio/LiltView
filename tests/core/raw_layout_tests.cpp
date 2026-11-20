#include "support.hpp"

#include <liltview/raw.h>

namespace
{
using namespace liltview::test;

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
    CHECK(lv_core_submit_raw(engine.core, &request, &id) == LV_UNSUPPORTED);
    CHECK(id == 0);

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
