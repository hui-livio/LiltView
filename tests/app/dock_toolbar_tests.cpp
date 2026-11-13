#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <imgui_internal.h>
#include <limits>
#include "ui/dock_toolbar.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"

using namespace liltview;

TEST_CASE("dock geometry anchors to canvas not image and collapses groups without shrinking targets", "[dock]")
{
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f, 3.0f})
        for (const auto width : {1000, 640, 400, 320, 210, 128, 80})
        {
            const ImVec2 min{27, 61}, max{min.x + width * scale, min.y + 600 * scale};
            const auto layout = dock_layout(min, max, scale, true, false);
            REQUIRE(layout.valid);
            CHECK(layout.bounds.min.x >= min.x);
            CHECK(layout.bounds.max.x <= max.x);
            CHECK((layout.bounds.min.x + layout.bounds.max.x) / 2 == Catch::Approx((min.x + max.x) / 2));
            CHECK(max.y - layout.bounds.max.y == Catch::Approx(16 * scale));
            CHECK(layout.bounds.max.y - layout.bounds.min.y == Catch::Approx(48 * scale));
            CHECK(layout.trigger.max.y - layout.trigger.min.y == Catch::Approx(32 * scale));
            for (const auto &button : layout.buttons)
            {
                CHECK(button.rect.max.x - button.rect.min.x == Catch::Approx(36 * scale));
                CHECK(button.rect.max.y - button.rect.min.y == Catch::Approx(36 * scale));
                CHECK(layout.bounds.contains(button.rect.min));
            }
            const auto has = [&](MenuAction action)
            {
                return std::any_of(layout.buttons.begin(), layout.buttons.end(), [&](auto &b) { return b.action == action; });
            };
            CHECK(has(MenuAction::rotate_left) == (width >= 468));
            CHECK(has(MenuAction::previous) == (width >= 372));
            if (width >= 468)
            {
                CHECK(layout.buttons.size() == 10);
                CHECK(layout.bounds.max.x - layout.bounds.min.x == Catch::Approx(444 * scale));
            }
            CHECK(layout.buttons.back().icon == DockIcon::more);
        }
    CHECK_FALSE(dock_layout({0, 0}, {20, 100}, 1, true, false).valid);
    CHECK_FALSE(dock_layout({0, 0}, {1000, 79}, 1, true, false).valid);
    CHECK_FALSE(dock_layout({0, 0}, {1000, 500}, 0, true, false).valid);
    CHECK_FALSE(dock_layout({0, 0}, {std::numeric_limits<float>::infinity(), 500}, 1, true, false).valid);
}

TEST_CASE("dock dwell fade and reentry use monotonic deadlines", "[dock]")
{
    DockVisibility state;
    const auto update = [&](double now, bool trigger, bool dock = false)
    {
        state.update(now, true, trigger, dock, false, false, false);
    };
    CHECK(state.opacity() == 0);
    CHECK_FALSE(state.needs_frames());
    update(0, true);
    update(.199, true);
    CHECK(state.opacity() == 0);
    CHECK(state.needs_frames());
    update(.1999, false);
    CHECK_FALSE(state.needs_frames());
    update(.200, true);
    update(.399, true);
    CHECK(state.opacity() == 0);
    update(.460, true);
    CHECK(state.opacity() == Catch::Approx(.5));
    update(.520, true);
    CHECK(state.opacity() == 1);
    CHECK_FALSE(state.needs_frames());
    update(.600, false, true); // Move upwards from the trigger into the dock.
    CHECK(state.opacity() == 1);
    update(.700, false);
    CHECK(state.opacity() == 1);
    update(.760, false);
    CHECK(state.opacity() == Catch::Approx(.5));
    update(.760, true); // Reverse without jumping opacity or another dwell.
    CHECK(state.opacity() == Catch::Approx(.5));
    update(.820, true);
    CHECK(state.opacity() == Catch::Approx(.75));
    update(.880, true);
    CHECK(state.opacity() == 1);
    update(.900, false);
    update(1.021, false);
    CHECK(state.opacity() == 0);
    CHECK_FALSE(state.needs_frames());
    // A slow/idle frame must account for elapsed time after the 200ms deadline.
    state.reset();
    update(3, true);
    update(3.32, true);
    CHECK(state.opacity() == Catch::Approx(1));
}

TEST_CASE("dock invalidation cancels timers and drag never reveals while menu and keyboard pin", "[dock]")
{
    for (const bool menu : {false, true})
    {
        DockVisibility state;
        state.update(0, true, true, false, false, false, false);
        state.update(.1, false, true, false, false, false, false);
        state.update(1, true, true, false, false, false, false);
        CHECK(state.opacity() == 0);
        state.update(1.05, true, true, false, true, false, false);
        state.update(2, true, true, true, true, false, false);
        CHECK(state.opacity() == 0);
        CHECK_FALSE(state.needs_frames());
        state.update(3, true, false, false, false, menu, !menu);
        state.update(3.13, true, false, false, false, menu, !menu);
        CHECK(state.opacity() == 1);
        state.update(4, false, false, false, false, menu, !menu);
        CHECK(state.opacity() == 0);
    }
}

TEST_CASE("dock toggle is a view-mode action with shared availability and translated next-action tooltip", "[dock]")
{
    for (const auto language : {Language::english, Language::chinese})
        for (const bool fitting : {true, false})
            for (const bool native : {true, false})
            {
                const auto menu    = build_menu(Settings{}, language, true, false, true, native, true, false, true, true);
                const auto layout  = dock_layout({0, 0}, {960, 640}, 1, fitting, false);
                const auto &button = layout.buttons[3];
                CHECK(button.action == (fitting ? MenuAction::actual_size : MenuAction::fit));
                CHECK(button.icon == (fitting ? DockIcon::actual_size : DockIcon::fit));
                const auto expected = fitting && !native ? MenuAction::none : button.action;
                CHECK(dock_toggle_action(menu, fitting, true) == expected);
                CHECK(dock_toggle_action(menu, fitting, false) == MenuAction::none);
                const auto tooltip = dock_tooltip(menu, button, language);
                CHECK(tooltip.find(fitting ? "Ctrl+1" : "F") != std::string::npos);
                CHECK(tooltip.find(translate(language, fitting ? Text::original_size : Text::fit)) != std::string::npos);
            }
    CHECK(dock_toggle_action({}, true, true) == MenuAction::none);
}

TEST_CASE("canvas gesture owns drags and rejects cross-UI loading and dragged double clicks", "[dock][canvas]")
{
    CanvasGesture gesture;
    CHECK_FALSE(gesture.update(true, true, true, true, false, false));
    CHECK(gesture.image_held());
    CHECK_FALSE(gesture.update(true, true, false, false, false, false));
    CHECK(gesture.update(true, true, true, true, false, true));
    gesture.update(true, true, false, false, false, false);
    gesture.update(true, false, true, true, false, true); // UI owns this press.
    CHECK_FALSE(gesture.image_held());
    gesture.update(true, true, false, false, false, false);
    CHECK_FALSE(gesture.update(true, true, true, true, false, true));
    gesture.update(true, false, false, true, true, false); // Image drag over Dock remains owned.
    CHECK(gesture.image_held());
    gesture.update(false, false, false, true, true, false); // A cold camera restore cannot hand ownership to Dock.
    CHECK(gesture.image_held());
    gesture.update(true, true, false, false, false, false);
    CHECK_FALSE(gesture.update(true, true, true, true, false, true));
    gesture.update(false, true, false, false, false, false);
    CHECK_FALSE(gesture.update(true, true, true, true, false, true));
    gesture.reset();
    gesture.update(false, true, true, true, false, false);
    gesture.update(true, true, false, true, true, false);
    CHECK_FALSE(gesture.image_held());
}

namespace
{
struct DockGui
{
    DockToolbar toolbar;
    DockLayout layout = dock_layout({0, 0}, {960, 600}, 1, true, false);
    MenuItem menu     = build_menu(Settings{}, Language::english, true, false, true, true, true, false);
    DockGui()
    {
        ImGui::CreateContext();
        auto &io        = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.DisplaySize  = {960, 640};
        io.DeltaTime    = .01f;
        io.IniFilename = io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        unsigned char *pixels = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.Fonts->SetTexID(1);
        apply_style(true, 1);
    }
    ~DockGui()
    {
        ImGui::DestroyContext();
    }
    MenuAction frame(bool ready = true, bool held = false, bool blocked = false, bool modal = false, bool camera_busy = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({960, 640});
        ImGui::Begin("dock_test", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        if (modal)
            ImGui::OpenPopup("blocker");
        toolbar.prepare(layout, ready, held, blocked, camera_busy);
        const auto cursor = ImGui::GetCursorScreenPos();
        const auto action = toolbar.draw(layout, menu, Language::english, ready, held, blocked, camera_busy);
        CHECK(ImGui::GetCursorScreenPos().x == cursor.x);
        CHECK(ImGui::GetCursorScreenPos().y == cursor.y);
        ImGui::Dummy({0, 0});
        if (ImGui::BeginPopupModal("blocker", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Blocked");
            ImGui::EndPopup();
        }
        ImGui::End();
        ImGui::Render();
        return action;
    }
    ImVec2 center(std::size_t i) const
    {
        const auto &r = layout.buttons[i].rect;
        return {(r.min.x + r.max.x) / 2, (r.min.y + r.max.y) / 2};
    }
    void reveal()
    {
        ImGui::GetIO().AddMousePosEvent(480, 590);
        for (int i = 0; i < 36; ++i)
            REQUIRE(frame() == MenuAction::none);
        REQUIRE(toolbar.hit(layout, center(3)));
    }
    MenuAction click(std::size_t i)
    {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(center(i).x, center(i).y);
        frame();
        io.AddMouseButtonEvent(0, true);
        const auto down = frame();
        CHECK(down == MenuAction::none);
        io.AddMouseButtonEvent(0, false);
        return frame();
    }
};
} // namespace

TEST_CASE("real dock widgets emit shared menu actions only when visible and enabled", "[dock][imgui]")
{
    DockGui gui;
    gui.frame();
    CHECK_FALSE(gui.toolbar.hit(gui.layout, gui.center(3)));
    CHECK(gui.click(3) == MenuAction::none); // Hidden buttons are not submitted.
    gui.reveal();
    CHECK(gui.click(0) == MenuAction::previous);
    CHECK(gui.click(1) == MenuAction::none); // Last image: next is disabled.
    CHECK(gui.click(2) == MenuAction::zoom_out);
    CHECK(gui.click(3) == MenuAction::actual_size);
    gui.layout = dock_layout({0, 0}, {960, 600}, 1, false, true);
    CHECK(gui.click(3) == MenuAction::fit);
    CHECK(gui.click(4) == MenuAction::zoom_in);
    CHECK(gui.click(5) == MenuAction::rotate_left);
    CHECK(gui.click(6) == MenuAction::rotate_right);
    CHECK(gui.click(7) == MenuAction::image_info);
    CHECK(gui.click(8) == MenuAction::fullscreen);
    gui.frame(false);
    CHECK_FALSE(gui.toolbar.hit(gui.layout, gui.center(3)));
    CHECK(gui.click(3) == MenuAction::none);
}

TEST_CASE("more popup reuses the entire View menu and pins dock until dismissed", "[dock][imgui]")
{
    DockGui gui;
    gui.frame();
    gui.reveal();
    CHECK(gui.click(9) == MenuAction::none);
    auto &g = *ImGui::GetCurrentContext();
    REQUIRE(g.OpenPopupStack.Size == 1);
    REQUIRE(g.OpenPopupStack[0].Window);
    gui.frame();
    gui.frame(); // Allow the auto-sized popup to finish its first measurement.
    const auto *popup_window = g.OpenPopupStack[0].Window;
    CHECK(popup_window->Pos.y + popup_window->Size.y <= gui.layout.bounds.min.y - 8 + .5f);
    CHECK(popup_window->Pos.x + popup_window->Size.x == Catch::Approx(gui.layout.bounds.max.x).margin(1));
    const auto *view = find_menu_item(gui.menu, 2);
    REQUIRE(view);
    // draw_menu uses the exact stable IDs used by the context menu/bar, including all four pan commands.
    for (const auto &item : view->children)
        if (!item.separator)
            CHECK(g.OpenPopupStack[0].Window->GetID((item.label + "###menu_" + std::to_string(item.id)).c_str()) != 0);
    ImGui::GetIO().AddMousePosEvent(20, 100);
    for (int i = 0; i < 45; ++i)
        gui.frame();
    CHECK(gui.toolbar.hit(gui.layout, gui.center(3)));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    gui.frame();
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 15; ++i)
        gui.frame();
    CHECK(g.OpenPopupStack.empty());
    CHECK_FALSE(gui.toolbar.hit(gui.layout, gui.center(3)));
}

TEST_CASE("dock blocks overlay clicks tooltips honor dwell and unrelated modals invalidate", "[dock][imgui]")
{
    DockGui gui;
    gui.frame();
    gui.reveal();
    auto &io          = ImGui::GetIO();
    const auto center = gui.center(3);
    io.AddMousePosEvent(center.x, center.y);
    for (int i = 0; i < 40; ++i)
        gui.frame();
    const auto tooltip_visible = []
    {
        for (auto *w : ImGui::GetCurrentContext()->Windows)
            if (w->Active && (w->Flags & ImGuiWindowFlags_Tooltip))
                return true;
        return false;
    };
    CHECK_FALSE(tooltip_visible());
    for (int i = 0; i < 10; ++i)
        gui.frame();
    CHECK(tooltip_visible());
    gui.frame(true, false, true);
    CHECK_FALSE(gui.toolbar.hit(gui.layout, center));
    gui.reveal();
    gui.frame(true, false, false, true);
    CHECK_FALSE(gui.toolbar.hit(gui.layout, center));
}


TEST_CASE("keyboard focus pins dock without pinning ordinary mouse focus and Escape releases it", "[dock][imgui]")
{
    DockGui gui;
    gui.frame();
    gui.reveal();
    CHECK(gui.click(3) == MenuAction::actual_size);
    CHECK_FALSE(gui.toolbar.has_keyboard_focus());
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(20, 100);
    io.AddKeyEvent(ImGuiKey_Tab, true);
    gui.frame();
    io.AddKeyEvent(ImGuiKey_Tab, false);
    for (int i = 0; i < 40; ++i)
        gui.frame();
    REQUIRE(gui.toolbar.has_keyboard_focus());
    CHECK(gui.toolbar.hit(gui.layout, gui.center(3)));
    io.AddKeyEvent(ImGuiKey_Enter, true);
    auto action = gui.frame();
    io.AddKeyEvent(ImGuiKey_Enter, false);
    const auto released = gui.frame();
    CHECK((action != MenuAction::none || released != MenuAction::none));
    io.AddKeyEvent(ImGuiKey_Escape, true);
    gui.frame();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    for (int i = 0; i < 15; ++i)
        gui.frame();
    CHECK_FALSE(gui.toolbar.has_keyboard_focus());
    CHECK_FALSE(gui.toolbar.hit(gui.layout, gui.center(3)));
}


TEST_CASE("holding or clicking a dock button never starts the hide animation", "[dock][imgui][regression]")
{
    DockGui gui;
    gui.frame();
    gui.reveal();
    auto &io         = ImGui::GetIO();
    const auto point = gui.center(5);
    io.AddMousePosEvent(point.x, point.y);
    gui.frame();
    io.AddMouseButtonEvent(0, true);
    CHECK(gui.frame() == MenuAction::none);
    const auto popup_color = ImGui::ColorConvertFloat4ToU32(current_ui_palette().popup);
    for (int i = 0; i < 25; ++i)
    {
        CAPTURE(i);
        if (i == 5)
            io.AddMousePosEvent(point.x + 8, point.y); // Still on the button, past ImGui's drag threshold.
        CHECK(gui.frame(true, i >= 5) == MenuAction::none);
        auto *window = ImGui::FindWindowByName("dock_test");
        REQUIRE(window);
        CHECK(std::any_of(window->DrawList->VtxBuffer.begin(), window->DrawList->VtxBuffer.end(),
                          [&](const ImDrawVert &vertex) { return vertex.col == popup_color; }));
    }
    io.AddMouseButtonEvent(0, false);
    CHECK(gui.frame() == MenuAction::rotate_left);
}


TEST_CASE("a busy camera retains a shown dock but cannot reveal hidden controls or dispatch actions", "[dock][imgui][regression]")
{
    DockGui gui;
    gui.frame();
    gui.reveal();
    auto &io         = ImGui::GetIO();
    const auto point = gui.center(5);
    io.AddMousePosEvent(point.x, point.y);
    gui.frame();
    const auto color = ImGui::ColorConvertFloat4ToU32(current_ui_palette().popup);
    for (int i = 0; i < 30; ++i)
    {
        if (i == 5)
            io.AddMouseButtonEvent(0, true);
        if (i == 8)
            io.AddMouseButtonEvent(0, false);
        CHECK(gui.frame(false, false, false, false, true) == MenuAction::none);
        const auto *window = ImGui::FindWindowByName("dock_test");
        REQUIRE(window);
        CHECK(std::any_of(window->DrawList->VtxBuffer.begin(), window->DrawList->VtxBuffer.end(),
                          [&](const ImDrawVert &vertex) { return vertex.col == color; }));
    }
    CHECK(gui.frame() == MenuAction::none);
    CHECK(gui.toolbar.hit(gui.layout, point));
    gui.frame(false); // Replacement/failed image still resets the overlay.
    io.AddMousePosEvent(480, 590);
    for (int i = 0; i < 40; ++i)
        CHECK(gui.frame(false, false, false, false, true) == MenuAction::none);
    CHECK_FALSE(gui.toolbar.hit(gui.layout, point));
}


TEST_CASE("More stays attached above the dock and scrolls rather than covering its icon in a short window",
          "[dock][imgui][regression]")
{
    DockGui gui;
    ImGui::GetIO().DisplaySize = {960, 320};
    gui.layout                 = dock_layout({0, 0}, {960, 280}, 1, true, false);
    gui.frame();
    ImGui::GetIO().AddMousePosEvent(480, 275);
    for (int i = 0; i < 36; ++i)
        gui.frame();
    CHECK(gui.click(9) == MenuAction::none);
    gui.frame();
    gui.frame();
    const auto &g = *ImGui::GetCurrentContext();
    REQUIRE(g.OpenPopupStack.Size == 1);
    const auto *menu = g.OpenPopupStack[0].Window;
    REQUIRE(menu);
    CHECK(menu->Pos.y >= 0);
    CHECK(menu->Pos.y + menu->Size.y <= gui.layout.bounds.min.y - 8 + .5f);
    CHECK(menu->ScrollMax.y > 0);
    CHECK(menu->ScrollbarY);
    CHECK_FALSE(ImRect(menu->Pos, {menu->Pos.x + menu->Size.x, menu->Pos.y + menu->Size.y}).Contains(gui.center(9)));
}
