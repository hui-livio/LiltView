#include "platform/path.hpp"
#include "ui/shell.hpp"
#include "ui/status_bar.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

using namespace liltview;

namespace
{
ImageDetails image()
{
    ImageDetails details;
    details.path              = std::filesystem::path(u8"/private/pictures/图像-100%.wrong");
    details.image.format      = LV_FORMAT_PNG;
    details.image.flags       = LV_INFO_SOURCE_SIZE_KNOWN;
    details.image.source_size = {1100, 654};
    details.image.canvas      = {550, 327};
    details.image.orientation = LV_ORIENTATION_ROTATE_90_CW;
    details.scale             = 0.856;
    return details;
}

struct Gui
{
    Gui()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {1280, 760};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.Fonts->SetTexID(1);
    }
    ~Gui()
    {
        ImGui::DestroyContext();
    }
};
} // namespace

TEST_CASE("status bar empty and initial loading states have no fabricated image parameters", "[status_bar]")
{
    for (const auto language : {Language::english, Language::chinese})
    {
        CAPTURE(language);
        const auto empty = status_bar_content(language, Text::ready, std::nullopt, "", {});
        CHECK(empty.status == translate(language, Text::ready));
        CHECK(empty.identity == translate(language, Text::no_image));
        CHECK(empty.metadata.empty());
        CHECK_FALSE(empty.failed);
        CHECK_FALSE(empty.busy);
        for (const auto status : {Text::loading, Text::loading_native, Text::loading_detail})
        {
            const auto loading = status_bar_content(language, status, std::nullopt, "/private/new.png", {});
            CHECK(loading.busy);
            CHECK_FALSE(loading.failed);
            CHECK(loading.identity == "new.png");
            CHECK(loading.metadata.empty());
            CHECK(loading.status_hint.find(translate(language, status)) != std::string::npos);
            CHECK(loading.status_hint.find("/private") == std::string::npos);
        }
    }
}

TEST_CASE("status bar shows basename actual source format raw dimensions and physical pixel zoom", "[status_bar]")
{
    auto details = image();
    DirectoryNavigationSnapshot navigation;
    navigation.selected_path = details.path;
    navigation.current       = 22;
    navigation.total         = 57;
    for (const auto language : {Language::english, Language::chinese})
    {
        auto content = status_bar_content(language, Text::ready, details, "other.png", navigation);
        CHECK(content.identity == "图像-100%.wrong  ·  22 / 57");
        CHECK(content.metadata == "PNG  ·  1100 × 654  ·  85.6%");
        CHECK(content.identity.find("private") == std::string::npos);
        CHECK(content.metadata.find("550") == std::string::npos);
        CHECK(content.metadata.find("654 × 1100") == std::string::npos);
    }
    const std::array formats{std::pair{LV_FORMAT_JPEG, "JPEG"}, std::pair{LV_FORMAT_PNG, "PNG"}, std::pair{LV_FORMAT_SVG, "SVG"},
                             std::pair{LV_FORMAT_WEBP, "WebP"}, std::pair{LV_FORMAT_BMP, "BMP"}, std::pair{LV_FORMAT_GIF, "GIF"},
                             std::pair{LV_FORMAT_TIFF, "TIFF"}};
    for (const auto &[format, expected] : formats)
    {
        details.image.format = format;
        details.scale        = 1;
        const auto content   = status_bar_content(Language::english, Text::ready, details, "", navigation);
        CHECK(content.metadata.starts_with(std::string(expected) + "  ·  "));
        CHECK(content.metadata.ends_with("100%"));
    }
}

TEST_CASE("status bar failures show the requested basename without stale image parameters or full paths", "[status_bar][error]")
{
    const auto details = image();
    DirectoryNavigationSnapshot navigation;
    navigation.selected_path = "/secret/failed.webp";
    navigation.current       = 23;
    navigation.total         = 57;
    for (const auto language : {Language::english, Language::chinese})
        for (const auto error : {Text::image_io_error, Text::image_unsupported, Text::image_corrupt, Text::image_limit,
                                 Text::image_memory, Text::image_changed, Text::image_error})
        {
            CAPTURE(language, error);
            const auto failed = status_bar_content(language, error, std::nullopt, "/secret/failed.webp", navigation);
            CHECK(failed.failed);
            CHECK_FALSE(failed.busy);
            CHECK(failed.status == translate(language, Text::status_failed));
            CHECK(failed.identity == "failed.webp  ·  23 / 57");
            CHECK(failed.metadata.empty());
            const auto retained = status_bar_content(language, error, details, "/secret/failed.webp", navigation);
            CHECK(retained.metadata.empty());
            CHECK(retained.identity == "failed.webp  ·  23 / 57");
            CHECK(retained.status_hint == std::string("failed.webp\n") + translate(language, error));
            CHECK(retained.status_hint.find("secret") == std::string::npos);
            CHECK(retained.identity.find("private") == std::string::npos);
        }
    const auto loading = status_bar_content(Language::english, Text::loading, details, "failed.webp", navigation);
    CHECK(loading.identity.ends_with("Previous image"));
    CHECK(loading.metadata == "PNG  ·  1100 × 654  ·  85.6%");
}

TEST_CASE("status bar marks unknown values explicitly and keeps directory feedback", "[status_bar]")
{
    auto details         = image();
    details.image.format = static_cast<lv_format>(0);
    details.image.flags  = 0;
    DirectoryNavigationSnapshot navigation;
    navigation.scanning = true;
    for (const double zoom : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::max()})
    {
        details.scale      = zoom;
        const auto unknown = status_bar_content(Language::english, Text::ready, details, "", navigation);
        CHECK(unknown.metadata == "Unknown  ·  Unknown  ·  Unknown");
        CHECK(unknown.identity.find(translate(Language::english, Text::scanning_directory)) != std::string::npos);
    }
    navigation.scanning = false;
    navigation.error    = std::make_error_code(std::errc::permission_denied);
    CHECK(status_bar_content(Language::english, Text::ready, details, "", navigation)
              .identity.find(translate(Language::english, Text::directory_error)) != std::string::npos);
}

TEST_CASE("status bar groups never overlap across narrow widths UI scales themes and languages", "[status_bar][ui][dpi]")
{
    Gui gui;
    for (const bool dark : {false, true})
        for (const auto language : {Language::english, Language::chinese})
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
                for (const float width : {0.0f, 60.0f, 240.0f, 600.0f, 1240.0f})
                    for (const bool populated : {false, true})
                    {
                        CAPTURE(dark, language, scale, width, populated);
                        apply_style(dark, scale);
                        ImGui::NewFrame();
                        ImGui::SetNextWindowPos({0, 0});
                        ImGui::SetNextWindowSize({1280, 760});
                        ImGui::Begin("status_test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
                        auto details       = image();
                        details.path       = std::u8string(300, u8'x') + u8"-中文-%s.png";
                        const auto content = status_bar_content(language, Text::loading,
                                                                populated ? std::optional{details} : std::nullopt, "", {});
                        const auto layout  = status_bar_layout(18, 18 + width, ImGui::CalcTextSize(content.status.c_str()).x,
                                                               ImGui::CalcTextSize(content.metadata.c_str()).x, scale);
                        for (const auto span : {layout.status, layout.identity, layout.metadata})
                        {
                            CHECK(span.min >= 18);
                            CHECK(span.max <= 18 + width);
                            CHECK(span.width() >= 0);
                        }
                        CHECK(layout.status.max <= layout.identity.min);
                        CHECK(layout.identity.max <= layout.metadata.min);
                        if (!populated)
                            CHECK(layout.metadata.width() == 0);
                        if (width >= 600 && scale == 1 && populated)
                            CHECK(layout.metadata.width() == ImGui::CalcTextSize(content.metadata.c_str()).x);
                        const auto active = ImGui::GetActiveID();
                        draw_status_bar_content(content, {18, 700}, {18 + width, 700 + 29 * scale}, scale);
                        CHECK(ImGui::GetActiveID() == active); // Labels cannot consume native drag/keyboard input.
                        ImGui::End();
                        ImGui::Render();
                        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
                    }
}
