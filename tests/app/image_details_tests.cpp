// Reviewed information-panel UT scenarios, recorded before implementation:
// 1. No image disables I/menu; toggling never repeats and both menu renderers share checked state.
// 2. File size KNOWN+zero is zero, missing KNOWN is unknown; unsupported metadata is never invented.
// 3. Original dimensions, EXIF-oriented dimensions, output canvas and physical-pixel scale stay distinct.
// 4. Unknown output color is explicit; RGBA8 straight-alpha is the actual pixel output, not source precision.
// 5. GIF unknown/known frame count, duration and total plays are separate; zero total plays means infinite.
// 6. Long UTF-8 paths containing percent signs remain literal; formatting performs no filesystem I/O.
// 7. A bounded right-side panel leaves the image canvas and its navigation buttons separate from the panel.
// 8. Real ImGui frames cover empty/details content, translated stable close IDs and nonmodal behavior.
// 9. Escape closes information before fullscreen; text inputs and other popups retain key priority.
// Viewer tests additionally cover old A -> pending B -> B row bands -> completion, failed B -> A,
// rapid A/B/C replacement, no-source queries, GIF completion metadata, and presented-camera scale.

#include "platform/path.hpp"
#include "ui/image_information.hpp"
#include "ui/shell.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace liltview;

namespace
{
ImageDetails example()
{
    ImageDetails details;
    details.path              = std::filesystem::path(u8"/missing/中文-%s-100%/photo.png");
    details.image.format      = LV_FORMAT_PNG;
    details.image.flags       = LV_INFO_SOURCE_SIZE_KNOWN | LV_INFO_FILE_BYTES_KNOWN;
    details.image.source_size = {123, 45};
    details.image.canvas      = {123, 45};
    details.image.orientation = LV_ORIENTATION_NORMAL;
    details.pixel_format      = LV_PIXEL_RGBA8;
    return details;
}

std::string field(Language language, const ImageDetails &details, Text label)
{
    for (const auto &row : image_information_rows(language, details))
        if (row.label == label)
            return row.value;
    throw std::runtime_error("Missing information field");
}

struct Context
{
    Context()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {800, 500};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
    }
    ~Context()
    {
        ImGui::DestroyContext();
    }
    bool frame(const std::optional<ImageDetails> &details, Language language)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 500});
        ImGui::Begin("information_test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        const bool closed = draw_image_information(language, details, {300, 20}, {640, 460});
        panel = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
            if (window->Active && std::string_view(window->Name).find("/image_information_") != std::string_view::npos)
                panel = window;
        REQUIRE(panel);
        close_id = panel->GetID("###close_image_information");
        const float size = std::min(28.0f * ImGui::GetStyle().FontScaleDpi,
                                    std::max(1.0f, panel->WorkRect.GetWidth()));
        close_rect = {{panel->WorkRect.Max.x - size, panel->DC.CursorStartPos.y},
                      {panel->WorkRect.Max.x, panel->DC.CursorStartPos.y + size}};
        CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
        return closed;
    }
    ImGuiWindow *panel = nullptr;
    ImGuiID close_id = 0;
    ImRect close_rect;
};
} // namespace

TEST_CASE("information file metadata distinguishes zero and unknown without touching the source", "[image_details]")
{
    auto details = example();
    for (const auto language : {Language::english, Language::chinese})
    {
        CHECK(field(language, details, Text::info_path) == platform::path_bytes(details.path));
        CHECK(field(language, details, Text::info_file_size) == std::string("0 ") + translate(language, Text::info_bytes));
        CHECK(field(language, details, Text::info_format) == "PNG");
        details.image.flags &= ~static_cast<lv_info_flags>(LV_INFO_FILE_BYTES_KNOWN);
        CHECK(field(language, details, Text::info_file_size) == translate(language, Text::info_unknown));
        details.image.file_bytes  = UINT64_MAX;
        details.image.flags      |= LV_INFO_FILE_BYTES_KNOWN;
        CHECK(field(language, details, Text::info_file_size).starts_with("18446744073709551615 "));
        details.image.file_bytes = 0;
    }
}

TEST_CASE("information dimensions orientation and physical zoom have independent meanings", "[image_details]")
{
    auto details         = example();
    details.image.canvas = {60, 22};
    for (lv_orientation orientation = 1; orientation <= 8; ++orientation)
    {
        details.image.orientation = orientation;
        CHECK(field(Language::english, details, Text::info_source_size) == "123 × 45");
        CHECK(field(Language::english, details, Text::info_display_size) == (orientation >= 5 ? "45 × 123" : "123 × 45"));
        CHECK(field(Language::english, details, Text::info_canvas_size) == "60 × 22");
        CHECK(field(Language::english, details, Text::info_orientation) == std::to_string(orientation));
    }
    details.scale = 1;
    CHECK(field(Language::english, details, Text::info_zoom) == "100.0%");
    details.scale = 1.25;
    CHECK(field(Language::english, details, Text::info_zoom) == "125.0%");
    details.scale = std::numeric_limits<double>::quiet_NaN();
    CHECK(field(Language::english, details, Text::info_zoom) == "Unknown");
    details.image.flags &= ~static_cast<lv_info_flags>(LV_INFO_SOURCE_SIZE_KNOWN);
    CHECK(field(Language::english, details, Text::info_source_size) == "Unknown");
    CHECK(field(Language::english, details, Text::info_display_size) == "Unknown");
}

TEST_CASE("information output describes actual pixel semantics and partial visibility", "[image_details]")
{
    auto details = example();
    CHECK(field(Language::english, details, Text::info_output) == "RGBA8 · straight alpha");
    CHECK(field(Language::english, details, Text::info_color) == "Unknown (no ICC/HDR conversion)");
    details.pixel_format.reset();
    CHECK(field(Language::english, details, Text::info_output) == "Unknown");
    details.color_space = LV_COLOR_SRGB;
    CHECK(field(Language::english, details, Text::info_color) == "sRGB");
    details.partial = true;
    CHECK(field(Language::english, details, Text::info_state) == "Original image is still loading");
    details.partial             = false;
    details.loading_replacement = true;
    CHECK(field(Language::english, details, Text::info_state) == "Showing the previous image while loading");
}

TEST_CASE("information animation known flags distinguish unknown zero and finite metadata", "[image_details][animation]")
{
    auto details = example();
    details.animation.emplace();
    CHECK(field(Language::english, details, Text::info_animation_frame) == "1 / Unknown");
    CHECK(field(Language::english, details, Text::info_frame_duration) == "Unknown");
    CHECK(field(Language::english, details, Text::info_animation_duration) == "Unknown");
    CHECK(field(Language::english, details, Text::info_animation_plays) == "Unknown");
    auto &animation            = *details.animation;
    animation.info.flags       = LV_ANIMATION_FRAME_COUNT_KNOWN | LV_ANIMATION_DURATION_KNOWN | LV_ANIMATION_PLAY_COUNT_KNOWN;
    animation.info.frame_count = 9;
    animation.info.duration_ns = 123456789;
    animation.frame.key.frame_index = 2;
    animation.frame.flags           = LV_ANIMATION_FRAME_DURATION_KNOWN;
    CHECK(field(Language::english, details, Text::info_animation_frame) == "3 / 9");
    CHECK(field(Language::english, details, Text::info_frame_duration) == "0.000 ms");
    CHECK(field(Language::english, details, Text::info_animation_duration) == "123.457 ms");
    CHECK(field(Language::english, details, Text::info_animation_plays) == "Infinite");
    CHECK(field(Language::chinese, details, Text::info_animation_plays) == "无限");
    animation.info.total_plays = 3;
    CHECK(field(Language::english, details, Text::info_animation_plays) == "3");
}

TEST_CASE("information layout reserves an in-bounds sibling and preserves a positive canvas at every scale",
          "[image_details][layout]")
{
    for (const ImVec2 size : {ImVec2{1000, 700}, {640, 480}, {80, 40}, {1, 1}})
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
        {
            const ImVec2 min{20, 30}, max{min.x + size.x, min.y + size.y};
            const auto layout = image_information_layout(min, max, scale, true);
            REQUIRE(layout.visible);
            CHECK(layout.canvas_max.x > min.x);
            CHECK(layout.canvas_max.x == layout.panel_min.x);
            CHECK(layout.panel_min.x < layout.panel_max.x);
            CHECK(layout.panel_min.y == min.y);
            CHECK(layout.panel_max.x == max.x);
            CHECK(layout.panel_max.y == max.y);
            CHECK(layout.panel_max.x - layout.panel_min.x <= size.x * 0.45f + 0.001f);
        }
    CHECK_FALSE(image_information_layout({0, 0}, {800, 500}, 1, false).visible);
    CHECK_FALSE(image_information_layout({0, 0}, {0, 500}, 1, true).visible);
    CHECK_FALSE(image_information_layout({0, 0}, {800, 500}, 0, true).visible);
}

TEST_CASE("information panel renders literal long paths and closes through a stable translated button", "[image_details][ui]")
{
    Context context;
    CHECK_FALSE(context.frame(std::nullopt, Language::english));
    auto details = example();
    details.path = std::filesystem::path(std::u8string(u8"/a/") + std::u8string(500, u8'x') + u8"-中文-%s-%d-100%.png");
    CHECK_FALSE(context.frame(details, Language::english));
    CHECK_FALSE(context.frame(details, Language::english)); // Let the new content reserve its scrollbar.
    auto &io = ImGui::GetIO();
    const auto center = context.close_rect.GetCenter();
    io.AddMousePosEvent(center.x, center.y);
    io.AddMouseButtonEvent(0, true);
    CHECK_FALSE(context.frame(details, Language::english));
    io.AddMouseButtonEvent(0, false);
    CHECK(context.frame(details, Language::chinese));
    CHECK_FALSE(context.frame(details, Language::chinese));
}

TEST_CASE("information close icon stays right aligned and mouse activation survives language changes", "[image_details][ui][theme]")
{
    for (const bool dark : {false, true})
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
        {
            CAPTURE(dark, scale);
            Context context;
            apply_style(dark, scale);
            // Warm up the child window and reserve its scrollbar width.
            CHECK_FALSE(context.frame(example(), Language::english));
            CHECK_FALSE(context.frame(example(), Language::english));
            const auto rect = context.close_rect;
            CHECK(rect.GetWidth() == 28 * scale);
            CHECK(rect.GetHeight() == 28 * scale);
            CHECK(rect.Min.x > context.panel->Pos.x + context.panel->Size.x / 2);
            CHECK(rect.Max.x <= context.panel->Pos.x + context.panel->Size.x);
            CHECK(rect.Min.y >= context.panel->Pos.y);
            const auto id = context.close_id;
            auto &io = ImGui::GetIO();
            const auto point = rect.GetCenter();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            CHECK_FALSE(context.frame(example(), Language::english));
            CHECK(ImGui::GetCurrentContext()->ActiveId == id);
            io.AddMouseButtonEvent(0, false);
            CHECK(context.frame(example(), Language::chinese));
            CHECK(context.close_id == id);
            CHECK_FALSE(context.frame(example(), Language::chinese));
        }
}

TEST_CASE("information close icon cancels a dragged-away click and ignores title clicks", "[image_details][ui]")
{
    Context context;
    apply_style(false, 1);
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    auto &io = ImGui::GetIO();
    const auto center = context.close_rect.GetCenter();
    io.AddMousePosEvent(center.x, center.y);
    io.AddMouseButtonEvent(0, true);
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    io.AddMousePosEvent(330, center.y);
    io.AddMouseButtonEvent(0, false);
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    io.AddMouseButtonEvent(0, true);
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    io.AddMouseButtonEvent(0, false);
    CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
}

TEST_CASE("information close icon supports keyboard activation without repeated dismissals", "[image_details][ui][shortcut]")
{
    for (const auto key : {ImGuiKey_Enter, ImGuiKey_Space})
    {
        CAPTURE(key);
        Context context;
        apply_style(true, 1);
        CHECK_FALSE(context.frame(std::nullopt, Language::english));
        CHECK_FALSE(context.frame(std::nullopt, Language::english));
        ImGui::FocusWindow(context.panel);
        auto rect = context.close_rect;
        rect.Translate({-context.panel->Pos.x, -context.panel->Pos.y});
        ImGui::SetNavID(context.close_id, ImGuiNavLayer_Main, context.panel->NavRootFocusScopeId, rect);
        ImGui::GetCurrentContext()->NavCursorVisible = true;
        ImGui::GetCurrentContext()->NavHighlightItemUnderNav = true;
        auto &io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        CHECK(context.frame(std::nullopt, Language::english));
        CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
        io.AddKeyEvent(key, false);
        CHECK_FALSE(context.frame(std::nullopt, Language::chinese));
    }
}

TEST_CASE("information shell toggle and dismiss retain a one-action Escape contract", "[image_details][ui]")
{
    Shell shell;
    CHECK_FALSE(shell.image_info_open());
    CHECK_FALSE(shell.dismiss_image_info());
    shell.toggle_image_info();
    CHECK(shell.image_info_open());
    CHECK(shell.dismiss_image_info()); // Caller consumes Escape; does not also leave fullscreen.
    CHECK_FALSE(shell.image_info_open());
    CHECK_FALSE(shell.dismiss_image_info()); // A following Escape can handle fullscreen.
}

TEST_CASE("information paths preserve UTF-8 Chinese and emoji independently of Windows code page", "[image_details][path]")
{
    auto details             = example();
    const std::u8string text = u8"/images/\u56fe\u50cf-\U0001f600-100%.png";
    details.path             = std::filesystem::path(text);
    const std::string expected(text.begin(), text.end());
    for (const auto language : {Language::english, Language::chinese})
    {
        CHECK(field(language, details, Text::info_path) == expected);
        Context context;
        CHECK_NOTHROW(context.frame(details, language));
    }
}
