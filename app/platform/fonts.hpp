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
};

SystemFont find_application_font();
} // namespace liltview

#endif
