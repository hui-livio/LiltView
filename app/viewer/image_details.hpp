#ifndef LILTVIEW_IMAGE_DETAILS_HPP
#define LILTVIEW_IMAGE_DETAILS_HPP

#include <filesystem>
#include <liltview/core.h>
#include <optional>

namespace liltview
{
struct AnimationDetails
{
    lv_animation_info info{};
    lv_animation_frame_info frame{};
};

// An owned, UI-thread snapshot of the image actually displayed, never a Core view.
struct ImageDetails
{
    std::filesystem::path path;
    lv_image_info image{};
    std::optional<lv_pixel_format> pixel_format;
    lv_color_space color_space = LV_COLOR_UNKNOWN;
    double scale               = 1; // Source pixel -> physical framebuffer pixel; already accounts for density.
    bool partial = false, loading_replacement = false;
    std::optional<AnimationDetails> animation;
};

inline lv_size_u32 oriented_image_size(lv_size_u32 size, lv_orientation orientation)
{
    return orientation >= LV_ORIENTATION_TRANSPOSE && orientation <= LV_ORIENTATION_ROTATE_270_CW ?
        lv_size_u32{size.height, size.width} :
        size;
}
} // namespace liltview

#endif
