#ifndef LILTVIEW_TILES_HPP
#define LILTVIEW_TILES_HPP
#include <cstdio>
#include <liltview/core.h>
#include <map>
#include <tuple>
#include <vector>
#include "viewport.hpp"

namespace liltview
{
// One immutable native-resolution scene. Small scenes stay on the GPU; large
// scenes keep compressed native tiles/mip levels in an anonymous local cache.
// Camera changes never submit a decode or resample image contents.
class Tiles
{
public:
    explicit Tiles(render::Renderer &renderer) :
        renderer_(renderer)
    {
    }
    ~Tiles()
    {
        clear();
    }
    Tiles(const Tiles &)            = delete;
    Tiles &operator=(const Tiles &) = delete;
    void begin(const lv_image_info &);
    void upload(const lv_region_info &);
    void clear() noexcept;
    void release_details() noexcept;
    void swap(Tiles &);
    bool prepare(const Viewport &); // Upload cached visible textures before building draw commands.
    void draw(const Viewport &, ImDrawList *) const;
    bool empty() const
    {
        return ready_ == 0;
    }
    std::size_t loaded_tiles() const
    {
        return ready_;
    }
    bool complete() const
    {
        return ready_ == tiles_.size() && ready_ != 0;
    }

private:
    struct Level
    {
        render::Size size{};
        std::uint64_t offset = 0;
        std::uint32_t bytes  = 0;
    };
    struct Tile
    {
        lv_rect_u32 rect{};
        render::Image overview{}, detail{};
        std::vector<Level> levels;
        unsigned overview_level = 0, detail_level = 0;
        std::uint64_t touched = 0;
        std::array<render::Point, 4> overview_uv{};
    };
    struct Atlas
    {
        lv_rect_u32 rect{};
        render::Image image{};
    };
    unsigned level_for(const Viewport &) const;
    bool visible(const Tile &, lv_rect_u32) const;
    render::Image texture(const Tile &, unsigned level) const;
    void make_room(std::uint64_t bytes);
    render::Image upload_texture(render::Size, unsigned level, const std::uint8_t *, std::uint64_t stride = 0,
                                 std::uint64_t bytes = 0);
    void open_cache();
    void upload_overview(Tile &, render::Size, unsigned, const std::uint8_t *);
    render::Renderer &renderer_;
    lv_image_info info_{};
    std::vector<Tile> tiles_;
    std::vector<Atlas> atlases_;
    std::map<std::tuple<std::uint32_t, unsigned, unsigned>, std::vector<Level>> uniform_levels_;
    unsigned atlas_edge_ = 0, atlas_columns_ = 0;
    std::FILE *cache_          = nullptr;
    std::uint64_t cache_bytes_ = 0, clock_ = 0;
    std::size_t ready_       = 0;
    unsigned overview_level_ = 0;
};
} // namespace liltview
#endif
