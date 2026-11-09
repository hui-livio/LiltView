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

TEST_CASE("JPEG visible scanlines match full decoding at native and reduced resolutions", "[region][jpeg]")
{
    Files files;
    const auto path = files.directory / "scanlines.jpg";
    checkerboard(path, 513, 1025, 7);
    Engine engine(1, 32 * 1024 * 1024);
    for (lv_size_u32 canvas : {lv_size_u32{513, 1025}, lv_size_u32{256, 512}})
    {
        submit_region(engine, path.string(), canvas, {0, 0, canvas.width, canvas.height});
        auto full = receive(engine);
        REQUIRE(full.terminal == LV_EVENT_COMPLETE);
        for (unsigned top : {0u, 17u, canvas.height / 2, canvas.height - 1})
        {
            CAPTURE(canvas.width, canvas.height, top);
            const lv_rect_u32 rect{13, top, 31, std::min(19u, canvas.height - top)};
            submit_region(engine, path.string(), canvas, rect);
            auto crop = receive(engine);
            REQUIRE(crop.terminal == LV_EVENT_COMPLETE);
            for (unsigned y = 0; y < rect.height; ++y)
                for (unsigned x = 0; x < rect.width * 4; ++x)
                    REQUIRE(crop.data[static_cast<std::size_t>(y) * rect.width * 4 + x] ==
                            full.data[(static_cast<std::size_t>(rect.y + y) * canvas.width + rect.x) * 4 + x]);
        }
    }
}

TEST_CASE("NATIVE decodes original pixels in monotonic row bands within a sub-image budget", "[native][stream]")
{
    Files files;
    constexpr unsigned width = 1024, height = 8192, cell = 7;
    const auto path = files.directory / "native.png";
    checkerboard(path, width, height, cell);
    constexpr std::uint64_t budget = 12ULL * 1024 * 1024;
    Engine engine(1, budget, 0, 0, 64);
    engine.timeout = std::chrono::seconds(15);
    auto text      = path.string();
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path        = {LV_PATH_POSIX_BYTES, text.data(), text.size()};
    request.target.kind = LV_TARGET_NATIVE;
    lv_request_id id    = 0;
    REQUIRE(lv_core_submit(engine.core, &request, &id) == LV_OK);
    unsigned last_y    = 0;
    std::uint64_t area = 0, tiles = 0;
    bool info = false;
    for (;;)
    {
        Event event(engine.next());
        const auto &view = event.view();
        if (view.type == LV_EVENT_IMAGE_INFO)
        {
            REQUIRE(view.as.image.canvas.width == width);
            REQUIRE(view.as.image.canvas.height == height);
            info = true;
        }
        else if (view.type == LV_EVENT_REGION_READY)
        {
            REQUIRE(info);
            const auto &tile = view.as.region;
            REQUIRE(tile.rect.y >= last_y);
            last_y             = tile.rect.y;
            const auto &pixels = *lv_pixels_get_view(tile.pixels);
            for (unsigned y = 0; y < tile.rect.height; y += 11)
                for (unsigned x = 0; x < tile.rect.width; x += 13)
                {
                    const bool white = (((tile.rect.x + x) / cell + (tile.rect.y + y) / cell) & 1u) != 0;
                    const auto *p    = pixels.data + y * pixels.stride_bytes + x * 4;
                    REQUIRE(p[0] == 255);
                    REQUIRE(p[1] == (white ? 255 : 0));
                    REQUIRE(p[2] == p[1]);
                    REQUIRE(p[3] == 255);
                }
            area += static_cast<std::uint64_t>(tile.rect.width) * tile.rect.height;
            ++tiles;
        }
        else
        {
            REQUIRE(view.type == LV_EVENT_COMPLETE);
            CHECK(view.as.completion.final_tile_count == tiles);
            CHECK(area == static_cast<std::uint64_t>(width) * height);
            break;
        }
        lv_core_stats stats{};
        REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
        CHECK(stats.accounted_cpu_bytes <= budget);
    }
}

TEST_CASE("NATIVE streams all regional formats and handles partial edge tiles", "[native][formats]")
{
    Files files;
    for (const auto *suffix : {".png", ".jpg", ".bmp", ".tiff"})
    {
        CAPTURE(suffix);
        const auto path = files.directory / (std::string("native") + suffix);
        checkerboard(path, 513, 769, 17);
        Engine engine(1, 32ULL * 1024 * 1024, 0, 0, 64);
        const auto text = path.string();
        lv_request_desc request;
        lv_request_desc_init(&request);
        request.path        = {LV_PATH_POSIX_BYTES, text.data(), text.size()};
        request.target.kind = LV_TARGET_NATIVE;
        lv_request_id id    = 0;
        REQUIRE(lv_core_submit(engine.core, &request, &id) == LV_OK);
        const auto image = receive(engine);
        check_colors(image, 513, 769, 17);
        CHECK(image.info.canvas.width == 513);
        CHECK(image.info.canvas.height == 769);
    }
}
