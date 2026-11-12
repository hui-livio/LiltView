#ifndef LILTVIEW_UI_THEME_HPP
#define LILTVIEW_UI_THEME_HPP

#include <imgui.h>

namespace liltview
{
// Semantic colors shared by ImGui widgets and custom-drawn chrome. Keep the
// image/canvas neutral; the blue-violet accent belongs to interaction only.
struct UiPalette
{
    ImVec4 canvas, surface, chrome, popup;
    ImVec4 text, secondary, separator;
    ImVec4 control, hover, pressed, accent, scrollbar;
    ImVec4 overlay, overlay_hover, overlay_pressed, overlay_text, overlay_border;
    ImVec4 close_hover, close_pressed, error;
};

const UiPalette &ui_palette(bool dark);
// Resolve from the applied style, not the persisted/system theme preference.
const UiPalette &current_ui_palette();
void apply_theme_colors(bool dark);
} // namespace liltview

#endif
