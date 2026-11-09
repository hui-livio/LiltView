#ifndef LILTVIEW_NAVIGATION_BUTTONS_HPP
#define LILTVIEW_NAVIGATION_BUTTONS_HPP

#include "menu/menu_model.hpp"
#include "viewer/navigation.hpp"

#include <imgui.h>

namespace liltview
{
struct NavigationButtonRect
{
    ImVec2 min{}, max{};
    bool contains(ImVec2 point) const noexcept;
};

struct NavigationButtonLayout
{
    NavigationButtonRect previous, next;
    bool valid = false;
};

struct NavigationButtonState
{
    bool visible = false, previous_enabled = false, next_enabled = false;
};

NavigationButtonLayout navigation_button_layout(ImVec2 canvas_min, ImVec2 canvas_max, float scale);
NavigationButtonState navigation_button_state(const DirectoryNavigation::Snapshot &, bool popup_open = false);
// Disabled buttons still exclude image pan/zoom beneath their visible bounds.
bool navigation_buttons_hit(const NavigationButtonLayout &, const NavigationButtonState &, ImVec2 mouse) noexcept;
struct NavigationButtonGesture
{
    bool held = false;
    // A press on either visible button owns the whole gesture, including its release frame.
    bool blocks(const NavigationButtonLayout &, const NavigationButtonState &, ImVec2 mouse, bool pressed, bool down) noexcept;
};
// Draw after the image, in its canvas window, so normal popup layering is retained.
MenuAction draw_navigation_buttons(Language, const DirectoryNavigation::Snapshot &, ImVec2 canvas_min, ImVec2 canvas_max,
                                   float scale);
} // namespace liltview

#endif
