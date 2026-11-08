#ifndef LILTVIEW_VIEWER_HPP
#define LILTVIEW_VIEWER_HPP
#include <chrono>
#include <filesystem>
#include <liltview/core.h>
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
    explicit Viewer(render::Renderer &);
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
    bool detail_ready() const
    {
        return !detail_.empty() && completed_detail_ == wanted_detail_ && !detail_failed_;
    }
    bool loading() const
    {
        return loading_;
    }
    bool has_image() const
    {
        return static_cast<bool>(display_);
    }
    bool can_actual_size() const
    {
        return display_ &&
            ((info_.capabilities & LV_CAP_REGION_DECODE) ||
             (info_.canvas.width == info_.source_size.width && info_.canvas.height == info_.source_size.height));
    }
    bool has_visible_image() const;
    bool settled() const
    {
        return !loading_ && pending_path_.empty() && (playback_.started() || request_ == 0) &&
            (!wanted_detail_.rect.width || viewport_.fitting() || detail_failed_ || completed_detail_ == wanted_detail_);
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
    void queue_details();
    void submit_details();
    void consume_details(const lv_event_view &);
    void clear_details();

    void fail(lv_error_code);
    enum class Work
    {
        static_image,
        static_detail,
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
    Viewport viewport_;
    Tiles detail_, pending_detail_;
    RegionTarget wanted_detail_{}, submitted_detail_{}, completed_detail_{};
    bool detail_failed_       = false;
    lv_event *preview_anchor_ = nullptr, *pending_anchor_ = nullptr;
    std::filesystem::path image_path_, opening_path_;

    lv_core *core_            = nullptr;
    lv_request_id request_    = 0;
    std::uint64_t generation_ = 0, submitted_generation_ = 0;
    std::chrono::steady_clock::time_point submitted_at_{};
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
