#include "ui/shell.hpp"
#include "ui/window_chrome.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

using namespace liltview;

TEST_CASE("borderless hit testing supports every resize edge and corner at display scales", "[chrome]")
{
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        CAPTURE(scale);
        WindowChromeState state;
        state.scale       = scale;
        state.line_height = 17 * scale;
        const auto m      = window_chrome_metrics({1120, 760}, state);
        CHECK(m.custom);
        CHECK(m.border == 2 * scale);
        CHECK(m.title_height == 36 * scale);
        CHECK(window_chrome_hit(m, {m.radius - 1, m.radius - 1}) == WindowHit::top_left);
        CHECK(window_chrome_hit(m, {1, 1}) == WindowHit::normal);
        CHECK(window_chrome_hit(m, {560, 1}) == WindowHit::top);
        CHECK(window_chrome_hit(m, {m.width - m.radius, m.radius - 1}) == WindowHit::top_right);
        CHECK(window_chrome_hit(m, {1119, 380}) == WindowHit::right);
        CHECK(window_chrome_hit(m, {m.width - m.radius, m.height - m.radius}) == WindowHit::bottom_right);
        CHECK(window_chrome_hit(m, {560, 759}) == WindowHit::bottom);
        CHECK(window_chrome_hit(m, {m.radius - 1, m.height - m.radius}) == WindowHit::bottom_left);
        CHECK(window_chrome_hit(m, {1, 380}) == WindowHit::left);
        CHECK(window_chrome_hit(m, {-1, 10}) == WindowHit::normal);
        CHECK(window_chrome_hit(m, {1120, 10}) == WindowHit::normal);
    }
}

TEST_CASE("only the title and status bars drag while window buttons menus and images stay interactive", "[chrome]")
{
    for (const float scale : {1.0f, 1.5f, 2.0f})
    {
        WindowChromeState state;
        state.scale       = scale;
        state.line_height = 17 * scale;
        const auto m      = window_chrome_metrics({1120, 760}, state);
        CHECK(window_chrome_hit(m, {50 * scale, m.border + m.title_height / 2}) == WindowHit::drag);
        CHECK(window_chrome_hit(m, {560, m.status_top() + m.status_height / 2}) == WindowHit::drag);
        CHECK(window_chrome_hit(m, {100, m.title_bottom() + 10}) == WindowHit::normal);
        CHECK(window_chrome_hit(m, {560, 380}) == WindowHit::normal);
        for (unsigned button = 0; button < 3; ++button)
            CHECK(window_chrome_hit(m,
                                    {m.controls_left() + (static_cast<float>(button) + 0.5f) * m.button_width,
                                     m.border + m.title_height / 2}) == WindowHit::normal);
    }
}

TEST_CASE("native decorations fullscreen and popups disable custom hit testing", "[chrome]")
{
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        WindowChromeState state;
        state.system_titlebar = mode == 0;
        state.fullscreen      = mode == 1;
        state.blocked         = mode == 2;
        const auto m          = window_chrome_metrics({1120, 760}, state);
        if (mode != 2)
        {
            CHECK_FALSE(m.custom);
            CHECK(m.border == 0);
            CHECK(m.title_height == 0);
        }
        for (const auto point : {ImVec2{1, 1}, {1119, 759}, {100, 20}, {100, 745}, {560, 380}})
            CHECK(window_chrome_hit(m, point) == WindowHit::normal);
    }
}

TEST_CASE("maximized borderless windows remain draggable but cannot be edge resized", "[chrome]")
{
    WindowChromeState state;
    state.maximized = true;
    const auto m    = window_chrome_metrics({1120, 760}, state);
    CHECK(window_chrome_hit(m, {1, 1}) == WindowHit::normal);
    CHECK(window_chrome_hit(m, {1, 380}) == WindowHit::normal);
    CHECK(window_chrome_hit(m, {1119, 759}) == WindowHit::normal);
    CHECK(window_chrome_hit(m, {100, 20}) == WindowHit::drag);
    CHECK(window_chrome_hit(m, {100, 745}) == WindowHit::drag);
    CHECK(window_chrome_hit(m, {1050, 20}) == WindowHit::normal);
}

TEST_CASE("window borders have strong contrast in light and dark themes", "[chrome]")
{
    const auto luminance = [](ImVec4 color)
    {
        const auto linear = [](float v)
        {
            return v <= 0.04045f ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * linear(color.x) + 0.7152 * linear(color.y) + 0.0722 * linear(color.z);
    };
    for (const bool dark : {false, true})
    {
        const auto color        = window_border_color(dark);
        const double border     = luminance(color);
        const double background = luminance(dark ? ImVec4{0.075f, 0.085f, 0.10f, 1} : ImVec4{0.94f, 0.94f, 0.94f, 1});
        CHECK(color.w == 1);
        CHECK((std::max(border, background) + 0.05) / (std::min(border, background) + 0.05) >= 4.5);
    }
}

namespace
{
class TitleRenderer final : public render::Renderer
{
public:
    render::Capabilities capabilities() const override
    {
        return {};
    }
    render::Size framebuffer_size() const override
    {
        return {960, 640};
    }
    render::Image create_image(render::ImageDesc) override
    {
        return {};
    }
    void update_image(render::Image, render::Rect, render::Pixels) override
    {
    }
    void destroy_image(render::Image) noexcept override
    {
    }
    void add_image(ImDrawList *, render::Image, render::Point, render::Point, render::Point, render::Point) override
    {
    }
    void add_image_quad(ImDrawList *, render::Image, const std::array<render::Point, 4> &,
                        const std::array<render::Point, 4> &) override
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
    render::Readback readback() override
    {
        return {};
    }
};

struct TitleContext
{
    TitleContext()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {960, 640};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
        apply_style(true, 1);
    }
    ~TitleContext()
    {
        ImGui::DestroyContext();
    }

    MenuAction frame(WindowChromeState state)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({960, 640});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("test_title", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        TitleRenderer renderer;
        const auto action =
            draw_window_titlebar(window_chrome_metrics({960, 640}, state), state, Language::english, renderer, {});
        ImGui::End();
        ImGui::Render();
        return action;
    }
};
} // namespace

TEST_CASE("custom caption buttons produce minimize maximize restore and close actions", "[chrome][ui]")
{
    for (unsigned button = 0; button < 3; ++button)
        for (const bool maximized : {false, true})
        {
            CAPTURE(button, maximized);
            TitleContext context;
            WindowChromeState state;
            state.maximized = maximized;
            const auto m    = window_chrome_metrics({960, 640}, state);
            context.frame(state);
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(m.controls_left() + (static_cast<float>(button) + 0.5f) * m.button_width, 20);
            io.AddMouseButtonEvent(0, true);
            CHECK(context.frame(state) == MenuAction::none);
            io.AddMouseButtonEvent(0, false);
            const MenuAction expected[]{MenuAction::minimize, MenuAction::maximize_restore, MenuAction::close_window};
            CHECK(context.frame(state) == expected[button]);
        }
}

TEST_CASE("rounded input regions agree with the visible corner geometry without full-window masks", "[chrome][rounding]")
{
    WindowChromeState state;
    const auto m = window_chrome_metrics({160, 100}, state);
    REQUIRE(m.radius == 12);
    const auto spans = rounded_window_spans(160, 100, m.radius);
    CHECK(spans.size() <= 25);
    for (const auto &r : spans)
    {
        CHECK(r.x >= 0);
        CHECK(r.y >= 0);
        CHECK(r.width > 0);
        CHECK(r.height > 0);
        CHECK(r.x + r.width <= 160);
        CHECK(r.y + r.height <= 100);
    }
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 160; ++x)
        {
            const bool covered = std::any_of(spans.begin(), spans.end(), [&](const WindowSpan &r)
                                             { return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height; });
            REQUIRE(covered == window_chrome_contains(m, {static_cast<float>(x), static_cast<float>(y)}));
        }
    CHECK_FALSE(window_chrome_contains(m, {0, 0}));
    CHECK(window_chrome_contains(m, {80, 0}));
    state.rounded     = false;
    const auto square = window_chrome_metrics({160, 100}, state);
    CHECK(square.radius == 0);
    CHECK(window_chrome_contains(square, {0, 0}));
    CHECK(window_chrome_hit(square, {0, 0}) == WindowHit::top_left);
    CHECK(rounded_window_spans(160, 100, 0).size() == 1);
    state.rounded   = true;
    state.maximized = true;
    CHECK(window_chrome_metrics({160, 100}, state).radius == 0);
}

TEST_CASE("decoration switches keep the picture viewport anchored with reversible geometry", "[chrome][placement]")
{
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        CAPTURE(scale);
        WindowChromeState custom;
        custom.scale             = scale;
        WindowChromeState native = custom;
        native.system_titlebar   = true;
        const auto before        = window_chrome_metrics({1120, 760}, custom);
        const auto switched      = preserve_content_placement(100, 150, 1120, 760, custom, native);
        CHECK(std::abs(switched.x - (100 + before.border)) <= 0.5f);
        CHECK(std::abs(switched.y - (150 + before.title_bottom())) <= 0.5f);
        CHECK(std::abs(switched.width - (1120 - 2 * before.border)) <= 1.0f);
        CHECK(std::abs(switched.height - (760 - 2 * before.border - before.title_height)) <= 0.5f);
        const auto restored = preserve_content_placement(switched.x, switched.y, switched.width, switched.height, native, custom);
        CHECK(restored.x == 100);
        CHECK(restored.y == 150);
        CHECK(restored.width == 1120);
        CHECK(restored.height == 760);
    }
    WindowChromeState custom, native;
    native.system_titlebar = true;
    const auto minimum     = preserve_content_placement(0, 0, 640, 480, custom, native);
    CHECK(minimum.width >= 640);
    CHECK(minimum.height >= 480);
}
