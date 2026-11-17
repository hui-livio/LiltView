#include "platform/image_information_window.hpp"
#include "platform/sdl_render_host.hpp"
#include "renderer/renderer.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace liltview;
void check(bool value, const char *message)
{
    if (!value)
        throw std::runtime_error(message);
}
void main_frame(render::Renderer &renderer, SDL_Window *window)
{
    renderer.begin_frame();
    new_imgui_platform_frame(window);
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled({4, 4}, {40, 40}, IM_COL32(220, 40, 30, 255));
    ImGui::Render();
    renderer.render_frame(ImGui::GetDrawData());
    const auto pixels = renderer.readback();
    const auto at     = (static_cast<std::size_t>(pixels.size.width) * 10 + 10) * 3;
    check(pixels.rgb[at] == 220 && pixels.rgb[at + 1] == 40 && pixels.rgb[at + 2] == 30,
          "Information window damaged the main renderer/context");
    renderer.present();
}
void exercise(SDL_Window *main_window, render::Renderer &renderer)
{
    auto *main_context    = ImGui::GetCurrentContext();
    auto &io              = ImGui::GetIO();
    auto *fonts           = io.Fonts;
    auto *platform        = io.BackendPlatformUserData;
    auto *backend         = io.BackendRendererUserData;
    auto *main_device     = SDL_GetRenderer(main_window);
    const auto main_id    = SDL_GetWindowID(main_window);
    const auto main_flags = SDL_GetWindowFlags(main_window);
    int main_width = 0, main_height = 0;
    check(SDL_GetWindowSize(main_window, &main_width, &main_height), SDL_GetError());
    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        ImageInformationWindow panel(main_window, {});
        check(ImGui::GetCurrentContext() == main_context, "Creation did not restore the main context");
        check(panel.window() != main_window && SDL_GetRenderer(panel.window()) != main_device, "Not a separate desktop renderer");
        check(!panel.visible(), "Information window should start hidden");
        const auto flags = SDL_GetWindowFlags(panel.window());
        check((flags & SDL_WINDOW_RESIZABLE) && !(flags & (SDL_WINDOW_MODAL | SDL_WINDOW_BORDERLESS)),
              "Information window needs independent native move/resize and must not be modal");
        const auto id = SDL_GetWindowID(panel.window());
        panel.show();
        check(panel.visible(), "Cannot show information window");
        check(SDL_SetWindowSize(panel.window(), 420, 440), SDL_GetError());
        SDL_SyncWindow(panel.window());
        const auto *video              = SDL_GetCurrentVideoDriver();
        const bool desktop_positioning = video && (std::string_view(video) == "windows" || std::string_view(video) == "x11");
        if (desktop_positioning)
        {
            int x = 0, y = 0;
            check(SDL_GetWindowPosition(main_window, &x, &y), SDL_GetError());
            check(SDL_SetWindowPosition(panel.window(), x + main_width + 40, y), SDL_GetError());
            SDL_SyncWindow(panel.window());
            int panel_x = 0, panel_y = 0;
            check(SDL_GetWindowPosition(panel.window(), &panel_x, &panel_y), SDL_GetError());
            check(panel_x >= x + main_width, "Information window is confined to the main client area");
        }
        ImageDetails details;
        details.path              = std::filesystem::path(u8"/images/独立面板-100%-😀.png");
        details.image.format      = LV_FORMAT_PNG;
        details.image.source_size = {4000, 3000};
        details.image.orientation = LV_ORIENTATION_NORMAL;
        details.image.flags       = LV_INFO_SOURCE_SIZE_KNOWN;
        details.scale             = 0.25;
        for (const bool dark : {true, false})
            for (const auto language : {Language::english, Language::chinese})
            {
                panel.draw(language, details, dark);
                panel.draw(language, std::nullopt, dark);
                check(ImGui::GetCurrentContext() == main_context && io.Fonts == fonts && io.BackendPlatformUserData == platform &&
                          io.BackendRendererUserData == backend,
                      "Information drawing leaked ImGui backend/atlas state into the main window");
                main_frame(renderer, main_window);
            }
        if (SDL_GetKeyboardFocus() == panel.window() && SDL_CaptureMouse(true))
        {
            auto *cursor            = SDL_GetCursor();
            const bool was_captured = (SDL_GetWindowFlags(panel.window()) & SDL_WINDOW_MOUSE_CAPTURE) != 0;
            main_frame(renderer, main_window);
            const bool captured    = (SDL_GetWindowFlags(panel.window()) & SDL_WINDOW_MOUSE_CAPTURE) != 0;
            const bool same_cursor = SDL_GetCursor() == cursor;
            SDL_CaptureMouse(false);
            check(captured == was_captured && same_cursor,
                  "Background main frame changed the active Info window's mouse capture/cursor");
        }
        SDL_Event event{};
        event.type            = SDL_EVENT_WINDOW_RESIZED;
        event.window.windowID = id;
        event.window.data1    = 420;
        event.window.data2    = 440;
        check(panel.process_event(event), "Information resize event was not routed to its own context");
        int width = 0, height = 0;
        check(SDL_GetWindowSize(main_window, &width, &height), SDL_GetError());
        check(width == main_width && height == main_height && SDL_GetWindowFlags(main_window) == main_flags,
              "Information window changed main size or modality");
        event              = {};
        event.type         = SDL_EVENT_KEY_DOWN;
        event.key.windowID = id;
        event.key.key      = SDLK_I;
        event.key.repeat   = true;
        check(panel.process_event(event) && panel.visible(), "Repeated I toggled the information window");
        event.key.repeat = false;
        event.key.mod    = SDL_KMOD_CTRL;
        check(panel.process_event(event) && panel.visible(), "Modified I toggled the information window");
        event.key.mod = SDL_KMOD_NONE;
        check(panel.process_event(event) && !panel.visible(), "I did not close the information window");
        panel.show();
        check(SDL_GetWindowID(panel.window()) == id, "Reopening recreated the information window");
        check(SDL_GetWindowSize(panel.window(), &width, &height) && width == 420 && height == 440,
              "Reopening lost the desktop window size");
        event.key.key = SDLK_ESCAPE;
        check(panel.process_event(event) && !panel.visible(), "Escape did not close just the panel");
        panel.show();
        event                 = {};
        event.type            = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = main_id;
        check(!panel.process_event(event) && panel.visible(), "Information window consumed the main close event");
        event.window.windowID = id;
        check(panel.process_event(event) && !panel.visible(), "Native close did not hide the panel");
        check(SDL_GetWindowFromID(main_id) == main_window && SDL_GetRenderer(main_window) == main_device &&
                  ImGui::GetCurrentContext() == main_context,
              "Closing the information window affected the main window");
    }
    main_frame(renderer, main_window);
    std::puts("PASS independent nonmodal desktop Info, native move/resize, context isolation, I/Esc/close and reopen");
}
} // namespace
int main()
{
    SDL_Window *window = nullptr;
    bool platform      = false;
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("Information parent", 320, 240, SDL_WINDOW_HIDDEN);
        check(window != nullptr, SDL_GetError());
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = ImGui::GetIO().LogFilename = nullptr;
        auto renderer = render::create_renderer({render::SurfaceKind::sdl_window, window, 1024 * 1024, false});
        platform      = initialize_imgui_platform(window);
        check(platform, "Main ImGui platform");
        main_frame(*renderer, window);
        exercise(window, *renderer);
        renderer.reset();
        ImGui_ImplSDL3_Shutdown();
        platform = false;
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Information window regression failed: %s\n", error.what());
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
