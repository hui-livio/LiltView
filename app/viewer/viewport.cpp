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
bool Viewport::same_view(const Viewport &other) const
{
    return min_.x == other.min_.x && min_.y == other.min_.y && max_.x == other.max_.x && max_.y == other.max_.y &&
        source_.width == other.source_.width && source_.height == other.source_.height && orientation_ == other.orientation_ &&
        rotation_ == other.rotation_ && density_ == other.density_ && scale_ == other.scale_ && center_x_ == other.center_x_ &&
        center_y_ == other.center_y_;
}
bool Viewport::same_layout(const Viewport &other) const
{
    return min_.x == other.min_.x && min_.y == other.min_.y && max_.x == other.max_.x && max_.y == other.max_.y &&
        source_.width == other.source_.width && source_.height == other.source_.height && orientation_ == other.orientation_ &&
        rotation_ == other.rotation_ && density_ == other.density_;
}
Viewport Viewport::toward(const Viewport &other, double fraction) const
{
    if (fraction >= 1)
        return other;
    if (fraction <= 0 || !same_layout(other))
        return *this;
    auto result   = *this;
    result.scale_ = std::lerp(scale_, other.scale_, fraction);
    // Interpolate the affine transform, not camera coordinates independently:
    // a source point under the wheel anchor stays at that anchor throughout.
    result.center_x_    = std::lerp(center_x_ * scale_, other.center_x_ * other.scale_, fraction) / result.scale_;
    result.center_y_    = std::lerp(center_y_ * scale_, other.center_y_ * other.scale_, fraction) / result.scale_;
    result.zoom_        = result.scale_;
    result.fit_         = false;
    result.initialized_ = true;
    result.clamp();
    return result;
}
bool Viewport::visible(lv_rect_u32 rect) const
{
    if (!source_.width || !source_.height || max_.x <= min_.x || max_.y <= min_.y)
        return false;
    const auto points = quad(rect, source_);
    float left = points[0].x, right = points[0].x, top = points[0].y, bottom = points[0].y;
    for (const auto &point : points)
    {
        left   = std::min(left, point.x);
        right  = std::max(right, point.x);
        top    = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    const auto density = static_cast<float>(density_);
    // Use rendered pixel-center coverage, not the decode/filter halo. An
    // offscreen neighbor must not hold up the entire displayed camera.
    const float first_x = std::floor(min_.x * density) + 0.5f;
    const float first_y = std::floor(min_.y * density) + 0.5f;
    const float last_x  = std::ceil(max_.x * density) - 0.5f;
    const float last_y  = std::ceil(max_.y * density) - 0.5f;
    return right * density >= first_x && left * density <= last_x && bottom * density >= first_y && top * density <= last_y;
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
lv_orientation Viewport::display_orientation() const
{
    // Compose screen-space quarter turns with all eight EXIF orientations,
    // including mirrored sources. Pixel and UV storage remain unchanged.
    static constexpr lv_orientation orientations[4][8]{
        {1, 2, 3, 4, 5, 6, 7, 8},
        {6, 7, 8, 5, 2, 3, 4, 1},
        {3, 4, 1, 2, 7, 8, 5, 6},
        {8, 5, 6, 7, 4, 1, 2, 3},
    };
    return orientations[rotation_][orientation_ >= 1 && orientation_ <= 8 ? orientation_ - 1 : 0];
}
void Viewport::rotate(int quarter_turns)
{
    const auto turns = static_cast<unsigned>((quarter_turns % 4 + 4) % 4);
    if (!turns || !source_.width || !source_.height)
        return;
    const double width  = display_orientation() >= 5 ? source_.height : source_.width;
    const double height = display_orientation() >= 5 ? source_.width : source_.height;
    const double x = center_x_, y = center_y_;
    switch (turns)
    {
    case 1:
        center_x_ = height - y;
        center_y_ = x;
        break;
    case 2:
        center_x_ = width - x;
        center_y_ = height - y;
        break;
    case 3:
        center_x_ = y;
        center_y_ = width - x;
        break;
    }
    rotation_ = (rotation_ + turns) % 4;
    // FIT recomputes the scale for swapped bounds; manual zoom preserves its
    // physical scale and source-space center, subject to the usual edge clamp.
    update(min_, max_, static_cast<float>(density_), source_, orientation_);
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
    const double width  = display_orientation() >= 5 ? source.height : source.width,
                 height = display_orientation() >= 5 ? source.width : source.height;
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
    const double width  = display_orientation() >= 5 ? source_.height : source_.width,
                 height = display_orientation() >= 5 ? source_.width : source_.height;
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
void Viewport::zoom(double wheel)
{
    zoom(wheel, {(min_.x + max_.x) * 0.5f, (min_.y + max_.y) * 0.5f});
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
    switch (display_orientation())
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
    switch (display_orientation())
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
RegionTarget Viewport::target(std::uint32_t edge, bool native) const
{
    if (!source_.width || !source_.height || !edge)
        return {};
    const double resolution = native ? 1.0 : std::min(1.0, std::pow(2.0, std::ceil(std::log2(scale_))));
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
    const double cx = (static_cast<double>(min_.x) + max_.x) / 2;
    const double cy = (static_cast<double>(min_.y) + max_.y) / 2;
    double snap_x = 0, snap_y = 0;
    if (scale_ >= 1 && std::abs(scale_ - std::round(scale_)) < 1e-9)
    {
        // Snap the shared image origin, NOT each tile independently. At 100%
        // one source pixel must cover exactly one physical framebuffer pixel,
        // including odd canvas sizes, fractional UI positions and HiDPI.
        const double origin_x = cx * density_ - center_x_ * scale_;
        const double origin_y = cy * density_ - center_y_ * scale_;
        snap_x                = std::round(origin_x) - origin_x;
        snap_y                = std::round(origin_y) - origin_y;
    }
    std::array<render::Point, 4> result{};
    for (unsigned i = 0; i < 4; ++i)
    {
        const auto p = orient(corners[i][0] * source_.width / canvas.width, corners[i][1] * source_.height / canvas.height);
        // Subtract the camera in double precision BEFORE converting to GPU floats.
        result[i] = {static_cast<float>(cx + ((p[0] - center_x_) * scale_ + snap_x) / density_),
                     static_cast<float>(cy + ((p[1] - center_y_) * scale_ + snap_y) / density_)};
    }
    return result;
}
} // namespace liltview
