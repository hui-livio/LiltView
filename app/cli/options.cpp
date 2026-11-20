#include "options.hpp"
#include "version.hpp"
#include "notices.hpp"

#include <CLI/CLI.hpp>

#include <charconv>
#include <cstdint>
#include <array>
#include <sstream>

namespace liltview
{
namespace
{
bool parse_size(std::string_view text)
{
    const auto x = text.find('x');
    if (x == std::string_view::npos || x == 0 || x + 1 == text.size()) return false;
    std::uint32_t w = 0, h = 0;
    const auto a = text.substr(0, x), b = text.substr(x + 1);
    const auto aw = std::from_chars(a.data(), a.data() + a.size(), w);
    const auto ah = std::from_chars(b.data(), b.data() + b.size(), h);
    return aw.ec == std::errc{} && aw.ptr == a.data() + a.size() && ah.ec == std::errc{} &&
        ah.ptr == b.data() + b.size() && w && h;
}
bool valid_plane(std::string_view text)
{
    std::stringstream stream{std::string(text)};
    std::string item;
    std::array<std::uint64_t, 4> values{};
    for (auto &value : values)
    {
        if (!std::getline(stream, item, ',' ) || item.empty()) return false;
        const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size()) return false;
    }
    return !std::getline(stream, item, ',') && values[1] >= values[2] && values[3] != 0;
}
}

CommandLineResult parse_command_line(int argc, const char *const *argv, std::ostream &out, std::ostream &error)
{
    CommandLineResult result;
    auto &options = result.options;
    CLI::App app{"LiltView " LILTVIEW_VERSION " - a lightweight image viewer", "liltview"};
    // Keep slash-prefixed paths as positional arguments on Windows too.
    app.allow_windows_style_options(false);
    app.set_version_flag("--version", "LiltView " LILTVIEW_VERSION);
    app.footer(
        "Open JPEG, PNG, SVG, static WebP, BMP, animated GIF, TIFF, static AVIF, static JPEG XL, static HEIF/HEIC, RAW/DNG previews, and OpenEXR. Large static JPEG/PNG/BMP/TIFF support regional detail.");

    bool licenses = false;
    app.add_flag("--licenses", licenses, "Print embedded licenses and third-party notices");
    std::string language;
    auto *language_option =
        app.add_option("--language", language, "Override the saved language")->check(CLI::IsMember({"system", "en-US", "zh-CN"}));
    app.add_option("--config-dir", options.config_directory, "Use a separate settings.yaml directory")->type_name("PATH");
    auto *smoke = app.add_flag("--smoke-test", options.smoke_test, "Exercise the window and exit")->disable_flag_override();
    app.add_option("--screenshot", options.screenshot, "Save a PPM during --smoke-test")->type_name("PATH")->needs(smoke);
    app.add_option("image", options.image_path, "Image to open (JPEG, PNG, SVG, WebP, BMP, GIF, TIFF, AVIF, JPEG XL, HEIF/HEIC, RAW/DNG, OpenEXR)")->type_name("PATH");
    RawInputOptions raw;
    auto *raw_layout = app.add_option("--raw-layout", raw.layout, "Explicit raw layout: rgb24,bgr24,rgba8,bgra8,gray8,i420,yv12,nv12,nv21,bayer8,bayer16")
        ->check(CLI::IsMember({"rgb24", "bgr24", "rgba8", "bgra8", "gray8", "i420", "yv12", "nv12", "nv21", "bayer8", "bayer16"}));
    auto *raw_size = app.add_option("--raw-size", raw.size, "Raw dimensions WIDTHxHEIGHT");
    auto *raw_plane = app.add_option("--raw-plane", raw.planes, "Raw plane OFFSET,STRIDE,ROW_BYTES,HEIGHT")->expected(1, 4);
    app.add_option("--raw-row-order", raw.row_order, "Raw row order")->check(CLI::IsMember({"top-down", "bottom-up"}));
    app.add_option("--raw-byte-order", raw.byte_order, "Raw byte order")->check(CLI::IsMember({"native", "little", "big"}));
    app.add_option("--raw-alpha", raw.alpha, "Raw alpha mode")->check(CLI::IsMember({"none", "opaque", "straight"}));
    app.add_option("--raw-matrix", raw.matrix, "YUV matrix");
    app.add_option("--raw-range", raw.range, "YUV range");
    app.add_option("--raw-siting", raw.siting, "YUV chroma siting");
    app.add_option("--raw-cfa", raw.cfa, "Bayer CFA");
    app.add_option("--raw-black-level", raw.black_level, "Bayer black level");
    app.add_option("--raw-white-level", raw.white_level, "Bayer white level");
    app.add_option("--raw-red-gain-q16", raw.red_gain_q16, "Bayer red gain Q16");
    app.add_option("--raw-green-red-gain-q16", raw.green_red_gain_q16, "Bayer green-on-red gain Q16");
    app.add_option("--raw-green-blue-gain-q16", raw.green_blue_gain_q16, "Bayer green-on-blue gain Q16");
    app.add_option("--raw-blue-gain-q16", raw.blue_gain_q16, "Bayer blue gain Q16");

    try
    {
        app.parse(argc, argv);
        if (licenses)
        {
            out << liltview_notices;
            result.exit_code = 0;
            return result;
        }
        if (*language_option)
            options.language = parse_language(language);
        if (*raw_layout || *raw_size || *raw_plane || raw.row_order != "top-down" || raw.byte_order != "native" ||
            raw.alpha != "opaque" || !raw.matrix.empty() || !raw.range.empty() || !raw.siting.empty() || !raw.cfa.empty() ||
            raw.black_level || raw.white_level || raw.red_gain_q16 != 65536 || raw.green_red_gain_q16 != 65536 ||
            raw.green_blue_gain_q16 != 65536 || raw.blue_gain_q16 != 65536)
        {
            if (!*raw_layout || !*raw_size || raw.planes.empty() || !parse_size(raw.size))
                throw CLI::ValidationError("--raw-layout, --raw-size and at least one --raw-plane are required");
            for (const auto &plane : raw.planes)
                if (!valid_plane(plane)) throw CLI::ValidationError("--raw-plane must be OFFSET,STRIDE,ROW_BYTES,HEIGHT");
            options.raw = raw;
        }
    } catch (const CLI::ParseError &failure)
    {
        result.exit_code = app.exit(failure, out, error);
    }
    return result;
}
} // namespace liltview
