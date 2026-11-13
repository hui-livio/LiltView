#ifndef LILTVIEW_SHELL_HPP
#define LILTVIEW_SHELL_HPP

#include "dock_toolbar.hpp"
#include "image_information.hpp"
#include "menu/menu_model.hpp"
#include "navigation_buttons.hpp"
#include "renderer/renderer.hpp"
#include "settings/settings.hpp"
#include "window_chrome.hpp"

#include <imgui.h>

namespace liltview
{
class Viewer;
class Shell
{
public:
    MenuAction draw(Language language, const MenuItem &menu, bool system_menu, render::Renderer &renderer, render::Image icon,
                    Viewer &viewer, const WindowChromeState &chrome, bool input_blocked = false);
    void dismiss_transient_menus();
    bool needs_animation_frame() const noexcept
    {
        return dock_.needs_frames();
    }
    bool dock_keyboard_focus() const noexcept
    {
        return dock_.has_keyboard_focus();
    }
    bool image_info_open() const noexcept
    {
        return image_info_open_;
    }
    void toggle_image_info() noexcept
    {
        image_info_open_ = !image_info_open_;
    }
    bool dismiss_image_info() noexcept
    {
        const bool dismissed = image_info_open_;
        image_info_open_     = false;
        return dismissed;
    }
    void show_about()
    {
        open_about_ = true;
    }

private:
    MenuAction draw_window_menu(const MenuItem &menu);
    MenuAction draw_canvas(Language language, const MenuItem &menu, render::Renderer &renderer, render::Image icon, Viewer &,
                           const WindowChromeMetrics &, bool input_blocked);
    void draw_status(Language language, const Viewer &, const WindowChromeMetrics &);
    void draw_about(Language language, render::Renderer &renderer, render::Image icon, float frame_radius);
    void draw_about_header(Language language, render::Renderer &renderer, render::Image icon);
    void draw_about_licenses(Language language);
    void draw_about_author(Language language);

    bool open_about_           = false;
    bool image_info_open_      = false;
    bool dock_image_info_open_ = false;
    NavigationButtonGesture navigation_gesture_;
    DockToolbar dock_;
    CanvasGesture canvas_gesture_;
    std::uint64_t image_generation_ = 0;
    float about_dim_ratio_ = 0;
};

void apply_style(bool dark, float scale);
} // namespace liltview

#endif
