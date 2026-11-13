#include "platform/path.hpp"
#include "viewer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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
    pending_scene_(renderer, wake_core),
    retired_scene_(renderer, wake_core)
{
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes       = 384ULL * 1024 * 1024;
    config.queue_pixel_bytes      = 4ULL * 1024 * 1024;
    config.cache_budget_bytes     = 64ULL * 1024 * 1024;
    config.prefetch_workers       = 1;
    config.animation.max_sessions = 3; // Foreground plus the two desired neighbors.
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
        prefetch_   = std::make_unique<PrefetchController>(
            [this](const std::filesystem::path &path, std::uint64_t tag, lv_request_id &id)
            {
                if (animation_warmup_->stats().outstanding_requests + prefetch_->stats().active >= 2)
                {
                    id = 0;
                    return static_cast<lv_status>(LV_BUSY);
                }
                lv_request_desc request;
                lv_request_desc_init(&request);
                const auto &bytes   = platform::path_bytes(path);
                request.path        = {platform::path_encoding, bytes.data(), bytes.size()};
                request.kind        = LV_REQUEST_PREFETCH;
                request.priority    = LV_PRIORITY_PREFETCH;
                request.target.kind = LV_TARGET_NATIVE;
                request.user_tag    = tag;
                return lv_core_submit(core_, &request, &id);
            },
            [this](lv_request_id id) { lv_core_cancel(core_, id); });
        animation_warmup_ = std::make_unique<AnimationWarmup>(core_);
    } catch (...)
    {
        lv_core_begin_shutdown(core_);
        while (!join_shutdown(10))
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
    if (preparation_) preparation_->cancel();
    if (retired_preparation_) retired_preparation_->cancel();
    scene_.retire_ready(); pending_scene_.retire_ready(); retired_scene_.retire_ready();
    prepared_animation_.reset();
    animation_warmup_->begin_shutdown();
    prefetch_->begin_shutdown();
    lv_core_begin_shutdown(core_);
    navigation_->shutdown();
}
bool Viewer::join_shutdown(std::uint32_t timeout_ms)
{
    const auto start = std::chrono::steady_clock::now();
    do
    {
        const bool workers = (!preparation_ || preparation_->done()) && (!retired_preparation_ || retired_preparation_->done()) &&
            scene_.retire_ready() && pending_scene_.retire_ready() && retired_scene_.retire_ready();
        const bool core = !core_ || lv_core_join(core_, 0) == LV_OK;
        if (workers && core) return true;
        if (!timeout_ms) return false;
        SDL_Delay(1);
    } while (std::chrono::steady_clock::now()-start < std::chrono::milliseconds(timeout_ms));
    return false;
}

void Viewer::shutdown()
{
    if (!core_)
        return;
    begin_shutdown();
    // Decoder checks cancellation at input/row boundaries; keep SDL alive until callbacks stop.
    while (!join_shutdown(10))
    {
    }
    preparation_.reset();
    retired_preparation_.reset();
    prefetch_.reset();
    animation_warmup_.reset();
    discard_pending();
    scene_.clear();
    pending_scene_.clear();
    retired_scene_.clear();
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
    load_started_ = std::chrono::steady_clock::now();
    static_stats_ = {}; static_stats_.generation = generation_;
    static_stats_.batch = static_mode_ == StaticUploadMode::batch;
    loading_frame_submitted_ = static_frame_submitted_ = false;
    spdlog::info("static_load_begin generation={} mode={} path={}", generation_, static_stats_.batch ? "batch" : "pipeline", platform::path_bytes(absolute));
    prepared_animation_.reset();
    adopting_animation_ = animation_warmup_->request_adoption(absolute, animation_bounds(), generation_);
    if (adopting_animation_)
        work_ = Work::animation_open;
    // Retire other READY canvases before foreground resource admission.
    animation_warmup_->update(generation_, {}, animation_bounds());
    animation_warmup_->interrupt_background();
    prefetch_->update(generation_, navigation_->snapshot());
    prefetch_->pump(false);
    close_animation();
    if (request_)
        lv_core_cancel(core_, request_);
    // Canvas buttons navigate after draw commands have been built. Keep the
    // progressive scene's textures alive until that frame has been submitted.
    // Further navigation before pump has no new pending scene to retire.
    if (!pending_scene_.empty())
        pending_scene_.swap(retired_scene_);
    discard_pending();
    playback_.reset();
    playback_.suspend(suspended_, Playback::Clock::now());
    try_animation_   = true;
    reset_animation_ = false;
    completed_plays_ = 0;
    visible_pending_ = false;
    viewport_.reset();
    pending_path_  = adopting_animation_ ? std::filesystem::path{} : absolute;
    selected_path_ = absolute;
    filename_      = platform::path_bytes(absolute.filename());
    error_         = 0;
    loading_       = true;
}

void Viewer::discard_pending()
{
    if (preparation_)
    {
        const auto cancelled = preparation_->stats();
        spdlog::debug("static_load_cancel generation={} prepared_tiles={} output_peak={}", preparation_->generation(), cancelled.tiles, cancelled.output_peak);
        preparation_->cancel();
        // A new static request is not submitted until this one has retired.
        if (retired_preparation_) std::terminate();
        retired_preparation_ = std::move(preparation_);
    }
    decode_complete_ = cpu_complete_ = false;
    expected_tiles_ = 0;
    renderer_.destroy_image(pending_);
    pending_      = {};
    pending_info_ = {};
    pending_pixel_format_.reset();
    pending_color_space_ = LV_COLOR_UNKNOWN;
    lv_event_release(pending_anchor_);
    pending_anchor_  = nullptr;
    pending_content_ = false;
    // A discarded progressive scene cannot still be waiting for visible
    // pixels. Failure presentation deliberately stops calling draw(), so it
    // cannot clear this flag on a later frame for us.
    visible_pending_ = false;
    if (!pending_scene_.retire_ready())
    {
        if (!retired_scene_.empty()) std::terminate();
        pending_scene_.swap(retired_scene_);
    }
    pending_scene_.clear();
    pending_scene_id_ = 0;
}

void Viewer::submit_pending()
{
    if (pending_path_.empty() || request_ || retired_preparation_ || !retired_scene_.empty())
        return;
    const auto path = platform::path_bytes(pending_path_);
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path     = {platform::path_encoding, path.data(), path.size()};
    request.user_tag = generation_;
    // Only animations use a bounded immutable FIT canvas. Static images are
    // decoded exactly once at native resolution, independent of the viewport.
    if (try_animation_)
    {
        request.target.as.fit_bounds = animation_bounds();
    }
    else
        request.target.kind = LV_TARGET_NATIVE;
    lv_status status;
    if (try_animation_)
    {
        animation_warmup_->interrupt_background();
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
    if (status == LV_OK)
        prefetch_->pump(false);
    if (status != LV_OK)
    {
        pending_path_.clear();
        fail(LV_ERROR_INTERNAL);
    }
}

void Viewer::accept_completed()
{
    auto displayed_path = selected_path_;
    renderer_.destroy_image(display_);
    display_ = pending_;
    pending_ = {};
    scene_.swap(pending_scene_);
    pending_scene_.swap(retired_scene_); // Retire old CPU readers without waiting on main-thread disk I/O.
    if (retired_scene_.retire_ready()) retired_scene_.clear();
    scene_id_         = pending_scene_id_;
    pending_scene_id_ = 0;
    info_             = pending_info_;
    displayed_path_   = std::move(displayed_path);
    pixel_format_     = pending_pixel_format_;
    color_space_      = pending_color_space_;
    displayed_animation_.reset();
    lv_event_release(preview_anchor_);
    preview_anchor_ = pending_anchor_;
    pending_anchor_ = nullptr;
    loading_        = false;
    error_          = 0;
    spdlog::info("Image displayed: {}, {}x{}, orientation={}, static_source_bytes_read={}", filename_, info_.source_size.width,
                 info_.source_size.height, info_.orientation, last_source_bytes_read_);
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
        pending_scene_.begin(pending_info_, true);
        preparation_ = std::make_unique<TilePreparation>(pending_scene_.overview_level(), static_mode_, wake_core, nullptr, generation_, TilePreparation::workers_for(pending_info_), static_cache_compression_);
    }
    if (current && view.type == LV_EVENT_REGION_READY)
    {
        const auto &pixels = *lv_pixels_get_view(view.as.region.pixels);
        if (static_stats_.first_region_ms < 0) static_stats_.first_region_ms = load_elapsed_ms();
        preparation_->submit(view.as.region);
        pending_pixel_format_ = pixels.format;
        pending_color_space_  = pixels.color_space;
        pending_content_      = true;
    }
    if (view.type == LV_EVENT_COMPLETE || view.type == LV_EVENT_ERROR || view.type == LV_EVENT_CANCELLED)
    {
        if (current)
        {
            if (view.type == LV_EVENT_COMPLETE && preparation_)
            {
                last_source_bytes_read_ = view.as.completion.source_bytes_read;
                decode_complete_ = true;
                expected_tiles_ = view.as.completion.final_tile_count;
                static_stats_.core_ms = view.as.completion.elapsed_ns / 1e6;
                static_stats_.decode_done_ms = load_elapsed_ms();
                preparation_->finish();
                spdlog::debug("static_load_decode generation={} core_ms={:.3f} observed_ms={:.3f} tiles={}", generation_, static_stats_.core_ms, static_stats_.decode_done_ms, expected_tiles_);
            }
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
    const auto pump_started = std::chrono::steady_clock::now();
    first_commit_this_frame_ = false;
    if (retired_preparation_ && retired_preparation_->done()) retired_preparation_.reset();
    if (retired_scene_.retire_ready()) retired_scene_.clear(); // No UI join on an outstanding cache read.
    navigation_->pump();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    lv_event *raw       = nullptr;
    bool consumed       = false;
    while (std::chrono::steady_clock::now() < deadline &&
           (!preparation_ || preparation_->can_accept() || preparation_->done()) && lv_core_poll_event(core_, &raw) == LV_OK)
    {
        consumed = true;
        std::unique_ptr<lv_event, decltype(&lv_event_release)> event(raw, lv_event_release);
        const auto &view = *lv_event_get_view(event.get());
        if (animation_warmup_->consume(view))
            continue;
        if (prefetch_->consume(view))
        {
            if (view.type == LV_EVENT_COMPLETE)
                spdlog::debug("Neighbor prefetch completed: request={}, source_bytes_read={}", view.request_id,
                              view.as.completion.source_bytes_read);
            else if (view.type == LV_EVENT_ERROR)
                spdlog::debug("Neighbor prefetch failed: request={}, error={}", view.request_id, view.as.error.code);
            continue; // RAII returns background metadata and terminals without touching the image.
        }
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
    advance_static();
    submit_pending();
    advance_animation();
    pump_neighbors();
    if (prepared_animation_)
        advance_animation();
    if (loading_ || static_stats_.ready_ms < 0)
        static_stats_.max_pump_ms = std::max(static_stats_.max_pump_ms,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-pump_started).count());
}

std::uint64_t Viewer::content_cache_bytes() const
{
    lv_core_stats stats{};
    return core_ && lv_core_get_stats(core_, &stats) == LV_OK ? stats.cache_bytes : 0;
}

void Viewer::draw(render::Point min, render::Point max, float density, bool interactive)
{
    // Align the canvas clipping rectangle with physical pixels too. Image
    // vertices were aligned already; fractional clip edges otherwise make
    // offscreen border tiles look necessary at HiDPI.
    density                = std::max(0.01f, density);
    min                    = {std::round(min.x * density) / density, std::round(min.y * density) / density};
    max                    = {std::round(max.x * density) / density, std::round(max.y * density) / density};
    set_viewport(min, max, density); // Hidden preparation also advances for non-Shell render clients.
    if (loading_ && work_ == Work::static_image) return;
    const bool reveal      = false;
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
                const bool ready = first_commit_this_frame_ && scene.covered(viewport_) ? true :
                    scene.prepare(viewport_, changing_view ? &presented_viewport_ : nullptr);
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
    return !(loading_ && work_ == Work::static_image) && has_image();
}
} // namespace liltview
