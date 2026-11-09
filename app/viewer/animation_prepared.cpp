#include "viewer.hpp"

#include <spdlog/spdlog.h>
#include <utility>

namespace liltview
{
void Viewer::install_prepared_animation()
{
    if (!prepared_animation_ || suspended_)
        return;
    const auto bounds = animation_bounds();
    if (bounds.width != prepared_animation_bounds_.width || bounds.height != prepared_animation_bounds_.height)
    {
        fallback_warm_animation();
        return;
    }
    auto prepared = std::move(*prepared_animation_);
    prepared_animation_.reset();
    render::Image first{}, second{};
    try
    {
        const auto &image = prepared.info.image;
        const bool native = image.canvas.width == image.source_size.width && image.canvas.height == image.source_size.height;
        const render::ImageDesc desc{{image.canvas.width, image.canvas.height},
                                     false,
                                     native ? render::ImageDesc::Magnification::nearest :
                                              render::ImageDesc::Magnification::linear};
        const auto upload_frame = [&](render::Image &target, const PreparedAnimationFrame &frame)
        {
            target = renderer_.create_image(desc);
            for (const auto &tile : frame.tiles)
            {
                const auto &pixels = *lv_pixels_get_view(tile.region.pixels);
                renderer_.update_image(
                    target, {tile.region.rect.x, tile.region.rect.y, tile.region.rect.width, tile.region.rect.height},
                    {pixels.data, pixels.byte_count, pixels.stride_bytes, {pixels.size.width, pixels.size.height}});
            }
        };
        // Keep all fallible driver work before the visible commit and frame-0
        // clock. A slow second upload must not consume the first frame's time.
        upload_frame(first, prepared.first);
        if (prepared.second)
            upload_frame(second, *prepared.second);
        const auto &first_pixels = *lv_pixels_get_view(prepared.first.tiles.front().region.pixels);
        pending_info_            = image;
        pending_pixel_format_    = first_pixels.format;
        pending_color_space_     = first_pixels.color_space;
        pending_content_         = true;
        pending_                 = std::exchange(first, {});
        animation_id_            = prepared.release_session();
        animation_info_          = prepared.info;
        incoming_frame_          = prepared.first.info;
        completed_plays_         = 0;
        reset_animation_         = false;
        work_                    = Work::animation_next;
        const auto now           = Playback::Clock::now();
        playback_.candidate(incoming_frame_, now);
        present_animation_frame(now);
        ++warmed_first_frames_presented_;
        spdlog::debug("Warm animation adopted: {}, epoch={}, frame=0", filename_, incoming_frame_.key.epoch);
        if (prepared.second)
        {
            const auto &pixels    = *lv_pixels_get_view(prepared.second->tiles.front().region.pixels);
            pending_              = std::exchange(second, {});
            pending_pixel_format_ = pixels.format;
            pending_color_space_  = pixels.color_space;
            incoming_frame_       = prepared.second->info;
            playback_.candidate(incoming_frame_, now);
        }
        else if (prepared.ended)
        {
            completed_plays_ = 1;
            if (animation_info_.total_plays == 0 || completed_plays_ < animation_info_.total_plays)
                reset_animation_ = true;
            else
                close_animation();
        }
    } catch (const std::exception &error)
    {
        renderer_.destroy_image(first);
        renderer_.destroy_image(second);
        spdlog::debug("Warm animation upload fallback: {}", error.what());
        fallback_warm_animation();
    }
}
} // namespace liltview
