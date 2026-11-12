#ifndef LILTVIEW_STATUS_BAR_HPP
#define LILTVIEW_STATUS_BAR_HPP

#include "i18n/translator.hpp"
#include "viewer/image_details.hpp"
#include "viewer/navigation.hpp"

#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>

namespace liltview
{
struct StatusBarContent
{
    std::string status, status_hint, identity, metadata;
    bool busy = false, failed = false;
};

// Only basenames enter the presentation. Metadata describes the visible image,
// not a pending replacement; no file access or image decoding is performed.
StatusBarContent status_bar_content(Language, Text status, const std::optional<ImageDetails> &visible,
                                    std::string_view requested_filename, const DirectoryNavigationSnapshot &);

struct StatusBarSpan
{
    float min = 0, max = 0;
    float width() const
    {
        return max - min;
    }
};
struct StatusBarLayout
{
    StatusBarSpan status, identity, metadata;
};
// Status and image parameters take priority; the filename uses remaining space.
StatusBarLayout status_bar_layout(float left, float right, float status_width, float metadata_width, float scale);
// Non-interactive text only: preserve native status-bar drag behavior.
void draw_status_bar_content(const StatusBarContent &, ImVec2 min, ImVec2 max, float scale);
} // namespace liltview

#endif
