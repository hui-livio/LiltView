// Manual real-image interaction benchmark; never registered with CTest.
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string>
#include <vector>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "viewer/viewer.hpp"

namespace
{
using namespace liltview;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
class MeasuredRenderer final : public render::Renderer
{
public:
    explicit MeasuredRenderer(render::Renderer &inner) :
        inner_(inner)
    {
    }
    double upload_ms = 0;
    unsigned uploads = 0;
    render::Capabilities capabilities() const override
    {
        return inner_.capabilities();
    }
    render::Size framebuffer_size() const override
    {
        return inner_.framebuffer_size();
    }
    render::Image create_image(render::ImageDesc d) override
    {
        const auto start  = Clock::now();
        auto image        = inner_.create_image(d);
        upload_ms        += ms(start);
        ++uploads;
        return image;
    }
    render::Image create_uploaded_image(render::ImageDesc d, render::Pixels p) override
    {
        const auto start  = Clock::now();
        auto image        = inner_.create_uploaded_image(d, p);
        upload_ms        += ms(start);
        ++uploads;
        return image;
    }
    bool upload_ready() override
    {
        return inner_.upload_ready();
    }
    render::UploadStats upload_stats() const override
    {
        return inner_.upload_stats();
    }
    void update_image(render::Image i, render::Rect r, render::Pixels p) override
    {
        const auto start = Clock::now();
        inner_.update_image(i, r, p);
        upload_ms += ms(start);
        ++uploads;
    }
    void destroy_image(render::Image i) noexcept override
    {
        inner_.destroy_image(i);
    }
    void add_image(ImDrawList *l, render::Image i, render::Point a, render::Point b, render::Point u, render::Point v) override
    {
        inner_.add_image(l, i, a, b, u, v);
    }
    void add_image_quad(ImDrawList *l, render::Image i, const std::array<render::Point, 4> &p,
                        const std::array<render::Point, 4> &u) override
    {
        inner_.add_image_quad(l, i, p, u);
    }
    void begin_frame() override
    {
        inner_.begin_frame();
    }
    void render_frame(ImDrawData *d) override
    {
        inner_.render_frame(d);
    }
    void present() override
    {
        inner_.present();
    }
    render::Readback readback() override
    {
        return inner_.readback();
    }

private:
    render::Renderer &inner_;
};
struct Sample
{
    double pump, prepare, upload, submit, present, total;
    bool pending, held;
    int commands;
};
Sample frame(Viewer &viewer, MeasuredRenderer &renderer)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        ImGui_ImplSDL3_ProcessEvent(&event);
    const auto start = Clock::now();
    viewer.pump();
    const auto pump = ms(start);
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("benchmark", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const auto old    = viewer.presented_view();
    const auto upload = renderer.upload_ms;
    const auto draw   = Clock::now();
    viewer.draw({0, 0}, {io.DisplaySize.x, io.DisplaySize.y}, io.DisplayFramebufferScale.x);
    const auto preparation = ms(draw);
    ImGui::End();
    ImGui::Render();
    const auto submit_start = Clock::now();
    renderer.render_frame(ImGui::GetDrawData());
    const auto submit        = ms(submit_start);
    const auto present_start = Clock::now();
    renderer.present();
    const auto present = ms(present_start);
    int commands       = 0;
    for (const auto *list : ImGui::GetDrawData()->CmdLists)
        commands += list->CmdBuffer.Size;
    return {pump,    preparation, renderer.upload_ms - upload, submit,
            present, ms(start),   viewer.detail_loading(),     viewer.detail_loading() && old.same_view(viewer.presented_view()),
            commands};
}
void report(const char *label, const std::vector<Sample> &samples, double elapsed, const Tiles::Stats &before,
            const Tiles::Stats &after, const MeasuredRenderer &renderer)
{
    std::vector<double> times;
    Sample worst{};
    unsigned pending = 0, held = 0;
    for (const auto &s : samples)
    {
        times.push_back(s.total);
        pending += s.pending;
        held    += s.held;
        if (s.total > worst.total)
            worst = s;
    }
    std::sort(times.begin(), times.end());
    const auto percentile = [&](std::size_t percent)
    {
        return times[(times.size() * percent + 99) / 100 - 1];
    };
    std::printf(
        "%s: elapsed=%.1fms frames=%zu p50=%.2f p95=%.2f max=%.2f [pump=%.2f prepare=%.2f upload=%.2f submit=%.2f present=%.2f cmds=%d] pending=%u held=%u reads=%llu restores=%llu gpu=%.1fMiB\n",
        label, elapsed, samples.size(), percentile(50), percentile(95), worst.total, worst.pump, worst.prepare, worst.upload,
        worst.submit, worst.present, worst.commands, pending, held,
        static_cast<unsigned long long>(after.ram.reads - before.ram.reads),
        static_cast<unsigned long long>(after.gpu_restores - before.gpu_restores),
        static_cast<double>(renderer.capabilities().image_bytes) / 1048576.0);
    std::fflush(stdout);
}
void settle(const char *label, Viewer &viewer, MeasuredRenderer &renderer)
{
    const auto start  = Clock::now();
    const auto before = viewer.cache_stats();
    std::vector<Sample> samples;
    do
    {
        samples.push_back(frame(viewer, renderer));
        if (viewer.failed())
            throw std::runtime_error("Image/cache load failed");
        if (ms(start) > 180000)
            throw std::runtime_error("Image/cache timed out");
        if (!viewer.settled())
            SDL_Delay(1);
    } while (!viewer.settled());
    report(label, samples, ms(start), before, viewer.cache_stats(), renderer);
}
} // namespace
int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: %s IMAGE [WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
            throw std::runtime_error(SDL_GetError());
        window = SDL_CreateWindow("LiltView benchmark", argc > 2 ? std::stoi(argv[2]) : 1600,
                                  argc > 3 ? std::stoi(argv[3]) : 1000, prepare_render_window() | SDL_WINDOW_HIDDEN);
        if (!window)
            throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        {
            auto real = render::create_renderer({render::SurfaceKind::sdl_window, window, 128ULL * 1024 * 1024, false});
            MeasuredRenderer renderer(*real);
            platform = initialize_imgui_platform(window);
            if (!platform)
                throw std::runtime_error("Cannot initialize ImGui platform");
            int interval = -1;
            auto *device = SDL_GetRenderer(window);
            SDL_GetRenderVSync(device, &interval);
            std::printf("video=%s renderer=SDL_Render/%s vsync=%d file=%s\n", SDL_GetCurrentVideoDriver(),
                        SDL_GetRendererName(device), interval, argv[1]);
            Viewer viewer(renderer);
            viewer.open(std::filesystem::path(argv[1]));
            settle("initial-fit", viewer, renderer);
            viewer.actual_size();
            settle("native-cold", viewer, renderer);
            for (int pass = 0; pass < 2; ++pass)
            {
                const auto start  = Clock::now();
                const auto before = viewer.cache_stats();
                std::vector<Sample> samples;
                for (int n = 0; n < 120; ++n)
                {
                    viewer.pan(n < 60 ? -16 : 16, 0);
                    samples.push_back(frame(viewer, renderer));
                    SDL_Delay(8);
                }
                report(pass ? "pan-repeat" : "pan", samples, ms(start), before, viewer.cache_stats(), renderer);
                settle("pan-settle", viewer, renderer);
            }
            for (int n = 0; n < 12; ++n)
            {
                const auto size = renderer.framebuffer_size();
                viewer.zoom(-1, {static_cast<float>(size.width) / 2, static_cast<float>(size.height) / 2});
                settle(("zoom-out-" + std::to_string(n)).c_str(), viewer, renderer);
            }
            for (int n = 0; n < 12; ++n)
            {
                const auto size = renderer.framebuffer_size();
                viewer.zoom(1, {static_cast<float>(size.width) / 2, static_cast<float>(size.height) / 2});
                settle(("zoom-in-" + std::to_string(n)).c_str(), viewer, renderer);
            }
            const auto uploads = renderer.upload_stats();
            std::printf(
                "staging=%llu/%llu bytes staged=%llu direct=%llu\n", static_cast<unsigned long long>(uploads.staging_bytes),
                static_cast<unsigned long long>(uploads.staging_limit), static_cast<unsigned long long>(uploads.staged_uploads),
                static_cast<unsigned long long>(uploads.direct_uploads));
            viewer.shutdown();
        }
        ImGui_ImplSDL3_Shutdown();
        platform = false;
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Benchmark: %s\n", error.what());
        if (platform)
            ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
