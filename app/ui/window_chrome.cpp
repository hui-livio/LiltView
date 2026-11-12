#include "window_chrome.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace liltview
{
WindowChromeMetrics window_chrome_metrics(ImVec2 size, const WindowChromeState &state)
{
    const bool custom = !state.system_titlebar && !state.fullscreen;
    const float scale = std::max(state.scale, 1.0f);
    return {size.x,
            size.y,
            custom ? scale : 0,
            custom ? 36 * scale : 0,
            state.line_height + 12 * scale,
            custom ? 6 * scale : 0,
            44 * scale,
            custom && state.rounded && !state.maximized ? 12 * scale : 0,
            custom,
            state.maximized,
            state.blocked};
}

WindowContentSize window_content_size_for_mode(int width, int height, const WindowChromeState &before,
                                               const WindowChromeState &after)
{
    const auto old         = window_chrome_metrics({static_cast<float>(width), static_cast<float>(height)}, before);
    const auto next        = window_chrome_metrics({static_cast<float>(width), static_cast<float>(height)}, after);
    const auto delta_width = static_cast<int>(std::lround(2 * (next.border - old.border)));
    const auto delta_height =
        static_cast<int>(std::lround(2 * (next.border - old.border) + next.title_height - old.title_height));
    return {std::max(640, width + delta_width), std::max(480, height + delta_height)};
}

std::vector<WindowSpan> rounded_window_spans(int width, int height, float radius, float fringe)
{
    if (width <= 0 || height <= 0)
        return {};
    radius = std::clamp(radius, 0.0f, static_cast<float>(std::min(width, height)) * 0.5f);
    if (radius == 0)
        return {{0, 0, width, height}};
    // Input-only/opaque regions use the pixel-center silhouette (fringe=0).
    // An alpha surface needs a conservative VISUAL region: retain the partial
    // coverage outside the ideal circle and let the compositor blend it.
    const float outer_radius = radius + std::max(0.0f, fringe);
    const int rows           = static_cast<int>(std::ceil(radius));
    std::vector<WindowSpan> spans;
    spans.reserve(static_cast<std::size_t>(rows * 2 + 1));
    for (int y = 0; y < rows; ++y)
    {
        const float dy  = radius - (static_cast<float>(y) + 0.5f);
        const int inset = std::max(
            0, static_cast<int>(std::ceil(radius - std::sqrt(std::max(0.0f, outer_radius * outer_radius - dy * dy)) - 0.5f)));
        spans.push_back({inset, y, width - 2 * inset, 1});
        spans.push_back({inset, height - 1 - y, width - 2 * inset, 1});
    }
    if (height > 2 * rows)
        spans.push_back({0, rows, width, height - 2 * rows});
    return spans;
}

bool window_chrome_contains(const WindowChromeMetrics &m, ImVec2 point)
{
    if (point.x < 0 || point.y < 0 || point.x >= m.width || point.y >= m.height)
        return false;
    if (m.radius == 0)
        return true;
    const float x = point.x + 0.5f, y = point.y + 0.5f;
    const float dx = std::max({m.radius - x, x - (m.width - m.radius), 0.0f});
    const float dy = std::max({m.radius - y, y - (m.height - m.radius), 0.0f});
    return dx * dx + dy * dy <= m.radius * m.radius;
}

WindowHit window_chrome_hit(const WindowChromeMetrics &m, ImVec2 point)
{
    if (!m.custom || m.blocked || !window_chrome_contains(m, point))
        return WindowHit::normal;
    if (!m.maximized)
    {
        const float corner = std::max(m.grip, m.radius);
        if (point.x < corner && point.y < corner)
            return WindowHit::top_left;
        if (point.x >= m.width - corner && point.y < corner)
            return WindowHit::top_right;
        if (point.x < corner && point.y >= m.height - corner)
            return WindowHit::bottom_left;
        if (point.x >= m.width - corner && point.y >= m.height - corner)
            return WindowHit::bottom_right;
        const bool left = point.x < m.grip, right = point.x >= m.width - m.grip;
        const bool top = point.y < m.grip, bottom = point.y >= m.height - m.grip;
        if (top)
            return WindowHit::top;
        if (bottom)
            return WindowHit::bottom;
        if (left)
            return WindowHit::left;
        if (right)
            return WindowHit::right;
    }
    if (point.y >= m.border && point.y < m.title_bottom() && point.x < m.controls_left())
        return WindowHit::drag;
    if (point.y >= m.status_top() && point.y < m.height - m.border)
        return WindowHit::drag;
    return WindowHit::normal;
}

ImVec4 window_border_color()
{
    // Use the resolved, applied style so explicit and system-following themes
    // share the same palette, including immediately after a live theme change.
    const auto background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    const bool dark       = background.x + background.y + background.z < 1.5f;
    // Palette A: mist silver (#818994) / muted blue (#6F8297). Keep the
    // 1-DIP outline opaque; only the rounded silhouette has coverage alpha.
    return ImGui::ColorConvertU32ToFloat4(dark ? IM_COL32(111, 130, 151, 255) : IM_COL32(129, 137, 148, 255));
}

namespace
{
MenuAction title_button(const char *id, Text tooltip, MenuAction action, ImVec2 min, ImVec2 max, const WindowChromeState &state,
                        Language language, unsigned glyph, float radius)
{
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton(id, {max.x - min.x, max.y - min.y});
    auto *list         = ImGui::GetWindowDrawList();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
    {
        const auto &p = current_ui_palette();
        const bool held = ImGui::IsItemActive();
        const ImVec4 hover = glyph == 2 ? (held ? p.close_pressed : p.close_hover) : (held ? p.pressed : p.hover);
        list->AddRectFilled(min, max, ImGui::GetColorU32(hover), glyph == 2 ? radius : 0, ImDrawFlags_RoundCornersTopRight);
        ImGui::SetTooltip("%s", translate(language, tooltip));
    }
    const auto color = ImGui::GetColorU32(hovered && glyph == 2 ? ImVec4{1, 1, 1, 1} : current_ui_palette().text);
    const ImVec2 center{std::round((min.x + max.x) * 0.5f), std::round((min.y + max.y) * 0.5f)};
    const float r = 5 * state.scale, stroke = std::max(1.0f, state.scale);
    if (glyph == 0)
        list->AddLine({center.x - r, center.y}, {center.x + r, center.y}, color, stroke);
    else if (glyph == 2)
    {
        list->AddLine({center.x - r, center.y - r}, {center.x + r, center.y + r}, color, stroke);
        list->AddLine({center.x + r, center.y - r}, {center.x - r, center.y + r}, color, stroke);
    }
    else if (state.maximized)
    {
        // Overlapping outlines distinguish Restore from Maximize without a font icon dependency.
        list->AddPolyline(std::array<ImVec2, 4>{{{center.x - r + 3 * stroke, center.y - r},
                                                 {center.x + r, center.y - r},
                                                 {center.x + r, center.y + r - 3 * stroke},
                                                 {center.x + r - 3 * stroke, center.y + r - 3 * stroke}}}
                              .data(),
                          4, color, 0, stroke);
        list->AddRect({center.x - r, center.y - r + 3 * stroke}, {center.x + r - 3 * stroke, center.y + r}, color, 0, 0, stroke);
    }
    else
        list->AddRect({center.x - r, center.y - r}, {center.x + r, center.y + r}, color, 0, 0, stroke);
    return pressed ? action : MenuAction::none;
}
} // namespace

MenuAction draw_window_titlebar(const WindowChromeMetrics &m, const WindowChromeState &state, Language language,
                                render::Renderer &renderer, render::Image icon)
{
    if (!m.custom)
        return MenuAction::none;
    const auto origin = ImGui::GetWindowPos();
    auto *list        = ImGui::GetWindowDrawList();
    const ImVec2 min{origin.x + m.border, origin.y + m.border};
    const float bottom = origin.y + m.title_bottom();
    list->AddRectFilled(min, {origin.x + m.width - m.border, bottom},
                        ImGui::GetColorU32(current_ui_palette().chrome),
                        std::max(0.0f, m.radius - m.border), ImDrawFlags_RoundCornersTop);
    const float icon_size = 22 * state.scale;
    const ImVec2 icon_min{min.x + 12 * state.scale, min.y + (m.title_height - icon_size) * 0.5f};
    list->PushClipRect(min, {origin.x + m.controls_left(), bottom}, true);
    renderer.add_image(list, icon, {icon_min.x, icon_min.y}, {icon_min.x + icon_size, icon_min.y + icon_size});
    list->AddText({icon_min.x + icon_size + 10 * state.scale, min.y + (m.title_height - ImGui::GetTextLineHeight()) * 0.5f},
                  ImGui::GetColorU32(current_ui_palette().text), "LiltView");
    list->PopClipRect();
    const auto button = [&](unsigned index, const char *id, Text tooltip, MenuAction action)
    {
        const float x = origin.x + m.controls_left() + static_cast<float>(index) * m.button_width;
        return title_button(id, tooltip, action, {x, min.y}, {x + m.button_width, bottom}, state, language, index,
                            std::max(0.0f, m.radius - m.border));
    };
    MenuAction result = button(0, "minimize_window", Text::minimize, MenuAction::minimize);
    const auto maximize =
        button(1, "maximize_window", state.maximized ? Text::restore : Text::maximize, MenuAction::maximize_restore);
    const auto close = button(2, "close_window", Text::close, MenuAction::close_window);
    if (maximize != MenuAction::none)
        result = maximize;
    if (close != MenuAction::none)
        result = close;
    return result;
}

void draw_window_background(const WindowChromeMetrics &m)
{
    if (!m.custom)
        return;
    const auto o = ImGui::GetWindowPos();
    // ImGui already drew the border-colored outer silhouette in Begin(). Keep
    // the content background inset so it cannot leak into the outer AA fringe.
    ImGui::GetWindowDrawList()->AddRectFilled(
        {o.x + m.border, o.y + m.border}, {o.x + m.width - m.border, o.y + m.height - m.border},
        ImGui::GetColorU32(ImGuiCol_WindowBg), std::max(0.0f, m.radius - m.border));
}

void draw_window_border(const WindowChromeMetrics &m)
{
    if (!m.custom)
        return;
    const auto o     = ImGui::GetWindowPos();
    auto *list       = ImGui::GetWindowDrawList();
    const auto color = ImGui::GetColorU32(window_border_color());
    // The background already supplies the rounded ring. A stroked AddRect
    // would add a half-pixel inset/inner fringe and make a 1-DIP line look thick.
    // Preserve foreground straight segments without doubling the curved outline.
    const float inset = m.radius;
    list->AddRectFilled({o.x + inset, o.y}, {o.x + m.width - inset, o.y + m.border}, color);
    list->AddRectFilled({o.x + inset, o.y + m.height - m.border}, {o.x + m.width - inset, o.y + m.height}, color);
    list->AddRectFilled({o.x, o.y + inset}, {o.x + m.border, o.y + m.height - inset}, color);
    list->AddRectFilled({o.x + m.width - m.border, o.y + inset}, {o.x + m.width, o.y + m.height - inset}, color);
}
} // namespace liltview
