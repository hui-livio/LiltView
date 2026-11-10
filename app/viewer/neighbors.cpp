#include "viewer.hpp"
#include "platform/path.hpp"

#include <algorithm>
#include <spdlog/spdlog.h>

namespace liltview
{
lv_size_u32 Viewer::animation_bounds() const
{
    const auto bounds = renderer_.framebuffer_size();
    const auto edge   = std::max(1u, renderer_.capabilities().max_texture_edge);
    return {std::clamp(bounds.width, 1u, edge), std::clamp(bounds.height, 1u, edge)};
}

void Viewer::fallback_warm_animation()
{
    adopting_animation_ = false;
    prepared_animation_.reset();
    close_animation();
    discard_pending();
    playback_.reset();
    playback_.suspend(suspended_, Playback::Clock::now());
    try_animation_   = true;
    reset_animation_ = false;
    pending_path_    = selected_path_;
    loading_         = true;
    error_           = 0;
    submit_pending();
}

void Viewer::pump_neighbors()
{
    const auto &directory = navigation_->snapshot();
    const bool idle       = !request_ && !loading_ && !visible_pending_ && !failed_restore_ && has_visible_image();
    prefetch_->update(generation_, directory);
    prefetch_->pump(idle);
    std::vector<std::filesystem::path> animations;
    if (!directory.scanning && !directory.error && !prefetch_->pending())
        for (const auto &path : {directory.previous_path, directory.next_path})
            if (path)
            {
                const auto format = prefetch_->observed_format(*path);
                // A full NATIVE working set can fail before INFO even though a
                // bounded animation FIT is valid. OPEN remains the classifier.
                if (!format || *format == LV_FORMAT_GIF)
                    animations.push_back(*path);
            }
    const auto bounds = animation_bounds();
    animation_warmup_->update(generation_, animations, bounds);
    const bool warm_idle = idle && !prefetch_->pending();
    // First retire released terminals without admitting work, then compute the
    // additional global quota from both controllers' actual outstanding IDs.
    animation_warmup_->pump(warm_idle, 0);
    const auto static_active = prefetch_->stats().active;
    const auto occupied      = static_active + animation_warmup_->stats().outstanding_requests;
    const auto available     = occupied >= 2 ? 0 : 2 - occupied;
    animation_warmup_->pump(warm_idle, available);
    if (auto prepared = animation_warmup_->take_adoption())
        if (adopting_animation_)
        {
            prepared_animation_.emplace(std::move(*prepared));
            prepared_animation_bounds_ = bounds;
            adopting_animation_        = false;
        }
    if (animation_warmup_->take_adoption_failure() && adopting_animation_)
    {
        spdlog::debug("Warm animation validation failed; reopening {}", platform::path_bytes(selected_path_));
        fallback_warm_animation();
    }
}
} // namespace liltview
