#ifndef LILTVIEW_TILES_HPP
#define LILTVIEW_TILES_HPP
#include <liltview/core.h>
#include <vector>
#include "viewport.hpp"

namespace liltview
{
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
    void begin(const lv_image_info &info);
    void upload(const lv_region_info &);
    void clear() noexcept;
    void swap(Tiles &);
    void draw(const Viewport &, ImDrawList *) const;
    const lv_image_info &info() const
    {
        return info_;
    }
    bool empty() const
    {
        return tiles_.empty();
    }

private:
    struct Tile
    {
        lv_rect_u32 rect;
        render::Image image;
        std::uint64_t id, revision;
    };
    render::Renderer &renderer_;
    lv_image_info info_{};
    std::vector<Tile> tiles_;
};
} // namespace liltview
#endif
