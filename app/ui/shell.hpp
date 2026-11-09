#ifndef LILTVIEW_SHELL_HPP
#define LILTVIEW_SHELL_HPP

#include "menu/menu_model.hpp"
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
                    Viewer &viewer, const WindowChromeState &chrome);
    void dismiss_transient_menus();
    void show_about()
    {
        open_about_ = true;
    }

private:
    MenuAction draw_window_menu(const MenuItem &menu);
    void draw_canvas(Language language, render::Renderer &renderer, render::Image icon, Viewer &, const WindowChromeMetrics &);
    void draw_status(Language language, const Viewer &, const WindowChromeMetrics &);
    void draw_about(Language language, render::Renderer &renderer, render::Image icon, float frame_radius);
    void draw_about_header(Language language, render::Renderer &renderer, render::Image icon);
    void draw_about_licenses(Language language);
    void draw_about_author(Language language);

    bool open_about_       = false;
    float about_dim_ratio_ = 0;
};

void apply_style(bool dark, float scale);
} // namespace liltview

#endif
