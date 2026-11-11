// Reviewed UT scenarios before implementation:
// 1. First/middle/last/single image snapshots enable only existing neighbors.
// 2. Empty selection hides both buttons; scanning/error/popup states disable both.
// 3. Layout uses symmetric 40x56 buttons at 12px canvas margins at all DPI scales.
// 4. Tiny/offset canvases retain non-overlapping in-bounds hit rectangles; invalid geometry draws nothing.
// 5. Hit testing includes disabled buttons to prevent dragging/zooming the image beneath them.
// 6. Actual ImGui press/release emits previous/next once, with stable IDs across language changes.
// 7. Disabled, hidden, scanning and errored buttons never emit actions.
// 8. A popup opened before or between press/release blocks navigation.
// 9. Dragging away before release cancels activation; rendering preserves the caller's cursor.
// 10. Enabled/disabled button presses capture image interaction until the release frame, even after hiding or popups.

#include "ui/navigation_buttons.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <utility>

using namespace liltview;

namespace
{
DirectoryNavigation::Snapshot snapshot(std::size_t current, std::size_t total)
{
    DirectoryNavigation::Snapshot value;
    if (!total)
        return value;
    value.selected_path = "/images/" + std::to_string(current) + ".png";
    value.current       = current;
    value.total         = total;
    value.can_previous  = current > 1;
    value.can_next      = current < total;
    if (value.can_previous)
        value.previous_path = "/images/" + std::to_string(current - 1) + ".png";
    if (value.can_next)
        value.next_path = "/images/" + std::to_string(current + 1) + ".png";
    return value;
}

ImVec2 center(const NavigationButtonRect &rect)
{
    return {(rect.min.x + rect.max.x) / 2, (rect.min.y + rect.max.y) / 2};
}

struct Context
{
    Context()
    {
        ImGui::CreateContext();
        auto &io       = ImGui::GetIO();
        io.DisplaySize = {960, 640};
        io.DeltaTime   = 1.0f / 60;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
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

    MenuAction frame(const DirectoryNavigation::Snapshot &value, Language language = Language::english, bool popup = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({960, 640});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("navigation_test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        if (popup)
            ImGui::OpenPopup("navigation_blocker");
        ImGui::SetCursorScreenPos({40, 50});
        const auto before = ImGui::GetCursorScreenPos();
        const auto action = draw_navigation_buttons(language, value, min, max, 1);
        const auto after  = ImGui::GetCursorScreenPos();
        CHECK(after.x == before.x);
        CHECK(after.y == before.y);
        // Hidden buttons submit no items; register the fixture's manually positioned cursor independently.
        ImGui::Dummy({0, 0});
        if (ImGui::BeginPopupModal("navigation_blocker", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Popup blocks navigation");
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::Render();
        return action;
    }

    static constexpr ImVec2 min{80, 100}, max{880, 540};
};
} // namespace

TEST_CASE("canvas navigation buttons follow neighbor availability and loading state", "[navigation_buttons]")
{
    for (const auto &[current, total] : {std::pair{1u, 3u}, {2u, 3u}, {3u, 3u}, {1u, 1u}})
    {
        auto value       = snapshot(current, total);
        const auto state = navigation_button_state(value);
        CHECK(state.visible);
        CHECK(state.previous_enabled == (current > 1));
        CHECK(state.next_enabled == (current < total));
        value.scanning = true;
        auto blocked   = navigation_button_state(value);
        CHECK(blocked.visible);
        CHECK_FALSE(blocked.previous_enabled);
        CHECK_FALSE(blocked.next_enabled);
        value.scanning = false;
        value.error    = std::make_error_code(std::errc::permission_denied);
        blocked        = navigation_button_state(value);
        CHECK(blocked.visible);
        CHECK_FALSE(blocked.previous_enabled);
        CHECK_FALSE(blocked.next_enabled);
    }
    const auto empty = navigation_button_state({});
    CHECK_FALSE(empty.visible);
    CHECK_FALSE(empty.previous_enabled);
    CHECK_FALSE(empty.next_enabled);
    const auto blocked = navigation_button_state(snapshot(2, 3), true);
    CHECK(blocked.visible);
    CHECK_FALSE(blocked.previous_enabled);
    CHECK_FALSE(blocked.next_enabled);
}

TEST_CASE("canvas navigation layout is symmetric and scales logical button dimensions", "[navigation_buttons]")
{
    const ImVec2 min{20, 30}, max{1020, 730};
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f})
    {
        const auto layout = navigation_button_layout(min, max, scale);
        REQUIRE(layout.valid);
        CHECK(layout.previous.min.x - min.x == Catch::Approx(12 * scale));
        CHECK(max.x - layout.next.max.x == Catch::Approx(12 * scale));
        CHECK(layout.previous.max.x - layout.previous.min.x == Catch::Approx(40 * scale));
        CHECK(layout.next.max.x - layout.next.min.x == Catch::Approx(40 * scale));
        CHECK(layout.previous.max.y - layout.previous.min.y == Catch::Approx(56 * scale));
        CHECK(layout.next.min.y == layout.previous.min.y);
        CHECK(layout.next.max.y == layout.previous.max.y);
        CHECK(center(layout.previous).y == Catch::Approx((min.y + max.y) / 2));
        CHECK(center(layout.next).y == Catch::Approx((min.y + max.y) / 2));
    }
}

TEST_CASE("tiny canvas navigation bounds remain separated and never extend outside the canvas", "[navigation_buttons]")
{
    for (const ImVec2 size : {ImVec2{100, 40}, {40, 20}, {8, 4}, {1, 1}})
        for (const float scale : {1.0f, 2.0f})
        {
            const ImVec2 min{83, 112}, max{83 + size.x, 112 + size.y};
            const auto layout = navigation_button_layout(min, max, scale);
            REQUIRE(layout.valid);
            CHECK(layout.previous.min.x >= min.x);
            CHECK(layout.previous.min.y >= min.y);
            CHECK(layout.previous.max.x <= layout.next.min.x);
            CHECK(layout.next.max.x <= max.x);
            CHECK(layout.next.max.y <= max.y);
            CHECK(layout.previous.min.x - min.x == Catch::Approx(max.x - layout.next.max.x));
            CHECK(layout.previous.contains(center(layout.previous)));
            CHECK_FALSE(layout.previous.contains(layout.previous.max));
        }
    for (const auto layout : {navigation_button_layout({0, 0}, {0, 10}, 1), navigation_button_layout({10, 0}, {0, 10}, 1),
                              navigation_button_layout({0, 0}, {10, 10}, 0),
                              navigation_button_layout({0, 0}, {10, 10}, std::numeric_limits<float>::quiet_NaN()),
                              navigation_button_layout({0, 0}, {std::numeric_limits<float>::infinity(), 10}, 1)})
        CHECK_FALSE(layout.valid);
}

TEST_CASE("disabled navigation buttons still prevent image interaction inside their bounds", "[navigation_buttons]")
{
    const auto layout = navigation_button_layout(Context::min, Context::max, 1);
    const auto single = navigation_button_state(snapshot(1, 1));
    CHECK(navigation_buttons_hit(layout, single, center(layout.previous)));
    CHECK(navigation_buttons_hit(layout, single, center(layout.next)));
    CHECK_FALSE(navigation_buttons_hit(layout, single, {480, 320}));
    CHECK_FALSE(navigation_buttons_hit(layout, navigation_button_state({}), center(layout.previous)));
    CHECK_FALSE(navigation_buttons_hit({}, single, center(layout.previous)));
}

TEST_CASE("button gestures exclude image pan through dragging outside and releasing", "[navigation_buttons]")
{
    const auto layout = navigation_button_layout(Context::min, Context::max, 1);
    for (const bool disabled : {false, true})
        for (const bool previous : {false, true})
        {
            auto state = navigation_button_state(snapshot(disabled ? 1 : 2, disabled ? 1 : 3));
            NavigationButtonGesture gesture;
            const auto point = center(previous ? layout.previous : layout.next);
            const ImVec2 outside{480, 320};
            CHECK_FALSE(gesture.blocks(layout, state, outside, false, false));
            CHECK(gesture.blocks(layout, state, point, true, true));
            CHECK(gesture.held);
            CHECK(gesture.blocks(layout, state, outside, false, true));
            state = {}; // Opening a popup or replacing the scene must not release a captured drag.
            CHECK(gesture.blocks(layout, state, outside, false, true));
            CHECK(gesture.blocks(layout, state, outside, false, false));
            CHECK_FALSE(gesture.held);
            CHECK_FALSE(gesture.blocks(layout, state, outside, false, false));
        }
    NavigationButtonGesture elsewhere;
    const auto state = navigation_button_state(snapshot(2, 3));
    CHECK_FALSE(elsewhere.blocks(layout, state, {480, 320}, true, true));
    CHECK(elsewhere.blocks(layout, state, center(layout.next), false, true));
    CHECK_FALSE(elsewhere.held); // Moving a drag started elsewhere across a button does not capture it.
    CHECK_FALSE(elsewhere.blocks(layout, state, {480, 320}, false, true));
}

TEST_CASE("canvas buttons emit previous and next on release with stable translated IDs", "[navigation_buttons][ui]")
{
    for (const bool previous : {true, false})
    {
        Context context;
        const auto value  = snapshot(2, 3);
        const auto layout = navigation_button_layout(Context::min, Context::max, 1);
        const auto point  = center(previous ? layout.previous : layout.next);
        CHECK(context.frame(value) == MenuAction::none);
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, true);
        CHECK(context.frame(value, Language::english) == MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        CHECK(context.frame(value, Language::chinese) == (previous ? MenuAction::previous : MenuAction::next));
        CHECK(context.frame(value) == MenuAction::none);
    }
}

TEST_CASE("unavailable canvas navigation buttons never activate", "[navigation_buttons][ui]")
{
    for (unsigned mode = 0; mode < 5; ++mode)
        for (const bool previous : {true, false})
        {
            CAPTURE(mode, previous);
            Context context;
            auto value = snapshot(2, 3);
            if (mode == 0)
                value = {};
            if (mode == 1)
                value = snapshot(1, 1);
            if (mode == 2)
                value.scanning = true;
            if (mode == 3)
                value.error = std::make_error_code(std::errc::io_error);
            if (mode == 4)
                value = previous ? snapshot(1, 3) : snapshot(3, 3);
            const auto layout = navigation_button_layout(Context::min, Context::max, 1);
            const auto point  = center(previous ? layout.previous : layout.next);
            CHECK(context.frame(value) == MenuAction::none);
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(point.x, point.y);
            io.AddMouseButtonEvent(0, true);
            CHECK(context.frame(value) == MenuAction::none);
            io.AddMouseButtonEvent(0, false);
            CHECK(context.frame(value) == MenuAction::none);
        }
}

TEST_CASE("popups block canvas navigation before and during a button click", "[navigation_buttons][ui]")
{
    for (const bool between_press_and_release : {false, true})
    {
        Context context;
        const auto value = snapshot(2, 3);
        const auto point = center(navigation_button_layout(Context::min, Context::max, 1).next);
        context.frame(value);
        if (!between_press_and_release)
            context.frame(value, Language::english, true);
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        io.AddMouseButtonEvent(0, true);
        CHECK(context.frame(value) == MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        CHECK(context.frame(value, Language::english, between_press_and_release) == MenuAction::none);
    }
}

TEST_CASE("dragging away cancels canvas navigation activation", "[navigation_buttons][ui]")
{
    Context context;
    const auto value = snapshot(2, 3);
    const auto point = center(navigation_button_layout(Context::min, Context::max, 1).next);
    context.frame(value);
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(point.x, point.y);
    io.AddMouseButtonEvent(0, true);
    CHECK(context.frame(value) == MenuAction::none);
    io.AddMousePosEvent(480, 320);
    io.AddMouseButtonEvent(0, false);
    CHECK(context.frame(value) == MenuAction::none);
}
