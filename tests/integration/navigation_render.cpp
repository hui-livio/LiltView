#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include "../fixtures/checkerboard.hpp"
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"
#include "viewer/viewer.hpp"

namespace
{
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void frame(liltview::Viewer &viewer, liltview::render::Renderer &renderer, int direction = 0)
{
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({800, 600});
    ImGui::Begin("navigation", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    viewer.draw({0, 0}, {800, 600}, 1, false);
    ImGui::End();
    // Application dispatches a canvas arrow action after Shell builds this frame.
    if (direction)
    {
        viewer.navigate(direction);
        viewer.navigate(-direction);
        viewer.navigate(direction);
    }
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    renderer.present();
}
void exercise(liltview::render::Renderer &renderer, const char *extension)
{
    struct Directory
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("liltview-navigation-render-" + std::to_string(SDL_GetPerformanceCounter()));
        Directory()
        {
            std::filesystem::create_directories(path);
        }
        ~Directory()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } directory;
    const std::string first = std::string("1.") + extension, last = std::string("3.") + extension;
    liltview::test::checkerboard(directory.path / first, 64, 64, 7);
    liltview::test::checkerboard(directory.path / (std::string("2.") + extension), 1024, 8192, 7);
    liltview::test::checkerboard(directory.path / last, 64, 64, 7);
    liltview::Viewer viewer(renderer);
    const auto settle = [&]
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            viewer.pump();
            viewer.set_viewport({0, 0}, {800, 600}, 1);
            check(!viewer.failed(), "Navigation image load failed");
            check(std::chrono::steady_clock::now() < deadline, "Navigation load timed out");
            SDL_Delay(1);
        } while (!viewer.settled() || viewer.navigation().scanning);
    };
    for (unsigned cycle = 0; cycle < 12; ++cycle)
    {
        const int direction = cycle % 2 == 0 ? 1 : -1;
        viewer.open(directory.path / (direction == 1 ? first : last));
        settle();
        frame(viewer, renderer);
        viewer.navigate(direction);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;)
        {
            viewer.pump();
            if (viewer.loading() && viewer.static_load_stats().tiles > 0)
            {
                check(!viewer.has_visible_image(), "Partial pending scene became visible");
                break;
            }
            check(std::chrono::steady_clock::now() < deadline, "No pending texture upload to navigate away from");
            SDL_Delay(1);
        }
        if (cycle % 4 < 2)
            frame(viewer, renderer, direction); // Canvas click after building draw commands.
        else
        {
            frame(viewer, renderer);
            viewer.navigate(direction); // Keyboard action between frames, before the next pump.
            viewer.pump();
            frame(viewer, renderer);
        }
        settle();
        frame(viewer, renderer);
        check(viewer.filename() == (direction == 1 ? last : first), "Wrong final navigation target");
        check(renderer.capabilities().image_bytes == liltview::render::image_storage_bytes({{64, 64}, true}),
              "Retired progressive textures were not released");
    }
    viewer.shutdown();
    check(renderer.capabilities().image_bytes == 0, "Navigation leaked image textures");
}
} // namespace
int main()
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), SDL_GetError());
        window = SDL_CreateWindow("Navigation regression", 800, 600, liltview::prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer =
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 4 * 1024 * 1024, false});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "Platform initialization failed");
        exercise(*renderer, "png");
        exercise(*renderer, "jpg");
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::puts("PASS navigation during a progressive frame");
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Navigation rendering failed: %s\n", error.what());
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
