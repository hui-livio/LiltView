#ifndef LILTVIEW_VIEWPORT_HPP
#define LILTVIEW_VIEWPORT_HPP
#include <array>
#include <liltview/core.h>
#include "renderer/renderer.hpp"

namespace liltview
{
struct RegionTarget
{
    lv_size_u32 canvas{};
    lv_rect_u32 rect{};
    bool operator==(const RegionTarget &) const;
};
class Viewport
{
public:
    void reset();
    bool same_view(const Viewport &) const;
    bool same_layout(const Viewport &) const;
    Viewport toward(const Viewport &, double fraction) const;
    bool visible(lv_rect_u32 native_rect) const;
    void update(render::Point min, render::Point max, float density, lv_size_u32 source, lv_orientation orientation);
    void fit();
    void actual_size();
    void pan(double x, double y);
    void zoom(double wheel, render::Point anchor);
    void zoom(double wheel); // Keyboard/menu zoom is anchored at the canvas center.
    bool fitting() const
    {
        return fit_;
    }
    RegionTarget target(std::uint32_t edge, bool native = false) const;
    double scale() const
    {
        return scale_;
    }
    std::array<render::Point, 4> quad(lv_rect_u32, lv_size_u32 canvas) const;

private:
    void clamp();
    std::array<double, 2> orient(double x, double y) const;
    std::array<double, 2> unorient(double x, double y) const;
    render::Point min_{}, max_{};
    lv_size_u32 source_{};
    lv_orientation orientation_ = LV_ORIENTATION_NORMAL;
    double density_ = 1, scale_ = 1, zoom_ = 1, fit_scale_ = 1, center_x_ = 0, center_y_ = 0;
    bool fit_ = true, initialized_ = false;
};
} // namespace liltview
#endif
