#pragma once
#include <cmath>
#include "../fixtures/checkerboard.hpp"
#include "support.hpp"
namespace liltview::test
{
struct RegionResult
{
    lv_image_info info{};
    lv_event_type terminal = 0;
    lv_error_info error{};
    std::vector<unsigned char> data;
    std::uint64_t tiles = 0, area = 0, peak = 0;
};
inline RegionResult receive(Engine &engine, lv_event **anchor = nullptr)
{
    RegionResult result;
    for (;;)
    {
        Event event(engine.next());
        const auto &v = event.view();
        if (v.type == LV_EVENT_IMAGE_INFO)
        {
            result.info = v.as.image;
            result.data.resize(static_cast<std::size_t>(result.info.region.width) * result.info.region.height * 4);
            if (anchor)
            {
                *anchor     = event.event;
                event.event = nullptr;
            }
        }
        else if (v.type == LV_EVENT_REGION_READY)
        {
            const auto &tile = v.as.region;
            const auto &p    = *lv_pixels_get_view(tile.pixels);
            const auto &rect = result.info.region;
            REQUIRE(tile.rect.x >= rect.x);
            REQUIRE(tile.rect.y >= rect.y);
            REQUIRE(static_cast<std::uint64_t>(tile.rect.x) + tile.rect.width <= static_cast<std::uint64_t>(rect.x) + rect.width);
            REQUIRE(static_cast<std::uint64_t>(tile.rect.y) + tile.rect.height <=
                    static_cast<std::uint64_t>(rect.y) + rect.height);
            const auto columns =
                (static_cast<std::uint64_t>(result.info.canvas.width) + result.info.tile_edge - 1) / result.info.tile_edge;
            CHECK(tile.tile_id ==
                  static_cast<std::uint64_t>(tile.rect.y / result.info.tile_edge) * columns +
                      tile.rect.x / result.info.tile_edge);
            for (unsigned y = 0; y < tile.rect.height; ++y)
                std::copy_n(p.data + y * p.stride_bytes, tile.rect.width * 4,
                            result.data.data() +
                                (static_cast<std::size_t>(tile.rect.y - rect.y + y) * rect.width + tile.rect.x - rect.x) * 4);
            ++result.tiles;
            result.area += static_cast<std::uint64_t>(tile.rect.width) * tile.rect.height;
        }
        else
        {
            result.terminal = v.type;
            if (v.type == LV_EVENT_ERROR)
                result.error = v.as.error;
            if (v.type == LV_EVENT_COMPLETE)
            {
                CHECK(v.as.completion.final_tile_count == result.tiles);
                CHECK(result.area == static_cast<std::uint64_t>(result.info.region.width) * result.info.region.height);
            }
            return result;
        }
        lv_core_stats stats{};
        REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
        result.peak = std::max(result.peak, stats.accounted_cpu_bytes);
    }
}
inline void submit_region(Engine &engine, const std::string &path, lv_size_u32 canvas, lv_rect_u32 rect)
{
    lv_request_desc desc;
    lv_request_desc_init(&desc);
    desc.path             = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    desc.target.kind      = LV_TARGET_REGION;
    desc.target.as.region = {canvas, rect};
    lv_request_id id      = 0;
    REQUIRE(lv_core_submit(engine.core, &desc, &id) == LV_OK);
}
inline void check_colors(const RegionResult &image, unsigned source_width, unsigned source_height, unsigned cell)
{
    REQUIRE(image.terminal == LV_EVENT_COMPLETE);
    for (unsigned y = 0; y < image.info.region.height; y += 7)
        for (unsigned x = 0; x < image.info.region.width; x += 7)
        {
            const double sx = (image.info.region.x + x + 0.5) * source_width / image.info.canvas.width;
            const double sy = (image.info.region.y + y + 0.5) * source_height / image.info.canvas.height;
            if (std::fmod(sx, cell) < 2 || std::fmod(sy, cell) < 2 || std::fmod(sx, cell) > cell - 2 ||
                std::fmod(sy, cell) > cell - 2)
                continue;
            const bool white = ((static_cast<unsigned>(sx) / cell + static_cast<unsigned>(sy) / cell) & 1u) != 0;
            const auto at    = (static_cast<std::size_t>(y) * image.info.region.width + x) * 4;
            CHECK(image.data[at] > 235);
            if (white)
                CHECK(image.data[at + 1] > 230);
            else
                CHECK(image.data[at + 1] < 25);
            CHECK(image.data[at + 3] == 255);
        }
}
} // namespace liltview::test
