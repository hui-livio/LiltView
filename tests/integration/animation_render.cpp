#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "viewer/viewer.hpp"

namespace
{
void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
void exercise(SDL_Window *window, liltview::render::Renderer &renderer, const char *path)
{
    liltview::Viewer viewer(renderer);
    viewer.open(path);
    bool red = false, blue = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !blue)
    {
        viewer.pump();
        check(!viewer.failed(), "GIF viewer failed");
        renderer.begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const auto size = renderer.framebuffer_size();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({static_cast<float>(size.width), static_cast<float>(size.height)});
        ImGui::Begin("canvas", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        viewer.draw({0, 0}, {static_cast<float>(size.width), static_cast<float>(size.height)}, 1);
        ImGui::End();
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        const auto pixels = renderer.readback();
        const auto at     = (static_cast<std::size_t>(pixels.size.height / 2) * pixels.size.width + pixels.size.width / 2) * 3;
        const auto r = pixels.rgb[at], g = pixels.rgb[at + 1], b = pixels.rgb[at + 2];
        if (r > 200 && g < 20 && b < 20)
            red = true;
        if (red && b > 200 && r < 20 && g < 20)
            blue = true;
        renderer.present();
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(20)))
            ImGui_ImplSDL3_ProcessEvent(&event);
        while (SDL_PollEvent(&event))
            ImGui_ImplSDL3_ProcessEvent(&event);
    }
    check(red && blue, "Real renderer did not display both GIF frames");
    viewer.shutdown();
    (void)window;
}
} // namespace
int main(int argc, char **argv)
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(argc == 2, "fixture path required");
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), SDL_GetError());
        window = SDL_CreateWindow("LiltView animation test", 320, 240, liltview::prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer =
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 1024 * 1024, false});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "platform initialization");
        exercise(window, *renderer, argv[1]);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::puts("PASS GIF red and blue frames rendered");
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Animation rendering failed: %s\n", error.what());
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
