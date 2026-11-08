#include <algorithm>
#include <spdlog/spdlog.h>
#include "viewer.hpp"

namespace liltview
{
void Viewer::close_animation()
{
    if (animation_id_)
        lv_core_close_animation(core_, animation_id_);
    animation_id_ = 0;
}
void Viewer::fail(lv_error_code error)
{
    ++generation_; // Ignore the cancelled job's remaining metadata/terminal.
    close_animation();
    playback_.reset();
    discard_pending();
    pending_path_.clear();
    loading_ = false;
    error_   = error;
}
void Viewer::prepare_image(bool animation)
{
    const render::ImageDesc desc{{pending_info_.canvas.width, pending_info_.canvas.height}, false};
    if (animation && pending_)
        return; // Reuse the prior display as the next back texture.
    const auto caps  = renderer_.capabilities();
    const auto bytes = render::image_storage_bytes(desc);
    if (bytes > caps.image_budget_bytes - caps.image_bytes)
    {
        if (animation && playback_.started())
            throw render::Error(render::ErrorCode::memory_budget, "Animation requires two complete image textures");
        renderer_.destroy_image(display_);
        display_ = {};
    }
    pending_ = renderer_.create_image(desc);
}
void Viewer::upload(const lv_region_info &region)
{
    const auto &pixels = *lv_pixels_get_view(region.pixels);
    renderer_.update_image(pending_, {region.rect.x, region.rect.y, region.rect.width, region.rect.height},
                           {pixels.data, pixels.byte_count, pixels.stride_bytes, {pixels.size.width, pixels.size.height}});
    pending_content_ = true;
}

void Viewer::consume_animation(const lv_event_view &view)
{
    if (view.type == LV_EVENT_ANIMATION_INFO)
    {
        animation_info_ = view.as.animation;
        pending_info_   = animation_info_.image;
    }
    else if (view.type == LV_EVENT_ANIMATION_FRAME_INFO)
    {
        incoming_frame_ = view.as.frame;
        if (incoming_frame_.key.animation_id != animation_id_ || incoming_frame_.key.epoch != animation_info_.epoch)
            throw render::Error(render::ErrorCode::invalid_argument, "Unexpected animation frame identity");
        pending_info_    = animation_info_.image;
        pending_content_ = false;
        prepare_image(true);
    }
    else if (view.type == LV_EVENT_ANIMATION_REGION_READY)
    {
        const auto &frame = view.as.frame_region;
        if (frame.key.animation_id != incoming_frame_.key.animation_id || frame.key.epoch != incoming_frame_.key.epoch ||
            frame.key.frame_index != incoming_frame_.key.frame_index)
            throw render::Error(render::ErrorCode::invalid_argument, "Animation tile identity mismatch");
        upload(frame.region);
    }
    else if (view.type == LV_EVENT_ANIMATION_END)
    {
        animation_info_ = view.as.animation_end.final_info;
        ++completed_plays_;
        if (animation_info_.total_plays == 0 || completed_plays_ < animation_info_.total_plays)
            reset_animation_ = true;
        else
        {
            close_animation();
            renderer_.destroy_image(pending_);
            pending_ = {};
        }
    }
    else if (view.type == LV_EVENT_COMPLETE)
    {
        if (work_ == Work::animation_open)
            pending_path_.clear();
        else if (work_ == Work::animation_next && view.as.completion.final_tile_count && pending_ && pending_content_)
            playback_.candidate(incoming_frame_, Playback::Clock::now());
        // An END request has no tiles, and therefore never overwrites the display.
    }
    else if (view.type == LV_EVENT_ERROR)
    {
        if (work_ == Work::animation_open && view.as.error.code == LV_ERROR_FORMAT_UNSUPPORTED)
        {
            close_animation();
            try_animation_ = false; // Probe by content, then reuse ordinary static path.
        }
        else
            fail(view.as.error.code);
    }
    else if (view.type == LV_EVENT_CANCELLED)
        fail(0);
}

void Viewer::advance_animation()
{
    const auto now = Playback::Clock::now();
    if (playback_.due(now))
    {
        if (!playback_.started())
        {
            renderer_.destroy_image(display_);
            display_ = {};
            lv_event_release(preview_anchor_);
            preview_anchor_ = nullptr;
        }
        std::swap(display_, pending_);
        info_ = pending_info_;
        playback_.presented(now);
        loading_ = false;
        error_   = 0;
        spdlog::debug("Animation frame displayed: {}, epoch={}, frame={}", filename_, incoming_frame_.key.epoch,
                      playback_.frame_index());
        if (playback_.frame_index() == 0)
            spdlog::info("Image displayed: {}, {}x{}, animation", filename_, info_.source_size.width, info_.source_size.height);
    }
    if (!animation_id_ || request_ || playback_.ready() || suspended_ || closing_)
        return;
    lv_animation_request_desc next;
    lv_animation_request_desc_init(&next);
    next.animation_id = animation_id_;
    next.user_tag     = generation_;
    next.action       = reset_animation_ ? LV_ANIMATION_RESET : LV_ANIMATION_NEXT;
    const auto status = lv_core_submit_animation(core_, &next, &request_);
    if (status == LV_BUSY)
        return;
    if (status != LV_OK)
    {
        fail(LV_ERROR_INTERNAL);
        return;
    }
    submitted_generation_ = generation_;
    work_                 = reset_animation_ ? Work::animation_reset : Work::animation_next;
    reset_animation_      = false;
}

void Viewer::suspend_animation(bool suspended)
{
    suspended_ = suspended;
    playback_.suspend(suspended, Playback::Clock::now());
}
int Viewer::wait_timeout_ms(int maximum) const
{
    return playback_.wait_ms(Playback::Clock::now(), maximum);
}
} // namespace liltview
