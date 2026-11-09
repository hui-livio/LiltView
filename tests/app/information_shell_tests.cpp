#include "ui/image_information.hpp"
#include "ui/shell.hpp"
#include "viewer/viewer.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
class LayoutRenderer final : public liltview::render::Renderer
{
public:
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
    void add_image(ImDrawList *, liltview::render::Image, liltview::render::Point, liltview::render::Point,
                   liltview::render::Point, liltview::render::Point) override
    {
    }
    void add_image_quad(ImDrawList *, liltview::render::Image, const std::array<liltview::render::Point, 4> &,
                        const std::array<liltview::render::Point, 4> &) override
    {
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
            {
                CAPTURE(scale, native_titlebar);
                apply_style(false, scale);
                auto &io                   = ImGui::GetIO();
                io.DisplaySize             = {640, 480};
                io.DisplayFramebufferScale = {scale, scale};
                ImGui::NewFrame();
                WindowChromeState chrome;
                chrome.system_titlebar = native_titlebar;
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
                REQUIRE(viewer.image_details());
                CHECK(viewer.image_details()->path == source.path);
            }
}
