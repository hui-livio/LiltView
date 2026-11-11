#include "platform/live_resize.hpp"
#include "platform/sdl_render_host.hpp"
#include "platform/window_shape.hpp"
#include "resources/icon_rgba.hpp"
#include "ui/shell.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
struct Graphics
{
    bool sdl = false, platform = false;
    SDL_Window *window = nullptr;
    std::unique_ptr<liltview::render::Renderer> renderer;
    std::unique_ptr<liltview::Viewer> viewer;
    std::unique_ptr<liltview::WindowShape> shape;
    liltview::render::Image icon{};
    ~Graphics()
    {
        shape.reset();
        viewer.reset();
        if (renderer)
        {
            renderer->destroy_image(icon);
            renderer.reset();
        }
        if (platform)
            ImGui_ImplSDL3_Shutdown();
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        if (sdl)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
};

// Glyphs must actually reach the NEW drawable edge, not merely exist in an
// ImGui draw list still clipped by an obsolete native region / framebuffer.
bool glyph_visible(const liltview::render::Readback &pixels, float x, float y, float density, bool dark)
{
    const int px    = static_cast<int>(std::round(x * density));
    const int py    = static_cast<int>(std::round(y * density));
    const int reach = static_cast<int>(std::ceil(2 * density));
    for (int row = std::max(0, py - reach); row <= std::min(static_cast<int>(pixels.size.height) - 1, py + reach); ++row)
        for (int col = std::max(0, px - reach); col <= std::min(static_cast<int>(pixels.size.width) - 1, px + reach); ++col)
        {
            const auto offset     = (static_cast<std::size_t>(row) * pixels.size.width + col) * 3;
            const auto brightness = (static_cast<int>(pixels.rgb[offset]) + pixels.rgb[offset + 1] + pixels.rgb[offset + 2]) / 3;
            if (dark ? brightness > 100 : brightness < 180)
                return true;
        }
    return false;
}
} // namespace

TEST_CASE("Windows live sizing presents title controls and full chrome at each new drawable size",
          "[windows][chrome][live-resize][gpu]")
{
    using namespace liltview;
    Graphics graphics;
    REQUIRE(SDL_Init(SDL_INIT_VIDEO));
    graphics.sdl     = true;
    const auto flags = prepare_render_window();
    graphics.window =
        SDL_CreateWindow("LiltView live sizing pixel test", 640, 480,
                         flags | SDL_WINDOW_HIDDEN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE | SDL_WINDOW_TRANSPARENT);
    REQUIRE(graphics.window);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().Fonts->AddFontDefault();
    try
    {
        graphics.renderer = render::create_renderer({render::SurfaceKind::sdl_window, graphics.window, 128 * 1024 * 1024, false});
    } catch (const render::Error &error)
    {
        SKIP("OpenGL 3.3 is unavailable in this Windows session: " << error.what());
    }
    REQUIRE(initialize_imgui_platform(graphics.window));
    graphics.platform = true;
    graphics.viewer   = std::make_unique<Viewer>(*graphics.renderer);
    graphics.shape    = std::make_unique<WindowShape>();
    const render::Size icon_size{resources::icon_size, resources::icon_size};
    graphics.icon = graphics.renderer->create_image({icon_size, true});
    graphics.renderer->update_image(
        graphics.icon, {0, 0, icon_size.width, icon_size.height},
        {resources::icon_rgba.data(), resources::icon_rgba.size(), icon_size.width * 4ULL, icon_size});
    Shell shell;
    const auto menu = build_menu(Settings{}, Language::english, true, false, false, false, false, false, false, false);
    WindowChromeState chrome;
    render::Readback pixels;
    WindowChromeMetrics metrics{};
    int frames      = 0;
    const auto draw = [&]
    {
        if (!graphics.shape->apply(graphics.window, chrome.rounded ? 12.0f * chrome.scale : 0.0f))
            throw std::runtime_error(SDL_GetError());
        graphics.renderer->begin_frame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        metrics = window_chrome_metrics(ImGui::GetMainViewport()->Size, chrome);
        shell.draw(Language::english, menu, false, *graphics.renderer, graphics.icon, *graphics.viewer, chrome);
        ImGui::Render();
        graphics.renderer->render_frame(ImGui::GetDrawData());
        pixels = graphics.renderer->readback();
        if (const auto *directory = SDL_getenv("LILTVIEW_TEST_RESIZE_CAPTURE"))
        {
            const auto name = std::string(chrome.dark ? "dark-" : "light-") + (chrome.rounded ? "rounded-" : "square-") +
                std::to_string(chrome.scale) + "-" + std::to_string(pixels.size.width) + "x" +
                std::to_string(pixels.size.height) + ".ppm";
            std::filesystem::create_directories(directory);
            std::ofstream output(std::filesystem::path(directory) / name, std::ios::binary);
            output << "P6\n" << pixels.size.width << ' ' << pixels.size.height << "\n255\n";
            output.write(reinterpret_cast<const char *>(pixels.rgb.data()), static_cast<std::streamsize>(pixels.rgb.size()));
            if (!output)
                throw std::runtime_error("Cannot capture live resize diagnostic frame");
        }
        graphics.renderer->present();
        ++frames;
    };
    apply_style(false, 1);
    draw(); // Build font textures and establish the initial viewport.
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(graphics.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    REQUIRE(hwnd);
    struct Sizing
    {
        HWND window;
        explicit Sizing(HWND hwnd) :
            window(hwnd)
        {
            SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0);
        }
        ~Sizing()
        {
            SendMessageW(window, WM_EXITSIZEMOVE, 0, 0);
        }
    } sizing(hwnd);
    LiveResizeRedraw live(SDL_GetWindowID(graphics.window), draw);
    for (const bool dark : {false, true})
        for (const bool rounded : {true, false})
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
                for (const auto size : {SIZE{960, 640}, SIZE{480, 320}, SIZE{800, 600}, SIZE{640, 480}})
                {
                    CAPTURE(dark, rounded, scale, size.cx, size.cy);
                    chrome.scale   = scale;
                    chrome.dark    = dark;
                    chrome.rounded = rounded;
                    apply_style(dark, scale);
                    REQUIRE(SDL_SetWindowSize(graphics.window, size.cx, size.cy));
                    const int before    = frames;
                    const auto deadline = SDL_GetTicks() + 2000;
                    // Real SDL native sizing timers, with no outer SDL event pump.
                    while (frames == before && SDL_GetTicks() < deadline)
                    {
                        MSG message{};
                        if (PeekMessageW(&message, hwnd, WM_TIMER, WM_TIMER, PM_REMOVE))
                            DispatchMessageW(&message);
                        else
                            SDL_Delay(1);
                    }
                    live.rethrow_if_failed();
                    REQUIRE(frames > before);
                    int width = 0, height = 0, pixel_width = 0, pixel_height = 0;
                    REQUIRE(SDL_GetWindowSize(graphics.window, &width, &height));
                    REQUIRE(SDL_GetWindowSizeInPixels(graphics.window, &pixel_width, &pixel_height));
                    CHECK(metrics.width == width);
                    CHECK(metrics.height == height);
                    REQUIRE(pixels.size.width == static_cast<std::uint32_t>(pixel_width));
                    REQUIRE(pixels.size.height == static_cast<std::uint32_t>(pixel_height));
                    const float density  = SDL_GetWindowPixelDensity(graphics.window);
                    const float center_y = metrics.border + metrics.title_height * 0.5f;
                    for (unsigned button = 0; button < 3; ++button)
                    {
                        CAPTURE(button);
                        const float center_x =
                            metrics.controls_left() + (static_cast<float>(button) + 0.5f) * metrics.button_width;
                        CHECK(glyph_visible(pixels, center_x + (button == 1 ? 5.0f * scale : 0.0f), center_y, density, dark));
                    }
                    bool footer_visible = false;
                    for (int offset = 20; offset <= 70; offset += 5)
                        footer_visible |= glyph_visible(pixels, metrics.width - static_cast<float>(offset) * scale,
                                                        metrics.status_top() + metrics.status_height * 0.5f, density, dark);
                    CHECK(footer_visible); // The right-aligned language label follows the resized bottom edge.
                    bool title_visible = false;
                    for (int x = 50; x <= 110; x += 4)
                        title_visible |= glyph_visible(pixels, static_cast<float>(x) * scale, center_y, density, dark);
                    CHECK(title_visible); // Scan the text span rather than depending on one font's glyph gaps.
                    REQUIRE(!pixels.alpha.empty());
                    // Right and bottom edges must be part of THIS frame, not empty
                    // alpha left behind by the previous drawable's dimensions.
                    CHECK(pixels.alpha[static_cast<std::size_t>(pixel_height / 2) * pixel_width + pixel_width - 1] == 255);
                    CHECK(pixels.alpha[static_cast<std::size_t>(pixel_height - 1) * pixel_width + pixel_width / 2] == 255);
                    const auto region = CreateRectRgn(0, 0, 0, 0);
                    REQUIRE(region);
                    const auto kind = GetWindowRgn(hwnd, region);
                    RECT bounds{};
                    GetRgnBox(region, &bounds);
                    if (rounded)
                    {
                        // Framebuffer-only checks miss HRGN clipping: every partially
                        // covered corner pixel must also survive the native region.
                        const int extent = static_cast<int>(std::ceil(metrics.radius * density));
                        for (unsigned corner = 0; corner < 4; ++corner)
                        {
                            int partial = 0;
                            for (int y = 0; y < extent; ++y)
                                for (int x = 0; x < extent; ++x)
                                {
                                    const int px     = (corner & 1) ? pixel_width - 1 - x : x;
                                    const int py     = (corner & 2) ? pixel_height - 1 - y : y;
                                    const auto alpha = pixels.alpha[static_cast<std::size_t>(py) * pixel_width + px];
                                    if (alpha == 0 || alpha == 255)
                                        continue;
                                    CAPTURE(corner, px, py, alpha);
                                    CHECK(PtInRegion(region, px, py));
                                    ++partial;
                                }
                            CHECK(partial > 0);
                        }
                    }
                    DeleteObject(region);
                    if (rounded)
                    {
                        CHECK(kind == COMPLEXREGION);
                        CHECK(bounds.right == pixel_width);
                        CHECK(bounds.bottom == pixel_height);
                    }
                    else
                        CHECK(kind == ERROR);
                }
}
