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
// Orientation is a UV transform; pixels and renderer stay backend independent.
std::array<ImVec2, 4> oriented_uv(lv_orientation orientation)
{
    constexpr std::array<std::array<unsigned, 4>, 8> order{{{{0, 1, 2, 3}},
                                                            {{1, 0, 3, 2}},
                                                            {{2, 3, 0, 1}},
                                                            {{3, 2, 1, 0}},
                                                            {{0, 3, 2, 1}},
                                                            {{3, 0, 1, 2}},
                                                            {{2, 1, 0, 3}},
                                                            {{1, 2, 3, 0}}}};
    const std::array<ImVec2, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const auto &indices = order[std::clamp(orientation, 1u, 8u) - 1];
    return {corners[indices[0]], corners[indices[1]], corners[indices[2]], corners[indices[3]]};
}
} // namespace

Viewer::Viewer(render::Renderer &renderer) :
    renderer_(renderer)
{
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes  = 384ULL * 1024 * 1024;
    config.queue_pixel_bytes = 4ULL * 1024 * 1024;
    config.prefetch_workers  = 0;
    config.wakeup            = wake_core;
    const auto result        = lv_core_create(&config, &core_);
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
    pending_path_    = absolute;
    filename_        = absolute.filename().string();
    error_           = 0;
    loading_         = true;
    actual_size_     = false;
}

void Viewer::discard_pending()
{
    renderer_.destroy_image(pending_);
    pending_         = {};
    pending_info_    = {};
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
    const auto bounds            = renderer_.framebuffer_size();
    const auto edge              = renderer_.capabilities().max_texture_edge;
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
    display_ = pending_;
    pending_ = {};
    info_    = pending_info_;
    loading_ = false;
    error_   = 0;
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
            consume(*lv_event_get_view(event.get()));
        } catch (const std::exception &error)
        {
            spdlog::error("Image upload: {}", error.what());
            if (request_)
                lv_core_cancel(core_, request_);
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
}

void Viewer::draw(render::Point min, render::Point max, float density)
{
    const bool reveal_pending = work_ == Work::static_image && pending_ && pending_content_ && loading_ &&
        std::chrono::steady_clock::now() - submitted_at_ >= std::chrono::milliseconds(100);
    const auto image       = reveal_pending ? pending_ : display_;
    const auto &image_info = reveal_pending ? pending_info_ : info_;
    if (!image)
        return;
    float width = static_cast<float>(image_info.canvas.width), height = static_cast<float>(image_info.canvas.height);
    if (image_info.orientation >= 5)
        std::swap(width, height);
    const float scale = actual_size_ ?
        1.0f / std::max(density, 1.0f) :
        std::min({(max.x - min.x) / width, (max.y - min.y) / height, 1.0f / std::max(density, 1.0f)});
    const float x = min.x + (max.x - min.x - width * scale) * 0.5f, y = min.y + (max.y - min.y - height * scale) * 0.5f;
    const auto uv = oriented_uv(image_info.orientation);
    renderer_.add_image_quad(ImGui::GetWindowDrawList(), image,
                             {{{x, y}, {x + width * scale, y}, {x + width * scale, y + height * scale}, {x, y + height * scale}}},
                             {{{uv[0].x, uv[0].y}, {uv[1].x, uv[1].y}, {uv[2].x, uv[2].y}, {uv[3].x, uv[3].y}}});
}

bool Viewer::has_visible_image() const
{
    return display_ ||
        (work_ == Work::static_image && pending_ && pending_content_ && loading_ &&
         std::chrono::steady_clock::now() - submitted_at_ >= std::chrono::milliseconds(100));
}
} // namespace liltview
