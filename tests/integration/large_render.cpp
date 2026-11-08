#include <SDL3/SDL.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "viewer/viewer.hpp"

namespace
{
using namespace liltview;
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
class LimitedRenderer final : public render::Renderer
{
public:
    explicit LimitedRenderer(render::Renderer &inner) :
        inner_(inner)
    {
    }
    render::Capabilities capabilities() const override
    {
        auto caps             = inner_.capabilities();
        caps.max_texture_edge = 128;
        return caps;
    }
    render::Size framebuffer_size() const override
    {
        return inner_.framebuffer_size();
    }
    render::Image create_image(render::ImageDesc desc) override
    {
        check(desc.size.width <= 128 && desc.size.height <= 128, "Viewer allocated an oversized texture");
        ++created;
        return inner_.create_image(desc);
    }
    void update_image(render::Image image, render::Rect rect, render::Pixels pixels) override
    {
        inner_.update_image(image, rect, pixels);
    }
    void destroy_image(render::Image image) noexcept override
    {
        inner_.destroy_image(image);
    }
    void add_image(ImDrawList *list, render::Image image, render::Point a, render::Point b, render::Point u,
                   render::Point v) override
    {
        inner_.add_image(list, image, a, b, u, v);
    }
    void add_image_quad(ImDrawList *list, render::Image image, const std::array<render::Point, 4> &p,
                        const std::array<render::Point, 4> &uv) override
    {
        inner_.add_image_quad(list, image, p, uv);
    }
    void begin_frame() override
    {
        inner_.begin_frame();
    }
    void render_frame(ImDrawData *data) override
    {
        inner_.render_frame(data);
    }
    void present() override
    {
        inner_.present();
    }
    render::Readback readback() override
    {
        return inner_.readback();
    }
    unsigned created = 0;

private:
    render::Renderer &inner_;
};
render::Readback render_view(Viewer &viewer, render::Renderer &renderer)
{
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const auto size = renderer.framebuffer_size();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({static_cast<float>(size.width), static_cast<float>(size.height)});
    ImGui::Begin("checkerboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    viewer.draw({0, 0}, {static_cast<float>(size.width), static_cast<float>(size.height)}, 1);
    ImGui::End();
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    auto pixels = renderer.readback();
    renderer.present();
    return pixels;
}
render::Readback settle(Viewer &viewer, render::Renderer &renderer, bool detail)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;)
    {
        viewer.pump();
        check(!viewer.failed(), "Viewer failed loading checkerboard");
        auto pixels = render_view(viewer, renderer);
        if (viewer.settled() && (!detail || viewer.detail_ready()))
            return pixels;
        check(std::chrono::steady_clock::now() < deadline, "Large viewer timed out");
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(5)))
            ImGui_ImplSDL3_ProcessEvent(&event);
        while (SDL_PollEvent(&event))
            ImGui_ImplSDL3_ProcessEvent(&event);
    }
}
void verify(const render::Readback &image, unsigned source, double zoom, double center_x, double center_y)
{
    unsigned checked = 0;
    for (unsigned y = 16; y < image.size.height - 16; y += 7)
        for (unsigned x = 16; x < image.size.width - 16; x += 7)
        {
            const double sx = center_x + (static_cast<double>(x) + 0.5 - image.size.width / 2.0) / zoom;
            const double sy = center_y + (static_cast<double>(y) + 0.5 - image.size.height / 2.0) / zoom;
            if (sx < 0 || sy < 0 || sx >= source || sy >= source)
                continue;
            // Preview is intentionally small; sample well inside each colored square.
            if (std::fmod(sx, 1024) < 180 || std::fmod(sy, 1024) < 180 || std::fmod(sx, 1024) > 844 || std::fmod(sy, 1024) > 844)
                continue;
            const bool white = ((static_cast<unsigned>(sx) / 1024 + static_cast<unsigned>(sy) / 1024) & 1u) != 0;
            const auto at    = (static_cast<std::size_t>(y) * image.size.width + x) * 3;
            check(image.rgb[at] > 225, "Black gap or wrong red checkerboard channel");
            check(white ? image.rgb[at + 1] > 225 : image.rgb[at + 1] < 30, "Incorrect red/white checkerboard position");
            ++checked;
        }
    check(checked > 20, "Too few checkerboard pixels verified");
}
void verify_sharp_edges(const render::Readback &image)
{
    // The native checkerboard boundary is exactly at the viewport center.
    // Probe every pixel beside it; the old reduced preview cannot pass this.
    for (unsigned y = 20; y < 240; y += 7)
        for (unsigned x = 240; x < 272; ++x)
        {
            const bool white = x >= 256;
            const auto at    = (static_cast<std::size_t>(y) * image.size.width + x) * 3;
            check(image.rgb[at] == 255 && image.rgb[at + 1] == (white ? 255 : 0) && image.rgb[at + 2] == (white ? 255 : 0),
                  "32K native detail is blurred or missing");
        }
}

void save(const std::filesystem::path &path, const render::Readback &image)
{
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << image.size.width << " " << image.size.height << "\n255\n";
    out.write(reinterpret_cast<const char *>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
}
void exercise(render::Renderer &real, const std::filesystem::path &path, const std::filesystem::path &folder)
{
    LimitedRenderer renderer(real);
    Viewer viewer(renderer);
    viewer.open(path);
    auto fit          = settle(viewer, renderer, false);
    const auto source = viewer.source_size();
    check(source.width == 32768 && source.height == 32768, "32K source dimensions lost");
    const double center = source.width / 2.0;
    verify(fit, source.width, 512.0 / source.width, center, center);
    save(folder / "checkerboard-fit.ppm", fit);
    viewer.actual_size();
    auto native = settle(viewer, renderer, true);
    verify(native, source.width, 1, center, center);
    save(folder / "checkerboard-native.ppm", native);
    verify_sharp_edges(native);
    viewer.zoom(4, {256, 256});
    auto magnified = settle(viewer, renderer, true);
    verify_sharp_edges(magnified);
    save(folder / "checkerboard-magnified.ppm", magnified);
    viewer.actual_size();
    settle(viewer, renderer, true);
    check(renderer.created > 4, "Detail did not use multiple textures");
    viewer.pan(-1024, 0);
    auto pan = settle(viewer, renderer, true);
    verify(pan, source.width, 1, center + 1024, center);
    save(folder / "checkerboard-pan.ppm", pan);
    viewer.zoom(-4, {256, 256});
    auto zoom = settle(viewer, renderer, true);
    verify(zoom, source.width, std::pow(1.25, -4), center + 1024, center);
    save(folder / "checkerboard-zoom.ppm", zoom);
    viewer.fit();
    settle(viewer, renderer, false);
    viewer.shutdown();
    std::puts("PASS 32K logical image, 128px maximum texture, FIT/native/pan/zoom red-white pixels");
}
} // namespace
int main(int argc, char **argv)
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(argc == 3, "Source and artifact directory required");
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), SDL_GetError());
        window = SDL_CreateWindow("LiltView large image", 512, 512, liltview::prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer =
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 16 * 1024 * 1024, false});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "Platform initialization");
        exercise(*renderer, argv[1], argv[2]);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Large renderer test: %s\n", error.what());
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
