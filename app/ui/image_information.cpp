#include "image_information.hpp"
#include "platform/path.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

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
    default:
        return nullptr;
    }
}
} // namespace

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

ImageInformationLayout image_information_layout(ImVec2 min, ImVec2 max, float scale, bool open)
{
    ImageInformationLayout layout;
    layout.canvas_max = max;
    if (!open || !std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(max.x) || !std::isfinite(max.y) ||
        !std::isfinite(scale) || scale <= 0)
        return layout;
    const float width = max.x - min.x, height = max.y - min.y;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
        return layout;
    const float panel_width = std::min(320 * scale, width * 0.45f);
    layout.panel_min        = {max.x - panel_width, min.y};
    layout.panel_max        = max;
    layout.canvas_max.x     = layout.panel_min.x;
    layout.visible          = layout.panel_min.x > min.x && layout.panel_min.x < max.x;
    return layout;
}

bool draw_image_information(Language language, const std::optional<ImageDetails> &details, ImVec2 min, ImVec2 max)
{
    if (max.x <= min.x || max.y <= min.y)
        return false;
    ImGui::SetCursorScreenPos(min);
    ImGui::BeginChild("image_information", {max.x - min.x, max.y - min.y}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoSavedSettings);
    const auto close  = std::string(translate(language, Text::close)) + "###close_image_information";
    const bool closed = ImGui::SmallButton(close.c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(translate(language, Text::image_info));
    ImGui::Separator();
    ImGui::PushTextWrapPos(0);
    if (!details)
        ImGui::TextUnformatted(translate(language, Text::no_image));
    else
        for (const auto &row : image_information_rows(language, *details))
        {
            ImGui::TextDisabled("%s", translate(language, row.label));
            ImGui::TextUnformatted(row.value.c_str());
            ImGui::Spacing();
        }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    return closed;
}
} // namespace liltview
