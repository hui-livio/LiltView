#include "viewer.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <imgui.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <stdexcept>

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
} // namespace

Viewer::Viewer(render::Renderer &renderer) :
    renderer_(renderer),
    detail_(renderer),
    pending_detail_(renderer)
{
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes  = 384ULL * 1024 * 1024;
    config.queue_pixel_bytes = 4ULL * 1024 * 1024;
    config.prefetch_workers  = 0;
    const auto max_edge      = renderer_.capabilities().max_texture_edge;
    while (config.tile_edge > max_edge && config.tile_edge > 16)
        config.tile_edge /= 2;
    config.wakeup     = wake_core;
    const auto result = lv_core_create(&config, &core_);
    if (result != LV_OK)
        throw std::runtime_error("Cannot initialize image core: " + std::to_string(result));
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
}
bool Viewer::join_shutdown()
{
    return !core_ || lv_core_join(core_, 0) == LV_OK;
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
    detail_.clear();
    pending_detail_.clear();
    lv_event_release(preview_anchor_);
    preview_anchor_ = nullptr;
    renderer_.destroy_image(display_);
    display_ = {};
    if (lv_core_destroy(&core_) != LV_OK)
        std::terminate();
}

void Viewer::open(const std::filesystem::path &path)
{
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec)
    {
        error_ = LV_ERROR_IO;
        return;
    }
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
    clear_details();
    viewport_.reset();
    opening_path_ = absolute;
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
    // Decode a bounded ordinary image once; window fit/100% is a GPU transform.
    const auto bounds = renderer_.framebuffer_size();
    // Bound preview storage by device and budget. Source dimensions are logical
    // and may be much larger than any single texture.
    const auto caps        = renderer_.capabilities();
    const auto budget_edge = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(caps.image_budget_bytes) / 16.0));
    const auto edge        = std::max(1u, std::min(caps.max_texture_edge, try_animation_ ? caps.max_texture_edge : budget_edge));
    request.target.as.fit_bounds = {std::clamp(bounds.width, 1u, edge), std::clamp(bounds.height, 1u, edge)};
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
    else
        submitted_at_ = std::chrono::steady_clock::now();
}

void Viewer::accept_completed()
{
    renderer_.destroy_image(display_);
    display_    = pending_;
    pending_    = {};
    info_       = pending_info_;
    image_path_ = opening_path_; // Keep the displayed source after a later open fails.
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
    if (current && work_ == Work::static_detail)
    {
        consume_details(view);
        return;
    }
    if (current && work_ != Work::static_image)
    {
        consume_animation(view);
        return;
    }
    if (current && view.type == LV_EVENT_IMAGE_INFO)
    {
        pending_info_ = view.as.image;
        prepare_image(false);
    }
    if (current && view.type == LV_EVENT_REGION_READY && pending_)
        upload(view.as.region);
    if (view.type == LV_EVENT_COMPLETE || view.type == LV_EVENT_ERROR || view.type == LV_EVENT_CANCELLED)
    {
        if (current)
        {
            if (view.type == LV_EVENT_COMPLETE && pending_)
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
            if (work_ == Work::static_detail)
            {
                ++generation_;
                pending_detail_.clear();
                detail_failed_ = true;
                error_         = LV_ERROR_MEMORY_BUDGET;
            }
            else
                fail(LV_ERROR_MEMORY_BUDGET);
        }
        // Even an exception while processing a terminal must allow the next request.
        if (terminal)
            request_ = 0;
    }
    if (consumed && std::chrono::steady_clock::now() >= deadline)
        wake_core(nullptr); // Continue draining next frame even without another queue edge.
    submit_pending();
    advance_animation();
    submit_details();
}

void Viewer::draw(render::Point min, render::Point max, float density)
{
    const bool reveal = work_ == Work::static_image && pending_ && pending_content_ && loading_ &&
        std::chrono::steady_clock::now() - submitted_at_ >= std::chrono::milliseconds(100);
    const auto image       = reveal ? pending_ : display_;
    const auto &image_info = reveal ? pending_info_ : info_;
    if (!image)
        return;
    viewport_.update(min, max, density, image_info.format == LV_FORMAT_SVG ? image_info.canvas : image_info.source_size,
                     image_info.orientation);
    if (!loading_ && !playback_.started() && (info_.capabilities & LV_CAP_REGION_DECODE) && ImGui::IsWindowHovered())
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
    queue_details();
    auto *list = ImGui::GetWindowDrawList();
    list->PushClipRect({min.x, min.y}, {max.x, max.y}, true);
    constexpr std::array<render::Point, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    renderer_.add_image_quad(list, image,
                             viewport_.quad({0, 0, image_info.canvas.width, image_info.canvas.height}, image_info.canvas), uv);
    if (!loading_ && !playback_.started())
        detail_.draw(viewport_, list);
    list->PopClipRect();
}

bool Viewer::has_visible_image() const
{
    return display_ ||
        (work_ == Work::static_image && pending_ && pending_content_ && loading_ &&
         std::chrono::steady_clock::now() - submitted_at_ >= std::chrono::milliseconds(100));
}
} // namespace liltview
