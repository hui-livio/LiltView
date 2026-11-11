#ifndef LILTVIEW_WINDOW_CHROME_HPP
#define LILTVIEW_WINDOW_CHROME_HPP

#include <imgui.h>
#include <vector>
#include "menu/menu_model.hpp"
#include "renderer/renderer.hpp"

namespace liltview
{
struct WindowChromeState
{
    bool system_titlebar = false;
    bool rounded         = true;
    bool fullscreen      = false;
    bool maximized       = false;
    bool blocked         = false;
    bool dark            = true;
    float scale          = 1;
    float line_height    = 17;
};

enum class WindowHit
{
    normal,
    drag,
    top_left,
    top,
    top_right,
    right,
    bottom_right,
    bottom,
    bottom_left,
    left
};

struct WindowChromeMetrics
{
    float width, height, border, title_height, status_height, grip, button_width, radius;
    bool custom, maximized, blocked;
    float title_bottom() const
    {
        return border + title_height;
    }
    float status_top() const
    {
        return height - border - status_height;
    }
    float controls_left() const
    {
        return width - border - button_width * 3;
    }
};

struct WindowContentSize
{
    int width, height;
};
WindowContentSize window_content_size_for_mode(int width, int height, const WindowChromeState &before,
                                               const WindowChromeState &after);
WindowChromeMetrics window_chrome_metrics(ImVec2 size, const WindowChromeState &);
struct WindowSpan
{
    int x, y, width, height;
};
std::vector<WindowSpan> rounded_window_spans(int width, int height, float radius);
bool window_chrome_contains(const WindowChromeMetrics &, ImVec2 point);
WindowHit window_chrome_hit(const WindowChromeMetrics &, ImVec2 point);
ImVec4 window_border_color();
MenuAction draw_window_titlebar(const WindowChromeMetrics &, const WindowChromeState &, Language, render::Renderer &,
                                render::Image icon);
void draw_window_background(const WindowChromeMetrics &);
void draw_window_border(const WindowChromeMetrics &);
} // namespace liltview

#endif
