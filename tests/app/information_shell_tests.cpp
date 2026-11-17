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
    liltview::render::Image last_image{};
    ImVec2 image_min{}, image_max{};
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
    void add_image(ImDrawList *, liltview::render::Image image, liltview::render::Point min, liltview::render::Point max,
                   liltview::render::Point, liltview::render::Point) override
    {
        if (image)
        {
            ++image_calls;
            last_image = image;
            image_min  = {min.x, min.y};
            image_max  = {max.x, max.y};
        }
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

TEST_CASE("information overlay toggle preserves the full main canvas FIT and navigation at every DPI",
          "[image_details][shell][regression]")
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
        (viewer.pump(), viewer.prepare_loading_view());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE_FALSE(viewer.failed());
    Shell shell;
    for (const auto language : {Language::english, Language::chinese})
        for (const float scale : {1.0f, 1.25f, 2.0f})
            for (const bool native_titlebar : {false, true})
                for (const bool dark : {false, true})
                {
                    CAPTURE(scale, native_titlebar, dark);
                    apply_style(dark, scale);
                    ImRect canvas_rect;
                    std::optional<Viewport> original_view;
                    for (const bool open : {false, true, false})
                    {
                        if (shell.image_info_open() != open)
                            shell.toggle_image_info();
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
                        const auto menu = build_menu(Settings{}, language, true, false, true, true, false, false, open, true);
                        CHECK(shell.draw(language, menu, false, renderer, {}, viewer, chrome) == MenuAction::none);
                        ImGuiWindow *canvas = nullptr;
                        for (auto *window : ImGui::GetCurrentContext()->Windows)
                        {
                            const auto name = std::string_view(window->Name);
                            if (window->Active && name.find("/canvas_") != std::string_view::npos &&
                                name.find("/image_information_") == std::string_view::npos)
                                canvas = window;
                            if (window->Active && name.find("/image_information_") != std::string_view::npos)
                                CHECK(open);
                        }
                        REQUIRE(canvas);
                        CHECK(canvas->Size.x > 0);
                        CHECK(canvas->Pos.x + canvas->Size.x >= metrics.width - metrics.border - 1);
                        CHECK(canvas->Pos.y + canvas->Size.y <= metrics.status_top() + 1);
                        const auto buttons = navigation_button_layout(canvas->InnerRect.Min, canvas->InnerRect.Max, scale);
                        REQUIRE(buttons.valid);
                        CHECK(buttons.next.max.x <= canvas->Pos.x + canvas->Size.x);
                        if (!original_view)
                        {
                            original_view = viewer.presented_view();
                            canvas_rect   = canvas->Rect();
                        }
                        else
                        {
                            CHECK(viewer.presented_view().same_view(*original_view));
                            CHECK(canvas->Rect().Min.x == canvas_rect.Min.x);
                            CHECK(canvas->Rect().Max.x == canvas_rect.Max.x);
                            CHECK(canvas->Rect().Min.y == canvas_rect.Min.y);
                            CHECK(canvas->Rect().Max.y == canvas_rect.Max.y);
                        }
                        CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
                        ImGui::Render();
                        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
                        const auto &p = current_ui_palette();
                        CHECK(ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_ChildBg]) ==
                              ImGui::ColorConvertFloat4ToU32(p.canvas));
                        REQUIRE(viewer.image_details());
                        CHECK(viewer.image_details()->path == source.path);
                    }
                }
}

TEST_CASE("loading shell centers the application icon above its text", "[shell][loading][dpi]")
{
    using namespace liltview;
    Source source;
    Gui gui;
    LayoutRenderer renderer;
    Viewer viewer(renderer);
    viewer.open(source.path);
    REQUIRE(viewer.loading());
    REQUIRE_FALSE(viewer.has_visible_image());
    const render::Image icon{99, 1, 1};
    for (const auto language : {Language::english, Language::chinese})
        for (const bool dark : {false, true})
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
                for (const bool panel : {false, true})
                    for (const bool native : {false, true})
                        for (const auto size : {ImVec2{320, 300}, ImVec2{1120, 760}})
                        {
                            CAPTURE(language, dark, scale, panel, native, size.x, size.y);
                            Shell shell;
                            if (panel)
                                shell.toggle_image_info();
                            apply_style(dark, scale);
                            auto &io                   = ImGui::GetIO();
                            io.DisplaySize             = size;
                            io.DisplayFramebufferScale = {1.25f, 1.25f};
                            ImGui::NewFrame();
                            WindowChromeState chrome;
                            chrome.system_titlebar = native;
                            chrome.dark            = dark;
                            chrome.scale           = scale;
                            chrome.line_height     = ImGui::GetTextLineHeight();
                            const auto menu = build_menu(Settings{}, language, true, false, true, true, false, false, true, true);
                            renderer.image_calls = 0;
                            ImGui::LogToBuffer();
                            ImGui::GetCurrentContext()->LogWindow = nullptr;
                            shell.draw(language, menu, false, renderer, icon, viewer, chrome);
                            const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
                            ImGui::LogFinish();
                            REQUIRE(renderer.image_calls == (native ? 1u : 2u));
                            CHECK(renderer.last_image.owner == icon.owner);
                            CHECK(text.find(translate(language, Text::loading)) != std::string::npos);
                            ImGuiWindow *canvas = nullptr;
                            for (auto *window : ImGui::GetCurrentContext()->Windows)
                                if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos &&
                                    std::string_view(window->Name).find("/image_information_") == std::string_view::npos)
                                    canvas = window;
                            REQUIRE(canvas);
                            const auto min = canvas->DC.CursorStartPos;
                            const auto max = canvas->WorkRect.Max;
                            CHECK(std::abs((renderer.image_min.x + renderer.image_max.x) * 0.5f - (min.x + max.x) * 0.5f) < 1);
                            CHECK(renderer.image_min.y >= min.y - 1);
                            CHECK(renderer.image_max.y > renderer.image_min.y);
                            ImRect text_bounds{{FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX}};
                            const auto text_color = ImGui::GetColorU32(current_ui_palette().secondary);
                            for (const auto &vertex : canvas->DrawList->VtxBuffer)
                                if (vertex.col == text_color)
                                    text_bounds.Add(vertex.pos);
                            REQUIRE(text_bounds.Min.y < text_bounds.Max.y);
                            CHECK(text_bounds.Min.y > renderer.image_max.y);
                            CHECK(std::abs((renderer.image_min.y + text_bounds.Max.y) * 0.5f - (min.y + max.y) * 0.5f) <
                                  ImGui::GetTextLineHeight() * 0.5f + 1);
                            CHECK(text_bounds.Max.y <= max.y + 1);
                            CHECK(std::abs(text_bounds.GetCenter().x - (min.x + max.x) * 0.5f) < 3 * scale);
                            CHECK_FALSE(canvas->ScrollbarY);
                            ImGui::Render();
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
            (viewer.pump(), viewer.prepare_loading_view());
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
                            // A failed replacement cannot expose stale image details.
                            CHECK(text.find("100%.bmp") == std::string::npos);
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
            (viewer.pump(), viewer.prepare_loading_view());
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
            if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos &&
                std::string_view(window->Name).find("/image_information_") == std::string_view::npos)
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
    REQUIRE_FALSE(context.viewer.has_visible_image());
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

TEST_CASE("canvas menu ignores chrome modals and the information overlay", "[shell][canvas_menu]")
{
    CanvasMenuContext context;
    context.load();
    context.shell.toggle_image_info();
    context.frame();
    for (const auto point : {ImVec2{20, 8}, ImVec2{400, 894}})
    {
        CAPTURE(point.x, point.y);
        context.right_click(point);
        CHECK_FALSE(context.popup());
    }
    context.right_click({850, 400}); // The Info overlay consumes right-clicks.
    CHECK_FALSE(context.popup());
    context.right_click({300, 400});
    REQUIRE(context.popup());
    context.close();
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
            while (!(context.viewer.loading() && context.viewer.static_load_stats().tiles > 0))
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                REQUIRE_FALSE(context.viewer.failed());
                (context.viewer.pump(), context.viewer.prepare_loading_view());
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
                (context.viewer.pump(), context.viewer.prepare_loading_view());
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
    bool focus_lost_in_frame = false;

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
            (viewer.pump(), viewer.prepare_loading_view());
        ImGui::NewFrame();
        focus_lost_in_frame = ImGui::GetIO().AppFocusLost;
        chrome.line_height = ImGui::GetTextLineHeight();
        const auto menu    = build_menu(Settings{}, Language::english, true, chrome.fullscreen, viewer.has_image(),
                                        viewer.can_actual_size(), viewer.navigation().can_previous, viewer.navigation().can_next,
                                        shell.image_info_open(), viewer.has_visible_image());
        const auto action  = shell.draw(Language::english, menu, false, renderer, {}, viewer, chrome, external_blocked);
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/canvas_") != std::string_view::npos &&
                std::string_view(window->Name).find("/image_information_") == std::string_view::npos)
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
                    // The Info panel does not change the Dock auto-hide policy.
                    s.shell.dismiss_image_info();
                    ImGui::GetIO().AddMousePosEvent(100, 100);
                    s.frames(15);
                    if (panel)
                        s.shell.toggle_image_info();
                    s.frames(2);
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

TEST_CASE("opening Info from the Dock preserves its anchor and normal auto hide", "[dock][shell][image_details][regression]")
{
    using namespace liltview;
    for (const bool fullscreen : {false, true})
    {
        CAPTURE(fullscreen);
        DockShellScenario s;
        s.chrome.fullscreen = fullscreen;
        s.frames(2);
        s.reveal();
        REQUIRE(s.dock_visible());
        const auto view   = s.viewer.presented_view();
        const auto bounds = s.layout.bounds;
        const auto button = std::find_if(s.layout.buttons.begin(), s.layout.buttons.end(),
                                         [](const DockButton &value) { return value.action == MenuAction::image_info; });
        REQUIRE(button != s.layout.buttons.end());
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent((button->rect.min.x + button->rect.max.x) / 2, (button->rect.min.y + button->rect.max.y) / 2);
        s.frames(2);
        io.AddMouseButtonEvent(0, true);
        CHECK(s.frame() == MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        REQUIRE(s.frame() == MenuAction::image_info);
        s.shell.toggle_image_info();
        for (unsigned frame = 0; frame < 20; ++frame)
        {
            CHECK(s.frame() == MenuAction::none);
            CHECK(s.dock_visible()); // No flash on opening while the pointer remains over the Dock.
        }
        CHECK(s.layout.bounds.min.x == bounds.min.x);
        CHECK(s.layout.bounds.min.y == bounds.min.y);
        io.AddMousePosEvent(1100, 200); // Read Info; the Dock may hide normally.
        s.frames(20);
        CHECK_FALSE(s.dock_visible());
        CHECK(s.shell.image_info_open());
        CHECK(s.viewer.presented_view().same_view(view));
        s.reveal();
        CHECK(s.dock_visible());
        s.shell.dismiss_image_info();
        CHECK(s.viewer.presented_view().same_view(view));
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
    CHECK(s.double_click({1200, 150}) == MenuAction::none); // Info owns its rectangle.
    CHECK(s.double_click({300, 200}) == MenuAction::actual_size);
    CHECK(s.double_click({300, 895}) == MenuAction::none);  // Status bar.
    CHECK(s.double_click({500, 15}) == MenuAction::none);   // Titlebar.
    s.external_blocked = true;
    CHECK(s.double_click({300, 200}) == MenuAction::none); // Native file chooser.
}

TEST_CASE("information toggle preserves a manually zoomed and panned camera", "[image_details][shell][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    s.viewer.zoom(4);
    s.settle();
    s.viewer.pan(17, -11);
    s.settle();
    REQUIRE_FALSE(s.viewer.fitting());
    const auto view = s.viewer.presented_view();
    const auto layout = s.layout;
    for (unsigned toggle = 0; toggle < 4; ++toggle)
    {
        s.shell.toggle_image_info();
        s.frames(2);
        CHECK(s.viewer.presented_view().same_view(view));
        CHECK_FALSE(s.viewer.fitting());
        CHECK(s.layout.bounds.min.x == layout.bounds.min.x);
        CHECK(s.layout.bounds.min.y == layout.bounds.min.y);
        CHECK(s.layout.bounds.max.x == layout.bounds.max.x);
        CHECK(s.layout.bounds.max.y == layout.bounds.max.y);
    }
}

TEST_CASE("full shell loading replacement failure and no-image cannot reveal or double-click", "[dock][shell]")
{
    using namespace liltview;
    DockShellScenario s;
    s.reveal();
    REQUIRE(s.dock_visible());
    s.viewer.open(s.source.directory / "missing.bmp");
    REQUIRE_FALSE(s.viewer.interaction_ready());
    REQUIRE_FALSE(s.viewer.has_visible_image()); // The canvas is hidden but the already shown Dock remains drawn.
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
        if (s.viewer.native_loading())
        {
            saw_native = true;
            CHECK_FALSE(s.viewer.has_visible_image());
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
    // The Info overlay must not reset the Dock or move its anchor.
    const auto view = s.viewer.presented_view();
    for (unsigned toggle = 0; toggle < 2; ++toggle)
    {
        s.shell.toggle_image_info();
        s.frame();
        CHECK(s.dock_visible());
        CHECK(s.layout.bounds.min.x == bounds.min.x);
        CHECK(s.layout.bounds.min.y == bounds.min.y);
        CHECK(s.viewer.presented_view().same_view(view));
    }
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


TEST_CASE("information panel owns its input without hiding the edge arrows or moving the image",
          "[image_details][shell][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    s.viewer.open(progressive_bmp(s.source, false));
    s.settle();
    s.viewer.zoom(8);
    s.settle();
    s.shell.toggle_image_info();
    s.frames(3);
    const auto view = s.viewer.presented_view();
    auto &io        = ImGui::GetIO();
    io.AddMousePosEvent(1100, 250);
    s.frames(2);
    io.AddMouseWheelEvent(0, 3);
    s.frames(3);
    CHECK(s.viewer.presented_view().same_view(view));
    io.AddMouseButtonEvent(0, true);
    s.frame();
    io.AddMousePosEvent(1150, 300);
    s.frames(2);
    io.AddMousePosEvent(400, 400); // Leaving the panel cannot turn its press into an image drag.
    s.frames(2);
    io.AddMouseButtonEvent(0, false);
    CHECK(s.frame() == MenuAction::none);
    CHECK(s.viewer.presented_view().same_view(view));
    CHECK_FALSE(s.dock_visible());
    CHECK(s.double_click({1200, 150}) == MenuAction::none);
    CHECK(s.viewer.presented_view().same_view(view));
    // The right arrow keeps its visual and hit area outside the Info panel.
    const ImVec2 min{0, 68}, max{1280, 870};
    const auto panel = image_information_layout(min, {max.x, s.layout.bounds.min.y}, 1);
    REQUIRE(panel.valid);
    const auto arrows = navigation_button_layout(min, max, 1, {panel.min, panel.max});
    CHECK(arrows.previous_visible);
    CHECK(arrows.next_visible);
    CHECK(panel.max.x + 12 <= arrows.next.min.x);
    const auto nav = navigation_button_state(s.viewer.navigation());
    CHECK(navigation_buttons_hit(arrows, nav, {arrows.next.min.x + 5, arrows.next.min.y + 5}));
    // The clear canvas still accepts normal zoom after a panel gesture.
    io.AddMousePosEvent(400, 350);
    s.frames(3);
    io.AddMouseWheelEvent(0, 1);
    s.settle();
    CHECK_FALSE(s.viewer.presented_view().same_view(view));
}

TEST_CASE("an image drag keeps ownership while crossing the information panel", "[image_details][shell][regression]")
{
    using namespace liltview;
    DockShellScenario s;
    s.viewer.open(progressive_bmp(s.source, false));
    s.settle();
    s.viewer.zoom(8);
    s.settle();
    s.shell.toggle_image_info();
    s.frames(3);
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(500, 300);
    s.frames(3);
    io.AddMouseButtonEvent(0, true);
    s.frame();
    io.AddMousePosEvent(600, 320);
    s.settle();
    const auto before = s.viewer.presented_view();
    io.AddMousePosEvent(1100, 350);
    s.settle();
    CHECK_FALSE(s.viewer.presented_view().same_view(before));
    io.AddMouseButtonEvent(0, false);
    CHECK(s.frame() == MenuAction::none);
    CHECK(s.shell.image_info_open());
}


TEST_CASE("reopening information starts at the file identity while retaining image view", "[image_details][shell][regression]")
{
    DockShellScenario s;
    s.shell.toggle_image_info();
    s.frames(3);
    ImGuiWindow *body = nullptr;
    for (auto *child : ImGui::GetCurrentContext()->Windows)
        if (child->Active && std::string_view(child->Name).find("/image_information_body_") != std::string_view::npos)
            body = child;
    REQUIRE(body);
    REQUIRE(body->ScrollMax.y > 0); // The fixture's long literal path exceeds the preferred height.
    const auto view = s.viewer.presented_view();
    auto &io        = ImGui::GetIO();
    io.AddMousePosEvent(body->Pos.x + 40, body->Pos.y + 40);
    s.frames(2);
    io.AddMouseWheelEvent(0, -8);
    s.frames(3);
    CHECK(body->Scroll.y > 0);
    s.shell.dismiss_image_info();
    s.frames(2);
    s.shell.toggle_image_info();
    s.frames(3);
    CHECK(body->Scroll.y == 0);
    CHECK(s.viewer.presented_view().same_view(view));
}


TEST_CASE("canvas popup retains priority when it overlaps the information panel", "[image_details][shell][canvas_menu]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.load();
    context.shell.toggle_image_info();
    context.frame();
    context.right_click({580, 150}); // Open on clear canvas, adjacent to the Info panel.
    auto *popup = context.popup();
    REQUIRE(popup);
    auto &io         = ImGui::GetIO();
    const auto first = popup->DC.CursorStartPos;
    io.AddMousePosEvent(first.x + 50, first.y + 4); // The popup now covers Info pixels.
    context.frame();
    REQUIRE(context.popup());
    io.AddMouseButtonEvent(0, true);
    CHECK(context.frame() == MenuAction::none);
    io.AddMouseButtonEvent(0, false);
    CHECK(context.frame() == MenuAction::previous);
    CHECK(context.shell.image_info_open());
}


TEST_CASE("both edge navigation buttons remain clickable with information open", "[image_details][shell][navigation_buttons]")
{
    using namespace liltview;
    CanvasMenuContext context;
    context.load();
    context.shell.toggle_image_info();
    for (const auto scale : {1.f, 1.5f, 2.f})
    {
        context.scale = scale;
        context.viewer.open(context.source.directory / "a.bmp");
        context.settle();
        context.frame();
        context.frame();
        auto *canvas = context.canvas();
        REQUIRE(canvas);
        const auto arrows = navigation_button_layout(canvas->WorkRect.Min, canvas->WorkRect.Max, scale);
        const auto dock   = dock_layout(canvas->WorkRect.Min, canvas->WorkRect.Max, scale, true, false);
        const auto panel  = image_information_layout(canvas->WorkRect.Min, {canvas->WorkRect.Max.x, dock.bounds.min.y}, scale);
        REQUIRE(panel.valid);
        CHECK(panel.max.x + 12 * scale <= arrows.next.min.x);
        const auto click = [&](const NavigationButtonRect &rect)
        {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent((rect.min.x + rect.max.x) / 2, (rect.min.y + rect.max.y) / 2);
            context.frame();
            context.frame();
            io.AddMouseButtonEvent(0, true);
            CHECK(context.frame() == MenuAction::none);
            io.AddMouseButtonEvent(0, false);
            return context.frame();
        };
        CHECK(click(arrows.next) == MenuAction::next);
        context.viewer.open(context.source.path);
        context.settle();
        context.frame();
        CHECK(click(arrows.previous) == MenuAction::previous);
        CHECK(context.shell.image_info_open());
    }
}

TEST_CASE("information arrow keys never focus full width layout containers", "[image_details][shell][keyboard][regression]")
{
    using namespace liltview;
    for (const bool native : {false, true})
        for (const bool dark : {false, true})
        {
            CAPTURE(native, dark);
            DockShellScenario s;
            s.chrome.system_titlebar = native;
            s.chrome.dark = dark;
            apply_style(dark, 1);
            auto &io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            s.shell.toggle_image_info();
            s.frames(3);
            for (const bool control : {false, true})
                for (const auto key : {ImGuiKey_UpArrow, ImGuiKey_DownArrow})
                    for (int press = 0; press < 4; ++press)
                    {
                        CAPTURE(control, key, press);
                        io.AddKeyEvent(ImGuiMod_Ctrl, control);
                        io.AddKeyEvent(key, true);
                        s.frames(3);
                        const auto &g = *ImGui::GetCurrentContext();
                        int containers = 0;
                        for (const auto *window : g.Windows)
                            if (window->Active && window->ParentWindow && window->ParentWindow->ParentWindow == nullptr)
                            {
                                INFO(window->Name);
                                ++containers;
                                CHECK_FALSE((g.NavCursorVisible && g.NavId == window->ChildId));
                            }
                        REQUIRE(containers == (native ? 1 : 2));
                        io.AddKeyEvent(key, false);
                        io.AddKeyEvent(ImGuiMod_Ctrl, false);
                        s.frames(2);
                    }
        }
}

TEST_CASE("flattened canvas retains explicit Dock keyboard navigation with information open", "[dock][image_details][shell][keyboard]")
{
    using namespace liltview;
    DockShellScenario s;
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s.shell.toggle_image_info();
    s.frames(3);
    s.reveal();
    for (int tab = 0; tab < 12 && !s.shell.dock_keyboard_focus(); ++tab)
    {
        io.AddKeyEvent(ImGuiKey_Tab, true);
        s.frames(2);
        io.AddKeyEvent(ImGuiKey_Tab, false);
        s.frames(2);
    }
    REQUIRE(s.shell.dock_keyboard_focus());
    const auto first = ImGui::GetCurrentContext()->NavId;
    io.AddKeyEvent(ImGuiKey_RightArrow, true);
    s.frames(2);
    io.AddKeyEvent(ImGuiKey_RightArrow, false);
    s.frames(2);
    CHECK(s.shell.dock_keyboard_focus());
    CHECK(ImGui::GetCurrentContext()->NavId != first);
    CHECK(s.shell.image_info_open());
}
