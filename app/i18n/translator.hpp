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
    pan_left,
    pan_right,
    pan_up,
    pan_down,
    zoom_in,
    zoom_out,
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
    loading_detail,
    loading_native,
    image_io_error,
    image_unsupported,
    image_corrupt,
    image_limit,
    image_memory,
    image_changed,
    image_error,
    system_titlebar,
    disable_rounding,
    minimize,
    maximize,
    restore,
    scanning_directory,
    directory_error,
    image_info,
    info_path,
    info_file_size,
    info_format,
    info_source_size,
    info_display_size,
    info_canvas_size,
    info_zoom,
    info_orientation,
    info_output,
    info_color,
    info_animation_frame,
    info_frame_duration,
    info_animation_duration,
    info_animation_plays,
    info_unknown,
    info_infinite,
    info_bytes,
    info_straight_alpha,
    info_color_unknown,
    info_state,
    info_partial,
    info_previous_loading,
    status_failed,
    status_loading_native,
    status_loading_detail,
    status_previous_image,
    image_load_failed,
    unsupported_format,
    count
};

Language resolve_language(Language preference, std::string_view system_locale, bool chinese_font);

const char *translate(Language language, Text text);

std::string_view language_code(Language language);

Language parse_language(std::string_view code);

} // namespace liltview

#endif
