#ifndef LILTVIEW_SHELL_HPP
#define LILTVIEW_SHELL_HPP

#include "menu/menu_model.hpp"
#include "settings/settings.hpp"

#include <imgui.h>

namespace liltview
{
struct ShellActions
{
    bool quit              = false;
    bool toggle_fullscreen = false;
};

class Shell
{
public:
    ShellActions draw(Settings &settings, Language language, const MenuItem &menu, bool system_menu, bool fullscreen,
                      ImTextureID icon);
    void show_about()
    {
        open_about_ = true;
    }

private:
    ShellActions draw_window_menu(Settings &settings, const MenuItem &menu);
    bool draw_toolbar(Language language, bool fullscreen);
    void draw_canvas(Language language, ImTextureID icon);
    void draw_status(Language language);
    void draw_about(Language language, ImTextureID icon);
    void draw_about_header(Language language, ImTextureID icon);
    void draw_about_licenses(Language language);
    void draw_about_author(Language language);

    bool open_about_ = false;
};

void apply_style(bool dark, float scale);
} // namespace liltview

#endif
