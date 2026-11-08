#include "options.hpp"
#include "version.hpp"

#include <CLI/CLI.hpp>

namespace liltview
{
CommandLineResult parse_command_line(int argc, const char *const *argv, std::ostream &out, std::ostream &error)
{
    CommandLineResult result;
    auto &options = result.options;
    CLI::App app{"LiltView " LILTVIEW_VERSION " - a lightweight image viewer", "liltview"};
    app.set_version_flag("--version", "LiltView " LILTVIEW_VERSION);
    app.footer("Open JPEG, PNG, SVG, static WebP, BMP, animated GIF, TIFF (first page). Large images are pending.");

    std::string language;
    auto *language_option =
        app.add_option("--language", language, "Override the saved language")->check(CLI::IsMember({"system", "en-US", "zh-CN"}));
    app.add_option("--config-dir", options.config_directory, "Use a separate settings.yaml directory")->type_name("PATH");
    auto *smoke = app.add_flag("--smoke-test", options.smoke_test, "Exercise the window and exit")->disable_flag_override();
    app.add_option("--screenshot", options.screenshot, "Save a PPM during --smoke-test")->type_name("PATH")->needs(smoke);
    app.add_option("image", options.image_path, "Image to open (JPEG, PNG, SVG, WebP, BMP, GIF, TIFF)")->type_name("PATH");

    try
    {
        app.parse(argc, argv);
        if (*language_option)
            options.language = parse_language(language);
    } catch (const CLI::ParseError &failure)
    {
        result.exit_code = app.exit(failure, out, error);
    }
    return result;
}
} // namespace liltview
