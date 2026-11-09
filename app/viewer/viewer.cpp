#include "viewer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <system_error>

namespace liltview
{
namespace
{
void LV_CALL wake_core(void *)
{
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}

std::vector<std::string> image_extensions(lv_core *core)
{
    struct FormatExtensions
    {
        lv_format format;
        std::vector<std::string> suffixes;
    };
    const FormatExtensions formats[]{{LV_FORMAT_JPEG, {".jpg", ".jpeg"}}, {LV_FORMAT_PNG, {".png"}}, {LV_FORMAT_SVG, {".svg"}},
                                     {LV_FORMAT_WEBP, {".webp"}},         {LV_FORMAT_BMP, {".bmp"}}, {LV_FORMAT_GIF, {".gif"}},
                                     {LV_FORMAT_TIFF, {".tif", ".tiff"}}};
    std::vector<std::string> extensions;
    for (const auto &format : formats)
    {
        lv_format_support support{};
        if (lv_core_query_format(core, format.format, &support) == LV_OK && support.available)
            extensions.insert(extensions.end(), format.suffixes.begin(), format.suffixes.end());
    }
    return extensions;
}
} // namespace

Viewer::Viewer(render::Renderer &renderer, CameraPolicy policy) :
    renderer_(renderer),
    camera_policy_(policy),
    scene_(renderer, wake_core),
    pending_scene_(renderer, wake_core)
{
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes  = 384ULL * 1024 * 1024;
    config.queue_pixel_bytes = 4ULL * 1024 * 1024;
    config.prefetch_workers  = 0;
    // 256px native tiles caused hundreds/thousands of GL images and draws at
    // intermediate zoom on real 12K–18K scenes. 512px keeps restore/draw work
    // bounded more usefully while preserving progressive row bands. Respect
    // smaller device limits below; the core API default remains unchanged.
    config.tile_edge    = 512;
    const auto max_edge = renderer_.capabilities().max_texture_edge;
    while (config.tile_edge > max_edge && config.tile_edge > 16)
        config.tile_edge /= 2;
    config.wakeup     = wake_core;
    const auto result = lv_core_create(&config, &core_);
    if (result != LV_OK)
        throw std::runtime_error("Cannot initialize image core: " + std::to_string(result));
    try
    {
        navigation_ = std::make_unique<DirectoryNavigation>(image_extensions(core_), [] { wake_core(nullptr); });
    } catch (...)
    {
        lv_core_begin_shutdown(core_);
        while (lv_core_join(core_, 10) == LV_TIMEOUT)
        {
        }
        lv_core_destroy(&core_);
        throw;
    }
}
Viewer::~Viewer()
{
    shutdown();
}

void Viewer::begin_shutdown()
{
    if (!core_ || closing_)
        return;
    closing_ = true;
    lv_core_begin_shutdown(core_);
    navigation_->shutdown();
}
bool Viewer::join_shutdown(std::uint32_t timeout_ms)
{
    return !core_ || lv_core_join(core_, timeout_ms) == LV_OK;
}

void Viewer::shutdown()
{
    if (!core_)
        return;
    begin_shutdown();
    // Decoder checks cancellation at input/row boundaries; keep SDL alive until callbacks stop.
    while (lv_core_join(core_, 10) == LV_TIMEOUT)
    {
    }
    discard_pending();
    scene_.clear();
    pending_scene_.clear();
    lv_event_release(preview_anchor_);
    preview_anchor_ = nullptr;
    renderer_.destroy_image(display_);
    display_ = {};
    if (lv_core_destroy(&core_) != LV_OK)
        std::terminate();
}

void Viewer::open(const std::filesystem::path &path)
{
    if (closing_)
        return;
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec)
    {
        error_ = LV_ERROR_IO;
        return;
    }
    // Preserve OS path resolution: folding "symlink/.." could open another file.
    navigation_->select(absolute);
    open_selected(absolute);
}

void Viewer::navigate(int direction)
{
    if (!closing_)
        if (auto path = navigation_->step(direction))
            open_selected(*path);
}

void Viewer::open_selected(const std::filesystem::path &absolute)
{
    ++generation_;
    close_animation();
    if (request_)
        lv_core_cancel(core_, request_);
    discard_pending();
    playback_.reset();
    playback_.suspend(suspended_, Playback::Clock::now());
    try_animation_   = true;
    reset_animation_ = false;
    completed_plays_ = 0;
    visible_pending_ = false;
    viewport_.reset();
    pending_path_ = absolute;
    filename_     = absolute.filename().string();
    error_        = 0;
    loading_      = true;
}

void Viewer::discard_pending()
{
    renderer_.destroy_image(pending_);
    pending_      = {};
    pending_info_ = {};
    lv_event_release(pending_anchor_);
    pending_anchor_  = nullptr;
    pending_content_ = false;
    pending_scene_.clear();
    pending_scene_id_ = 0;
}

void Viewer::submit_pending()
{
    if (pending_path_.empty() || request_)
        return;
    const auto path = pending_path_.native();
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path     = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.user_tag = generation_;
    // Only animations use a bounded immutable FIT canvas. Static images are
    // decoded exactly once at native resolution, independent of the viewport.
    if (try_animation_)
    {
        const auto bounds            = renderer_.framebuffer_size();
        const auto edge              = std::max(1u, renderer_.capabilities().max_texture_edge);
        request.target.as.fit_bounds = {std::clamp(bounds.width, 1u, edge), std::clamp(bounds.height, 1u, edge)};
    }
    else
        request.target.kind = LV_TARGET_NATIVE;
    lv_status status;
    if (try_animation_)
    {
        lv_animation_open_desc open;
        lv_animation_open_desc_init(&open);
        open.path     = request.path;
        open.target   = request.target;
        open.user_tag = request.user_tag;
        work_         = Work::animation_open;
        status        = lv_core_open_animation(core_, &open, &animation_id_, &request_);
    }
    else
    {
        work_  = Work::static_image;
        status = lv_core_submit(core_, &request, &request_);
    }
    if (status == LV_BUSY)
        return;
    if (!try_animation_)
        pending_path_.clear();
    submitted_generation_ = generation_;
    if (status != LV_OK)
    {
        pending_path_.clear();
        fail(LV_ERROR_INTERNAL);
    }
}

void Viewer::accept_completed()
{
    renderer_.destroy_image(display_);
    display_ = pending_;
    pending_ = {};
    scene_.swap(pending_scene_);
    pending_scene_.clear();
    scene_id_         = pending_scene_id_;
    pending_scene_id_ = 0;
    info_             = pending_info_;
    lv_event_release(preview_anchor_);
    preview_anchor_ = pending_anchor_;
    pending_anchor_ = nullptr;
    loading_        = false;
    error_          = 0;
    spdlog::info("Image displayed: {}, {}x{}, orientation={}", filename_, info_.source_size.width, info_.source_size.height,
                 info_.orientation);
}

void Viewer::consume(const lv_event_view &view)
{
    if (view.request_id != request_)
        return;
    const bool current = view.user_tag == generation_ && submitted_generation_ == generation_;
    if (current && work_ != Work::static_image)
    {
        consume_animation(view);
        return;
    }
    if (current && view.type == LV_EVENT_IMAGE_INFO)
    {
        pending_info_     = view.as.image;
        pending_scene_id_ = view.request_id;
        // Preserve the old source/cache on failure, but do not let optional old
        // GPU residency prevent admission of the replacement native scene.
        scene_.release_details();
        pending_scene_.begin(pending_info_);
    }
    if (current && view.type == LV_EVENT_REGION_READY)
    {
        pending_scene_.upload(view.as.region);
        pending_content_ = true;
    }
    if (view.type == LV_EVENT_COMPLETE || view.type == LV_EVENT_ERROR || view.type == LV_EVENT_CANCELLED)
    {
        if (current)
        {
            if (view.type == LV_EVENT_COMPLETE && !pending_scene_.empty())
                accept_completed();
            else
            {
                error_   = view.type == LV_EVENT_ERROR ? view.as.error.code : 0;
                loading_ = false;
                discard_pending();
            }
        }
        request_ = 0;
    }
}

void Viewer::pump()
{
    if (closing_)
        return;
    navigation_->pump();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    lv_event *raw       = nullptr;
    bool consumed       = false;
    while (std::chrono::steady_clock::now() < deadline && lv_core_poll_event(core_, &raw) == LV_OK)
    {
        consumed = true;
        std::unique_ptr<lv_event, decltype(&lv_event_release)> event(raw, lv_event_release);
        const auto &view    = *lv_event_get_view(event.get());
        const bool terminal = view.request_id == request_ &&
            (view.type == LV_EVENT_COMPLETE || view.type == LV_EVENT_ERROR || view.type == LV_EVENT_CANCELLED);
        try
        {
            const bool anchor = view.type == LV_EVENT_IMAGE_INFO && work_ == Work::static_image && view.user_tag == generation_ &&
                view.request_id == request_;
            consume(*lv_event_get_view(event.get()));
            if (anchor && view.user_tag == generation_)
            {
                lv_event_release(pending_anchor_);
                pending_anchor_ = event.release();
            }
        } catch (const std::exception &error)
        {
            spdlog::error("Image upload: {}", error.what());
            if (request_)
                lv_core_cancel(core_, request_);
            const auto *io = dynamic_cast<const std::system_error *>(&error);
            fail(io ? LV_ERROR_IO : LV_ERROR_MEMORY_BUDGET);
        }
        // Even an exception while processing a terminal must allow the next request.
        if (terminal)
            request_ = 0;
    }
    if (consumed && std::chrono::steady_clock::now() >= deadline)
        wake_core(nullptr); // Continue draining next frame even without another queue edge.
    submit_pending();
    advance_animation();
}

void Viewer::draw(render::Point min, render::Point max, float density, bool interactive)
{
    // Align the canvas clipping rectangle with physical pixels too. Image
    // vertices were aligned already; fractional clip edges otherwise make
    // offscreen border tiles look necessary at HiDPI.
    density                = std::max(0.01f, density);
    min                    = {std::round(min.x * density) / density, std::round(min.y * density) / density};
    max                    = {std::round(max.x * density) / density, std::round(max.y * density) / density};
    const bool native      = work_ == Work::static_image;
    const bool reveal      = native && loading_ && !pending_scene_.empty();
    auto &scene            = reveal ? pending_scene_ : scene_;
    const auto &image_info = reveal ? pending_info_ : info_;
    const auto image       = display_;
    if (scene.empty() && !image)
        return;
    viewport_.update(min, max, density, image_info.source_size, image_info.orientation);
    if (interactive && !playback_.started() && ImGui::IsWindowHovered())
    {
        const auto &io = ImGui::GetIO();
        if (io.MousePos.x >= min.x && io.MousePos.x <= max.x && io.MousePos.y >= min.y && io.MousePos.y <= max.y)
        {
            if (io.MouseWheel)
                viewport_.zoom(io.MouseWheel, {io.MousePos.x, io.MousePos.y});
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                viewport_.pan(io.MouseDelta.x, io.MouseDelta.y);
        }
    }
    auto *list       = ImGui::GetWindowDrawList();
    visible_pending_ = false;
    if (!scene.empty() && !playback_.started())
    {
        const auto scene_id = reveal ? pending_scene_id_ : scene_id_;
        if (presented_scene_id_ != scene_id)
        {
            // A new source is allowed to appear row by row. Its identity remains
            // stable when the pending scene is promoted after COMPLETE.
            presented_scene_id_ = scene_id;
            presented_viewport_ = viewport_;
            failed_restore_.reset();
        }
        if (failed_restore_ && !failed_restore_->same_view(viewport_))
            retry_restore();
        const bool changing_view = !presented_viewport_.same_view(viewport_);
        if (!failed_restore_)
        {
            try
            {
                const bool ready = scene.prepare(viewport_, changing_view ? &presented_viewport_ : nullptr);
                visible_pending_ = !ready;
                // Streaming may extend the current image, but a new camera is
                // committed atomically, never a few restored tiles at a time.
                if (ready)
                {
                    scene.commit_restore();
                    presented_viewport_ = viewport_;
                }
                else if (!loading_ && changing_view && camera_policy_ == CameraPolicy::covered_motion &&
                         presented_viewport_.same_layout(viewport_))
                {
                    // Keep every presented view complete, but do not freeze the
                    // camera just because the final destination is still cold.
                    // Intermediate views use resident textures at sufficient
                    // resolution: no preview stretching and no uncovered tiles.
                    double low = 0, high = 1;
                    auto next = presented_viewport_;
                    for (unsigned step = 0; step < 8; ++step)
                    {
                        const double fraction = (low + high) / 2;
                        auto candidate        = presented_viewport_.toward(viewport_, fraction);
                        if (scene.covered(candidate))
                        {
                            low  = fraction;
                            next = candidate;
                        }
                        else
                            high = fraction;
                    }
                    if (low > 0 && !next.same_view(presented_viewport_))
                    {
                        scene.commit_restore();
                        presented_viewport_ = next;
                    }
                }
            } catch (const std::exception &error)
            {
                spdlog::error("Native texture cache: {}", error.what());
                failed_restore_ = viewport_;
                error_          = dynamic_cast<const std::system_error *>(&error) ? LV_ERROR_IO : LV_ERROR_MEMORY_BUDGET;
            }
        }
        if (!failed_restore_ && scene.upload_pending())
            wake_core(nullptr); // Worker wakes handle I/O; only ready GPU work schedules another frame.
    }
    list->PushClipRect({min.x, min.y}, {max.x, max.y}, true);
    if (!scene.empty() && !playback_.started())
        scene.draw(presented_viewport_, list, !presented_viewport_.same_view(viewport_));
    else if (image)
    {
        constexpr std::array<render::Point, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        renderer_.add_image_quad(
            list, image, viewport_.quad({0, 0, image_info.canvas.width, image_info.canvas.height}, image_info.canvas), uv);
    }
    list->PopClipRect();
}

bool Viewer::has_visible_image() const
{
    return has_image() || (work_ == Work::static_image && loading_ && !pending_scene_.empty());
}
} // namespace liltview
