#include "theme.hpp"

#include <cstdint>

namespace liltview
{
namespace
{
constexpr ImVec4 rgb(std::uint32_t hex, float alpha = 1.0f)
{
    return {static_cast<float>((hex >> 16) & 255) / 255, static_cast<float>((hex >> 8) & 255) / 255,
            static_cast<float>(hex & 255) / 255, alpha};
}

// Mist: soft chrome, a white reading surface and a neutral gray image stage.
constexpr UiPalette light{.canvas          = rgb(0xECEEF2),
                          .surface         = rgb(0xF8F9FC),
                          .chrome          = rgb(0xF4F5F8),
                          .popup           = rgb(0xFFFFFF),
                          .text            = rgb(0x242731),
                          .secondary       = rgb(0x626A78),
                          .separator       = rgb(0xD9DDE5),
                          .control         = rgb(0xEAECF1),
                          .hover           = rgb(0xEFEDF9),
                          .pressed         = rgb(0xDAD7F3),
                          .accent          = rgb(0x5E57CC),
                          .scrollbar       = rgb(0xADB3BF),
                          .overlay         = rgb(0x2B2D36, 0.90f),
                          .overlay_hover   = rgb(0x46415F, 0.98f),
                          .overlay_pressed = rgb(0x5B5594),
                          .overlay_text    = rgb(0xF8F9FC),
                          .overlay_border  = rgb(0xFFFFFF, 0.20f),
                          .close_hover     = rgb(0xC9364F),
                          .close_pressed   = rgb(0xAD2540),
                          .error           = rgb(0xAD2540)};

// Graphite: neutral charcoal rather than blue-black; raised panels stay quiet.
constexpr UiPalette dark{.canvas          = rgb(0x17181C),
                         .surface         = rgb(0x202127),
                         .chrome          = rgb(0x1D1E23),
                         .popup           = rgb(0x272830),
                         .text            = rgb(0xE9EAF0),
                         .secondary       = rgb(0xA5A7B3),
                         .separator       = rgb(0x363842),
                         .control         = rgb(0x2B2D35),
                         .hover           = rgb(0x343143),
                         .pressed         = rgb(0x454063),
                         .accent          = rgb(0xAAA3FF),
                         .scrollbar       = rgb(0x555968),
                         .overlay         = rgb(0x2B2D36, 0.92f),
                         .overlay_hover   = rgb(0x46415F, 0.98f),
                         .overlay_pressed = rgb(0x5B5594),
                         .overlay_text    = rgb(0xF8F9FC),
                         .overlay_border  = rgb(0xFFFFFF, 0.20f),
                         .close_hover     = rgb(0xC9364F),
                         .close_pressed   = rgb(0xAD2540),
                         .error           = rgb(0xFF9BAA)};
} // namespace

const UiPalette &ui_palette(bool dark_mode)
{
    return dark_mode ? dark : light;
}

const UiPalette &current_ui_palette()
{
    const auto background = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    return ui_palette(background.x + background.y + background.z < 1.5f);
}

void apply_theme_colors(bool dark_mode)
{
    const auto &p                         = ui_palette(dark_mode);
    auto &style                           = ImGui::GetStyle();
    auto *c                               = style.Colors;
    style.DisabledAlpha                   = 0.48f;
    c[ImGuiCol_Text]                      = p.text;
    c[ImGuiCol_TextDisabled]              = p.secondary;
    c[ImGuiCol_WindowBg]                  = p.surface;
    c[ImGuiCol_ChildBg]                   = p.canvas;
    c[ImGuiCol_PopupBg]                   = p.popup;
    c[ImGuiCol_Border]                    = p.separator;
    c[ImGuiCol_BorderShadow]              = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg]                   = p.control;
    c[ImGuiCol_FrameBgHovered]            = p.hover;
    c[ImGuiCol_FrameBgActive]             = p.pressed;
    c[ImGuiCol_TitleBg]                   = p.chrome;
    c[ImGuiCol_TitleBgActive]             = p.chrome;
    c[ImGuiCol_TitleBgCollapsed]          = p.chrome;
    c[ImGuiCol_MenuBarBg]                 = p.chrome;
    c[ImGuiCol_ScrollbarBg]               = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab]             = p.scrollbar;
    c[ImGuiCol_ScrollbarGrabHovered]      = p.secondary;
    c[ImGuiCol_ScrollbarGrabActive]       = p.accent;
    c[ImGuiCol_CheckMark]                 = p.accent;
    c[ImGuiCol_SliderGrab]                = p.scrollbar;
    c[ImGuiCol_SliderGrabActive]          = p.accent;
    c[ImGuiCol_Button]                    = p.control;
    c[ImGuiCol_ButtonHovered]             = p.hover;
    c[ImGuiCol_ButtonActive]              = p.pressed;
    c[ImGuiCol_Header]                    = p.hover;
    c[ImGuiCol_HeaderHovered]             = p.hover;
    c[ImGuiCol_HeaderActive]              = p.pressed;
    c[ImGuiCol_Separator]                 = p.separator;
    c[ImGuiCol_SeparatorHovered]          = p.accent;
    c[ImGuiCol_SeparatorActive]           = p.accent;
    c[ImGuiCol_ResizeGrip]                = p.separator;
    c[ImGuiCol_ResizeGripHovered]         = p.accent;
    c[ImGuiCol_ResizeGripActive]          = p.accent;
    c[ImGuiCol_InputTextCursor]           = p.accent;
    c[ImGuiCol_Tab]                       = p.control;
    c[ImGuiCol_TabHovered]                = p.hover;
    c[ImGuiCol_TabSelected]               = p.pressed;
    c[ImGuiCol_TabSelectedOverline]       = p.accent;
    c[ImGuiCol_TabDimmed]                 = p.chrome;
    c[ImGuiCol_TabDimmedSelected]         = p.control;
    c[ImGuiCol_TabDimmedSelectedOverline] = p.scrollbar;
    c[ImGuiCol_PlotLines]                 = p.secondary;
    c[ImGuiCol_PlotLinesHovered]          = p.accent;
    c[ImGuiCol_PlotHistogram]             = p.secondary;
    c[ImGuiCol_PlotHistogramHovered]      = p.accent;
    c[ImGuiCol_TableHeaderBg]             = p.control;
    c[ImGuiCol_TableBorderStrong]         = p.separator;
    c[ImGuiCol_TableBorderLight]          = p.separator;
    c[ImGuiCol_TableRowBg]                = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt]             = p.chrome;
    c[ImGuiCol_TextLink]                  = p.accent;
    c[ImGuiCol_TextSelectedBg]            = p.pressed;
    c[ImGuiCol_TreeLines]                 = p.separator;
    c[ImGuiCol_DragDropTarget]            = p.accent;
    c[ImGuiCol_NavCursor]                 = p.accent;
    c[ImGuiCol_NavWindowingHighlight]     = p.accent;
    c[ImGuiCol_NavWindowingDimBg]         = rgb(0x101116, 0.35f);
    c[ImGuiCol_ModalWindowDimBg]          = rgb(0x101116, dark_mode ? 0.62f : 0.32f);
}
} // namespace liltview
