#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <thread>
#include "../fixtures/checkerboard.hpp"
#include "platform/file_io.hpp"
#include "viewer/tile_preparation.hpp"
#include "viewer/tile_reader.hpp"

namespace
{
using namespace liltview;
struct CoreTile
{
    std::filesystem::path file = std::filesystem::temp_directory_path() /
        ("liltview-preparation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".png");
    lv_core *core = nullptr;
    lv_region_info region{};
    CoreTile()
    {
        test::checkerboard(file, 512, 512, 17);
        lv_core_config config;
        lv_core_config_init(&config);
        config.tile_edge         = 512;
        config.cpu_budget_bytes  = 64ULL * 1024 * 1024;
        config.queue_pixel_bytes = 4ULL * 1024 * 1024;
        REQUIRE(lv_core_create(&config, &core) == LV_OK);
        lv_request_desc request;
        lv_request_desc_init(&request);
        const auto path     = file.u8string();
        request.path        = {LV_PATH_UTF8, path.data(), path.size()};
        request.target.kind = LV_TARGET_NATIVE;
        lv_request_id id    = 0;
        REQUIRE(lv_core_submit(core, &request, &id) == LV_OK);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!region.pixels)
        {
            lv_event *event = nullptr;
            if (lv_core_poll_event(core, &event) == LV_OK)
            {
                const auto &view = *lv_event_get_view(event);
                if (view.type == LV_EVENT_REGION_READY)
                {
                    region = view.as.region;
                    lv_pixels_retain(region.pixels);
                }
                lv_event_release(event);
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ~CoreTile()
    {
        lv_pixels_release(region.pixels);
        lv_core_begin_shutdown(core);
        while (lv_core_join(core, 10) == LV_TIMEOUT)
        {
        }
        lv_core_destroy(&core);
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
    }
};
template <class Predicate>
void until(Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!ready())
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
} // namespace
TEST_CASE("first preparation publishes pixels before terminal and releases borrowed inputs", "[preparation]")
{
    CoreTile source;
    TilePreparation preparation(0, TilePreparation::Mode::pipeline, nullptr, nullptr, 77);
    preparation.submit(source.region);
    PreparedTile tile;
    until([&] { return preparation.pop(tile); });
    CHECK_FALSE(preparation.done()); // Producer has not signalled finish; consumer still makes progress.
    CHECK(tile.generation == 77);
    CHECK(tile.pixels.size() == 512 * 512 * 4);
    CHECK(tile.pixels[3] == 255);
    CHECK(preparation.stats().input_peak <= 2);
    preparation.finish();
    until([&] { return preparation.done(); });
    CHECK(preparation.drained());
    CHECK_FALSE(preparation.error());
    CHECK(preparation.take_cache() == nullptr);
}
TEST_CASE("batch preparation withholds every texture until the producer has finished", "[preparation]")
{
    CoreTile source;
    TilePreparation preparation(0, TilePreparation::Mode::batch);
    preparation.submit(source.region);
    until([&] { return preparation.stats().tiles == 1; });
    PreparedTile tile;
    CHECK_FALSE(preparation.pop(tile));
    CHECK_FALSE(preparation.done());
    preparation.finish();
    until([&] { return preparation.done(); });
    REQUIRE(preparation.pop(tile));
    CHECK(tile.pixels.size() == 512 * 512 * 4);
    CHECK(preparation.drained());
    CHECK_FALSE(preparation.error());
}
TEST_CASE("cancelling a full preparation queue wakes backpressure without a UI join", "[preparation][cancel]")
{
    CoreTile source;
    TilePreparation preparation(0, TilePreparation::Mode::pipeline);
    std::uint64_t submitted = 0;
    until(
        [&]
        {
            if (preparation.can_accept())
            {
                auto region    = source.region;
                region.tile_id = submitted++;
                preparation.submit(region);
            }
            return preparation.stats().output_peak > 7ULL * 1024 * 1024 && !preparation.can_accept();
        });
    CHECK(preparation.stats().output_peak <= 8ULL * 1024 * 1024);
    CHECK(preparation.stats().input_peak <= 2);
    preparation.cancel();
    until([&] { return preparation.done(); });
    CHECK(preparation.drained());
    CHECK_FALSE(preparation.error());
}
TEST_CASE("prepared cache is readable after transfer and retains all source levels", "[preparation][cache]")
{
    CoreTile source;
    TilePreparation preparation(2, TilePreparation::Mode::pipeline);
    preparation.submit(source.region);
    preparation.finish();
    until([&] { return preparation.done(); });
    REQUIRE_FALSE(preparation.error());
    PreparedTile tile;
    REQUIRE(preparation.pop(tile));
    REQUIRE(tile.levels.size() == 10);
    CHECK(tile.level == 2);
    CHECK(tile.size.width == 128);
    auto *file = preparation.take_cache();
    REQUIRE(file);
    {
        TileReader reader(platform::file_descriptor(file));
        const auto &level = tile.levels.front();
        reader.request({{{0, 0}, level.size, level.offset, level.bytes}});
        TileReader::Data decoded;
        until(
            [&]
            {
                decoded = reader.find({0, 0});
                return static_cast<bool>(decoded);
            });
        REQUIRE(decoded->size() == 512 * 512 * 4);
        const auto &pixels = *lv_pixels_get_view(source.region.pixels);
        CHECK(std::equal(decoded->begin(), decoded->end(), pixels.data));
    }
    std::fclose(file);
    release_tile_cache(preparation.stats().cache_bytes);
}
TEST_CASE("preparation errors terminate and return retained inputs", "[preparation][error]")
{
    CoreTile source;
    TilePreparation preparation(0, TilePreparation::Mode::pipeline);
    auto invalid       = source.region;
    invalid.rect.width = 1024;
    preparation.submit(invalid);
    preparation.finish();
    until([&] { return preparation.done(); });
    CHECK(preparation.error() != nullptr);
    CHECK(preparation.drained());
}

TEST_CASE("tile cache budget is shared across displayed and pending scenes", "[preparation][budget]")
{
    constexpr auto limit = 16ULL * 1024 * 1024 * 1024;
    reserve_tile_cache(limit);
    CHECK_THROWS_AS(reserve_tile_cache(1), render::Error);
    release_tile_cache(limit);
    CHECK_NOTHROW(reserve_tile_cache(1));
    release_tile_cache(1);
}
