// Real SDL_Render warm-adoption UT: static 1.bmp -> content-GIF 2.png, READY without
// additional GPU image bytes, then read back genuine warmed red 0 and real blue 1.
// Assert one warmed installation/two actual presentations and retain the original non-warm mode.
#include <SDL3/SDL.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string_view>
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

liltview::render::Readback draw_frame(liltview::Viewer &viewer, liltview::render::Renderer &renderer)
{
    renderer.begin_frame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const auto size = renderer.framebuffer_size();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({static_cast<float>(size.width), static_cast<float>(size.height)});
    ImGui::Begin("canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    viewer.draw({0, 0}, {static_cast<float>(size.width), static_cast<float>(size.height)}, 1, false);
    ImGui::End();
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    return renderer.readback();
}

std::array<unsigned char, 3> center_pixel(const liltview::render::Readback &pixels)
{
    const auto at = (static_cast<std::size_t>(pixels.size.height / 2) * pixels.size.width + pixels.size.width / 2) * 3;
    check(pixels.size.width && pixels.size.height && at + 2 < pixels.rgb.size(), "Invalid renderer readback");
    return {pixels.rgb[at], pixels.rgb[at + 1], pixels.rgb[at + 2]};
}

void events(const liltview::Viewer &viewer)
{
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, viewer.wait_timeout_ms(5)))
        ImGui_ImplSDL3_ProcessEvent(&event);
    while (SDL_PollEvent(&event))
        ImGui_ImplSDL3_ProcessEvent(&event);
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
        const auto pixels = center_pixel(draw_frame(viewer, renderer));
        const auto r = pixels[0], g = pixels[1], b = pixels[2];
        if (r > 200 && g < 20 && b < 20)
            red = true;
        if (red && b > 200 && r < 20 && g < 20)
            blue = true;
        renderer.present();
        events(viewer);
    }
    check(red && blue, "Real renderer did not display both GIF frames");
    viewer.shutdown();
    (void)window;
}

void exercise_warm(liltview::render::Renderer &renderer, const std::filesystem::path &fixture)
{
    struct Directory
    {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("liltview-gl-warm-" + std::to_string(SDL_GetPerformanceCounter()));
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
    std::filesystem::copy_file(fixture.parent_path() / "red.bmp", directory.path / "1.bmp");
    std::filesystem::copy_file(fixture, directory.path / "2.png");
    liltview::Viewer viewer(renderer);
    viewer.open(directory.path / "1.bmp");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!viewer.settled())
    {
        viewer.pump();
        check(!viewer.failed(), "Static image failed before GIF warmup");
        check(std::chrono::steady_clock::now() < deadline, "Static image did not settle");
        events(viewer);
    }
    const auto image_bytes = renderer.capabilities().image_bytes;
    const auto uploads     = renderer.upload_stats();
    deadline               = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (viewer.animation_warmup_stats().ready != 1 || viewer.prefetch_pending() || viewer.navigation().scanning)
    {
        viewer.pump();
        check(!viewer.failed(), "GIF neighbor warmup changed the foreground error state");
        check(viewer.prefetch_stats().active + viewer.animation_warmup_stats().outstanding_requests <= 2,
              "Global background request quota exceeded");
        check(renderer.capabilities().image_bytes == image_bytes, "Background GIF warmup allocated GPU images");
        check(renderer.upload_stats().staged_uploads == uploads.staged_uploads &&
                  renderer.upload_stats().direct_uploads == uploads.direct_uploads,
              "Background GIF warmup uploaded pixels");
        check(std::chrono::steady_clock::now() < deadline, "GIF neighbor did not reach READY");
        events(viewer);
    }
    check(viewer.animation_frames_presented() == 0, "Background GIF warmup presented a frame");
    viewer.navigate(1);
    bool red = false, blue = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!blue)
    {
        viewer.pump();
        check(!viewer.failed(), "Warmed GIF foreground playback failed");
        const auto pixels  = center_pixel(draw_frame(viewer, renderer));
        const auto details = viewer.image_details();
        const bool warmed  = viewer.warmed_first_frames_presented() == 1 && details && details->animation &&
            details->path == directory.path / "2.png";
        if (warmed && viewer.displayed_frame_index() == 0 && pixels[0] > 200 && pixels[1] < 20 && pixels[2] < 20)
        {
            check(viewer.animation_frames_presented() == 1, "Warmed frame zero was repeated");
            red = true;
        }
        if (warmed && red && viewer.displayed_frame_index() == 1 && pixels[2] > 200 && pixels[0] < 20 && pixels[1] < 20)
            blue = true;
        if (warmed && !red)
            check(viewer.displayed_frame_index() == 0, "Warmed first frame was skipped before an actual red draw");
        renderer.present();
        check(std::chrono::steady_clock::now() < deadline, "Warmed GIF did not draw red then blue");
        if (!blue)
            events(viewer);
    }
    check(red && blue, "SDL_Render failed warmed red/blue readback");
    check(viewer.warmed_first_frames_presented() == 1, "Warmed first frame was installed more than once");
    check(viewer.animation_frames_presented() == 2, "Warmed playback presented an incorrect number of frames");
    check(viewer.animation_warmup_stats().adopted == 1, "GIF was not adopted from the real warmed session");
    viewer.shutdown();
    check(renderer.capabilities().image_bytes == 0, "Warmed GIF shutdown leaked GPU images");
}
} // namespace
int main(int argc, char **argv)
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(argc == 2 || (argc == 3 && std::string_view(argv[2]) == "--warm"), "fixture path [--warm] required");
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS), SDL_GetError());
        window = SDL_CreateWindow("LiltView animation test", 320, 240, liltview::prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        auto renderer =
            liltview::render::create_renderer({liltview::render::SurfaceKind::sdl_window, window, 1024 * 1024, false});
        platform = liltview::initialize_imgui_platform(window);
        check(platform, "platform initialization");
        if (argc == 3)
            exercise_warm(*renderer, argv[1]);
        else
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
