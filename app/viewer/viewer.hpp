#ifndef LILTVIEW_VIEWER_HPP
#define LILTVIEW_VIEWER_HPP
#include <chrono>
#include <filesystem>
#include <liltview/core.h>
#include <optional>
#include <string>
#include "animation_warmup.hpp"
#include "image_details.hpp"
#include "navigation.hpp"
#include "playback.hpp"
#include "prefetch.hpp"
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
    void navigate(int direction);
    const DirectoryNavigation::Snapshot &navigation() const
    {
        return navigation_->snapshot();
    }
    void pump();
    using StaticUploadMode = TilePreparation::Mode;
    void set_static_upload_mode(StaticUploadMode mode) { if (loading_) throw std::logic_error("Cannot change upload mode during a load"); static_mode_ = mode; }
    void set_static_cache_compression(bool enabled) { if (loading_) throw std::logic_error("Cannot change cache compression during a load"); static_cache_compression_ = enabled; }
    void prepare_loading_view() { if (loading_) set_viewport(canvas_min_, canvas_max_, canvas_density_); }
    struct StaticLoadStats
    {
        std::uint64_t generation = 0, tiles = 0, uploaded_bytes = 0;
        double core_ms = 0, first_region_ms = -1, decode_done_ms = -1, prepare_done_ms = -1;
        double first_upload_ms = -1, last_upload_ms = -1, ready_ms = -1, submitted_ms = -1, presented_ms = -1;
        double loading_presented_ms = -1, upload_cpu_ms = 0, max_upload_slice_ms = 0, max_pump_ms = 0;
        TilePreparation::Stats preparation;
        bool batch = false;
    };
    const StaticLoadStats &static_load_stats() const { return static_stats_; }
    void note_frame_submitted();
    void note_frame_presented();
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
    void draw(render::Point min, render::Point max, float density, bool interactive = true);
    void fit();
    void actual_size();
    void rotate(int quarter_turns);
    void set_viewport(render::Point min, render::Point max, float density = 1);
    void pan(double x, double y);
    void zoom(double wheel, render::Point anchor);
    void zoom(double wheel); // Keyboard/menu zoom is anchored at the canvas center.
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
    std::optional<ImageDetails> image_details() const;
    bool prefetch_pending() const
    {
        return (prefetch_ && prefetch_->pending()) || (animation_warmup_ && animation_warmup_->pending());
    }
    PrefetchController::Stats prefetch_stats() const
    {
        return prefetch_ ? prefetch_->stats() : PrefetchController::Stats{};
    }
    // Last successful static DISPLAY only; opening also probes for GIF by content.
    std::uint64_t last_source_bytes_read() const
    {
        return last_source_bytes_read_;
    }
    std::uint64_t content_cache_bytes() const;
    AnimationWarmup::Stats animation_warmup_stats() const
    {
        return animation_warmup_ ? animation_warmup_->stats() : AnimationWarmup::Stats{};
    }
    std::uint64_t warmed_first_frames_presented() const
    {
        return warmed_first_frames_presented_;
    }
    std::uint64_t animation_frames_presented() const
    {
        return animation_frames_presented_;
    }
    bool settled() const
    {
        return !loading_ && !visible_pending_ && pending_path_.empty() && !retired_preparation_ && retired_scene_.empty() &&
            (playback_.started() || request_ == 0);
    }
    // Foreground identity and camera mode, never inferred from numerical zoom.
    bool image_loaded() const
    {
        return !closing_ && !loading_ && pending_path_.empty() && (playback_.started() || request_ == 0) && !failed() &&
            has_visible_image() && selected_path_ == displayed_path_;
    }
    bool interaction_ready() const
    {
        return image_loaded() && !visible_pending_ && !failed_restore_;
    }
    bool fitting() const
    {
        return viewport_.fitting();
    }
    std::uint64_t image_generation() const
    {
        return generation_;
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
    bool join_shutdown(std::uint32_t timeout_ms = 0);

private:
    void open_selected(const std::filesystem::path &);
    void submit_pending();
    void consume(const lv_event_view &);
    void discard_pending();
    void accept_completed();
    void advance_static();
    void commit_static_if_ready();
    double load_elapsed_ms() const;
    StaticUploadMode static_mode_ = StaticUploadMode::pipeline;
    bool static_cache_compression_ = true;
    StaticLoadStats static_stats_;
    std::chrono::steady_clock::time_point load_started_;
    std::unique_ptr<TilePreparation> preparation_, retired_preparation_;
    bool decode_complete_ = false, cpu_complete_ = false;
    Tiles::UploadBudget static_budget_;
    bool first_commit_this_frame_ = false;
    std::uint64_t expected_tiles_ = 0;
    bool loading_frame_submitted_ = false, static_frame_submitted_ = false;
    render::Point canvas_min_{}, canvas_max_{800, 600};
    float canvas_density_ = 1;
    void prepare_image(bool animation);
    void upload(const lv_region_info &);
    void consume_animation(const lv_event_view &);
    void advance_animation();
    void present_animation_frame(Playback::Time);
    void install_prepared_animation();
    void fallback_warm_animation();
    lv_size_u32 animation_bounds() const;
    void pump_neighbors();
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
    Tiles scene_, pending_scene_, retired_scene_;
    bool visible_pending_     = false;
    lv_event *preview_anchor_ = nullptr, *pending_anchor_ = nullptr;

    lv_core *core_ = nullptr;
    std::unique_ptr<DirectoryNavigation> navigation_;
    std::unique_ptr<PrefetchController> prefetch_;
    std::unique_ptr<AnimationWarmup> animation_warmup_;
    std::optional<PreparedAnimation> prepared_animation_;
    lv_size_u32 prepared_animation_bounds_{};
    bool adopting_animation_                     = false;
    std::uint64_t warmed_first_frames_presented_ = 0, animation_frames_presented_ = 0;
    std::uint64_t last_source_bytes_read_ = 0;
    lv_request_id request_                = 0;
    std::uint64_t generation_ = 0, submitted_generation_ = 0;
    std::filesystem::path pending_path_;
    std::filesystem::path selected_path_, displayed_path_;
    std::string filename_;
    render::Image display_, pending_;
    lv_image_info info_{}, pending_info_{};
    std::optional<lv_pixel_format> pixel_format_, pending_pixel_format_;
    lv_color_space color_space_ = LV_COLOR_UNKNOWN, pending_color_space_ = LV_COLOR_UNKNOWN;
    std::optional<AnimationDetails> displayed_animation_;
    bool loading_         = false;
    bool pending_content_ = false;
    bool closing_         = false;
    lv_error_code error_  = 0;
};
} // namespace liltview
#endif
