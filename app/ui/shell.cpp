#include "shell.hpp"

#include "version.hpp"
#include "viewer/viewer.hpp"

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

void begin_main_window(bool system_menu)
{
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("LiltView###main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | (system_menu ? 0 : ImGuiWindowFlags_MenuBar));
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
    if (dark)
    {
        style.Colors[ImGuiCol_WindowBg]     = {0.075f, 0.085f, 0.10f, 1.0f};
        style.Colors[ImGuiCol_ChildBg]      = {0.055f, 0.063f, 0.075f, 1.0f};
        style.Colors[ImGuiCol_MenuBarBg]    = {0.10f, 0.11f, 0.13f, 1.0f};
        style.Colors[ImGuiCol_PopupBg]      = {0.095f, 0.105f, 0.12f, 1.0f};
        style.Colors[ImGuiCol_Button]       = {0.15f, 0.17f, 0.20f, 1.0f};
        style.Colors[ImGuiCol_Text]         = {0.91f, 0.93f, 0.95f, 1.0f};
        style.Colors[ImGuiCol_TextDisabled] = {0.54f, 0.58f, 0.64f, 1.0f};
    }
    else
    {
        style.Colors[ImGuiCol_ChildBg] = {0.95f, 0.96f, 0.97f, 1.0f};
        style.Colors[ImGuiCol_PopupBg] = {0.98f, 0.98f, 0.98f, 1.0f};
    }
    style.ScaleAllSizes(scale);
    style.FontSizeBase = 17.0f;
    style.FontScaleDpi = scale;
}

MenuAction Shell::draw(Language language, const MenuItem &menu, bool system_menu, render::Renderer &renderer, render::Image icon,
                       Viewer &viewer)
{
    // 按区域组织一帧；窗口、子画布和弹窗的 Begin/End 在对应函数中配对。
    MenuAction action = MenuAction::none;
    begin_main_window(system_menu);
    if (!system_menu)
        action = draw_window_menu(menu);

    draw_canvas(language, renderer, icon, viewer);
    draw_status(language, viewer);
    draw_about(language, renderer, icon);
    ImGui::End();
    return action;
}

MenuAction Shell::draw_window_menu(const MenuItem &menu)
{
    if (!ImGui::BeginMenuBar())
        return MenuAction::none;
    const auto selected = draw_menu(menu);
    ImGui::EndMenuBar();
    return selected;
}

void Shell::draw_canvas(Language language, render::Renderer &renderer, render::Image icon, Viewer &viewer)
{
    // 为状态栏预留空间；空状态内容以后可替换为图片视图。
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    const float status_height = ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("canvas", {0.0f, -status_height}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const auto available = ImGui::GetContentRegionAvail();
    if (viewer.has_visible_image())
    {
        const auto position = ImGui::GetCursorScreenPos();
        viewer.draw({position.x, position.y}, {position.x + available.x, position.y + available.y},
                    ImGui::GetIO().DisplayFramebufferScale.x);
        ImGui::EndChild();
        return;
    }
    if (viewer.loading())
    {
        centered_text(translate(language, Text::loading), ImGui::GetWindowWidth(), true);
        ImGui::EndChild();
        return;
    }
    if (viewer.failed())
    {
        const auto message = image_error_text(viewer.error());
        ImGui::PushTextWrapPos();
        ImGui::TextWrapped("%s", translate(language, message));
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        return;
    }
    const float scale      = ImGui::GetStyle().FontScaleDpi;
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
    ImGui::EndChild();
}

void Shell::draw_status(Language language, const Viewer &viewer)
{
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    const float scale = ImGui::GetStyle().FontScaleDpi;
    Text status       = Text::ready;
    if (viewer.loading())
        status = viewer.native_loading() ? Text::loading_native : Text::loading;
    else if (viewer.detail_loading())
        status = Text::loading_detail;
    if (viewer.failed())
        status = image_error_text(viewer.error());
    if (viewer.filename().empty())
        ImGui::TextDisabled("%s  /  %s", t(status), t(Text::no_image));
    else
        ImGui::TextDisabled("%s  |  %s  %u x %u", t(status), viewer.filename().c_str(), viewer.source_size().width,
                            viewer.source_size().height);
    const char *language_name = language == Language::chinese ? "简体中文" : "English";
    ImGui::SameLine(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - ImGui::CalcTextSize(language_name).x - 22.0f * scale));
    ImGui::TextDisabled("%s", language_name);
}

void Shell::draw_about(Language language, render::Renderer &renderer, render::Image icon)
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
    if (ImGui::BeginPopupModal("about_dialog", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
    {
        draw_about_header(language, renderer, icon);
        draw_about_licenses(language);
        draw_about_author(language);
        if (ImGui::Button(id(Text::close, "close_about").c_str()) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
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
