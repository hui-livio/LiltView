#ifndef LILTVIEW_IMAGE_INFORMATION_HPP
#define LILTVIEW_IMAGE_INFORMATION_HPP

#include "i18n/translator.hpp"
#include "viewer/image_details.hpp"

#include <imgui.h>
#include <string>
#include <vector>

namespace liltview
{
struct ImageInformationRow
{
    Text label;
    std::string value;
};

// Known source container format, never inferred from a file extension.
const char *image_format_name(lv_format);

// Presentation only: no file access, Core calls, rendering or metadata parsing.
std::vector<ImageInformationRow> image_information_rows(Language, const ImageDetails &);

// A canvas overlay: geometry is resolved before any image input, never a viewport inset.
struct ImageInformationLayout
{
    ImVec2 min{}, max{};
    bool valid = false;
    bool contains(ImVec2 point) const noexcept;
};
ImageInformationLayout image_information_layout(ImVec2 canvas_min, ImVec2 canvas_max, float scale);
// The header stays fixed while the full information body scrolls. Returns true on close.
bool draw_image_information(Language, const std::optional<ImageDetails> &, const ImageInformationLayout &,
                            bool accept_input = true);
} // namespace liltview

#endif
