#include "tiles.hpp"
#include <algorithm>
#include <utility>

namespace liltview
{
void Tiles::begin(const lv_image_info &info)
{
    clear();
    info_ = info;
}
void Tiles::clear() noexcept
{
    for (const auto &tile : tiles_)
        renderer_.destroy_image(tile.image);
    tiles_.clear();
    info_ = {};
}
void Tiles::swap(Tiles &other)
{
    tiles_.swap(other.tiles_);
    std::swap(info_, other.info_);
}
void Tiles::upload(const lv_region_info &region)
{
    auto found = std::find_if(tiles_.begin(), tiles_.end(), [&](const Tile &t) { return t.id == region.tile_id; });
    if (found != tiles_.end() && found->revision >= region.revision)
        return;
    if (found == tiles_.end())
    {
        const bool native = info_.canvas.width == info_.source_size.width && info_.canvas.height == info_.source_size.height;
        auto image        = renderer_.create_image(
            {{region.rect.width, region.rect.height},
             false,
             native ? render::ImageDesc::Magnification::nearest : render::ImageDesc::Magnification::linear});
        try
        {
            tiles_.push_back({region.rect, image, region.tile_id, 0});
        } catch (...)
        {
            renderer_.destroy_image(image);
            throw;
        }
        found = tiles_.end() - 1;
    }
    const auto &pixels = *lv_pixels_get_view(region.pixels);
    renderer_.update_image(found->image, {0, 0, region.rect.width, region.rect.height},
                           {pixels.data, pixels.byte_count, pixels.stride_bytes, {pixels.size.width, pixels.size.height}});
    found->revision = region.revision;
}
void Tiles::draw(const Viewport &viewport, ImDrawList *list) const
{
    constexpr std::array<render::Point, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    for (const auto &tile : tiles_)
        renderer_.add_image_quad(list, tile.image, viewport.quad(tile.rect, info_.canvas), uv);
}
} // namespace liltview
