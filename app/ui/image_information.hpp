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

// Draw inside a dedicated desktop window, never in the main canvas layout.
// The native window owns its title bar and close action.
void draw_image_information(Language, const std::optional<ImageDetails> &, ImVec2 min, ImVec2 max);
} // namespace liltview

#endif
