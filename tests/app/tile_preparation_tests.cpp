#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <thread>
#include "../fixtures/checkerboard.hpp"
#include "platform/file_io.hpp"
#include "viewer/tile_preparation.hpp"
#include "viewer/cache_codec.hpp"
#include "viewer/tile_pixels.hpp"
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
    CHECK(tile.pixels.data() == lv_pixels_get_view(source.region.pixels)->data);
    CHECK(tile.borrowed);
    CHECK(preparation.stats().workers == 0);
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
TEST_CASE("cancelling a full retained-input queue wakes backpressure without a UI join", "[preparation][cancel]")
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
            return preparation.stats().tiles >= 2 && !preparation.can_accept();
        });
    CHECK(preparation.stats().output_peak <= 8ULL * 1024 * 1024);
    CHECK(preparation.stats().input_peak <= 4);
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
    auto file = preparation.take_cache();
    REQUIRE(file);
    {
        TileReader reader(file);
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
    file.reset();
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
    constexpr auto limit = MemoryTileCache::budget;
    reserve_tile_cache(limit);
    CHECK_THROWS_AS(reserve_tile_cache(1), render::Error);
    release_tile_cache(limit);
    CHECK_NOTHROW(reserve_tile_cache(1));
    release_tile_cache(1);
}

TEST_CASE("LZ4 cache blocks preserve bytes and detect corrupt truncated or mismatched data", "[preparation][lz4]")
{
    liltview::CacheEncoder encoder;
    for (std::size_t bytes : {4u, 60u, 1024u, 1048576u})
    {
        std::vector<std::uint8_t> data(bytes);
        std::uint32_t state = 123;
        for (auto &byte : data) { state = state * 1664525u + 1013904223u; byte = static_cast<std::uint8_t>(state >> 24); }
        const auto encoded = encoder.encode(data);
        std::vector<std::uint8_t> compressed(encoded.begin(), encoded.end());
        CHECK(liltview::decode_cache(compressed, bytes) == data);
        CHECK_THROWS(liltview::decode_cache(compressed, bytes + 4));
        auto corrupt = compressed;
        corrupt[4] ^= 1; // Checksum, even if the LZ4 stream remains structurally valid.
        CHECK_THROWS(liltview::decode_cache(corrupt, bytes));
        compressed.pop_back();
        CHECK_THROWS(liltview::decode_cache(compressed, bytes));
    }
    CHECK_THROWS(encoder.encode({}));
    CHECK_THROWS(liltview::decode_cache({}, 4));
}
TEST_CASE("preparation workers adapt to actual pixel work rather than a 4K cutoff", "[preparation][workers]")
{
    lv_image_info info{};
    info.tile_edge = 512;
    info.canvas = {1920, 1080};
    CHECK(TilePreparation::workers_for(info) == 1);
    info.canvas = {4096, 4096};
    CHECK(TilePreparation::workers_for(info) == (std::thread::hardware_concurrency() == 1 ? 1u : 2u));
    info.canvas = {8192, 8192};
    CHECK(TilePreparation::workers_for(info) <= 2);
    info.canvas = {17952, 13200};
    CHECK(TilePreparation::workers_for(info) <= 2);
    info.canvas = {32768, 16};
    CHECK(TilePreparation::workers_for(info) == 1); // A long edge alone is not a large CPU task.
}
TEST_CASE("two preparation workers keep independent LZ4 levels and bounded shared output", "[preparation][workers][cache]")
{
    CoreTile source;
    const bool compression = GENERATE(true, false);
    TilePreparation preparation(2, TilePreparation::Mode::pipeline, nullptr, nullptr, 101, 2, compression);
    std::uint64_t submitted = 0;
    std::vector<PreparedTile> tiles;
    const auto *pixels = lv_pixels_get_view(source.region.pixels)->data;
    std::vector<std::vector<std::uint8_t>> expected{std::vector<std::uint8_t>(pixels, pixels + 512 * 512 * 4)};
    render::Size expected_size{512, 512};
    while (expected_size.width > 1)
    {
        expected.push_back(tile_pixels::reduce(expected.back(), expected_size));
        expected_size = {(expected_size.width + 1) / 2, (expected_size.height + 1) / 2};
    }
    until([&] {
        if (submitted < 24 && preparation.can_accept())
        {
            auto region = source.region;
            region.tile_id = submitted++;
            preparation.submit(region);
        }
        PreparedTile tile;
        while (preparation.pop(tile)) tiles.push_back(std::move(tile));
        return submitted == 24;
    });
    preparation.finish();
    until([&] { return preparation.done(); });
    PreparedTile tile;
    while (preparation.pop(tile)) tiles.push_back(std::move(tile));
    REQUIRE_FALSE(preparation.error());
    REQUIRE(tiles.size() == 24);
    if (!compression) CHECK(preparation.stats().compress_ms == 0);
    CHECK(preparation.stats().workers == 2);
    CHECK(preparation.stats().worker_peak <= 2);
    CHECK(preparation.stats().input_peak <= 8);
    CHECK(preparation.stats().output_peak <= 8ULL * 1024 * 1024);
    CHECK(preparation.stats().scratch_limit_bytes == 8ULL * 1024 * 1024);
    auto file = preparation.take_cache();
    REQUIRE(file);
    {
        TileReader reader(file);
        std::vector<bool> seen(24);
        for (const auto &ready : tiles)
        {
            REQUIRE(ready.id < seen.size());
            CHECK_FALSE(seen[ready.id]);
            seen[ready.id] = true;
            CHECK(ready.generation == 101);
            REQUIRE(ready.levels.size() == 10);
            for (unsigned level = 0; level < ready.levels.size(); ++level)
            {
                const auto &entry = ready.levels[level];
                reader.request({{{ready.id, level}, entry.size, entry.offset, entry.bytes}});
                TileReader::Data decoded;
                until([&] { decoded = reader.find({ready.id, level}); return static_cast<bool>(decoded); });
                CHECK(decoded->size() == static_cast<std::size_t>(entry.size.width) * entry.size.height * 4);
                CHECK((*decoded)[3] == 255);
                CHECK(decoded == expected[level]);
                if (!compression)
                {
                    const auto stored = file->find(entry.offset, entry.bytes);
                    CHECK_FALSE(stored->compressed);
                    CHECK(decoded.data() == stored->data.data());
                }
            }
        }
    }
    file.reset();
}
