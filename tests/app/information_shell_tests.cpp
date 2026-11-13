#include "ui/image_information.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_approx.hpp>
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
    unsigned image_calls = 0, upload_delay_ms = 0;
    bool fail_upload = false;
    liltview::render::Capabilities capabilities() const override
    {
        return {liltview::render::Backend::sdl_renderer, 4096, 128 * 1024 * 1024, 0};
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
        if (fail_upload)
        {
            fail_upload = false;
            throw std::runtime_error("Injected progressive upload failure");
        }
        if (upload_delay_ms)
            SDL_Delay(upload_delay_ms);
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

std::filesystem::path progressive_bmp(const Source &source, bool corrupt)
{
    constexpr std::uint32_t width = 2048, height = 2048;
    const auto path            = source.directory / (corrupt ? "progressive-corrupt.bmp" : "progressive.bmp");
    const std::uint32_t offset = corrupt ? 58 : 54, stride = width * (corrupt ? 1 : 3);
    std::array<unsigned char, 54> header{};
    const auto put = [&](unsigned at, std::uint32_t value)
    {
        for (unsigned i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    header[0] = 'B';
    header[1] = 'M';
    put(2, offset + stride * height);
    put(10, offset);
    put(14, 40);
    put(18, width);
    put(22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height))); // Top-down rows.
    header[26] = 1;
    header[28] = corrupt ? 8 : 24;
    put(34, stride * height);
    if (corrupt)
        put(46, 1); // Only palette index zero is valid.
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(header.data()), header.size());
    if (corrupt)
    {
        const char palette[]{0, 0, static_cast<char>(255), 0};
        output.write(palette, sizeof(palette));
    }
    std::string row(stride, '\0');
    for (unsigned y = 0; y < height; ++y)
    {
        if (corrupt && y == height - 1)
            row[0] = 1; // A real decoder error after multiple valid native row bands.
        output.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    REQUIRE(output.good());
    return path;
}
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

namespace
{
struct CanvasMenuContext
{
    Source source;
    Gui gui;
    LayoutRenderer renderer;
    liltview::Viewer viewer{renderer};
    liltview::Shell shell;
    liltview::Language language = liltview::Language::english;
    bool native = true, dark = false, fullscreen = false;
    float scale = 1;
    std::string text;
    liltview::MenuItem menu;

    CanvasMenuContext()
    {
        ImGui::GetIO().DisplaySize  = {960, 900};
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        std::filesystem::copy_file(source.path, source.directory / "a.bmp");
    }
    void settle()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!viewer.settled() || viewer.navigation().scanning)
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            viewer.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void load()
    {
        viewer.open(source.path);
        settle();
        REQUIRE_FALSE(viewer.failed());
        frame();
    }
    liltview::MenuAction frame()
    {
        using namespace liltview;
        apply_style(dark, scale);
        ImGui::NewFrame();
        WindowChromeState chrome;
        chrome.system_titlebar = native;
        chrome.dark            = dark;
        chrome.scale           = scale;
        chrome.line_height     = ImGui::GetTextLineHeight();
        menu                   = build_menu(Settings{}, language, true, fullscreen, viewer.has_image(), viewer.can_actual_size(),
                                            viewer.navigation().can_previous, viewer.navigation().can_next, shell.image_info_open(),
                                            viewer.has_visible_image());
        ImGui::LogToBuffer();
        ImGui::GetCurrentContext()->LogWindow = nullptr;
        const auto action                     = shell.draw(language, menu, false, renderer, {}, viewer, chrome);
        text                                  = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return action;
    }
    ImGuiWindow *canvas() const
    {
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos)
                return window;
        return nullptr;
    }
    ImGuiWindow *popup() const
    {
        auto *window = canvas();
        if (window)
            for (const auto &entry : ImGui::GetCurrentContext()->OpenPopupStack)
                if (entry.PopupId == window->GetID("canvas_view_menu"))
                    return entry.Window;
        return nullptr;
    }
    liltview::MenuAction right_click(ImVec2 point)
    {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        CHECK(frame() == liltview::MenuAction::none);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        const auto action = frame();
        frame(); // Popup sizing and hover settle on the next frame.
        return action;
    }
    ImVec2 center() const
    {
        const auto *window = canvas();
        REQUIRE(window);
        return window->InnerRect.GetCenter();
    }
    void close()
    {
        shell.dismiss_transient_menus();
        frame();
    }
};
} // namespace

TEST_CASE("canvas right click shares View entries shortcuts and actions in both themes and languages", "[shell][canvas_menu]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.load();
    for (const auto language : {Language::english, Language::chinese})
        for (const bool dark : {false, true})
            for (const bool native : {false, true})
                for (const float scale : {1.0f, 1.5f})
                {
                    CAPTURE(language, dark, native, scale);
                    context.language = language;
                    context.dark     = dark;
                    context.native   = native;
                    context.scale    = scale;
                    context.frame();
                    // Blank space around the small image belongs to the canvas too.
                    auto point  = context.center();
                    point.x    -= 100;
                    REQUIRE(context.right_click(point) == MenuAction::none);
                    auto *popup = context.popup();
                    REQUIRE(popup);
                    const auto *view = find_menu_item(context.menu, 2);
                    REQUIRE(view);
                    std::size_t previous = 0;
                    for (const auto &item : view->children)
                        if (!item.separator)
                        {
                            const auto position = context.text.find(item.label, previous);
                            REQUIRE(position != std::string::npos);
                            previous = position + item.label.size();
                        }
                    CHECK(context.text.find("Ctrl+↑") != std::string::npos);
                    CHECK(context.text.find("Ctrl+←") != std::string::npos);
                    CHECK(context.text.find("Ctrl+→") != std::string::npos);
                    CHECK(context.text.find("Shift+←") != std::string::npos);
                    CHECK(context.text.find("F11") != std::string::npos);
                    // The first enabled row dispatches exactly the same action
                    // as the corresponding View entry (the current file is last).
                    REQUIRE(find_menu_item(*view, 21)->enabled);
                    const auto first = popup->DC.CursorStartPos;
                    auto &io         = ImGui::GetIO();
                    io.AddMousePosEvent(first.x + 8, first.y + 4);
                    context.frame();
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                    CHECK(context.frame() == MenuAction::none);
                    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    CHECK(context.frame() == MenuAction::previous);
                    CHECK_FALSE(context.popup());
                    context.frame();
                }
}

TEST_CASE("canvas menu rejects empty queued loading and failed images and closes stale popups", "[shell][canvas_menu][loading]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.frame();
    context.right_click(context.center());
    CHECK_FALSE(context.popup());
    context.viewer.open(context.source.path);
    REQUIRE_FALSE(context.viewer.settled());
    // No pump: deterministically exercise queued/initial loading, even with
    // worker threads that can decode the tiny fixture almost immediately.
    context.right_click(context.center());
    CHECK_FALSE(context.popup());
    context.settle();
    context.frame();
    context.right_click(context.center());
    REQUIRE(context.popup());
    // Loading a replacement invalidates an already-open popup, even when the
    // previous scene remains visible. No shortcut can move that cached scene.
    context.viewer.open(context.source.directory / "a.bmp");
    REQUIRE_FALSE(context.viewer.settled());
    REQUIRE(context.viewer.has_visible_image());
    context.frame();
    CHECK_FALSE(context.popup());
    context.right_click(context.center());
    CHECK_FALSE(context.popup());
    const ShortcutContext loading{false, false, false, !context.viewer.settled()};
    CHECK(menu_action_for_input(context.menu, {"Shift", "Right"}, false, loading) == MenuAction::none);
    context.settle();
    context.frame();
    context.right_click(context.center());
    REQUIRE(context.popup());
    CHECK(menu_action_for_input(context.menu, {"Shift", "Right"}, false, {false, false, false, !context.viewer.settled()}) ==
          MenuAction::pan_right);
    context.viewer.open(context.source.directory / "missing.png");
    context.settle();
    REQUIRE(context.viewer.failed());
    REQUIRE(context.viewer.has_visible_image());
    context.frame();
    CHECK_FALSE(context.popup());
    context.right_click(context.center());
    CHECK_FALSE(context.popup());
    // Failure must not permanently disable the next successful load.
    context.load();
    context.right_click(context.center());
    CHECK(context.popup());
}

TEST_CASE("canvas menu does not open from chrome information panel or over a modal", "[shell][canvas_menu]")
{
    CanvasMenuContext context;
    context.load();
    context.shell.toggle_image_info();
    context.frame();
    for (const auto point : {ImVec2{20, 8}, ImVec2{400, 894}, ImVec2{940, 400}})
    {
        CAPTURE(point.x, point.y);
        context.right_click(point);
        CHECK_FALSE(context.popup());
    }
    context.shell.show_about();
    context.frame();
    context.right_click(context.center());
    CHECK_FALSE(context.popup());
}

TEST_CASE("canvas menu preserves disabled View actions and dismisses without dispatch", "[shell][canvas_menu]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.viewer.open(context.source.directory / "a.bmp");
    context.settle();
    context.frame();
    context.right_click(context.center());
    auto *popup = context.popup();
    REQUIRE(popup);
    REQUIRE_FALSE(find_menu_item(context.menu, 21)->enabled);
    const auto first = popup->DC.CursorStartPos;
    auto &io         = ImGui::GetIO();
    io.AddMousePosEvent(first.x + 8, first.y + 4);
    context.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    CHECK(context.frame() == MenuAction::none);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK(context.frame() == MenuAction::none);
    REQUIRE(context.popup());
    io.AddKeyEvent(ImGuiKey_Escape, true);
    CHECK(context.frame() == MenuAction::none);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    context.frame();
    CHECK_FALSE(context.popup());
    context.right_click(context.center());
    REQUIRE(context.popup());
    context.close();
    CHECK_FALSE(context.popup());
}

TEST_CASE("progressive decode and upload failures clear pending pixels and restore shortcuts", "[shell][loading][image_failure]")
{
    using namespace liltview;
    for (const bool corrupt : {false, true})
        for (const bool replacement : {false, true})
        {
            CAPTURE(corrupt, replacement);
            CanvasMenuContext context;
            if (replacement)
                context.load();
            const auto path                  = progressive_bmp(context.source, corrupt);
            context.renderer.upload_delay_ms = 5; // One upload crosses the pump budget: presentation precedes the terminal.
            context.viewer.open(path);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!(context.viewer.loading() && context.viewer.has_visible_image() && context.viewer.detail_loading()))
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                REQUIRE_FALSE(context.viewer.failed());
                context.viewer.pump();
                context.frame();
                SDL_Delay(1);
            }
            const auto loading = ShortcutContext{false, false, false, !context.viewer.settled()};
            CHECK(menu_action_for_input(context.menu, {"Control", "o"}, false, loading) == MenuAction::none);
            if (!corrupt)
                context.renderer.fail_upload = true;
            while (!context.viewer.failed())
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                context.viewer.pump();
                context.frame();
                SDL_Delay(1);
            }
            context.settle(); // Drains cancelled terminals, without drawing the discarded scene again.
            const lv_error_code expected = corrupt ? LV_ERROR_CORRUPT_DATA : LV_ERROR_MEMORY_BUDGET;
            CHECK(context.viewer.error() == expected);
            CHECK_FALSE(context.viewer.loading());
            CHECK_FALSE(context.viewer.detail_loading());
            CHECK(context.viewer.settled());
            context.renderer.image_calls = 0;
            context.frame();
            CHECK(context.renderer.image_calls == 0);
            const auto failed = ShortcutContext{false, false, false, !context.viewer.settled()};
            for (const bool repeat : {false, true})
            {
                CHECK(menu_action_for_input(context.menu, {"Control", "o"}, repeat, failed) ==
                      (repeat ? MenuAction::none : MenuAction::open));
                CHECK(menu_action_for_input(context.menu, {"Control", "q"}, repeat, failed) ==
                      (repeat ? MenuAction::none : MenuAction::quit));
            }
            context.renderer.upload_delay_ms = 0;
            context.load();
            CHECK(context.viewer.settled());
            CHECK(context.renderer.image_calls > 0);
        }
}

TEST_CASE("failure illustration and text fit minimum windows across DPI and scroll when necessary", "[shell][image_failure][dpi]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.viewer.open(context.source.directory / "missing.bmp");
    context.settle();
    const auto failure_window = []() -> ImGuiWindow *
    {
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/image_failure_") != std::string_view::npos)
                return window;
        return nullptr;
    };
    for (const auto language : {Language::english, Language::chinese})
        for (const bool dark : {false, true})
            for (const bool native : {false, true})
                for (const bool panel : {false, true})
                    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
                    {
                        CAPTURE(language, dark, native, panel, scale);
                        context.language           = language;
                        context.dark               = dark;
                        context.native             = native;
                        context.scale              = scale;
                        ImGui::GetIO().DisplaySize = {640, 480};
                        if (context.shell.image_info_open() != panel)
                            context.shell.toggle_image_info();
                        context.frame();
                        context.frame();
                        auto *failure = failure_window();
                        REQUIRE(failure);
                        CHECK(failure->ScrollMax.x == 0);
                        CHECK(failure->ScrollMax.y == 0);
                        CHECK(failure->DC.CursorMaxPos.y <= failure->InnerRect.Max.y + 1);
                        CHECK(context.text.find(translate(language, Text::image_io_error)) != std::string::npos);
                        // The error badge has actual geometry, not a zero-sized icon.
                        const auto color = ImGui::GetColorU32(current_ui_palette().error);
                        float top = failure->InnerRect.Max.y, bottom = failure->InnerRect.Min.y;
                        for (const auto &vertex : failure->DrawList->VtxBuffer)
                            if (vertex.col == color)
                            {
                                top    = std::min(top, vertex.pos.y);
                                bottom = std::max(bottom, vertex.pos.y);
                            }
                        CHECK(bottom - top >= 8 * scale);
                        CHECK(top >= failure->InnerRect.Min.y);
                        CHECK(bottom <= failure->InnerRect.Max.y);
                    }
    // Deliberately smaller than the supported minimum: content stays reachable
    // via a real scrollbar, instead of reducing the illustration to zero.
    context.language           = Language::english;
    context.native             = false;
    context.scale              = 2;
    ImGui::GetIO().DisplaySize = {400, 300};
    context.frame();
    context.frame();
    auto *failure = failure_window();
    REQUIRE(failure);
    REQUIRE(failure->ScrollMax.y > 0);
    CHECK(failure->ScrollMax.x == 0);
    const auto max_scroll = failure->ScrollMax.y;
    ImGui::GetIO().AddMousePosEvent(failure->InnerRect.GetCenter().x, failure->InnerRect.GetCenter().y);
    context.frame();
    ImGui::GetIO().AddMouseWheelEvent(0, -1);
    context.frame();
    CHECK(failure->Scroll.y > 0);
    ImGui::SetScrollY(failure, max_scroll);
    context.frame();
    CHECK(failure->DC.CursorMaxPos.y <= failure->InnerRect.Max.y + 1);
    // A child must not swallow the still-usable canvas navigation overlays.
    ImGui::GetIO().DisplaySize = {640, 480};
    context.viewer.open(context.source.directory / "z-missing.bmp");
    context.settle();
    context.frame();
    context.frame();
    const auto *canvas = context.canvas();
    REQUIRE(canvas);
    const auto arrows = navigation_button_layout(canvas->InnerRect.Min, canvas->InnerRect.Max, context.scale);
    const auto point =
        ImVec2{(arrows.previous.min.x + arrows.previous.max.x) * 0.5f, (arrows.previous.min.y + arrows.previous.max.y) * 0.5f};
    ImGui::GetIO().AddMousePosEvent(point.x, point.y);
    context.frame();
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    CHECK(context.frame() == MenuAction::none);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    CHECK(context.frame() == MenuAction::previous);
}


namespace
{
// Full Shell + real asynchronous Viewer, without a native window or GPU. Inputs
// traverse the same ImGui and action path as the desktop application.
struct DockShellScenario
{
    Source source;
    Gui gui;
    LayoutRenderer renderer;
    liltview::Viewer viewer{renderer};
    liltview::Shell shell;
    liltview::WindowChromeState chrome;
    liltview::DockLayout layout;
    bool external_blocked = false;

    DockShellScenario()
    {
        ImGui::GetIO().DisplaySize = {1280, 900};
        ImGui::GetIO().DeltaTime   = .01f;
        liltview::apply_style(true, 1);
        ImGui::GetStyle().AntiAliasedLinesUseTex = false;
        viewer.open(source.path);
        settle();
    }
    liltview::MenuAction frame(bool pump = true)
    {
        using namespace liltview;
        if (pump)
            viewer.pump();
        ImGui::NewFrame();
        chrome.line_height = ImGui::GetTextLineHeight();
        const auto menu    = build_menu(Settings{}, Language::english, true, chrome.fullscreen, viewer.has_image(),
                                        viewer.can_actual_size(), viewer.navigation().can_previous, viewer.navigation().can_next,
                                        shell.image_info_open(), viewer.has_visible_image());
        const auto action  = shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome, external_blocked);
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos)
                layout =
                    dock_layout(window->WorkRect.Min, window->WorkRect.Max, chrome.scale, viewer.fitting(), chrome.fullscreen);
        ImGui::Render();
        return action;
    }
    void settle()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        do
        {
            frame();
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (!viewer.interaction_ready() && !viewer.failed());
    }
    void frames(int n)
    {
        for (int i = 0; i < n; ++i)
            CHECK(frame() == liltview::MenuAction::none);
    }
    void reveal()
    {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent((layout.trigger.min.x + layout.trigger.max.x) / 2, layout.trigger.max.y - 4);
        frames(36);
    }
    ImVec2 dock_center(std::size_t i = 3)
    {
        const auto &r = layout.buttons[i].rect;
        return {(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
    }
    ImVec4 dock_pixel() const
    {
        return corner_pixel(*ImGui::GetDrawData(),
                            {layout.bounds.min.x + 24 * chrome.scale, layout.bounds.min.y + 2 * chrome.scale});
    }
    bool dock_visible() const
    {
        const auto c = dock_pixel(), p = liltview::current_ui_palette().popup;
        return std::abs(c.x - p.x) < .01f && std::abs(c.y - p.y) < .01f && std::abs(c.z - p.z) < .01f;
    }
    liltview::MenuAction double_click(ImVec2 point)
    {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        frames(40); // Separate from any earlier gesture/double-click chain.
        io.AddMouseButtonEvent(0, true);
        CHECK(frame() == liltview::MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        CHECK(frame() == liltview::MenuAction::none);
        io.AddMouseButtonEvent(0, true);
        const auto action = frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        return action;
    }
};
} // namespace

TEST_CASE("full shell dock overlays without changing FIT in both themes DPI panels and fullscreen", "[dock][shell]")
{
    using namespace liltview;
    DockShellScenario s;
    for (const bool dark : {true, false})
        for (const bool fullscreen : {true, false})
            for (const bool panel : {false, true})
                for (const float scale : {1.0f, 1.5f, 2.0f})
                {
                    CAPTURE(dark, fullscreen, panel, scale);
                    apply_style(dark, scale);
                    ImGui::GetStyle().AntiAliasedLinesUseTex = false;
                    s.chrome.dark                            = dark;
                    s.chrome.fullscreen                      = fullscreen;
                    s.chrome.scale                           = scale;
                    s.chrome.system_titlebar                 = !fullscreen;
                    if (s.shell.image_info_open() != panel)
                        s.shell.toggle_image_info();
                    ImGui::GetIO().AddMousePosEvent(100, 100);
                    s.frames(15);
                    const auto before = s.viewer.presented_view();
                    REQUIRE(s.layout.valid);
                    CHECK_FALSE(s.dock_visible());
                    s.reveal();
                    CHECK(s.dock_visible());
                    CHECK(s.viewer.presented_view().same_view(before));
                    ImGui::GetIO().AddMousePosEvent(100, 100);
                    s.frames(15);
                    CHECK_FALSE(s.dock_visible());
                    CHECK(s.viewer.presented_view().same_view(before));
                    CHECK(s.viewer.fitting());
                }
}

TEST_CASE("full shell double-click uses view mode including FIT at exactly 100 percent and manual zoom", "[dock][shell]")
{
    using namespace liltview;
    DockShellScenario s;
    for (const bool fullscreen : {false, true})
    {
        s.chrome.fullscreen = fullscreen;
        s.frames(2);
        REQUIRE(s.viewer.fitting());
        REQUIRE(s.viewer.presented_view().scale() == 1);              // Tiny image: FIT numerically equals 100%.
        CHECK(s.double_click({300, 200}) == MenuAction::actual_size); // Blank canvas is included.
        s.viewer.actual_size();
        s.settle();
        CHECK_FALSE(s.viewer.fitting());
        CHECK_FALSE(s.dock_visible());
        CHECK(s.double_click({300, 200}) == MenuAction::fit);
        s.viewer.fit();
        s.settle();
        s.viewer.zoom(1);
        s.settle();
        REQUIRE_FALSE(s.viewer.fitting());
        CHECK(s.double_click({300, 200}) == MenuAction::fit);
        s.viewer.fit();
        s.settle();
        CHECK_FALSE(s.dock_visible());
    }
    s.shell.toggle_image_info();
    s.frames(2);
    CHECK(s.double_click({1200, 150}) == MenuAction::none); // Information panel.
    CHECK(s.double_click({300, 895}) == MenuAction::none);  // Status bar.
    CHECK(s.double_click({500, 15}) == MenuAction::none);   // Titlebar.
    s.external_blocked = true;
    CHECK(s.double_click({300, 200}) == MenuAction::none); // Native file chooser.
}

TEST_CASE("full shell loading replacement failure and no-image cannot reveal or double-click", "[dock][shell]")
{
    using namespace liltview;
    DockShellScenario s;
    s.reveal();
    REQUIRE(s.dock_visible());
    s.viewer.open(s.source.directory / "missing.bmp");
    REQUIRE_FALSE(s.viewer.interaction_ready());
    REQUIRE(s.viewer.has_visible_image()); // Previous image does not enable new gestures, but the Dock remains drawn.
    s.frame(false);
    CHECK(s.dock_visible());
    CHECK(s.double_click({300, 200}) == MenuAction::none);
    s.settle();
    REQUIRE(s.viewer.failed());
    s.reveal();
    CHECK_FALSE(s.dock_visible());
    s.viewer.open(s.source.path);
    s.settle();
    CHECK(s.viewer.interaction_ready());
    s.reveal();
    CHECK(s.dock_visible());

    // A fresh empty Viewer is equally unavailable, even after a long bottom dwell.
    Viewer empty(s.renderer);
    Shell shell;
    ImGui::GetIO().AddMousePosEvent(640, 860);
    for (int i = 0; i < 50; ++i)
    {
        ImGui::NewFrame();
        CHECK(shell.draw(Language::english, build_menu(Settings{}, Language::english, true, false), false, s.renderer, {}, empty,
                         s.chrome) == MenuAction::none);
        ImGui::Render();
        CHECK_FALSE(shell.needs_animation_frame());
    }
}

TEST_CASE("full shell a canvas drag crosses dock and edge arrows without activation or input theft", "[dock][shell]")
{
    using namespace liltview;
    DockShellScenario s;
    auto &io = ImGui::GetIO();
    s.viewer.actual_size();
    s.settle();
    io.AddMousePosEvent(300, 200);
    s.frames(40);
    io.AddMouseButtonEvent(0, true);
    s.frame();
    io.AddMousePosEvent(640, s.layout.trigger.max.y - 5);
    s.frames(40);
    CHECK_FALSE(s.dock_visible());
    io.AddMouseButtonEvent(0, false);
    s.frame();
    s.frames(36);
    REQUIRE(s.dock_visible());
    const auto center = s.dock_center();
    io.AddMousePosEvent(center.x, center.y);
    s.frames(2);
    io.AddMouseButtonEvent(0, true);
    CHECK(s.frame() == MenuAction::none);
    io.AddMousePosEvent(310, 210);
    s.frames(2);
    io.AddMouseButtonEvent(0, false);
    CHECK(s.frame() == MenuAction::none);
    CHECK_FALSE(s.viewer.fitting()); // A drag originating on Dock was not treated as a double-click.
}


TEST_CASE("dock dwell and double-click remain unavailable throughout progressive native loading", "[dock][shell][loading]")
{
    using namespace liltview;
    DockShellScenario s;
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(640, s.layout.trigger.max.y - 4);
    s.frames(10); // Start but do not finish the old image's hover timer.
    s.renderer.upload_delay_ms = 5;
    s.viewer.open(progressive_bmp(s.source, false));
    bool saw_native     = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!s.viewer.interaction_ready())
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        REQUIRE_FALSE(s.viewer.failed());
        s.frame();
        if (s.viewer.native_loading() && s.viewer.has_visible_image())
        {
            saw_native = true;
            CHECK_FALSE(s.viewer.interaction_ready());
            CHECK_FALSE(s.dock_visible());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(saw_native);
    // Completion is not allowed to inherit the previous image's dwell time.
    CHECK_FALSE(s.dock_visible());
    s.frames(10);
    CHECK_FALSE(s.dock_visible());
    s.frames(26);
    CHECK(s.dock_visible());
}


TEST_CASE("Dock navigation retains opacity and position across previous and next image requests", "[dock][shell][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    const auto first = s.source.directory / "a.bmp";
    for (const auto name : {"a.bmp", "b.bmp", "c.bmp"})
        std::filesystem::copy_file(s.source.path, s.source.directory / name);
    s.viewer.open(first);
    s.settle();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (s.viewer.navigation().scanning)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        s.frame();
        SDL_Delay(1);
    }
    s.reveal();
    REQUIRE(s.dock_visible());
    const auto bounds = s.layout.bounds;
    auto &io          = ImGui::GetIO();
    for (const int direction : {1, 1, -1, -1})
    {
        CAPTURE(direction, s.viewer.filename());
        const auto &rect = s.layout.buttons[direction < 0 ? 0 : 1].rect;
        io.AddMousePosEvent((rect.min.x + rect.max.x) / 2, (rect.min.y + rect.max.y) / 2);
        s.frames(2);
        io.AddMouseButtonEvent(0, true);
        CHECK(s.frame() == MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        REQUIRE(s.frame() == (direction < 0 ? MenuAction::previous : MenuAction::next));
        s.viewer.navigate(direction);
        REQUIRE_FALSE(s.viewer.interaction_ready());
        s.frame(false); // Deterministic queued state, before even a tiny cached image can complete.
        REQUIRE(s.dock_visible());
        do
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            s.frame();
            REQUIRE_FALSE(s.viewer.failed());
            REQUIRE(s.dock_visible());
            CHECK(s.layout.bounds.min.x == bounds.min.x);
            CHECK(s.layout.bounds.min.y == bounds.min.y);
            SDL_Delay(1);
        } while (!s.viewer.interaction_ready());
    }
    // Info changes the available canvas; this is the one action allowed to reset visibility.
    s.shell.toggle_image_info();
    s.frame();
    CHECK_FALSE(s.dock_visible());
    CHECK(s.layout.bounds.min.x != bounds.min.x);
    s.reveal();
    REQUIRE(s.dock_visible());
    s.shell.toggle_image_info();
    s.frame();
    CHECK_FALSE(s.dock_visible());
}

TEST_CASE("a shown Dock survives progressive replacement but still auto hides when the pointer leaves",
          "[dock][shell][loading][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    s.reveal();
    REQUIRE(s.dock_visible());
    s.renderer.upload_delay_ms = 5;
    s.viewer.open(progressive_bmp(s.source, false));
    s.frame(false);
    REQUIRE(s.dock_visible());
    bool saw_native     = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        s.frame();
        REQUIRE_FALSE(s.viewer.failed());
        REQUIRE(s.dock_visible());
        saw_native |= s.viewer.native_loading();
        SDL_Delay(1);
    } while (!s.viewer.interaction_ready());
    REQUIRE(saw_native);
    ImGui::GetIO().AddMousePosEvent(100, 100);
    s.frames(15);
    CHECK_FALSE(s.dock_visible());
}

TEST_CASE("full shell image arrows keep navigating after a mouse Dock action has auto hidden",
          "[dock][shell][shortcut][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    auto &io        = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    for (const auto name : {"a.bmp", "b.bmp", "c.bmp"})
        std::filesystem::copy_file(s.source.path, s.source.directory / name);
    s.viewer.open(s.source.directory / "a.bmp");
    s.settle();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (s.viewer.navigation().scanning)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        s.frame();
        SDL_Delay(1);
    }
    s.reveal();
    const auto point = s.dock_center();
    io.AddMousePosEvent(point.x, point.y);
    s.frame();
    io.AddMouseButtonEvent(0, true);
    s.frame();
    io.AddMouseButtonEvent(0, false);
    REQUIRE(s.frame() == MenuAction::actual_size);
    s.viewer.actual_size();
    s.settle();
    io.AddMousePosEvent(300, 200);
    s.frames(30);
    REQUIRE_FALSE(s.dock_visible());
    for (const int direction : {1, 1, -1, -1})
    {
        CAPTURE(direction, s.viewer.filename());
        REQUIRE_FALSE(s.shell.dock_keyboard_focus());
        const auto menu = build_menu(Settings{}, Language::english, true, false, s.viewer.has_image(), s.viewer.can_actual_size(),
                                     s.viewer.navigation().can_previous, s.viewer.navigation().can_next);
        const ShortcutContext context{false, false, false, !s.viewer.settled(), s.shell.dock_keyboard_focus()};
        io.AddKeyEvent(direction > 0 ? ImGuiKey_RightArrow : ImGuiKey_LeftArrow, true);
        REQUIRE(menu_action_for_input(menu, {direction > 0 ? "Right" : "Left"}, false, context) ==
                (direction > 0 ? MenuAction::next : MenuAction::previous));
        s.viewer.navigate(direction); // Application dispatches shortcuts before ImGui::NewFrame.
        s.frame(false);
        io.AddKeyEvent(direction > 0 ? ImGuiKey_RightArrow : ImGuiKey_LeftArrow, false);
        s.settle();
        s.frames(15);
        CHECK_FALSE(s.dock_visible());
        CHECK_FALSE(s.shell.dock_keyboard_focus());
    }
    CHECK(s.viewer.filename() == "a.bmp");
}
