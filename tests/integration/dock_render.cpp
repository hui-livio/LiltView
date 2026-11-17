// Opt-in GPU/UI regression. Uses a hidden SDL window, synthetic ImGui inputs,
// the production Shell/Viewer/renderer, and optional PPM captures for visual QA.
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <imgui_internal.h>
#include <stdexcept>
#include <thread>
#include "platform/fonts.hpp"
#include "platform/sdl_render_host.hpp"
#include "resources/icon_rgba.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"
#include "viewer/viewer.hpp"

using namespace liltview;
namespace
{
void check(bool condition, const char *reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}
void save(const std::filesystem::path &path, const render::Readback &pixels)
{
    std::ofstream stream(path, std::ios::binary);
    stream << "P6\n" << pixels.size.width << ' ' << pixels.size.height << "\n255\n";
    stream.write(reinterpret_cast<const char *>(pixels.rgb.data()), static_cast<std::streamsize>(pixels.rgb.size()));
    check(stream.good(), "Cannot save dock capture");
}
struct Scenario
{
    render::Renderer &renderer;
    Viewer viewer{renderer};
    Shell shell;
    render::Image icon;
    Scenario(render::Renderer &value) :
        renderer(value)
    {
        const render::Size size{resources::icon_size, resources::icon_size};
        icon = renderer.create_image({size, true});
        renderer.update_image(icon, {0, 0, size.width, size.height},
                              {resources::icon_rgba.data(), resources::icon_rgba.size(), size.width * 4ULL, size});
    }
    ~Scenario()
    {
        renderer.destroy_image(icon);
    }
    WindowChromeState chrome;
    DockLayout layout;
    Language language = Language::chinese;
    MenuAction frame()
    {
        viewer.pump();
        renderer.begin_frame();
        auto &io                   = ImGui::GetIO();
        const auto size            = renderer.framebuffer_size();
        io.DisplaySize             = {static_cast<float>(size.width), static_cast<float>(size.height)};
        io.DisplayFramebufferScale = {1, 1};
        io.DeltaTime               = .01f;
        ImGui::NewFrame();
        chrome.line_height = ImGui::GetTextLineHeight();
        const auto menu = build_menu(Settings{}, language, true, chrome.fullscreen, viewer.has_image(), viewer.can_actual_size(),
                                     viewer.navigation().can_previous, viewer.navigation().can_next, shell.image_info_open(),
                                     viewer.has_visible_image());
        const auto action = shell.draw(language, menu, false, renderer, icon, viewer, chrome);
        for (const auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos &&
                std::string_view(window->Name).find("/image_information_") == std::string_view::npos)
                layout =
                    dock_layout(window->WorkRect.Min, window->WorkRect.Max, chrome.scale, viewer.fitting(), chrome.fullscreen);
        ImGui::Render();
        renderer.render_frame(ImGui::GetDrawData());
        return action;
    }
    void frames(int count)
    {
        for (int i = 0; i < count; ++i)
            check(frame() == MenuAction::none, "Unexpected UI action during idle frames");
    }
    void settle()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do
        {
            frame();
            check(!viewer.failed(), "Image load failed");
            check(std::chrono::steady_clock::now() < deadline, "Image loading timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!viewer.interaction_ready());
    }
};
void exercise(render::Renderer &renderer, const std::filesystem::path &captures)
{
    Scenario s{renderer};
    s.viewer.open(std::filesystem::path(LILTVIEW_SOURCE_DIR) / "assets/preview/image-view-light-zh-cn.jpg");
    s.settle();
    for (const bool dark : {true, false})
        for (const bool fullscreen : {false, true})
            for (const bool information : {false, true})
            {
                apply_style(dark, 1);
                s.chrome.dark       = dark;
                s.chrome.fullscreen = fullscreen;
                s.shell.dismiss_image_info();
                auto &io = ImGui::GetIO();
                io.AddMousePosEvent(100, 100);
                s.frames(15);
                if (information)
                    s.shell.toggle_image_info();
                s.frames(2);
                const auto before      = renderer.readback();
                const auto view        = s.viewer.presented_view();
                const std::string stem = std::string(dark ? "dark" : "light") + (fullscreen ? "-fullscreen" : "-window") +
                    (information ? "-information" : "");
                if (!captures.empty())
                    save(captures / (stem + "-hidden.ppm"), before);
                io.AddMousePosEvent((s.layout.trigger.min.x + s.layout.trigger.max.x) / 2, s.layout.trigger.max.y - 4);
                s.frames(36);
                const auto shown = renderer.readback();
                check(s.viewer.presented_view().same_view(view), "Dock changed the image viewport/FIT");
                unsigned changed = 0;
                for (unsigned y = 0; y < shown.size.height; ++y)
                    for (unsigned x = 0; x < shown.size.width; ++x)
                    {
                        const auto i = (static_cast<std::size_t>(y) * shown.size.width + x) * 3;
                        if (shown.rgb[i] == before.rgb[i] && shown.rgb[i + 1] == before.rgb[i + 1] &&
                            shown.rgb[i + 2] == before.rgb[i + 2])
                            continue;
                        ++changed;
                        const DockRect fringe{{s.layout.bounds.min.x - 1, s.layout.bounds.min.y - 1},
                                              {s.layout.bounds.max.x + 1, s.layout.bounds.max.y + 1}};
                        check(fringe.contains({static_cast<float>(x), static_cast<float>(y)}),
                              "Dock changed pixels outside its bounds");
                    }
                check(changed > 1000, "Dock did not render into the GPU framebuffer");
                if (!captures.empty())
                    save(captures / (stem + "-shown.ppm"), shown);
                io.AddMousePosEvent(100, 100);
                s.frames(15);
                check(renderer.readback().rgb == before.rgb, "Hidden dock left pixels/layout behind");
                std::printf("PASS %s: overlay-only %u changed pixels; hidden framebuffer identical\n", stem.c_str(), changed);
            }
    s.shell.dismiss_image_info();
    s.chrome.fullscreen = false;
    s.frames(2);
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(300, 200);
    s.frames(40);
    io.AddMouseButtonEvent(0, true);
    s.frame();
    io.AddMouseButtonEvent(0, false);
    s.frame();
    io.AddMouseButtonEvent(0, true);
    check(s.frame() == MenuAction::actual_size, "Real rendered canvas double-click did not select 100%");
    s.viewer.actual_size();
    io.AddMouseButtonEvent(0, false);
    s.settle();
    io.AddMousePosEvent((s.layout.trigger.min.x + s.layout.trigger.max.x) / 2, s.layout.trigger.max.y - 4);
    s.frames(36);
    if (!captures.empty())
        save(captures / "light-actual-size-shown.ppm", renderer.readback());
    const auto &more = s.layout.buttons.back().rect;
    io.AddMousePosEvent((more.min.x + more.max.x) / 2, (more.min.y + more.max.y) / 2);
    s.frames(2);
    const auto steady = renderer.readback();
    const auto sample = (static_cast<std::size_t>(s.layout.bounds.min.y + 2) * steady.size.width +
                         static_cast<std::size_t>(s.layout.bounds.min.x + 24)) *
        3;
    io.AddMouseButtonEvent(0, true);
    for (int i = 0; i < 25; ++i)
    {
        s.frame();
        const auto held = renderer.readback();
        check(held.rgb[sample] == steady.rgb[sample] && held.rgb[sample + 1] == steady.rgb[sample + 1] &&
                  held.rgb[sample + 2] == steady.rgb[sample + 2],
              "Pressing a Dock button faded its background");
    }
    io.AddMouseButtonEvent(0, false);
    s.frame();
    s.frames(2);
    check(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1, "More did not open shared View menu");
    if (!captures.empty())
        save(captures / "light-more-menu.ppm", renderer.readback());
    std::puts("PASS rendered double-click mode/icon change and More popup");
}

void information_variants(render::Renderer &renderer, SDL_Window *window, const std::filesystem::path &captures)
{
    Scenario s{renderer};
    s.viewer.open(std::filesystem::path(LILTVIEW_SOURCE_DIR) / "assets/icons/preview-color.png");
    s.settle();
    struct Variant
    {
        int width, height;
        float scale;
        const char *name;
    };
    for (const auto &variant : {Variant{1120, 860, 1, "full"}, Variant{640, 480, 1, "small"}, Variant{1120, 760, 2, "2x"}})
        for (const bool dark : {false, true})
            for (const auto language : {Language::chinese, Language::english})
            {
                check(SDL_SetWindowSize(window, variant.width, variant.height), SDL_GetError());
                SDL_PumpEvents();
                apply_style(dark, variant.scale);
                s.chrome.scale      = variant.scale;
                s.chrome.dark       = dark;
                s.chrome.fullscreen = false;
                s.language          = language;
                s.shell.dismiss_image_info();
                ImGui::GetIO().AddMousePosEvent(100, 100);
                s.frames(3);
                const auto view = s.viewer.presented_view();
                s.shell.toggle_image_info();
                s.frames(3);
                ImGuiWindow *panel = nullptr, *body = nullptr;
                for (auto *child : ImGui::GetCurrentContext()->Windows)
                {
                    if (!child->Active)
                        continue;
                    const auto name = std::string_view(child->Name);
                    if (name.find("/image_information_body_") != std::string_view::npos)
                        body = child;
                    else if (name.find("/image_information_") != std::string_view::npos)
                        panel = child;
                }
                check(panel && body, "Info overlay and scroll body are missing");
                for (const auto *child : ImGui::GetCurrentContext()->Windows)
                    if (child->Active && std::string_view(child->Name).find("/canvas_") != std::string_view::npos &&
                        std::string_view(child->Name).find("/image_information_") == std::string_view::npos)
                    {
                        const auto arrows =
                            navigation_button_layout(child->WorkRect.Min, child->WorkRect.Max, variant.scale,
                                                     {panel->Pos, {panel->Pos.x + panel->Size.x, panel->Pos.y + panel->Size.y}});
                        check(arrows.previous_visible && arrows.next_visible, "Info hides an edge navigation button");
                        check(panel->Pos.x + panel->Size.x + 12 * variant.scale <= arrows.next.min.x,
                              "Info intrudes into the right navigation lane");
                    }
                check(panel->Pos.y + panel->Size.y < s.layout.bounds.min.y, "Info covers the Dock");
                check(panel->Scroll.y == 0 && !panel->ScrollbarY, "Info header scrolled with its body");
                check(s.viewer.presented_view().same_view(view), "Info changed the image camera");
                if (variant.scale > 1 || variant.height < 500)
                    check(body->ScrollMax.y > 0, "Small Info panel lost its scrollable details");
                const std::string stem = std::string(dark ? "dark" : "light") + "-information-" + variant.name +
                    (language == Language::chinese ? "-zh-cn" : "-en-us");
                if (!captures.empty())
                    save(captures / (stem + ".ppm"), renderer.readback());
                if (body->ScrollMax.y > 0)
                {
                    auto &io = ImGui::GetIO();
                    io.AddMousePosEvent(body->Pos.x + 40, body->Pos.y + 40);
                    s.frames(2);
                    io.AddMouseWheelEvent(0, -8);
                    s.frames(3);
                    check(body->Scroll.y > 0, "Info body cannot scroll");
                    check(panel->Scroll.y == 0, "Info header moved during body scroll");
                    check(s.viewer.presented_view().same_view(view), "Info wheel zoomed the image");
                    if (!captures.empty())
                        save(captures / (stem + "-scrolled.ppm"), renderer.readback());
                }
                auto &io = ImGui::GetIO();
                io.AddMousePosEvent(panel->Pos.x + panel->Size.x - 22 * variant.scale, panel->Pos.y + 22 * variant.scale);
                s.frames(2);
                io.AddMouseButtonEvent(0, true);
                s.frame();
                io.AddMouseButtonEvent(0, false);
                s.frame();
                check(!s.shell.image_info_open(), "Info close action did not dismiss the overlay");
                check(s.viewer.presented_view().same_view(view), "Closing Info changed the image camera");
                std::printf("PASS %s: camera preserved, fixed header, bounded overlay and close\n", stem.c_str());
            }
}
} // namespace
int main(int argc, char **argv)
{
    SDL_Window *window = nullptr;
    try
    {
        check(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("LiltView dock GPU regression", 1120, 760, prepare_render_window() | SDL_WINDOW_HIDDEN);
        check(window, SDL_GetError());
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.IniFilename = io.LogFilename  = nullptr;
        io.ConfigFlags                  |= ImGuiConfigFlags_NavEnableKeyboard;
        check(load_font_candidate(
                  {std::string(LILTVIEW_SOURCE_DIR) + "/assets/fonts/HarmonyOS_Sans_SC_Regular.ttf", 0, true, true}),
              "Cannot load bundled CJK font");
        auto renderer = render::create_renderer({render::SurfaceKind::sdl_window, window, 128 * 1024 * 1024, false});
        const std::filesystem::path captures = argc > 1 ? argv[1] : "";
        if (!captures.empty())
            std::filesystem::create_directories(captures);
        exercise(*renderer, captures);
        information_variants(*renderer, window, captures);
        renderer.reset();
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception &error)
    {
        std::fprintf(stderr, "Dock render: %s\n", error.what());
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
