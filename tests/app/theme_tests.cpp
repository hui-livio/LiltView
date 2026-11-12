#include "ui/shell.hpp"
#include "ui/theme.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace liltview;

namespace
{
struct ThemeContext
{
    ThemeContext()
    {
        ImGui::CreateContext();
    }
    ~ThemeContext()
    {
        ImGui::DestroyContext();
    }
};

// Check the 8-bit values actually sent to the renderer, not idealized tokens.
ImVec4 quantized(ImVec4 color)
{
    return ImGui::ColorConvertU32ToFloat4(ImGui::ColorConvertFloat4ToU32(color));
}

double luminance(ImVec4 color)
{
    color             = quantized(color);
    const auto linear = [](double value)
    {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.x) + 0.7152 * linear(color.y) + 0.0722 * linear(color.z);
}

double contrast(ImVec4 foreground, ImVec4 background)
{
    const double a = luminance(foreground), b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

ImVec4 over(ImVec4 foreground, ImVec4 background)
{
    foreground = quantized(foreground);
    return {foreground.x * foreground.w + background.x * (1 - foreground.w),
            foreground.y * foreground.w + background.y * (1 - foreground.w),
            foreground.z * foreground.w + background.z * (1 - foreground.w), 1};
}
} // namespace

TEST_CASE("both themes keep primary and secondary text readable on their intended surfaces", "[theme][contrast]")
{
    ThemeContext context;
    for (const bool dark : {false, true})
    {
        CAPTURE(dark);
        apply_style(dark, 1);
        const auto &p     = current_ui_palette();
        const auto &style = ImGui::GetStyle();
        for (const auto surface :
             {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_PopupBg, ImGuiCol_MenuBarBg, ImGuiCol_Button, ImGuiCol_ButtonHovered,
              ImGuiCol_ButtonActive, ImGuiCol_FrameBg, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive})
        {
            CAPTURE(surface);
            CHECK(contrast(style.Colors[ImGuiCol_Text], style.Colors[surface]) >= 4.5);
        }
        // Information labels, shortcuts and the footer are secondary text, not disabled widgets.
        for (const auto surface : {p.surface, p.canvas, p.chrome, p.popup, p.control})
            CHECK(contrast(p.secondary, surface) >= 4.5);
        for (const auto surface : {p.surface, p.chrome, p.popup})
            CHECK(contrast(p.accent, surface) >= 3);
        CHECK(contrast({1, 1, 1, 1}, p.close_hover) >= 4.5);
        CHECK(contrast({1, 1, 1, 1}, p.close_pressed) >= 4.5);
    }
}

TEST_CASE("navigation overlays keep arrow contrast on both black and white images", "[theme][contrast][navigation_buttons]")
{
    for (const bool dark : {false, true})
    {
        CAPTURE(dark);
        const auto &p = ui_palette(dark);
        for (const auto overlay : {p.overlay, p.overlay_hover, p.overlay_pressed})
            for (const auto image : {ImVec4{0, 0, 0, 1}, ImVec4{1, 1, 1, 1}})
                CHECK(contrast(p.overlay_text, over(overlay, image)) >= 4.5);
    }
}

TEST_CASE("theme changes reset every color without accumulating DPI or leaking old accent colors", "[theme][dpi]")
{
    ThemeContext context;
    for (const bool dark : {false, true, false})
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
        {
            CAPTURE(dark, scale);
            apply_style(dark, scale);
            const auto expected = ImGui::GetStyle();
            // Simulate switching from a different theme and display scale.
            apply_style(!dark, 2);
            apply_style(dark, scale);
            const auto &style = ImGui::GetStyle();
            const auto &p     = current_ui_palette();
            CHECK(&p == &ui_palette(dark));
            CHECK(style.WindowPadding.x == std::floor(18 * scale));
            CHECK(style.WindowPadding.y == std::floor(14 * scale));
            CHECK(style.FontScaleDpi == scale);
            for (int index = 0; index < ImGuiCol_COUNT; ++index)
            {
                CAPTURE(index);
                const auto color = style.Colors[index];
                for (const float channel : {color.x, color.y, color.z, color.w})
                {
                    CHECK(std::isfinite(channel));
                    CHECK(channel >= 0);
                    CHECK(channel <= 1);
                }
                CHECK(ImGui::ColorConvertFloat4ToU32(color) == ImGui::ColorConvertFloat4ToU32(expected.Colors[index]));
            }
            for (const auto accent : {ImGuiCol_CheckMark, ImGuiCol_TextLink, ImGuiCol_NavCursor, ImGuiCol_ScrollbarGrabActive})
                CHECK(ImGui::ColorConvertFloat4ToU32(style.Colors[accent]) == ImGui::ColorConvertFloat4ToU32(p.accent));
            CHECK(ImGui::ColorConvertFloat4ToU32(p.surface) != ImGui::ColorConvertFloat4ToU32(p.canvas));
            CHECK(std::max({p.canvas.x, p.canvas.y, p.canvas.z}) - std::min({p.canvas.x, p.canvas.y, p.canvas.z}) < 0.03f);
            CHECK(luminance(p.surface) > luminance(p.canvas));
        }
}
