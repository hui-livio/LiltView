#include "shell.hpp"
#include "theme.hpp"
#include "status_bar.hpp"

#include "version.hpp"
#include "viewer/viewer.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace liltview
{
namespace
{
std::string label(Language language, Text text, const char *id)
{
    return std::string(translate(language, text)) + "###" + id;
}

void centered_text(const char *text, float width, bool muted = false)
{
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (width - ImGui::CalcTextSize(text).x) * 0.5f));
    if (muted)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text);
    if (muted)
        ImGui::PopStyleColor();
}

void aligned_image(render::Renderer &renderer, render::Image image, ImVec2 size)
{
    // 对齐到实际 framebuffer 像素，避免居中布局的小数坐标造成额外的半像素模糊。
    const auto density = ImGui::GetIO().DisplayFramebufferScale;
    auto position      = ImGui::GetCursorScreenPos();
    position.x         = std::round(position.x * density.x) / density.x;
    position.y         = std::round(position.y * density.y) / density.y;
    size.x             = std::round(size.x * density.x) / density.x;
    size.y             = std::round(size.y * density.y) / density.y;
    ImGui::SetCursorScreenPos(position);
    renderer.add_image(ImGui::GetWindowDrawList(), image, {position.x, position.y}, {position.x + size.x, position.y + size.y});
    ImGui::Dummy(size);
}

Text image_error_text(lv_error_code code)
{
    switch (code)
    {
    case LV_ERROR_IO:
        return Text::image_io_error;
    case LV_ERROR_FORMAT_UNSUPPORTED:
        return Text::image_unsupported;
    case LV_ERROR_CORRUPT_DATA:
        return Text::image_corrupt;
    case LV_ERROR_DIMENSION_LIMIT:
        return Text::image_limit;
    case LV_ERROR_MEMORY_BUDGET:
    case LV_ERROR_OUT_OF_MEMORY:
        return Text::image_memory;
    case LV_ERROR_SOURCE_CHANGED:
        return Text::image_changed;
    default:
        return Text::image_error;
    }
}

// Theme-aware broken-image illustration, not the previous image. The reason is
// ordinary wrapped text directly below it, readable without hover or a dialog.
void draw_image_failure(Language language, Text reason, ImVec2 min, ImVec2 max, float scale)
{
    const auto &palette      = current_ui_palette();
    const float width        = std::max(1.0f, max.x - min.x);
    const float wrap         = std::max(1.0f, std::min(520 * scale, width - 48 * scale));
    const char *message      = translate(language, reason == Text::image_unsupported ? Text::unsupported_format : reason);
    const auto message_size  = ImGui::CalcTextSize(message, nullptr, false, wrap);
    const float title_height = ImGui::GetTextLineHeight();
    const float icon_size    = std::clamp(max.y - min.y - title_height - message_size.y - 50 * scale, 0.0f, 72 * scale);
    const float icon_scale   = icon_size / 72;
    const float height       = icon_size + 22 * scale + title_height + 12 * scale + message_size.y;
    const float top          = std::max(min.y + 8 * scale, min.y + (max.y - min.y - height) * 0.5f);
    const ImVec2 icon_min{min.x + (width - icon_size) * 0.5f, top};
    const auto pt = [&](float x, float y)
    {
        return ImVec2{icon_min.x + x * icon_scale, icon_min.y + y * icon_scale};
    };
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pt(0, 0), pt(72, 72), ImGui::GetColorU32(palette.surface), 16 * icon_scale);
    draw->AddRect(pt(12, 16), pt(58, 54), ImGui::GetColorU32(palette.secondary), 4 * icon_scale, 0, 1.5f * icon_scale);
    draw->AddCircleFilled(pt(24, 28), 4 * icon_scale, ImGui::GetColorU32(palette.secondary));
    const ImVec2 mountain[]{pt(15, 49), pt(29, 36), pt(39, 44), pt(47, 37), pt(56, 46)};
    draw->AddPolyline(mountain, 5, ImGui::GetColorU32(palette.secondary), 0, 1.5f * icon_scale);
    draw->AddCircleFilled(pt(56, 54), 13 * icon_scale, ImGui::GetColorU32(palette.surface));
    draw->AddCircle(pt(56, 54), 10 * icon_scale, ImGui::GetColorU32(palette.error), 0, 1.5f * icon_scale);
    draw->AddLine(pt(52, 50), pt(60, 58), ImGui::GetColorU32(palette.error), 1.8f * icon_scale);
    draw->AddLine(pt(60, 50), pt(52, 58), ImGui::GetColorU32(palette.error), 1.8f * icon_scale);
    const auto *title = translate(language, Text::image_load_failed);
    ImGui::SetCursorScreenPos(
        {min.x + std::max(0.0f, (width - ImGui::CalcTextSize(title).x) * 0.5f), top + icon_size + 22 * scale});
    ImGui::TextUnformatted(title);
    ImGui::SetCursorScreenPos(
        {min.x + (width - message_size.x) * 0.5f, top + icon_size + 22 * scale + title_height + 12 * scale});
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::PushStyleColor(ImGuiCol_Text, palette.secondary);
    ImGui::TextUnformatted(message);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

// Keep small chrome arcs smooth at fractional UI scales and dense framebuffers.
// This also applies to the root silhouette drawn inside Begin(). Restore shared
// ImGui data afterwards so this does not change other windows' drawing policy.
struct ChromeCurveQuality
{
    ImDrawListSharedData &data = *ImGui::GetDrawListSharedData();
    float saved_error          = data.CircleSegmentMaxError;
    explicit ChromeCurveQuality(bool rounded)
    {
        if (rounded)
        {
            const auto density = ImGui::GetIO().DisplayFramebufferScale;
            data.SetCircleTessellationMaxError(std::min(saved_error, 0.1f / std::max({1.0f, density.x, density.y})));
        }
    }
    ~ChromeCurveQuality()
    {
        data.SetCircleTessellationMaxError(saved_error);
    }
};

void begin_main_window(bool system_menu, bool custom_titlebar, float radius)
{
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    // Preserve the original main-window flags and ImGui background behavior.
    // Only tint its outer silhouette; the content background is drawn inset.
    if (custom_titlebar)
        ImGui::PushStyleColor(ImGuiCol_WindowBg, window_border_color());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, radius);
    ImGui::Begin("LiltView###main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ((system_menu || custom_titlebar) ? 0 : ImGuiWindowFlags_MenuBar));
    ImGui::PopStyleVar(2);
    if (custom_titlebar)
        ImGui::PopStyleColor();
}

void rounded_modal_dim(float radius, ImVec4 color)
{
    const auto *viewport = ImGui::GetMainViewport();
    const ImVec2 max{viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y};
    auto *list = ImGui::GetWindowDrawList();
    // Like ImGui's modal dim insertion, prepend a dedicated draw command behind
    // this popup's background. Unlike its viewport-sized quad, preserve alpha
    // outside the rounded native window (including while About is open).
    list->AddDrawCmd();
    list->PushClipRect(viewport->Pos, max, false);
    list->AddRectFilled(viewport->Pos, max, ImGui::GetColorU32(color), radius);
    const auto command = list->CmdBuffer.back();
    list->CmdBuffer.pop_back();
    list->CmdBuffer.push_front(command);
    list->AddDrawCmd();
    list->PopClipRect();
}

MenuAction draw_menu(const MenuItem &root)
{
    MenuAction result = MenuAction::none;
    for (const auto &item : root.children)
    {
        if (item.separator)
        {
            ImGui::Separator();
            continue;
        }
        const auto title = item.label + "###menu_" + std::to_string(item.id);
        if (!item.children.empty())
        {
            if (ImGui::BeginMenu(title.c_str(), item.enabled))
            {
                const auto selected = draw_menu(item);
                if (selected != MenuAction::none)
                    result = selected;
                ImGui::EndMenu();
            }
        }
        else
        {
            std::string shortcut;
            for (const auto &key : item.shortcut)
            {
                if (!shortcut.empty())
                    shortcut += "+";
                if (key == "Control")
                    shortcut += "Ctrl";
                else if (key == "Up")
                    shortcut += "↑";
                else if (key == "Down")
                    shortcut += "↓";
                else if (key == "Left")
                    shortcut += "←";
                else if (key == "Right")
                    shortcut += "→";
                else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
                    shortcut += static_cast<char>(key[0] - 'a' + 'A');
                else
                    shortcut += key;
            }
            if (ImGui::MenuItem(title.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), item.checked, item.enabled))
                result = item.action;
        }
    }
    return result;
}
} // namespace

void apply_style(bool dark, float scale)
{
    ImGui::GetStyle() = ImGuiStyle{};
    if (dark)
        ImGui::StyleColorsDark();
    else
        ImGui::StyleColorsLight();
    auto &style            = ImGui::GetStyle();
    style.WindowPadding    = {18.0f, 14.0f};
    style.FramePadding     = {12.0f, 7.0f};
    style.ItemSpacing      = {10.0f, 10.0f};
    style.WindowRounding   = 8.0f;
    style.FrameRounding    = 5.0f;
    style.PopupRounding    = 7.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;
    apply_theme_colors(dark);
    style.ScaleAllSizes(scale);
    style.FontSizeBase = 17.0f;
    style.FontScaleDpi = scale;
}

void Shell::dismiss_transient_menus()
{
    ImGui::ClosePopupsExceptModals();
}

MenuAction Shell::draw(Language language, const MenuItem &menu, bool system_menu, render::Renderer &renderer, render::Image icon,
                       Viewer &viewer, const WindowChromeState &chrome)
{
    // 按区域组织一帧；窗口、子画布和弹窗的 Begin/End 在对应函数中配对。
    const auto metrics = window_chrome_metrics(ImGui::GetMainViewport()->Size, chrome);
    const ChromeCurveQuality quality(metrics.custom && metrics.radius > 0);
    MenuAction action = MenuAction::none;
    begin_main_window(system_menu, metrics.custom, metrics.radius);
    draw_window_background(metrics);
    if (metrics.custom)
    {
        action = draw_window_titlebar(metrics, chrome, language, renderer, icon);
        ImGui::SetCursorPos({metrics.border, metrics.title_bottom()});
        if (!system_menu)
        {
            const float menu_height = ImGui::GetFrameHeight();
            ImGui::BeginChild("window_menu", {metrics.width - 2 * metrics.border, menu_height}, ImGuiChildFlags_None,
                              ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const auto selected = draw_window_menu(menu);
            if (selected != MenuAction::none)
                action = selected;
            ImGui::EndChild();
            ImGui::SetCursorPos({metrics.border, metrics.title_bottom() + menu_height});
        }
    }
    else if (!system_menu)
        action = draw_window_menu(menu);
    if (action == MenuAction::quit || action == MenuAction::close_window)
    {
        // 菜单 / 自绘关闭按钮直接交还应用层，不再准备或上传图片。
        ImGui::End();
        return action;
    }

    const auto canvas_action = draw_canvas(language, renderer, icon, viewer, metrics);
    if (canvas_action != MenuAction::none)
        action = canvas_action;
    draw_status(language, viewer, metrics);
    draw_window_border(metrics);
    draw_about(language, renderer, icon, metrics.radius);
    ImGui::End();
    return action;
}

MenuAction Shell::draw_window_menu(const MenuItem &menu)
{
    if (!ImGui::BeginMenuBar())
        return MenuAction::none;
    // 画布贴边不影响菜单文字原有的水平缩进。
    ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    const auto selected = draw_menu(menu);
    ImGui::EndMenuBar();
    return selected;
}

MenuAction Shell::draw_canvas(Language language, render::Renderer &renderer, render::Image icon, Viewer &viewer,
                              const WindowChromeMetrics &chrome)
{
    // 画布填满无内边距的主窗口，仅为底部状态栏预留高度。
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    ImGui::SetCursorPosX(chrome.border);
    const float canvas_height = std::max(1.0f, ImGui::GetWindowPos().y + chrome.status_top() - ImGui::GetCursorScreenPos().y);
    const auto scale          = ImGui::GetStyle().FontScaleDpi;
    const auto body_min       = ImGui::GetCursorScreenPos();
    const ImVec2 body_max{body_min.x + std::max(1.0f, chrome.width - 2 * chrome.border), body_min.y + canvas_height};
    const auto information = image_information_layout(body_min, body_max, scale, image_info_open_);
    ImGui::BeginChild("canvas", {std::max(1.0f, information.canvas_max.x - body_min.x), canvas_height}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto available = ImGui::GetContentRegionAvail();
    const auto position  = ImGui::GetCursorScreenPos();
    const ImVec2 end{position.x + available.x, position.y + available.y};
    const auto buttons = navigation_button_layout(position, end, scale);
    const auto state   = navigation_button_state(
        viewer.navigation(), ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    const bool blocked =
        navigation_gesture_.blocks(buttons, state, ImGui::GetIO().MousePos, ImGui::IsMouseClicked(ImGuiMouseButton_Left),
                                   ImGui::IsMouseDown(ImGuiMouseButton_Left));
    const auto finish = [&]
    {
        const auto selected = draw_navigation_buttons(language, viewer.navigation(), position, end, scale);
        // Navigation restores the cursor after the reason text. Submit its
        // trailing spacing explicitly rather than extending child bounds via SetCursorPos.
        ImGui::Dummy({0, 0});
        ImGui::EndChild();
        if (information.visible &&
            draw_image_information(language, viewer.failed() ? std::nullopt : viewer.image_details(), information.panel_min,
                                   information.panel_max))
            image_info_open_ = false;
        return selected;
    };
    if (viewer.failed())
    {
        draw_image_failure(language, image_error_text(viewer.error()), position, end, scale);
        return finish();
    }
    if (viewer.has_visible_image())
    {
        viewer.draw({position.x, position.y}, {position.x + available.x, position.y + available.y},
                    ImGui::GetIO().DisplayFramebufferScale.x, !blocked);
        return finish();
    }
    if (viewer.loading())
    {
        centered_text(translate(language, Text::loading), ImGui::GetWindowWidth(), true);
        return finish();
    }
    const float image_size = 82.0f * scale;
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), (available.y - 200.0f * scale) * 0.5f));
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - image_size) * 0.5f);
    aligned_image(renderer, icon, {image_size, image_size});
    ImGui::Dummy({0.0f, 10.0f * scale});
    ImGui::PushFont(nullptr, 27.0f);
    centered_text("LiltView", ImGui::GetWindowWidth());
    ImGui::PopFont();
    centered_text(t(Text::tagline), ImGui::GetWindowWidth(), true);
    ImGui::Dummy({0.0f, 15.0f * scale});
    centered_text(t(Text::empty_hint), ImGui::GetWindowWidth(), true);
    return finish();
}

void Shell::draw_status(Language language, const Viewer &viewer, const WindowChromeMetrics &chrome)
{
    Text status = Text::ready;
    if (viewer.loading())
        status = viewer.native_loading() ? Text::loading_native : Text::loading;
    else if (viewer.detail_loading())
        status = Text::loading_detail;
    if (viewer.failed())
        status = image_error_text(viewer.error());
    const auto content = status_bar_content(language, status, viewer.image_details(), viewer.filename(), viewer.navigation());
    const auto origin         = ImGui::GetWindowPos();
    const auto size           = ImGui::GetWindowSize();
    const auto &style         = ImGui::GetStyle();
    const float top           = origin.y + chrome.status_top();
    const float left          = origin.x + chrome.border + style.WindowPadding.x;
    const float right         = origin.x + size.x - chrome.border - style.WindowPadding.x;
    auto *list                = ImGui::GetWindowDrawList();
    list->AddRectFilled({origin.x + chrome.border, top},
                        {origin.x + size.x - chrome.border, origin.y + size.y - chrome.border},
                        ImGui::GetColorU32(current_ui_palette().chrome), std::max(0.0f, chrome.radius - chrome.border),
                        ImDrawFlags_RoundCornersBottom);
    // Thin top rule and three independently clipped groups; no language label.
    list->AddLine({origin.x + chrome.border, top}, {origin.x + size.x - chrome.border, top},
                  ImGui::GetColorU32(current_ui_palette().separator));
    draw_status_bar_content(content, {left, top}, {right, origin.y + size.y - chrome.border}, style.FontScaleDpi);
}

void Shell::draw_about(Language language, render::Renderer &renderer, render::Image icon, float frame_radius)
{
    // OpenPopup 和 BeginPopupModal 使用同一 ID，并处于相同的父窗口作用域。
    const auto id = [language](Text text, const char *stable_id)
    {
        return label(language, text, stable_id);
    };
    const float scale    = ImGui::GetStyle().FontScaleDpi;
    const auto *viewport = ImGui::GetMainViewport();
    if (open_about_)
    {
        ImGui::OpenPopup("about_dialog");
        open_about_ = false;
    }
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(viewport->Size.x - 36.0f * scale, 440.0f * scale), 0.0f}, ImGuiCond_Appearing);
    auto dim = ImGui::GetStyleColorVec4(ImGuiCol_ModalWindowDimBg);
    if (frame_radius > 0)
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4{0, 0, 0, 0});
    const bool visible =
        ImGui::BeginPopupModal("about_dialog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    if (frame_radius > 0)
        ImGui::PopStyleColor();
    if (visible)
    {
        if (frame_radius > 0)
        {
            about_dim_ratio_  = std::min(1.0f, about_dim_ratio_ + ImGui::GetIO().DeltaTime * 6);
            dim.w            *= about_dim_ratio_;
            rounded_modal_dim(frame_radius, dim);
        }
        draw_about_header(language, renderer, icon);
        draw_about_licenses(language);
        draw_about_author(language);
        if (ImGui::Button(id(Text::close, "close_about").c_str()) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    else
        about_dim_ratio_ = 0;
}

void Shell::draw_about_header(Language language, render::Renderer &renderer, render::Image icon)
{
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    const float scale = ImGui::GetStyle().FontScaleDpi;
    aligned_image(renderer, icon, {56.0f * scale, 56.0f * scale});
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted("LiltView");
    ImGui::TextDisabled("%s", LILTVIEW_VERSION);
    ImGui::EndGroup();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", t(Text::about_description));
}

void Shell::draw_about_licenses(Language language)
{
    ImGui::Spacing();
    ImGui::TextWrapped("%s", translate(language, Text::license));
    ImGui::TextDisabled("Copyright 2026 LiltView contributors");
    ImGui::TextWrapped(
        "libjpeg-turbo / libpng / zlib / libwebp / PlutoSVG / PlutoVG / giflib / LibTIFF\nThis software is based in part on the work of the Independent JPEG Group.");
    ImGui::Spacing();
    ImGui::TextWrapped("HarmonyOS Sans SC Regular\nCopyright 2021 Huawei Device Co., Ltd.");
    ImGui::TextWrapped("HarmonyOS Sans Fonts License Agreement");
    ImGui::Spacing();
}

void Shell::draw_about_author(Language language)
{
    ImGui::Spacing();
    ImGui::TextDisabled("%s", translate(language, Text::author));
    ImGui::SameLine();
    ImGui::TextUnformatted(LILTVIEW_AUTHOR);
    ImGui::TextLinkOpenURL(LILTVIEW_AUTHOR_EMAIL, "mailto:" LILTVIEW_AUTHOR_EMAIL);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", translate(language, Text::project_homepage));
    ImGui::TextLinkOpenURL("github.com/hui-livio/LiltView", LILTVIEW_PROJECT_URL);
}
} // namespace liltview
