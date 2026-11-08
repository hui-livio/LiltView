#include <cstdio>
#include <sys/resource.h>
#include "region_support.hpp"
using namespace liltview::test;
namespace
{
void exercise(const char *suffix, unsigned edge)
{
    const std::filesystem::path folder = LILTVIEW_LARGE_FIXTURE_DIR;
    const auto path                    = folder / (std::string("checkerboard-") + std::to_string(edge) + suffix);
    checkerboard(path, edge, edge, 1024);
    Engine engine(1, 32 * 1024 * 1024, 0, 0, 64);
    engine.timeout = std::chrono::seconds(60);
    engine.submit(path.string(), {512, 512});
    lv_event *anchor = nullptr;
    auto fit         = receive(engine, &anchor);
    REQUIRE(fit.terminal == LV_EVENT_COMPLETE);
    REQUIRE(anchor);
    CHECK(fit.info.source_size.width == edge);
    CHECK(fit.info.source_size.height == edge);
    CHECK(fit.info.load_mode == LV_LOAD_LARGE);
    check_colors(fit, edge, edge, 1024);
    const lv_rect_u32 rect{edge / 2 - 253, edge / 2 - 251, 509, 503};
    submit_region(engine, path.string(), {edge, edge}, rect);
    auto crop = receive(engine);
    REQUIRE(crop.terminal == LV_EVENT_COMPLETE);
    CHECK(crop.info.canvas.width == edge);
    CHECK(crop.info.source_revision == fit.info.source_revision);
    check_colors(crop, edge, edge, 1024);
    submit_region(engine, path.string(), {edge, edge}, {edge - 17, edge - 13, 17, 13});
    auto corner = receive(engine);
    check_colors(corner, edge, edge, 1024);
    const auto peak = std::max({fit.peak, crop.peak, corner.peak});
    CHECK(peak < 32 * 1024 * 1024);
    lv_event_release(anchor);
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    std::printf("PASS %ux%u %s, source=%llu bytes, tracked peak=%llu bytes, process max RSS=%ld KiB\n", edge, edge, suffix,
                static_cast<unsigned long long>(std::filesystem::file_size(path)), static_cast<unsigned long long>(peak),
                usage.ru_maxrss);
}
} // namespace
TEST_CASE("16K PNG checkerboard streams under 32 MiB core budget", "[large_png]")
{
    exercise(".png", 16384);
}
TEST_CASE("16K JPEG checkerboard streams under 32 MiB core budget", "[large_jpeg]")
{
    exercise(".jpg", 16384);
}
TEST_CASE("16K BMP checkerboard above 64 MiB file limit uses bounded row reads", "[large_bmp]")
{
    exercise(".bmp", 16384);
}
TEST_CASE("32K TIFF checkerboard exceeds 16K without whole-image allocation", "[large_tiff]")
{
    exercise(".tiff", 32768);
}
