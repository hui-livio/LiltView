// Reviewed information-panel UT scenarios, recorded before implementation:
// 1. No image disables I/menu; toggling never repeats and both menu renderers share checked state.
// 2. File size KNOWN+zero is zero, missing KNOWN is unknown; unsupported metadata is never invented.
// 3. Original dimensions, EXIF-oriented dimensions, output canvas and physical-pixel scale stay distinct.
// 4. Unknown output color is explicit; RGBA8 straight-alpha is the actual pixel output, not source precision.
// 5. GIF unknown/known frame count, duration and total plays are separate; zero total plays means infinite.
// 6. Long UTF-8 paths containing percent signs remain literal; formatting performs no filesystem I/O.
// 7. A canvas overlay leaves the main image canvas, FIT and navigation geometry unchanged.
// 8. Real ImGui frames cover empty/details content, a translated fixed header and scrollable content and nonmodal behavior.
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
        io.IniFilename = io.LogFilename  = nullptr;
        io.ConfigFlags                  |= ImGuiConfigFlags_NavEnableKeyboard;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int atlas_width = 0, atlas_height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
        io.Fonts->SetTexID(1);
    }
    ~Context()
    {
        ImGui::DestroyContext();
    }
    void frame(const std::optional<ImageDetails> &details, Language language)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({800, 500});
        ImGui::Begin("information_test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        closed = draw_image_information(language, details, {{300, 20}, {300 + width, 460}, true});
        text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        panel = body = nullptr;
        for (auto *window : ImGui::GetCurrentContext()->Windows)
        {
            const auto name = std::string_view(window->Name);
            if (!window->Active)
                continue;
            if (name.find("/image_information_body_") != std::string_view::npos)
                body = window;
            else if (name.find("/image_information_") != std::string_view::npos)
                panel = window;
        }
        REQUIRE(panel);
        REQUIRE(body);
        CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
    ImGuiWindow *panel = nullptr, *body = nullptr;
    bool closed = false;
    std::string text;
    float width = 340;
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

TEST_CASE("information content renders long literal paths without a duplicate title bar", "[image_details][ui]")
{
    auto details = example();
    details.path = std::filesystem::path(std::u8string(u8"/a/") + std::u8string(500, u8'x') + u8"-中文-%s-%d-100%.png");
    for (const auto language : {Language::english, Language::chinese})
    {
        Context context;
        context.frame(details, language);
        context.frame(details, language); // Reserve the scrollbar after wrapping the long path.
        CHECK(field(language, details, Text::info_path) == platform::path_bytes(details.path));
        CHECK(context.text.find("100%.png") != std::string::npos);
        CHECK(context.text.find(translate(language, Text::image_info)) != std::string::npos);
        CHECK(context.text.find(translate(language, Text::info_path)) < context.text.find("/a/"));
        CHECK(context.body->ScrollMax.y > 0);
        CHECK_FALSE(context.panel->ScrollbarY);
    }
}

TEST_CASE("information has one fixed header and complete scrollable content in both themes and at high DPI",
          "[image_details][ui][theme]")
{
    for (const bool dark : {false, true})
        for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
            for (const auto language : {Language::english, Language::chinese})
                for (const float width : {260.0f, 340.0f})
                {
                    CAPTURE(dark, scale, language, width);
                    Context context;
                    context.width = width;
                    apply_style(dark, scale);
                    context.frame(std::nullopt, language);
                    CHECK(context.text.find(translate(language, Text::no_image)) != std::string::npos);
                    CHECK(context.text.find(translate(language, Text::image_info)) != std::string::npos);
                    CHECK_FALSE(context.panel->ScrollbarY);
                    CHECK(context.body->Pos.y > context.panel->Pos.y);
                    context.frame(example(), language);
                    CHECK(context.text.find(translate(language, Text::info_path)) != std::string::npos);
                    CHECK(context.text.find(translate(language, Text::image_info)) != std::string::npos);
                    CHECK(context.panel->DC.NavLayersActiveMaskNext != 0); // Keyboard-accessible close action.
                    CHECK(context.text.find(translate(language, Text::info_image_parameters)) != std::string::npos);
                    CHECK(context.text.find(translate(language, Text::info_output_group)) != std::string::npos);
                    CHECK_FALSE(context.panel->Collapsed);
                    CHECK_FALSE(context.panel->ScrollbarY);
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

TEST_CASE("information overlay stays within the canvas and rejects invalid geometry", "[image_details][ui][geometry]")
{
    for (const auto scale : {1.f, 1.25f, 2.f})
    {
        const ImVec2 start{10 * scale, 20 * scale}, end{1010 * scale, 820 * scale};
        const auto layout = image_information_layout(start, end, scale);
        REQUIRE(layout.valid);
        CHECK(layout.max.x == end.x - 64 * scale);
        CHECK(layout.min.y == start.y + 16 * scale);
        CHECK(layout.max.x - layout.min.x == 360 * scale);
        CHECK(layout.max.y - layout.min.y == 562 * scale);
        CHECK(layout.contains(layout.min));
        CHECK_FALSE(layout.contains(layout.max));
        const auto small = image_information_layout(start, {300 * scale, 300 * scale}, scale);
        REQUIRE(small.valid);
        CHECK(small.min.x >= start.x);
        CHECK(small.max.y <= 300 * scale);
    }
    CHECK_FALSE(image_information_layout({0, 0}, {100, 80}, 1).valid);
    CHECK_FALSE(image_information_layout({0, 0}, {800, 600}, 0).valid);
    CHECK_FALSE(image_information_layout({0, 0}, {800, 600}, std::numeric_limits<float>::quiet_NaN()).valid);
}

TEST_CASE("information close button dismisses without scrolling or changing the title", "[image_details][ui]")
{
    Context context;
    auto details = example();
    details.path = std::filesystem::path(std::u8string(900, u8'x'));
    context.frame(details, Language::english);
    context.frame(details, Language::english);
    REQUIRE(context.body->ScrollMax.y > 0);
    const auto header_y = context.panel->Pos.y;
    auto &io            = ImGui::GetIO();
    io.AddMousePosEvent(context.body->Pos.x + 50, context.body->Pos.y + 60);
    context.frame(details, Language::english);
    io.AddMouseWheelEvent(0, -4);
    context.frame(details, Language::english);
    context.frame(details, Language::english);
    CHECK(context.body->Scroll.y > 0);
    CHECK(context.panel->Pos.y == header_y);
    CHECK(context.panel->Scroll.y == 0);
    io.AddMousePosEvent(context.panel->Pos.x + context.width - 22, header_y + 22);
    context.frame(details, Language::english);
    io.AddMouseButtonEvent(0, true);
    context.frame(details, Language::english);
    CHECK_FALSE(context.closed);
    io.AddMouseButtonEvent(0, false);
    context.frame(details, Language::english);
    CHECK(context.closed);
}


TEST_CASE("information copy action uses the complete literal UTF-8 path", "[image_details][ui][clipboard]")
{
    Context context;
    const auto details = example();
    context.frame(details, Language::english);
    context.frame(details, Language::english);
    ImGui::SetClipboardText("unchanged");
    // Activate the same keyboard-navigable copy item without relying on translated geometry.
    const auto copy_id = context.body->GetID("###copy_image_path");
    ImGui::ActivateItemByID(copy_id);
    context.frame(details, Language::english);
    CHECK(std::string(ImGui::GetClipboardText()) == platform::path_bytes(details.path));
    CHECK_FALSE(context.closed);
}

TEST_CASE("information copy tooltip disappears after a mouse click leaves the button", "[image_details][ui][tooltip][regression]")
{
    for (const auto language : {Language::english, Language::chinese})
    {
        Context context;
        const auto details = example();
        auto &io = ImGui::GetIO();
        std::string clipboard = "unchanged";
        auto &platform_io = ImGui::GetPlatformIO();
        platform_io.Platform_ClipboardUserData = &clipboard;
        platform_io.Platform_SetClipboardTextFn = [](ImGuiContext *ctx, const char *text)
        {
            *static_cast<std::string *>(ctx->PlatformIO.Platform_ClipboardUserData) = text;
        };
        const auto frame = [&] { context.frame(details, language); };
        const auto tooltip_visible = []
        {
            for (const auto *window : ImGui::GetCurrentContext()->Windows)
                if (window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_Tooltip))
                    return true;
            return false;
        };
        frame();
        frame();
        const auto copy_id = context.body->GetID("###copy_image_path");
        bool found = false;
        for (float y = context.body->InnerRect.Min.y + 14; y < context.body->InnerRect.Max.y; y += 7)
        {
            io.AddMousePosEvent(context.body->WorkRect.Max.x - 14, y);
            frame();
            frame();
            if (ImGui::GetCurrentContext()->HoveredId == copy_id)
            {
                found = true;
                break;
            }
        }
        REQUIRE(found);
        for (int i = 0; i < 45; ++i)
            frame();
        REQUIRE(tooltip_visible());

        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frame();
        REQUIRE(clipboard == platform::path_bytes(details.path));
        REQUIRE(ImGui::GetCurrentContext()->NavId == copy_id);
        io.AddMousePosEvent(context.panel->Pos.x + 45, context.panel->Pos.y + 22);
        for (int i = 0; i < 4; ++i)
            frame();
        CHECK_FALSE(tooltip_visible());
        CHECK(ImGui::GetCurrentContext()->NavId == copy_id); // Mouse focus may remain, but cannot pin a tooltip.
        CHECK_FALSE(context.closed);
        // Keyboard focus still receives a tooltip through ImGui's navigation-aware hover handling.
        for (int tab = 0; tab < 12 && !(ImGui::GetCurrentContext()->NavId == copy_id &&
                                       ImGui::GetCurrentContext()->NavCursorVisible); ++tab)
        {
            io.AddKeyEvent(ImGuiKey_Tab, true);
            frame();
            io.AddKeyEvent(ImGuiKey_Tab, false);
            frame();
            frame();
        }
        REQUIRE(ImGui::GetCurrentContext()->NavId == copy_id);
        REQUIRE(ImGui::GetCurrentContext()->NavCursorVisible);
        for (int i = 0; i < 45; ++i)
            frame();
        CHECK(tooltip_visible());
        io.AddMousePosEvent(10, 10);
        for (int i = 0; i < 4; ++i)
            frame();
        CHECK_FALSE(tooltip_visible());
    }
}
