#ifndef LILTVIEW_VIEWER_HPP
#define LILTVIEW_VIEWER_HPP
#include <chrono>
#include <filesystem>
#include <liltview/core.h>
#include <optional>
#include <string>
#include "playback.hpp"
#include "renderer/renderer.hpp"
#include "tiles.hpp"

namespace liltview
{
// UI-thread owner of the C engine, current request and displayed GPU image.
class Viewer
{
public:
    enum class CameraPolicy
    {
        covered_motion,
        atomic
    };
    explicit Viewer(render::Renderer &, CameraPolicy = CameraPolicy::covered_motion);
    ~Viewer();
    Viewer(const Viewer &)            = delete;
    Viewer &operator=(const Viewer &) = delete;
    void open(const std::filesystem::path &);
    void pump();
    void suspend_animation(bool suspended);
    int wait_timeout_ms(int maximum) const;
    bool animating() const
    {
        return animation_id_ != 0 && playback_.started();
    }
    std::uint64_t displayed_frame_index() const
    {
        return playback_.frame_index();
    }
    void draw(render::Point min, render::Point max, float density);
    void fit();
    void actual_size();
    void set_viewport(render::Point min, render::Point max, float density = 1);
    void pan(double x, double y);
    void zoom(double wheel, render::Point anchor);
    std::size_t loaded_tiles() const
    {
        return loading_ ? pending_scene_.loaded_tiles() : scene_.loaded_tiles();
    }
    const Viewport &presented_view() const
    {
        return presented_viewport_;
    }
    Tiles::Stats cache_stats() const
    {
        return loading_ ? pending_scene_.stats() : scene_.stats();
    }
    bool detail_ready() const
    {
        return !loading_ && !scene_.empty() && !visible_pending_ && !failed_restore_;
    }
    bool detail_loading() const
    {
        return visible_pending_;
    }
    bool native_loading() const
    {
        return loading_ && work_ == Work::static_image;
    }
    bool loading() const
    {
        return loading_;
    }
    bool has_image() const
    {
        return display_ || !scene_.empty();
    }
    bool can_actual_size() const
    {
        return !scene_.empty() || (loading_ && !pending_scene_.empty()) ||
            (display_ && info_.canvas.width == info_.source_size.width && info_.canvas.height == info_.source_size.height);
    }
    bool has_visible_image() const;
    bool settled() const
    {
        return !loading_ && !visible_pending_ && pending_path_.empty() && (playback_.started() || request_ == 0);
    }
    bool failed() const
    {
        return error_ != 0;
    }
    lv_error_code error() const
    {
        return error_;
    }
    const std::string &filename() const
    {
        return filename_;
    }
    lv_size_u32 source_size() const
    {
        if (loading_ || failed())
            return pending_info_.source_size;
        return info_.source_size;
    }
    void shutdown();
    void begin_shutdown();
    bool join_shutdown();

private:
    void submit_pending();
    void consume(const lv_event_view &);
    void discard_pending();
    void accept_completed();
    void prepare_image(bool animation);
    void upload(const lv_region_info &);
    void consume_animation(const lv_event_view &);
    void advance_animation();
    void close_animation();
    void retry_restore();

    void fail(lv_error_code);
    enum class Work
    {
        static_image,
        animation_open,
        animation_next,
        animation_reset
    };
    Work work_          = Work::animation_open;
    bool try_animation_ = true, reset_animation_ = false, suspended_ = false;
    lv_animation_id animation_id_ = 0;
    lv_animation_info animation_info_{};
    lv_animation_frame_info incoming_frame_{};
    std::uint64_t completed_plays_ = 0;
    Playback playback_;

    render::Renderer &renderer_;
    CameraPolicy camera_policy_;
    Viewport viewport_, presented_viewport_;
    std::optional<Viewport> failed_restore_;
    lv_request_id scene_id_ = 0, pending_scene_id_ = 0, presented_scene_id_ = 0;
    Tiles scene_, pending_scene_;
    bool visible_pending_     = false;
    lv_event *preview_anchor_ = nullptr, *pending_anchor_ = nullptr;

    lv_core *core_            = nullptr;
    lv_request_id request_    = 0;
    std::uint64_t generation_ = 0, submitted_generation_ = 0;
    std::filesystem::path pending_path_;
    std::string filename_;
    render::Image display_, pending_;
    lv_image_info info_{}, pending_info_{};
    bool loading_         = false;
    bool pending_content_ = false;
    bool closing_         = false;
    lv_error_code error_  = 0;
};
} // namespace liltview
#endif
