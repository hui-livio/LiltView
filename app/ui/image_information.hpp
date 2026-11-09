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

// Presentation only: no file access, Core calls, rendering or metadata parsing.
std::vector<ImageInformationRow> image_information_rows(Language, const ImageDetails &);

struct ImageInformationLayout
{
    ImVec2 canvas_max{}, panel_min{}, panel_max{};
    bool visible = false;
};

ImageInformationLayout image_information_layout(ImVec2 min, ImVec2 max, float scale, bool open);
// Draw as a sibling of the canvas, outside its image and navigation hit region.
// Returns true only when the close button is activated; this is not a popup.
bool draw_image_information(Language, const std::optional<ImageDetails> &, ImVec2 min, ImVec2 max);
} // namespace liltview

#endif
