#include "viewport.hpp"
#include <algorithm>
#include <cmath>

namespace liltview
{
bool RegionTarget::operator==(const RegionTarget &other) const
{
    return canvas.width == other.canvas.width && canvas.height == other.canvas.height && rect.x == other.rect.x &&
        rect.y == other.rect.y && rect.width == other.rect.width && rect.height == other.rect.height;
}
void Viewport::reset()
{
    *this = {};
}
void Viewport::fit()
{
    fit_         = true;
    initialized_ = false;
}
void Viewport::actual_size()
{
    fit_  = false;
    zoom_ = scale_ = 1;
    clamp();
}
void Viewport::update(render::Point min, render::Point max, float density, lv_size_u32 source, lv_orientation orientation)
{
    min_         = min;
    max_         = max;
    density_     = std::max(0.01, static_cast<double>(density));
    source_      = source;
    orientation_ = orientation;
    if (!source.width || !source.height)
        return;
    const double width  = orientation_ >= 5 ? source.height : source.width,
                 height = orientation_ >= 5 ? source.width : source.height;
    fit_scale_          = std::min(
        {static_cast<double>(max.x - min.x) * density_ / width, static_cast<double>(max.y - min.y) * density_ / height, 1.0});
    fit_scale_ = std::max(fit_scale_, 1e-12);
    scale_     = fit_ ? fit_scale_ : zoom_;
    if (!initialized_ || fit_)
    {
        center_x_    = width / 2;
        center_y_    = height / 2;
        initialized_ = true;
    }
    clamp();
}
void Viewport::clamp()
{
    if (!source_.width || !source_.height)
        return;
    const double width  = orientation_ >= 5 ? source_.height : source_.width,
                 height = orientation_ >= 5 ? source_.width : source_.height;
    const double half_w = std::max(0.0, static_cast<double>(max_.x - min_.x)) * density_ / (2 * scale_);
    const double half_h = std::max(0.0, static_cast<double>(max_.y - min_.y)) * density_ / (2 * scale_);
    center_x_           = width > 2 * half_w ? std::clamp(center_x_, half_w, width - half_w) : width / 2;
    center_y_           = height > 2 * half_h ? std::clamp(center_y_, half_h, height - half_h) : height / 2;
}
void Viewport::pan(double x, double y)
{
    if (fit_)
        return;
    center_x_ -= x * density_ / scale_;
    center_y_ -= y * density_ / scale_;
    clamp();
}
void Viewport::zoom(double wheel, render::Point anchor)
{
    if (!source_.width || !source_.height)
        return;
    const double cx = (min_.x + max_.x) / 2, cy = (min_.y + max_.y) / 2;
    const double x = center_x_ + (anchor.x - cx) * density_ / scale_, y = center_y_ + (anchor.y - cy) * density_ / scale_;
    zoom_     = std::clamp(scale_ * std::pow(1.25, wheel), fit_scale_, 32.0);
    fit_      = false;
    scale_    = zoom_;
    center_x_ = x - (anchor.x - cx) * density_ / scale_;
    center_y_ = y - (anchor.y - cy) * density_ / scale_;
    clamp();
}
std::array<double, 2> Viewport::orient(double x, double y) const
{
    const double w = source_.width, h = source_.height;
    switch (orientation_)
    {
    case 2:
        return {w - x, y};
    case 3:
        return {w - x, h - y};
    case 4:
        return {x, h - y};
    case 5:
        return {y, x};
    case 6:
        return {h - y, x};
    case 7:
        return {h - y, w - x};
    case 8:
        return {y, w - x};
    default:
        return {x, y};
    }
}
std::array<double, 2> Viewport::unorient(double x, double y) const
{
    const double w = source_.width, h = source_.height;
    switch (orientation_)
    {
    case 2:
        return {w - x, y};
    case 3:
        return {w - x, h - y};
    case 4:
        return {x, h - y};
    case 5:
        return {y, x};
    case 6:
        return {y, h - x};
    case 7:
        return {w - y, h - x};
    case 8:
        return {w - y, x};
    default:
        return {x, y};
    }
}
RegionTarget Viewport::target(std::uint32_t edge) const
{
    if (!source_.width || !source_.height || !edge)
        return {};
    const double resolution = std::min(1.0, std::pow(2.0, std::ceil(std::log2(scale_))));
    const lv_size_u32 canvas{std::max(1u, static_cast<std::uint32_t>(source_.width * resolution)),
                             std::max(1u, static_cast<std::uint32_t>(source_.height * resolution))};
    const double hw = static_cast<double>(max_.x - min_.x) * density_ / (2 * scale_),
                 hh = static_cast<double>(max_.y - min_.y) * density_ / (2 * scale_);
    auto a = unorient(center_x_ - hw, center_y_ - hh), b = unorient(center_x_ + hw, center_y_ + hh);
    const double x0  = std::clamp(std::min(a[0], b[0]) * canvas.width / source_.width, 0.0, static_cast<double>(canvas.width));
    const double y0  = std::clamp(std::min(a[1], b[1]) * canvas.height / source_.height, 0.0, static_cast<double>(canvas.height));
    const double x1  = std::clamp(std::max(a[0], b[0]) * canvas.width / source_.width, 0.0, static_cast<double>(canvas.width));
    const double y1  = std::clamp(std::max(a[1], b[1]) * canvas.height / source_.height, 0.0, static_cast<double>(canvas.height));
    const auto left  = static_cast<std::uint32_t>(std::floor(std::max(0.0, x0 - 1) / edge) * edge);
    const auto top   = static_cast<std::uint32_t>(std::floor(std::max(0.0, y0 - 1) / edge) * edge);
    const auto right = static_cast<std::uint32_t>(std::min(static_cast<double>(canvas.width), std::ceil((x1 + 1) / edge) * edge));
    const auto bottom =
        static_cast<std::uint32_t>(std::min(static_cast<double>(canvas.height), std::ceil((y1 + 1) / edge) * edge));
    return {canvas, {left, top, right - left, bottom - top}};
}
std::array<render::Point, 4> Viewport::quad(lv_rect_u32 rect, lv_size_u32 canvas) const
{
    const std::array<std::array<double, 2>, 4> corners{
        {{static_cast<double>(rect.x), static_cast<double>(rect.y)},
         {static_cast<double>(rect.x) + rect.width, static_cast<double>(rect.y)},
         {static_cast<double>(rect.x) + rect.width, static_cast<double>(rect.y) + rect.height},
         {static_cast<double>(rect.x), static_cast<double>(rect.y) + rect.height}}};
    std::array<render::Point, 4> result{};
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto p = orient(corners[i][0] * source_.width / canvas.width, corners[i][1] * source_.height / canvas.height);
        // Subtract the camera in double precision BEFORE converting to GPU floats.
        result[i] = {static_cast<float>((min_.x + max_.x) / 2 + (p[0] - center_x_) * scale_ / density_),
                     static_cast<float>((min_.y + max_.y) / 2 + (p[1] - center_y_) * scale_ / density_)};
    }
    return result;
}
} // namespace liltview
