#include "image_information.hpp"
#include "navigation_buttons.hpp"
#include "platform/path.hpp"
#include "theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace liltview
{
namespace
{
std::string dimensions(lv_size_u32 size)
{
    return std::to_string(size.width) + " × " + std::to_string(size.height);
}

std::string decimal(double value, int precision)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(precision) << value;
    return text.str();
}

const char *format_name(lv_format format)
{
    switch (format)
    {
    case LV_FORMAT_JPEG:
        return "JPEG";
    case LV_FORMAT_PNG:
        return "PNG";
    case LV_FORMAT_SVG:
        return "SVG";
    case LV_FORMAT_WEBP:
        return "WebP";
    case LV_FORMAT_BMP:
        return "BMP";
    case LV_FORMAT_GIF:
        return "GIF";
    case LV_FORMAT_TIFF:
        return "TIFF";
    case LV_FORMAT_QOI:
        return "QOI";
    case LV_FORMAT_ICO:
        return "ICO";
    case LV_FORMAT_CUR:
        return "CUR";
    case LV_FORMAT_JPEG2000:
        return "JPEG 2000";
    case LV_FORMAT_AVIF:
        return "AVIF";
    case LV_FORMAT_JPEGXL:
        return "JPEG XL";
    case LV_FORMAT_HEIF:
        return "HEIF / HEIC";
    case LV_FORMAT_OPENEXR:
        return "OpenEXR";
    default:
        return nullptr;
    }
}

} // namespace

const char *image_format_name(lv_format format)
{
    return format_name(format);
}

std::vector<ImageInformationRow> image_information_rows(Language language, const ImageDetails &details)
{
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    const auto unknown    = std::string(t(Text::info_unknown));
    const auto &image     = details.image;
    const bool size_known = (image.flags & LV_INFO_SOURCE_SIZE_KNOWN) != 0;
    const auto *format    = format_name(image.format);
    std::vector<ImageInformationRow> rows{
        {Text::info_path, platform::path_bytes(details.path)},
        {Text::info_file_size,
         image.flags & LV_INFO_FILE_BYTES_KNOWN ? std::to_string(image.file_bytes) + " " + t(Text::info_bytes) : unknown},
        {Text::info_format, format ? format : unknown},
        {Text::info_source_size, size_known ? dimensions(image.source_size) : unknown},
        {Text::info_display_size, size_known ? dimensions(oriented_image_size(image.source_size, image.orientation)) : unknown},
        {Text::info_canvas_size, image.canvas.width && image.canvas.height ? dimensions(image.canvas) : unknown},
        {Text::info_zoom, std::isfinite(details.scale) && details.scale > 0 ? decimal(details.scale * 100, 1) + "%" : unknown},
        {Text::info_orientation, image.orientation >= 1 && image.orientation <= 8 ? std::to_string(image.orientation) : unknown},
        {Text::info_output,
         details.pixel_format == LV_PIXEL_RGBA8 ? std::string("RGBA8 · ") + t(Text::info_straight_alpha) : unknown},
        {Text::info_color, details.color_space == LV_COLOR_SRGB ? "sRGB" : t(Text::info_color_unknown)}};
    if (details.partial)
        rows.push_back({Text::info_state, t(Text::info_partial)});
    else if (details.loading_replacement)
        rows.push_back({Text::info_state, t(Text::info_previous_loading)});
    if (details.animation)
    {
        const auto &animation = *details.animation;
        const auto &info      = animation.info;
        const auto frame =
            animation.frame.key.frame_index == UINT64_MAX ? unknown : std::to_string(animation.frame.key.frame_index + 1);
        rows.push_back(
            {Text::info_animation_frame,
             frame + " / " + ((info.flags & LV_ANIMATION_FRAME_COUNT_KNOWN) ? std::to_string(info.frame_count) : unknown)});
        rows.push_back({Text::info_frame_duration,
                        animation.frame.flags & LV_ANIMATION_FRAME_DURATION_KNOWN ?
                            decimal(static_cast<double>(animation.frame.source_duration_ns) / 1000000, 3) + " ms" :
                            unknown});
        rows.push_back({Text::info_animation_duration,
                        info.flags & LV_ANIMATION_DURATION_KNOWN ?
                            decimal(static_cast<double>(info.duration_ns) / 1000000, 3) + " ms" :
                            unknown});
        rows.push_back({Text::info_animation_plays,
                        !(info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN) ? unknown :
                            info.total_plays == 0                     ? std::string(t(Text::info_infinite)) :
                                                                        std::to_string(info.total_plays)});
    }
    return rows;
}

bool ImageInformationLayout::contains(ImVec2 point) const noexcept
{
    return valid && point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
}

ImageInformationLayout image_information_layout(ImVec2 canvas_min, ImVec2 canvas_max, float scale)
{
    ImageInformationLayout layout;
    if (!std::isfinite(scale) || scale <= 0 || !std::isfinite(canvas_min.x) || !std::isfinite(canvas_min.y) ||
        !std::isfinite(canvas_max.x) || !std::isfinite(canvas_max.y))
        return layout;
    const float margin = 16 * scale;
    // Keep the edge navigation lane clear, including a 12 DIP gap from its hit box.
    // Info remains an overlay; this reservation never changes the image viewport.
    const auto navigation = navigation_button_layout(canvas_min, canvas_max, scale);
    const float right =
        navigation.valid ? std::min(canvas_max.x - margin, navigation.next.min.x - 12 * scale) : canvas_max.x - margin;
    const float width  = std::min(360 * scale, right - canvas_min.x - margin);
    const float height = std::min(562 * scale, canvas_max.y - canvas_min.y - 2 * margin);
    if (width < 160 * scale || height < 90 * scale)
        return layout;
    layout.max   = {right, canvas_min.y + margin + height};
    layout.min   = {layout.max.x - width, canvas_min.y + margin};
    layout.valid = true;
    return layout;
}

namespace
{
std::string readable_file_size(const ImageDetails &details, Language language)
{
    if (!(details.image.flags & LV_INFO_FILE_BYTES_KNOWN))
        return translate(language, Text::info_unknown);
    const auto bytes = details.image.file_bytes;
    if (bytes < 1024)
        return std::to_string(bytes) + " " + translate(language, Text::info_bytes);
    const char *units[]{"KB", "MB", "GB", "TB", "PB", "EB"};
    double value  = static_cast<double>(bytes) / 1024;
    unsigned unit = 0;
    while (value >= 1024 && unit + 1 < std::size(units))
    {
        value /= 1024;
        ++unit;
    }
    return decimal(value, 2) + " " + units[unit];
}

// Text and icons stay opaque; only the reading surface is translucent.
bool information_button(const char *id, ImVec2 position, float scale, bool copy)
{
    ImGui::SetCursorScreenPos(position);
    const bool pressed = ImGui::InvisibleButton(id, {28 * scale, 28 * scale}, ImGuiButtonFlags_EnableNav);
    const auto &p      = current_ui_palette();
    auto *draw         = ImGui::GetWindowDrawList();
    // IsItemHovered also handles visible keyboard navigation; mouse focus alone must not pin feedback.
    if (ImGui::IsItemHovered())
        draw->AddRectFilled(position, {position.x + 28 * scale, position.y + 28 * scale},
                            ImGui::GetColorU32(ImGui::IsItemActive() ? p.pressed : p.hover), 5 * scale);
    const auto color = ImGui::GetColorU32(p.information_secondary);
    const auto point = [=](float x, float y)
    {
        return ImVec2{position.x + x * scale, position.y + y * scale};
    };
    if (copy)
    {
        draw->AddRect(point(8, 10), point(17, 21), color, scale, 0, scale);
        draw->AddPolyline(std::array<ImVec2, 3>{point(12, 7), point(20, 7), point(20, 17)}.data(), 3, color, 0, scale);
    }
    else
    {
        draw->AddLine(point(10, 10), point(18, 18), color, 1.4f * scale);
        draw->AddLine(point(18, 10), point(10, 18), color, 1.4f * scale);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    {
        const auto label = std::string_view(id).substr(0, std::string_view(id).find("##"));
        ImGui::SetTooltip("%.*s", static_cast<int>(label.size()), label.data());
    }
    return pressed;
}
} // namespace

bool draw_image_information(Language language, const std::optional<ImageDetails> &details, const ImageInformationLayout &layout,
                            bool accept_input)
{
    if (!layout.valid)
        return false;
    const auto t = [language](Text text)
    {
        return translate(language, text);
    };
    const auto &p     = current_ui_palette();
    const float scale = ImGui::GetStyle().FontScaleDpi;
    const float width = layout.max.x - layout.min.x, height = layout.max.y - layout.min.y;
    const float border = 1.2f * scale, header = 44 * scale;
    const auto cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(layout.min);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.information_surface);
    ImGui::PushStyleColor(ImGuiCol_Border, p.information_border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
    const auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoMove | (accept_input ? 0 : ImGuiWindowFlags_NoInputs);
    const bool visible = ImGui::BeginChild("image_information", {width, height}, ImGuiChildFlags_Borders, flags);
    bool close         = false;
    if (visible)
    {
        ImGui::PushFont(nullptr, 17);
        const ImVec2 center{layout.min.x + 22 * scale, layout.min.y + 22 * scale};
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec4 icon_color{p.information_border.x, p.information_border.y, p.information_border.z, 1};
        draw->AddCircle(center, 6.5f * scale, ImGui::GetColorU32(icon_color), 0, scale);
        const auto info_size = ImGui::CalcTextSize("i");
        draw->AddText({center.x - info_size.x / 2, center.y - info_size.y / 2}, ImGui::GetColorU32(p.text), "i");
        ImGui::SetCursorScreenPos({layout.min.x + 40 * scale, center.y - ImGui::GetTextLineHeight() / 2});
        ImGui::PushClipRect({layout.min.x + 40 * scale, layout.min.y}, {layout.max.x - 75 * scale, layout.min.y + header}, true);
        ImGui::TextUnformatted(t(Text::image_info));
        ImGui::PopClipRect();
        ImGui::PopFont();
        ImGui::PushFont(nullptr, 12);
        const ImVec2 key_min{layout.max.x - 64 * scale, layout.min.y + 13 * scale};
        draw->AddRect(key_min, {key_min.x + 18 * scale, key_min.y + 18 * scale}, ImGui::GetColorU32(p.separator), 3 * scale);
        const auto key_size = ImGui::CalcTextSize("I");
        draw->AddText({key_min.x + (18 * scale - key_size.x) / 2, key_min.y + (18 * scale - key_size.y) / 2},
                      ImGui::GetColorU32(p.information_secondary), "I");
        ImGui::PopFont();
        const auto close_label = std::string(t(Text::close)) + "###close_image_information";
        close = information_button(close_label.c_str(), {layout.max.x - 36 * scale, layout.min.y + 8 * scale}, scale, false);

        ImGui::SetCursorScreenPos({layout.min.x + border, layout.min.y + header});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, {0, 0, 0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * scale, 10 * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 8 * scale});
        ImGui::PushFont(nullptr, 16);
        if (ImGui::IsWindowAppearing())
            ImGui::SetNextWindowScroll({0, 0});
        if (ImGui::BeginChild(
                "image_information_body", {width - 2 * border, height - header - border}, ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | (accept_input ? 0 : ImGuiWindowFlags_NoInputs)))
        {
            ImGui::PushTextWrapPos(0);
            if (!details)
                ImGui::TextUnformatted(t(Text::no_image));
            else
            {
                const auto rows  = image_information_rows(language, *details);
                const auto value = [&](Text key) -> const char *
                {
                    for (const auto &row : rows)
                        if (row.label == key)
                            return row.value.c_str();
                    return t(Text::info_unknown);
                };
                const auto gap = [scale](float amount)
                {
                    ImGui::Dummy({0, amount * scale});
                };
                const auto group = [&](Text key)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, p.information_secondary);
                    ImGui::TextUnformatted(t(key));
                    ImGui::PopStyleColor();
                };
                const auto row = [&](Text key)
                {
                    const auto pos          = ImGui::GetCursorScreenPos();
                    const float available   = ImGui::GetContentRegionAvail().x;
                    const float label_width = available * .43f;
                    const float value_width = std::max(1.0f, available - label_width - 8 * scale);
                    const auto label_size   = ImGui::CalcTextSize(t(key), nullptr, false, label_width);
                    const auto value_size   = ImGui::CalcTextSize(value(key), nullptr, false, value_width);
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + label_width);
                    ImGui::TextColored(p.information_secondary, "%s", t(key));
                    ImGui::PopTextWrapPos();
                    ImGui::SetCursorScreenPos({pos.x + available - value_size.x, pos.y});
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + value_width);
                    ImGui::TextUnformatted(value(key));
                    ImGui::PopTextWrapPos();
                    ImGui::SetCursorScreenPos({pos.x, pos.y + std::max(label_size.y, value_size.y) + 12 * scale});
                };
                ImGui::PushFont(nullptr, 18);
                const auto filename = platform::path_bytes(details->path.filename());
                ImGui::TextUnformatted(filename.c_str());
                ImGui::PopFont();
                const auto summary = std::string(value(Text::info_format)) + "  ·  " + readable_file_size(*details, language);
                ImGui::TextColored(p.information_secondary, "%s", summary.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", value(Text::info_file_size));
                if (details->partial || details->loading_replacement)
                    ImGui::TextWrapped("%s", value(Text::info_state));
                gap(10);
                ImGui::Separator();
                gap(2);
                row(Text::info_source_size);
                row(Text::info_zoom);
                ImGui::Separator();
                gap(2);
                const auto path_position = ImGui::GetCursorScreenPos();
                group(Text::info_path);
                const auto next_position = ImGui::GetCursorScreenPos();
                const auto copy_label    = std::string(t(Text::info_copy_path)) + "###copy_image_path";
                if (information_button(
                        copy_label.c_str(),
                        {path_position.x + ImGui::GetContentRegionAvail().x - 28 * scale, path_position.y - 7 * scale}, scale,
                        true))
                    ImGui::SetClipboardText(value(Text::info_path));
                ImGui::SetCursorScreenPos(next_position);
                ImGui::TextUnformatted(value(Text::info_path));
                gap(8);
                group(Text::info_image_parameters);
                row(Text::info_display_size);
                row(Text::info_canvas_size);
                row(Text::info_orientation);
                gap(2);
                group(Text::info_output_group);
                row(Text::info_output);
                row(Text::info_color);
                if (details->animation)
                {
                    gap(2);
                    group(Text::info_animation_group);
                    row(Text::info_animation_frame);
                    row(Text::info_frame_duration);
                    row(Text::info_animation_duration);
                    row(Text::info_animation_plays);
                }
                gap(8);
                group(Text::info_available_only);
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
    ImGui::SetCursorScreenPos(cursor);
    return close;
}
} // namespace liltview
