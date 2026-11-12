#include "navigation_buttons.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>
#include <imgui_internal.h>

namespace liltview
{
bool NavigationButtonRect::contains(ImVec2 point) const noexcept
{
    return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
}

NavigationButtonLayout navigation_button_layout(ImVec2 canvas_min, ImVec2 canvas_max, float scale)
{
    NavigationButtonLayout layout;
    if (!std::isfinite(canvas_min.x) || !std::isfinite(canvas_min.y) || !std::isfinite(canvas_max.x) ||
        !std::isfinite(canvas_max.y) || !std::isfinite(scale) || scale <= 0)
        return layout;
    const float width = canvas_max.x - canvas_min.x, height = canvas_max.y - canvas_min.y;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        return layout;
    const float margin        = std::min(12 * scale, width / 8);
    const float gap           = std::min(8 * scale, width / 8);
    const float button_width  = std::min(40 * scale, (width - 2 * margin - gap) / 2);
    const float button_height = std::min(56 * scale, height);
    const float top           = canvas_min.y + (height - button_height) / 2;
    layout.previous           = {{canvas_min.x + margin, top}, {canvas_min.x + margin + button_width, top + button_height}};
    layout.next               = {{canvas_max.x - margin - button_width, top}, {canvas_max.x - margin, top + button_height}};
    layout.valid              = layout.previous.min.x < layout.previous.max.x && layout.previous.min.y < layout.previous.max.y &&
        layout.next.min.x < layout.next.max.x && layout.next.min.y < layout.next.max.y;
    return layout;
}

NavigationButtonState navigation_button_state(const DirectoryNavigation::Snapshot &snapshot, bool popup_open)
{
    NavigationButtonState state;
    state.visible          = !snapshot.selected_path.empty();
    const bool available   = state.visible && !snapshot.scanning && !snapshot.error && !popup_open;
    state.previous_enabled = available && snapshot.can_previous;
    state.next_enabled     = available && snapshot.can_next;
    return state;
}

bool navigation_buttons_hit(const NavigationButtonLayout &layout, const NavigationButtonState &state, ImVec2 mouse) noexcept
{
    return state.visible && layout.valid && (layout.previous.contains(mouse) || layout.next.contains(mouse));
}

bool NavigationButtonGesture::blocks(const NavigationButtonLayout &layout, const NavigationButtonState &state, ImVec2 mouse,
                                     bool pressed, bool down) noexcept
{
    const bool hit = navigation_buttons_hit(layout, state, mouse);
    if (pressed && hit)
        held = true;
    const bool blocked = hit || held;
    if (!down)
        held = false;
    return blocked;
}

namespace
{
MenuAction draw_button(Language language, const NavigationButtonRect &rect, const char *id, bool previous, bool enabled,
                       float scale)
{
    const ImVec2 size{rect.max.x - rect.min.x, rect.max.y - rect.min.y};
    ImGui::SetCursorScreenPos(rect.min);
    ImGui::BeginDisabled(!enabled);
    // Arrow keys belong to image shortcuts; clicking an overlay must not leave nav focus behind.
    const bool pressed      = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_NoNavFocus);
    const bool hovered      = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
    const bool held         = ImGui::IsItemActive();
    const auto &p = current_ui_palette();
    const ImVec4 background = held ? p.overlay_pressed : hovered ? p.overlay_hover : p.overlay;
    auto *draw              = ImGui::GetWindowDrawList();
    const float rounding    = std::min({8 * scale, size.x / 2, size.y / 2});
    draw->AddRectFilled(rect.min, rect.max, ImGui::GetColorU32(background), rounding);
    draw->AddRect(rect.min, rect.max, ImGui::GetColorU32(p.overlay_border), rounding, 0, std::max(1.0f, scale));
    const ImVec2 center{rect.min.x + size.x / 2, rect.min.y + size.y / 2};
    const float half_height = std::min(9 * scale, size.y / 4);
    const float half_width  = std::min(4.5f * scale, size.x / 4);
    const float sign        = previous ? -1.0f : 1.0f;
    const ImVec2 arrow[]{{center.x - sign * half_width, center.y - half_height},
                         {center.x + sign * half_width, center.y},
                         {center.x - sign * half_width, center.y + half_height}};
    const float thickness = std::min({2 * scale, size.x / 8, size.y / 8});
    draw->AddPolyline(arrow, 3, ImGui::GetColorU32(p.overlay_text), ImDrawFlags_None, thickness);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_NoNavOverride))
        ImGui::SetTooltip("%s", translate(language, previous ? Text::previous : Text::next));
    ImGui::EndDisabled();
    return pressed && enabled ? (previous ? MenuAction::previous : MenuAction::next) : MenuAction::none;
}
} // namespace

MenuAction draw_navigation_buttons(Language language, const DirectoryNavigation::Snapshot &snapshot, ImVec2 canvas_min,
                                   ImVec2 canvas_max, float scale)
{
    const auto layout     = navigation_button_layout(canvas_min, canvas_max, scale);
    const bool popup_open = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    const auto state      = navigation_button_state(snapshot, popup_open);
    if (!layout.valid || !state.visible)
        return MenuAction::none;
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::PushID("canvas_navigation");
    ImGui::PushClipRect(canvas_min, canvas_max, true);
    auto action     = draw_button(language, layout.previous, "previous", true, state.previous_enabled, scale);
    const auto next = draw_button(language, layout.next, "next", false, state.next_enabled, scale);
    if (next != MenuAction::none)
        action = next;
    ImGui::PopClipRect();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(cursor);
    return action;
}
} // namespace liltview
