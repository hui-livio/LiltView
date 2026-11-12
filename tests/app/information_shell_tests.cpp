#include "ui/image_information.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
class LayoutRenderer final : public liltview::render::Renderer
{
public:
    unsigned image_calls = 0;
    liltview::render::Capabilities capabilities() const override
    {
        return {liltview::render::Backend::opengl3, 4096, 128 * 1024 * 1024, 0};
    }
    liltview::render::Size framebuffer_size() const override
    {
        return {1280, 960};
    }
    liltview::render::Image create_image(liltview::render::ImageDesc) override
    {
        return {1, next_++, 1};
    }
    void update_image(liltview::render::Image, liltview::render::Rect, liltview::render::Pixels) override
    {
    }
    void destroy_image(liltview::render::Image) noexcept override
    {
    }
    void add_image(ImDrawList *, liltview::render::Image image, liltview::render::Point, liltview::render::Point,
                   liltview::render::Point, liltview::render::Point) override
    {
        if (image)
            ++image_calls;
    }
    void add_image_quad(ImDrawList *, liltview::render::Image image, const std::array<liltview::render::Point, 4> &,
                        const std::array<liltview::render::Point, 4> &) override
    {
        if (image)
            ++image_calls;
    }
    void begin_frame() override
    {
    }
    void render_frame(ImDrawData *) override
    {
    }
    void present() override
    {
    }
    liltview::render::Readback readback() override
    {
        return {};
    }

private:
    std::uint32_t next_ = 1;
};

struct Gui
{
    Gui()
    {
        if (!SDL_Init(SDL_INIT_EVENTS))
            throw std::runtime_error(SDL_GetError());
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.IniFilename = io.LogFilename = nullptr;
        io.DeltaTime                    = 1.0f / 60;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
    }
    ~Gui()
    {
        ImGui::DestroyContext();
        SDL_Quit();
    }
};

// Sample the solid geometry at the window corners using the backend's usual
// source-alpha blending. Tests disable textured AA lines so no font-atlas
// sampling is needed; text and icons are outside the sampled corner fringe.
ImVec4 corner_pixel(const ImDrawData &data, ImVec2 point)
{
    ImVec4 pixel{}; // Premultiplied RGB over a transparent framebuffer.
    const auto cross = [](ImVec2 a, ImVec2 b, ImVec2 c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    };
    for (const auto *list : data.CmdLists)
        for (const auto &command : list->CmdBuffer)
        {
            if (command.UserCallback || point.x < command.ClipRect.x || point.y < command.ClipRect.y ||
                point.x >= command.ClipRect.z || point.y >= command.ClipRect.w)
                continue;
            for (unsigned i = 0; i < command.ElemCount; i += 3)
            {
                const auto &a = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i]];
                const auto &b = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i + 1]];
                const auto &c = list->VtxBuffer[command.VtxOffset + list->IdxBuffer[command.IdxOffset + i + 2]];
                const float area = cross(a.pos, b.pos, c.pos);
                if (std::abs(area) < 0.00001f)
                    continue;
                const float u = cross(b.pos, c.pos, point) / area;
                const float v = cross(c.pos, a.pos, point) / area;
                const float w = 1 - u - v;
                if (u < 0 || v < 0 || w < 0)
                    continue;
                const auto ca = ImGui::ColorConvertU32ToFloat4(a.col);
                const auto cb = ImGui::ColorConvertU32ToFloat4(b.col);
                const auto cc = ImGui::ColorConvertU32ToFloat4(c.col);
                const float alpha = u * ca.w + v * cb.w + w * cc.w;
                pixel = {(u * ca.x + v * cb.x + w * cc.x) * alpha + pixel.x * (1 - alpha),
                         (u * ca.y + v * cb.y + w * cc.y) * alpha + pixel.y * (1 - alpha),
                         (u * ca.z + v * cb.z + w * cc.z) * alpha + pixel.z * (1 - alpha),
                         alpha + pixel.w * (1 - alpha)};
            }
        }
    return pixel;
}

struct Source
{
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("liltview-information-shell-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / (std::string(100, 'x') + "-中文-%s-100%.bmp");
    Source()
    {
        std::filesystem::create_directories(directory);
        const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "red.bmp";
        std::filesystem::copy_file(fixture, path);
    }
    ~Source()
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};
} // namespace

TEST_CASE("full information shell keeps the image navigation panel and footer disjoint at small sizes and DPI",
          "[image_details][shell]")
{
    using namespace liltview;
    Source source;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    viewer.open(source.path);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!viewer.settled())
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        viewer.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(viewer.failed());
    Shell shell;
    shell.toggle_image_info();
    for (const auto language : {Language::english, Language::chinese})
        for (const float scale : {1.0f, 1.25f, 2.0f})
            for (const bool native_titlebar : {false, true})
                for (const bool dark : {false, true})
                {
                    CAPTURE(scale, native_titlebar, dark);
                    apply_style(dark, scale);
                    auto &io                   = ImGui::GetIO();
                    io.DisplaySize             = {640, 480};
                    io.DisplayFramebufferScale = {scale, scale};
                    ImGui::NewFrame();
                    WindowChromeState chrome;
                    chrome.system_titlebar = native_titlebar;
                    chrome.dark            = dark;
                    chrome.scale           = scale;
                    chrome.line_height     = ImGui::GetTextLineHeight();
                    const auto metrics     = window_chrome_metrics(io.DisplaySize, chrome);
                    const auto menu        = build_menu(Settings{}, language, true, false, true, true, false, false, true, true);
                    CHECK(shell.draw(language, menu, false, renderer, {}, viewer, chrome) == MenuAction::none);
                    ImGuiWindow *canvas = nullptr, *panel = nullptr;
                    for (auto *window : ImGui::GetCurrentContext()->Windows)
                    {
                        const auto name = std::string_view(window->Name);
                        if (window->Active && name.find("/canvas_") != std::string_view::npos)
                            canvas = window;
                        if (window->Active && name.find("/image_information_") != std::string_view::npos)
                            panel = window;
                    }
                    REQUIRE(canvas);
                    REQUIRE(panel);
                    CHECK(canvas->Size.x > 0);
                    CHECK(canvas->Pos.x + canvas->Size.x <= panel->Pos.x + 1);
                    CHECK(panel->Pos.x + panel->Size.x <= 640);
                    CHECK(canvas->Pos.y + canvas->Size.y <= metrics.status_top() + 1);
                    CHECK(panel->Pos.y + panel->Size.y <= metrics.status_top() + 1);
                    const auto buttons = navigation_button_layout(canvas->InnerRect.Min, canvas->InnerRect.Max, scale);
                    REQUIRE(buttons.valid);
                    CHECK(buttons.next.max.x <= panel->Pos.x + 1);
                    CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
                    ImGui::Render();
                    CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
                    // Sample blank margins (no text/icons) to verify custom-drawn surfaces
                    // use the applied palette in native and custom chrome modes alike.
                    const auto &p = current_ui_palette();
                    const auto &data = *ImGui::GetDrawData();
                    const auto check_color = [](ImVec4 actual, ImVec4 expected)
                    {
                        CHECK(std::abs(actual.x - expected.x) < 0.005f);
                        CHECK(std::abs(actual.y - expected.y) < 0.005f);
                        CHECK(std::abs(actual.z - expected.z) < 0.005f);
                        CHECK(actual.w > 0.99f);
                    };
                    check_color(corner_pixel(data, {panel->Pos.x + 5.371f * scale, panel->Pos.y + 80.213f * scale}), p.surface);
                    check_color(corner_pixel(data, {canvas->Pos.x + 5.371f * scale,
                                                   canvas->Pos.y + canvas->Size.y - 8.213f * scale}), p.canvas);
                    check_color(corner_pixel(data, {320.371f, metrics.status_top() + 2.213f * scale}), p.chrome);
                    // The panel's ChildBg override must not leak into the next frame/canvas.
                    CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ChildBg]) ==
                          ImGui::ColorConvertFloat4ToU32(p.canvas));
                    REQUIRE(viewer.image_details());
                    CHECK(viewer.image_details()->path == source.path);
                }
}

TEST_CASE("rounded shell corners have only border-colored antialiasing without background halos", "[chrome][rounding][shell]")
{
    using namespace liltview;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    Shell shell;
    for (const bool dark : {false, true})
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
            for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
            {
                CAPTURE(dark, scale, density);
                apply_style(dark, scale);
                ImGui::GetStyle().AntiAliasedLinesUseTex = false;
                auto &io                   = ImGui::GetIO();
                io.DisplaySize             = {640, 480};
                io.DisplayFramebufferScale = {density, density};
                ImGui::NewFrame();
                WindowChromeState chrome;
                chrome.dark        = dark;
                chrome.scale       = scale;
                chrome.line_height = ImGui::GetTextLineHeight();
                const auto metrics = window_chrome_metrics(io.DisplaySize, chrome);
                const auto menu    = build_menu(Settings{}, Language::english, true, false, true, true, false, false, true, true);
                const float saved_error = ImGui::GetDrawListSharedData()->CircleSegmentMaxError;
                CHECK(shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome) == MenuAction::none);
                CHECK(ImGui::GetDrawListSharedData()->CircleSegmentMaxError == saved_error);
                ImGui::Render();
                const auto &data = *ImGui::GetDrawData();
                const auto border = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(window_border_color()));
                for (unsigned corner = 0; corner < 4; ++corner)
                {
                    CAPTURE(corner);
                    const bool right = (corner & 1) != 0, bottom = (corner & 2) != 0;
                    // Small offsets avoid double-counting shared triangle edges.
                    const auto sample = [&](float x, float y)
                    {
                        return corner_pixel(data, {right ? 640 - x : x, bottom ? 480 - y : y});
                    };
                    CHECK(sample(0.501f / density, 0.502f / density).w == 0);
                    int checked = 0;
                    const int extent = static_cast<int>(std::ceil(metrics.radius * density));
                    for (int y = 0; y < extent; ++y)
                        for (int x = 0; x < extent; ++x)
                        {
                            const float px = (static_cast<float>(x) + 0.501f) / density;
                            const float py = (static_cast<float>(y) + 0.502f) / density;
                            const float distance = std::hypot(metrics.radius - px, metrics.radius - py);
                            if (distance < metrics.radius - 0.25f || distance > metrics.radius + 0.75f)
                                continue;
                            const auto pixel = sample(px, py);
                            if (pixel.w < 0.01f)
                                continue;
                            CAPTURE(x, y, pixel.w);
                            CHECK(std::abs(pixel.x / pixel.w - border.x) < 0.01f);
                            CHECK(std::abs(pixel.y / pixel.w - border.y) < 0.01f);
                            CHECK(std::abs(pixel.z / pixel.w - border.z) < 0.01f);
                            ++checked;
                        }
                    CHECK(checked > 0);
                    // A coarse arc can be antialiased yet still visibly faceted.
                    // Its half-coverage contour must follow the ideal circle to
                    // within 0.12 framebuffer pixels (not 0.12 logical units).
                    for (int step = 1; step < 16; ++step)
                    {
                        const float angle = static_cast<float>(step) * 1.570796327f / 16;
                        const auto edge   = sample(metrics.radius * (1 - std::cos(angle)) + 0.0001f,
                                                   metrics.radius * (1 - std::sin(angle)) + 0.0002f);
                        CAPTURE(step, edge.w);
                        CHECK(std::abs(edge.w - 0.5f) < 0.12f / density);
                    }
                }
            }
}

TEST_CASE("square shell corners remain opaque and have uninterrupted borders", "[chrome][rounding][shell]")
{
    using namespace liltview;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    Shell shell;
    for (const bool dark : {false, true})
        for (const bool maximized : {false, true})
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
                for (const float density : {1.0f, 1.25f, 1.5f, 2.0f})
                {
                    CAPTURE(dark, maximized, scale, density);
                    apply_style(dark, scale);
                    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
                    auto &io                                 = ImGui::GetIO();
                    io.DisplaySize                           = {640, 480};
                    io.DisplayFramebufferScale               = {density, density};
                    ImGui::NewFrame();
                    WindowChromeState chrome;
                    chrome.dark  = dark;
                    chrome.scale = scale;
                    // Maximizing must also give square corners with rounding enabled.
                    chrome.rounded     = maximized;
                    chrome.maximized   = maximized;
                    chrome.line_height = ImGui::GetTextLineHeight();
                    const auto metrics = window_chrome_metrics(io.DisplaySize, chrome);
                    REQUIRE(metrics.radius == 0);
                    const auto menu =
                        build_menu(Settings{}, Language::english, true, false, true, true, false, false, true, true);
                    shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome);
                    ImGui::Render();
                    const auto &data  = *ImGui::GetDrawData();
                    const auto border = ImGui::ColorConvertU32ToFloat4(ImGui::GetColorU32(window_border_color()));
                    for (unsigned corner = 0; corner < 4; ++corner)
                        for (int y = 0; y < 6; ++y)
                            for (int x = 0; x < 6; ++x)
                            {
                                CAPTURE(corner, x, y);
                                const float px = (x + 0.501f) / density, py = (y + 0.502f) / density;
                                const auto pixel =
                                    corner_pixel(data, {(corner & 1) ? 640 - px : px, (corner & 2) ? 480 - py : py});
                                CHECK(std::abs(pixel.w - 1) < 0.001f);
                                if (px < metrics.border || py < metrics.border)
                                {
                                    CHECK(std::abs(pixel.x - border.x) < 0.01f);
                                    CHECK(std::abs(pixel.y - border.y) < 0.01f);
                                    CHECK(std::abs(pixel.z - border.z) < 0.01f);
                                }
                            }
                }
}

TEST_CASE("main window keeps its original flags and resize regions through resize and theme changes", "[chrome][resize][shell]")
{
    using namespace liltview;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    Shell shell;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
        for (const bool rounded : {true, false})
            for (const auto size : {ImVec2{640, 480}, ImVec2{1280, 960}, ImVec2{853, 617}, ImVec2{640, 480}})
                for (const bool dark : {false, true})
                {
                    CAPTURE(scale, rounded, size.x, size.y, dark);
                    apply_style(dark, scale);
                    ImGui::GetIO().DisplaySize = size;
                    ImGui::NewFrame();
                    WindowChromeState chrome;
                    chrome.dark = dark;
                    chrome.scale = scale;
                    chrome.rounded = rounded;
                    const auto menu = build_menu(Settings{}, Language::english, true, false, true, true, false, false, true, true);
                    CHECK(shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome) == MenuAction::none);
                    const auto *main = ImGui::FindWindowByName("LiltView###main");
                    REQUIRE(main);
                    CHECK((main->Flags & ImGuiWindowFlags_NoBackground) == 0);
                    CHECK((main->Flags & ImGuiWindowFlags_NoResize) != 0); // Native resize, not ImGui resize grips.
                    CHECK(main->Size.x == size.x);
                    CHECK(main->Size.y == size.y);
                    const auto m = window_chrome_metrics(size, chrome);
                    const float corner = std::max(m.grip, m.radius) - 1;
                    CHECK(m.grip == 6 * scale);
                    CHECK(window_chrome_hit(m, {0, size.y / 2}) == WindowHit::left);
                    CHECK(window_chrome_hit(m, {size.x - 1, size.y / 2}) == WindowHit::right);
                    CHECK(window_chrome_hit(m, {size.x / 2, 0}) == WindowHit::top);
                    CHECK(window_chrome_hit(m, {size.x / 2, size.y - 1}) == WindowHit::bottom);
                    CHECK(window_chrome_hit(m, {corner, corner}) == WindowHit::top_left);
                    CHECK(window_chrome_hit(m, {size.x - 1 - corner, corner}) == WindowHit::top_right);
                    CHECK(window_chrome_hit(m, {corner, size.y - 1 - corner}) == WindowHit::bottom_left);
                    CHECK(window_chrome_hit(m, {size.x - 1 - corner, size.y - 1 - corner}) == WindowHit::bottom_right);
                    CHECK(window_chrome_hit(m, {m.grip, size.y / 2}) == WindowHit::normal);
                    CHECK(window_chrome_hit(m, {size.x - m.grip - 1, size.y / 2}) == WindowHit::normal);
                    CHECK(window_chrome_hit(m, {size.x / 2, m.title_bottom() / 2}) == WindowHit::drag);
                    CHECK(window_chrome_hit(m, {size.x / 2, m.status_top() + m.status_height / 2}) == WindowHit::drag);
                    ImGui::Render();
                }
}

TEST_CASE("failed replacement shows a failure illustration and its reason below instead of the old image",
          "[shell][image_failure]")
{
    using namespace liltview;
    Source source;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    const auto wait = [&]
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!viewer.settled())
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            viewer.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto unsupported = source.directory / "unsupported-%s.xyz";
    std::ofstream(unsupported, std::ios::binary) << "not an image";
    const auto corrupt = source.directory / "corrupt.bmp";
    std::filesystem::copy_file(source.path, corrupt);
    std::filesystem::resize_file(corrupt, 24);
    const std::array failures{std::pair{source.directory / "missing.png", Text::image_io_error},
                              std::pair{unsupported, Text::unsupported_format}, std::pair{corrupt, Text::image_corrupt}};
    for (const auto &[path, reason] : failures)
    {
        viewer.open(source.path);
        wait();
        REQUIRE_FALSE(viewer.failed());
        Shell shell;
        apply_style(false, 1);
        ImGui::GetIO().DisplaySize = {640, 480};
        ImGui::NewFrame();
        WindowChromeState chrome;
        chrome.system_titlebar = true;
        chrome.line_height     = ImGui::GetTextLineHeight();
        const auto menu        = build_menu(Settings{}, Language::english, true, false, true, true, false, false, true, true);
        renderer.image_calls   = 0;
        shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome);
        ImGui::Render();
        REQUIRE(renderer.image_calls > 0);
        viewer.open(path);
        wait();
        REQUIRE(viewer.failed());
        // The old scene may remain cached internally, but must never be drawn
        // or described as the failed replacement in the information panel.
        for (const auto language : {Language::english, Language::chinese})
            for (const bool dark : {false, true})
                for (const float scale : {1.0f, 1.25f, 2.0f})
                    for (const bool native : {false, true})
                        for (const bool panel : {false, true})
                        {
                            CAPTURE(path, language, dark, scale, native, panel);
                            apply_style(dark, scale);
                            chrome.system_titlebar = native;
                            chrome.dark            = dark;
                            chrome.scale           = scale;
                            if (shell.image_info_open() != panel)
                                shell.toggle_image_info();
                            ImGui::NewFrame();
                            chrome.line_height = ImGui::GetTextLineHeight();
                            ImGui::LogToBuffer();
                            // Keep logging across child-window End calls, then
                            // inspect before LogFinish clears the text buffer.
                            ImGui::GetCurrentContext()->LogWindow = nullptr;
                            renderer.image_calls                  = 0;
                            shell.draw(language, menu, false, renderer, {}, viewer, chrome);
                            const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
                            ImGui::LogFinish();
                            ImGui::Render();
                            CHECK(renderer.image_calls == 0);
                            const auto title   = text.find(translate(language, Text::image_load_failed));
                            const auto message = text.find(translate(language, reason));
                            REQUIRE(title != std::string::npos);
                            REQUIRE(message != std::string::npos);
                            CHECK(message > title);
                            CHECK(text.substr(title, message - title).find('\n') != std::string::npos);
                            CHECK(text.find(std::string(100, 'x')) == std::string::npos);
                            if (panel)
                                CHECK(text.find(translate(language, Text::no_image)) != std::string::npos);
                        }
    }
}
