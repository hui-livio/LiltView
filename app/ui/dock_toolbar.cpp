#include "dock_toolbar.hpp"
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include "menu/imgui_menu.hpp"
#include "theme.hpp"

namespace liltview
{
bool DockRect::contains(ImVec2 p) const noexcept
{
    return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y;
}

DockLayout dock_layout(ImVec2 min, ImVec2 max, float scale, bool fitting, bool fullscreen)
{
    DockLayout out;
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(max.x) ||
        !std::isfinite(max.y) || max.x <= min.x || max.y <= min.y)
        return out;
    out.trigger      = {{min.x, std::max(min.y, max.y - 32 * scale)}, max};
    const float room = (max.x - min.x) / scale - 24; // Canvas margins, not image whitespace.
    if (room < 48 || max.y - min.y < 80 * scale)
        return out;
    bool rotation = room >= 444, navigation = room >= 348, zoom = room >= 252, extras = room >= 120;
    // Collapse rotations, then previous/next, then zoom/extras only on extreme canvases.
    std::vector<std::vector<std::pair<MenuAction, DockIcon>>> groups;
    if (navigation)
        groups.push_back({{MenuAction::previous, DockIcon::previous}, {MenuAction::next, DockIcon::next}});
    if (zoom)
        groups.push_back({{MenuAction::zoom_out, DockIcon::zoom_out},
                          {fitting ? MenuAction::actual_size : MenuAction::fit, fitting ? DockIcon::actual_size : DockIcon::fit},
                          {MenuAction::zoom_in, DockIcon::zoom_in}});
    if (rotation)
        groups.push_back({{MenuAction::rotate_left, DockIcon::rotate_left}, {MenuAction::rotate_right, DockIcon::rotate_right}});
    groups.push_back({});
    if (extras)
    {
        groups.back().push_back({MenuAction::image_info, DockIcon::info});
        groups.back().push_back({MenuAction::fullscreen, fullscreen ? DockIcon::leave_fullscreen : DockIcon::fullscreen});
    }
    groups.back().push_back({MenuAction::none, DockIcon::more});
    std::size_t count = 0;
    for (const auto &group : groups)
        count += group.size();
    const float width = (12 + 36 * count + 24 * (groups.size() - 1)) * scale;
    out.bounds        = {{(min.x + max.x - width) / 2, max.y - 64 * scale}, {(min.x + max.x + width) / 2, max.y - 16 * scale}};
    float x           = out.bounds.min.x + 6 * scale;
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        if (i)
        {
            out.separators.push_back(x + 12 * scale);
            x += 24 * scale;
        }
        for (const auto &[action, icon] : groups[i])
        {
            out.buttons.push_back(
                {{{x, out.bounds.min.y + 6 * scale}, {x + 36 * scale, out.bounds.max.y - 6 * scale}}, action, icon});
            x += 36 * scale;
        }
    }
    out.valid = true;
    return out;
}

const MenuItem *dock_menu_item(const MenuItem &root, MenuAction action)
{
    if (action == MenuAction::none)
        return nullptr;
    if (root.action == action)
        return &root;
    for (const auto &child : root.children)
        if (const auto *item = dock_menu_item(child, action))
            return item;
    return nullptr;
}

MenuAction dock_toggle_action(const MenuItem &menu, bool fitting, bool ready)
{
    const auto action = fitting ? MenuAction::actual_size : MenuAction::fit;
    const auto *view  = find_menu_item(menu, 2);
    const auto *item  = view ? dock_menu_item(*view, action) : nullptr;
    return ready && view && view->enabled && item && item->enabled ? action : MenuAction::none;
}

std::string dock_tooltip(const MenuItem &menu, const DockButton &button, Language language)
{
    if (button.icon == DockIcon::more)
        return translate(language, Text::more);
    const auto *item = dock_menu_item(menu, button.action);
    if (!item)
        return {};
    std::string text    = button.icon == DockIcon::actual_size ? translate(language, Text::original_size) : item->label;
    const auto shortcut = menu_shortcut_label(item->shortcut);
    if (!shortcut.empty())
        text += " · " + shortcut;
    return text;
}

void DockVisibility::reset() noexcept
{
    dwell_.reset();
    fade_start_ = 0;
    from_ = to_ = alpha_ = 0;
}
void DockVisibility::cancel_activation() noexcept
{
    // Pending dwell belongs to the old image, but a visible overlay belongs to
    // the canvas. Never send it through hidden/fade-in again on navigation.
    dwell_.reset();
    if (alpha_ == 0)
        reset();
}
float DockVisibility::evaluate(double now) const noexcept
{
    const float step = static_cast<float>(std::clamp((now - fade_start_) / 0.120, 0.0, 1.0));
    return from_ + (to_ - from_) * step;
}
void DockVisibility::target(bool show, double now) noexcept
{
    const float next = show ? 1.0f : 0.0f;
    if (next == to_)
        return;
    from_       = evaluate(now);
    fade_start_ = now;
    to_         = next;
}
void DockVisibility::update(double now, bool ready, bool trigger, bool dock, bool image_held, bool menu, bool focus)
{
    if (!ready)
    {
        reset();
        return;
    }
    alpha_             = evaluate(now);
    const bool pointer = !image_held && (trigger || (alpha_ > 0 && dock));
    if (menu || focus || (pointer && alpha_ > 0))
    {
        dwell_.reset();
        target(true, now);
    }
    else if (pointer)
    {
        if (!dwell_)
            dwell_ = now;
        if (now - *dwell_ >= 0.200 - 1e-9)
        {
            target(true, *dwell_ + 0.200);
            dwell_.reset();
        }
    }
    else
    {
        dwell_.reset();
        target(false, now);
    }
    alpha_ = evaluate(now);
}
bool DockVisibility::needs_frames() const noexcept
{
    return dwell_.has_value() || alpha_ != to_;
}

void CanvasGesture::reset() noexcept
{
    held_ = dragged_ = previous_click_ = false;
}
bool CanvasGesture::update(bool ready, bool hit, bool pressed, bool down, bool dragging, bool double_clicked) noexcept
{
    if (!ready)
    {
        previous_click_ = false;
        if (pressed || !down)
            held_ = dragged_ = false;
        return false;
    }
    bool toggle = false;
    if (pressed)
    {
        toggle          = hit && previous_click_ && double_clicked && !dragging;
        held_           = hit;
        dragged_        = false;
        previous_click_ = false;
    }
    if (held_ && dragging)
        dragged_ = true;
    if (!down)
    {
        if (held_)
            previous_click_ = !dragged_ && hit;
        held_ = dragged_ = false;
    }
    return toggle;
}

namespace
{
constexpr auto popup = "dock_view_menu";
ImGuiID button_id(const DockButton &button)
{
    // Mode changes replace icons/labels, not widget identity or keyboard focus.
    const int id = (button.action == MenuAction::fit || button.action == MenuAction::actual_size) ?
        static_cast<int>(MenuAction::fit) :
        static_cast<int>(button.action);
    return ImGui::GetID(id);
}
void icon(ImDrawList *draw, DockIcon type, ImVec2 center, float s, ImU32 color)
{
    const auto p = [=](float x, float y)
    {
        return ImVec2{center.x + x * s, center.y + y * s};
    };
    const auto line = [&](float x1, float y1, float x2, float y2)
    {
        draw->AddLine(p(x1, y1), p(x2, y2), color, 1.65f * s);
    };
    if (type == DockIcon::previous || type == DockIcon::next)
    {
        const float sign = type == DockIcon::previous ? -1.0f : 1.0f;
        line(-3 * sign, -7, 3 * sign, 0);
        line(3 * sign, 0, -3 * sign, 7);
    }
    else if (type == DockIcon::zoom_in || type == DockIcon::zoom_out)
    {
        draw->AddCircle(p(-2.5f, -2.5f), 5.5f * s, color, 24, 1.65f * s);
        line(1.5f, 1.5f, 8, 8);
        line(-5.2f, -2.5f, 0.2f, -2.5f);
        if (type == DockIcon::zoom_in)
            line(-2.5f, -5.2f, -2.5f, 0.2f);
    }
    else if (type == DockIcon::actual_size)
    {
        const float size     = 12 * s;
        const auto text_size = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, "1:1");
        draw->AddText(ImGui::GetFont(), size, {center.x - text_size.x / 2, center.y - text_size.y / 2}, color, "1:1");
    }
    else if (type == DockIcon::rotate_left || type == DockIcon::rotate_right)
    {
        const float sign = type == DockIcon::rotate_left ? 1.0f : -1.0f;
        draw->PathClear();
        for (int i = 0; i <= 28; ++i)
        {
            const float a = (-150 + 290.0f * i / 28) * IM_PI / 180;
            draw->PathLineTo(p(sign * std::cos(a) * 7, std::sin(a) * 7));
        }
        draw->PathStroke(color, ImDrawFlags_None, 1.65f * s);
        line(sign * -6.06f, -3.5f, sign * -6.06f, -8);
        line(sign * -6.06f, -3.5f, sign * -1.5f, -3.5f);
    }
    else if (type == DockIcon::info)
    {
        draw->AddCircle(center, 7.5f * s, color, 32, 1.65f * s);
        draw->AddCircleFilled(p(0, -3.5f), 1 * s, color);
        line(0, -0.5f, 0, 4);
    }
    else if (type == DockIcon::more)
    {
        for (float x : {-6.0f, 0.0f, 6.0f})
            draw->AddCircleFilled(p(x, 0), 1.4f * s, color);
    }
    else
    {
        // Fit has a center image rectangle; fullscreen uses four open corners.
        for (const float x : {-1.0f, 1.0f})
            for (const float y : {-1.0f, 1.0f})
            {
                const bool inward = type == DockIcon::leave_fullscreen;
                const float a = inward ? 3.5f : 8, b = inward ? 8 : 3.5f;
                line(x * a, y * a, x * b, y * a);
                line(x * a, y * a, x * a, y * b);
            }
        if (type == DockIcon::fit)
            draw->AddRect(p(-4, -3), p(4, 3), color, 0, 0, 1.4f * s);
    }
}
} // namespace

bool DockToolbar::hit(const DockLayout &layout, ImVec2 mouse) const noexcept
{
    return layout.valid && visibility_.opacity() > 0 && layout.bounds.contains(mouse);
}
void DockToolbar::reset()
{
    visibility_.reset();
    tooltip_pending_ = focused_ = pressed_ = false;
}
void DockToolbar::image_changed() noexcept
{
    visibility_.cancel_activation();
    tooltip_pending_ = focused_ = pressed_ = false;
}
bool DockToolbar::keyboard_focus(const DockLayout &layout) const
{
    const auto &g = *ImGui::GetCurrentContext();
    if (!g.NavCursorVisible || g.NavWindow != ImGui::GetCurrentWindow())
        return false;
    ImGui::PushID("dock_toolbar");
    bool focused = false;
    for (const auto &button : layout.buttons)
        focused |= g.NavId == button_id(button);
    ImGui::PopID();
    return focused;
}
bool DockToolbar::prepare(const DockLayout &layout, bool ready, bool image_held, bool blocked, bool retain_visible)
{
    const bool more      = ImGui::IsPopupOpen(popup);
    const bool any_popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    // IsWindowHovered() normally becomes false while ANY widget is active,
    // including a button in this very window. That is not a pointer exit.
    const bool hovered = !blocked && !any_popup && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const auto mouse   = ImGui::GetIO().MousePos;
    if (!ready || blocked || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        pressed_ = false;
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        pressed_ = hovered && hit(layout, mouse);
    const bool image_drag = image_held && !pressed_;
    // Camera restores and image replacement must not flash a visible overlay
    // off; retaining it still cannot reveal previously hidden controls.
    const bool visible_ready = ready || (retain_visible && visibility_.opacity() > 0);
    visibility_.update(ImGui::GetTime(), visible_ready && layout.valid && !blocked && (!any_popup || more),
                       hovered && layout.trigger.contains(mouse), hovered && hit(layout, mouse), image_drag, more,
                       pressed_ || keyboard_focus(layout));
    return !image_drag && hit(layout, mouse);
}

MenuAction DockToolbar::draw(const DockLayout &layout, const MenuItem &menu, Language language, bool ready, bool image_held,
                             bool blocked, bool retain_visible)
{
    tooltip_pending_   = false;
    const auto *view   = find_menu_item(menu, 2);
    const bool display = (ready || retain_visible) && layout.valid && view && view->enabled && !blocked;
    ready              = ready && display;
    if (!display)
        reset();
    const float alpha     = visibility_.opacity();
    const bool image_drag = image_held && !pressed_;
    const bool more_open  = ImGui::IsPopupOpen(popup);
    MenuAction action     = MenuAction::none;
    bool open_more        = false;
    if (display && alpha > 0)
    {
        const auto cursor   = ImGui::GetCursorScreenPos();
        const auto &palette = current_ui_palette();
        const float s       = (layout.bounds.max.y - layout.bounds.min.y) / 48;
        ImGui::PushID("dock_toolbar");
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
        ImGui::PushClipRect(layout.bounds.min, layout.bounds.max, true);
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(layout.bounds.min, layout.bounds.max, ImGui::GetColorU32(palette.popup), 14 * s);
        draw->AddRect(layout.bounds.min, layout.bounds.max, ImGui::GetColorU32(palette.separator), 14 * s, 0, s);
        for (const auto x : layout.separators)
            draw->AddLine({x, layout.bounds.min.y + 12 * s}, {x, layout.bounds.max.y - 12 * s},
                          ImGui::GetColorU32(palette.separator), s);
        for (const auto &button : layout.buttons)
        {
            const auto *item    = dock_menu_item(*view, button.action);
            const bool enabled  = ready && !image_drag && (button.icon == DockIcon::more || (item && item->enabled));
            const bool selected = (item && item->checkable && item->checked) || (button.icon == DockIcon::more && more_open);
            ImGui::SetCursorScreenPos(button.rect.min);
            ImGui::BeginDisabled(!enabled);
            // The ID is generated from the action, including a stable shared FIT/100% slot.
            ImGui::PushOverrideID(button_id(button));
            const bool pressed = ImGui::InvisibleButton("", {36 * s, 36 * s}, ImGuiButtonFlags_EnableNav);
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
            const bool focused = ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible;
            if (selected || hovered || ImGui::IsItemActive() || focused)
            {
                const auto bg = ImGui::IsItemActive() ? palette.pressed : palette.hover;
                draw->AddRectFilled(button.rect.min, button.rect.max, ImGui::GetColorU32(bg), 8 * s);
                if (focused)
                    draw->AddRect(button.rect.min, button.rect.max, ImGui::GetColorU32(palette.accent), 8 * s, 0, s);
            }
            const ImVec2 center{button.rect.min.x + 18 * s, button.rect.min.y + 18 * s};
            icon(draw, button.icon, center, s, ImGui::GetColorU32(selected || hovered ? palette.accent : palette.text));
            if (pressed && enabled)
            {
                if (button.icon == DockIcon::more)
                    open_more = true;
                else
                    action = button.action;
            }
            ImGui::EndDisabled();
            const bool tooltip_hover =
                ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_NoNavOverride);
            const auto &g = *ImGui::GetCurrentContext();
            if (tooltip_hover && !image_drag && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                tooltip_pending_ = g.HoveredIdTimer < 0.450f;
                if (!tooltip_pending_)
                {
                    const auto text = dock_tooltip(*view, button, language);
                    ImGui::SetNextWindowPos({center.x, layout.bounds.min.y - 8 * s}, ImGuiCond_Always, {0.5f, 1});
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(text.c_str());
                    ImGui::EndTooltip();
                }
            }
        }
        ImGui::PopClipRect();
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(cursor);
    }
    if (open_more)
        ImGui::OpenPopup(popup);
    if (display && ImGui::IsPopupOpen(popup))
    {
        const float scale            = (layout.bounds.max.y - layout.bounds.min.y) / 48;
        const auto *viewport         = ImGui::GetMainViewport();
        const float padding          = ImGui::GetStyle().DisplaySafeAreaPadding.y;
        const float bottom           = layout.bounds.min.y - 8 * scale;
        const float available_height = std::max(1.0f, bottom - viewport->WorkPos.y - padding);
        // Attach to the Dock, not the pointer. Auto-resize is height-constrained
        // so a short window scrolls the menu above the still-visible More icon.
        ImGui::SetNextWindowPos({layout.bounds.max.x, bottom}, ImGuiCond_Always, {1, 1});
        ImGui::SetNextWindowSizeConstraints({0, 0}, {std::max(1.0f, viewport->WorkSize.x - 2 * padding), available_height});
    }
    if (ImGui::BeginPopup(popup))
    {
        if (display)
        {
            ImGui::BeginDisabled(!ready);
            const auto selected = draw_menu(*view);
            if (ready && selected != MenuAction::none)
                action = selected;
            ImGui::EndDisabled();
        }
        else
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    focused_ = ready && keyboard_focus(layout);
    return action;
}
} // namespace liltview
