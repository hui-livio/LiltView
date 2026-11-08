#include "region_support.hpp"
using namespace liltview::test;


TEST_CASE("regional codecs crop scaled checkerboards with global tile coordinates", "[region]")
{
    Files files;
    for (const auto suffix : {".png", ".jpg", ".bmp", ".tiff"})
    {
        CAPTURE(suffix);
        const auto path = files.directory / (std::string("checker") + suffix);
        checkerboard(path, 97, 65, 16);
        Engine engine(1, 32 * 1024 * 1024);
        engine.submit(path.string(), {48, 48});
        lv_event *anchor = nullptr;
        auto preview     = receive(engine, &anchor);
        REQUIRE(preview.terminal == LV_EVENT_COMPLETE);
        REQUIRE(anchor);
        CHECK(preview.info.capabilities & LV_CAP_REGION_DECODE);
        submit_region(engine, path.string(), {97, 65}, {13, 7, 53, 39});
        auto crop = receive(engine);
        check_colors(crop, 97, 65, 16);
        CHECK(crop.info.source_revision == preview.info.source_revision);
        CHECK(crop.info.region.x == 13);
        CHECK(crop.info.canvas.width == 97);
        submit_region(engine, path.string(), {48, 32}, {7, 3, 31, 23});
        auto scaled = receive(engine);
        check_colors(scaled, 97, 65, 16);
        CHECK(scaled.info.source_revision == preview.info.source_revision);
        lv_event_release(anchor);
    }
}

TEST_CASE("REGION aspect and unsupported codecs return asynchronous target errors", "[region][errors]")
{
    Files files;
    auto path = files.directory / "aspect.png";
    checkerboard(path, 97, 65, 16);
    Engine engine;
    submit_region(engine, path.string(), {100, 100}, {0, 0, 10, 10});
    auto result = receive(engine);
    REQUIRE(result.terminal == LV_EVENT_ERROR);
    CHECK(result.error.code == LV_ERROR_TARGET_UNSUPPORTED);
    CHECK(result.area == 0);
}
