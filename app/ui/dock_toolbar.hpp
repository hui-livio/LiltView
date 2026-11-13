#ifndef LILTVIEW_DOCK_TOOLBAR_HPP
#define LILTVIEW_DOCK_TOOLBAR_HPP

#include <array>
#include <imgui.h>
#include <optional>
#include <vector>
#include "menu/menu_model.hpp"

namespace liltview
{
struct DockRect
{
    ImVec2 min{}, max{};
    bool contains(ImVec2 p) const noexcept;
};
enum class DockIcon
{
    previous,
    next,
    zoom_out,
    actual_size,
    fit,
    zoom_in,
    rotate_left,
    rotate_right,
    info,
    fullscreen,
    leave_fullscreen,
    more
};
struct DockButton
{
    DockRect rect;
    MenuAction action = MenuAction::none;
    DockIcon icon     = DockIcon::more;
};
struct DockLayout
{
    DockRect bounds, trigger;
    std::vector<DockButton> buttons;
    std::vector<float> separators;
    bool valid = false;
};
DockLayout dock_layout(ImVec2 canvas_min, ImVec2 canvas_max, float scale, bool fitting, bool fullscreen);
const MenuItem *dock_menu_item(const MenuItem &, MenuAction);
MenuAction dock_toggle_action(const MenuItem &, bool fitting, bool ready);
std::string dock_tooltip(const MenuItem &, const DockButton &, Language);

// Time is monotonic seconds. Visibility is independent of frame rate and canvas layout.
class DockVisibility
{
public:
    void reset() noexcept;
    void cancel_activation() noexcept;
    void update(double now, bool ready, bool trigger_hovered, bool dock_hovered, bool image_held, bool menu_open,
                bool keyboard_focus);
    float opacity() const noexcept
    {
        return alpha_;
    }
    bool needs_frames() const noexcept;

private:
    float evaluate(double now) const noexcept;
    void target(bool show, double now) noexcept;
    std::optional<double> dwell_;
    double fade_start_ = 0;
    float from_ = 0, to_ = 0, alpha_ = 0;
};

// A press owns its entire gesture. Entering an overlay mid-drag cannot steal it;
// presses that began on UI/loading pixels can never become canvas drags/double-clicks.
class CanvasGesture
{
public:
    bool update(bool ready, bool canvas_hit, bool pressed, bool down, bool dragging, bool double_clicked) noexcept;
    bool image_held() const noexcept
    {
        return held_;
    }
    void reset() noexcept;

private:
    bool held_ = false, dragged_ = false, previous_click_ = false;
};

class DockToolbar
{
public:
    // Call before Viewer::draw so both wheel and drag input respect the visible overlay.
    bool prepare(const DockLayout &, bool ready, bool image_held, bool input_blocked, bool retain_visible = false);
    MenuAction draw(const DockLayout &, const MenuItem &, Language, bool ready, bool image_held, bool input_blocked,
                    bool retain_visible = false);
    bool hit(const DockLayout &, ImVec2) const noexcept;
    bool needs_frames() const noexcept
    {
        return visibility_.needs_frames() || tooltip_pending_;
    }
    bool has_keyboard_focus() const noexcept
    {
        return focused_;
    }
    void reset();
    void image_changed() noexcept;

private:
    bool keyboard_focus(const DockLayout &) const;
    DockVisibility visibility_;
    bool tooltip_pending_ = false, focused_ = false, pressed_ = false;
};
} // namespace liltview
#endif
