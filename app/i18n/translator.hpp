#ifndef LILTVIEW_TRANSLATOR_HPP
#define LILTVIEW_TRANSLATOR_HPP

#include <cstddef>
#include <string_view>

namespace liltview
{
enum class Language
{
    system,
    english,
    chinese
};

enum class Text
{
    file,
    open,
    exit,
    view,
    fullscreen,
    leave_fullscreen,
    settings,
    language,
    follow_system,
    english,
    chinese,
    appearance,
    dark,
    light,
    help,
    about,
    close,
    previous,
    next,
    fit,
    actual_size,
    tagline,
    empty_title,
    empty_hint,
    ready,
    no_image,
    unavailable,
    font_missing,
    about_description,
    license,
    author,
    project_homepage,
    loading,
    image_io_error,
    image_unsupported,
    image_corrupt,
    image_limit,
    image_memory,
    image_changed,
    image_error,
    count
};

Language resolve_language(Language preference, std::string_view system_locale, bool chinese_font);

const char *translate(Language language, Text text);

std::string_view language_code(Language language);

Language parse_language(std::string_view code);

} // namespace liltview

#endif
