#ifndef LILTVIEW_FONTS_HPP
#define LILTVIEW_FONTS_HPP

#include <string>

namespace liltview
{
struct SystemFont
{
    std::string path;
    int face_index = 0;
    bool chinese   = false;
    bool bundled   = false;
};

SystemFont find_application_font();
SystemFont find_system_font();
// Reads a bounded font file, verifies bundled resource integrity, then asks
// ImGui to load it with recoverable errors; returns false for fallback.
bool load_font_candidate(const SystemFont &font);
} // namespace liltview

#endif
