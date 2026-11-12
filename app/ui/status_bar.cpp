#include "status_bar.hpp"
#include "image_information.hpp"
#include "platform/path.hpp"
#include "theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace liltview
{
StatusBarContent status_bar_content(Language language, Text status, const std::optional<ImageDetails> &visible,
                                    std::string_view requested_filename, const DirectoryNavigationSnapshot &navigation)
{
    StatusBarContent result;
    result.busy          = status == Text::loading || status == Text::loading_native || status == Text::loading_detail;
    result.failed        = status != Text::ready && !result.busy;
    result.status_hint   = translate(language, status);
    const auto slash     = requested_filename.find_last_of("/\\");
    const auto requested = std::string(requested_filename.substr(slash == std::string_view::npos ? 0 : slash + 1));
    if (!requested.empty() && (result.failed || result.busy))
        result.status_hint = requested + "\n" + result.status_hint;
    result.status = translate(language,
                              result.failed                      ? Text::status_failed :
                                  status == Text::loading_native ? Text::status_loading_native :
                                  status == Text::loading_detail ? Text::status_loading_detail :
                                                                   status);
    // Also strip directories from caller-supplied names defensively. A tooltip
    // must never reintroduce the full path omitted from the status bar.
    result.identity = visible && !result.failed ? platform::path_bytes(visible->path.filename()) : requested;
    if (result.identity.empty())
        result.identity = translate(language, Text::no_image);
    const bool selected_visible = result.failed || !visible || visible->path == navigation.selected_path;
    if (visible && !selected_visible && result.busy)
        result.identity += std::string("  ·  ") + translate(language, Text::status_previous_image);
    if (selected_visible && navigation.total && navigation.current)
        result.identity += "  ·  " + std::to_string(navigation.current) + " / " + std::to_string(navigation.total);
    if (navigation.scanning)
        result.identity += std::string("  ·  ") + translate(language, Text::scanning_directory);
    else if (navigation.error)
        result.identity += std::string("  ·  ") + translate(language, Text::directory_error);
    if (!visible || result.failed)
        return result;
    const auto unknown  = translate(language, Text::info_unknown);
    const auto *format  = image_format_name(visible->image.format);
    const auto &size    = visible->image.source_size;
    const bool known    = (visible->image.flags & LV_INFO_SOURCE_SIZE_KNOWN) && size.width && size.height;
    result.metadata     = format ? format : unknown;
    result.metadata    += "  ·  " + (known ? std::to_string(size.width) + " × " + std::to_string(size.height) : unknown);
    std::ostringstream zoom;
    zoom.imbue(std::locale::classic());
    if (std::isfinite(visible->scale * 100) && visible->scale > 0)
        zoom << std::fixed << std::setprecision(1) << visible->scale * 100 << '%';
    else
        zoom << unknown;
    auto ratio = zoom.str();
    if (ratio.ends_with(".0%"))
        ratio.erase(ratio.size() - 3, 2);
    result.metadata += "  ·  " + ratio;
    return result;
}

StatusBarLayout status_bar_layout(float left, float right, float status_width, float metadata_width, float scale)
{
    right             = std::max(left, right);
    const float width = right - left, gap = 16 * std::max(1.0f, scale);
    const float state = std::clamp(status_width, 0.0f, width * (metadata_width > 0 ? 0.32f : 0.5f));
    StatusBarLayout result{{left, left + state}, {right, right}, {right, right}};
    if (metadata_width > 0)
    {
        const float minimum   = std::min(right, result.status.max + gap);
        const float available = right - minimum;
        result.metadata.min   = std::max(minimum, right - std::clamp(metadata_width, 0.0f, available));
    }
    const float begin = std::min(right, result.status.max + gap);
    const float end   = metadata_width > 0 ? std::max(begin, result.metadata.min - gap) : right;
    result.identity   = {begin, end};
    return result;
}

void draw_status_bar_content(const StatusBarContent &content, ImVec2 min, ImVec2 max, float scale)
{
    if (max.x <= min.x || max.y <= min.y)
        return;
    const auto &p          = current_ui_palette();
    const auto status_size = ImGui::CalcTextSize(content.status.c_str());
    const auto layout = status_bar_layout(min.x, max.x, status_size.x, ImGui::CalcTextSize(content.metadata.c_str()).x, scale);
    auto *draw        = ImGui::GetWindowDrawList();
    const float y     = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5f;
    const auto text   = [&](StatusBarSpan span, const std::string &value, ImVec4 color, const std::string &hint)
    {
        if (span.width() <= 0 || value.empty())
            return;
        draw->PushClipRect({span.min, min.y}, {span.max, max.y}, true);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::RenderTextEllipsis(draw, {span.min, y}, {span.max, max.y}, span.max, value.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        draw->PopClipRect();
        if (ImGui::IsMouseHoveringRect({span.min, min.y}, {span.max, max.y}) &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
            ImGui::SetTooltip("%s", hint.c_str());
    };
    text(layout.status, content.status, content.failed ? p.error : content.busy ? p.accent : p.text, content.status_hint);
    text(layout.identity, content.identity, p.secondary, content.identity);
    text(layout.metadata, content.metadata, p.text, content.metadata);
    // Hairline separators reinforce the three groups without making them widgets.
    const auto separator = [&](float x)
    {
        draw->AddLine({x, min.y + 8 * scale}, {x, max.y - 8 * scale}, ImGui::GetColorU32(p.separator));
    };
    if (layout.identity.width() > 0)
        separator((layout.status.max + layout.identity.min) * 0.5f);
    if (layout.metadata.width() > 0)
        separator(layout.metadata.min - 8 * scale);
}
} // namespace liltview
