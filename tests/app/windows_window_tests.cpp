#include "platform/window_shape.hpp"
#include "platform/live_resize.hpp"
#include "platform/window_theme.hpp"
#include "ui/window_chrome.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <windows.h>

#include <dwmapi.h>

namespace
{
struct NativeWindow
{
    SDL_Window *window = nullptr;
    explicit NativeWindow(bool transparent = true)
    {
        if (SDL_Init(SDL_INIT_VIDEO))
            window = SDL_CreateWindow("LiltView native frame test", 640, 480,
                                      SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | (transparent ? SDL_WINDOW_TRANSPARENT : 0));
    }
    ~NativeWindow()
    {
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    HWND hwnd() const
    {
        return static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    }
};

int window_region(HWND hwnd)
{
    const auto region = CreateRectRgn(0, 0, 0, 0);
    const int result  = GetWindowRgn(hwnd, region);
    DeleteObject(region);
    return result;
}
// Observe the real non-client recalculation, not only the stored DWM flag.
struct FrameObserver
{
    HWND window;
    WNDPROC previous;
    int recalculations = 0;
    int nonclient_paints = 0;
    std::vector<LPARAM> activation_paint_parameters;
    int focus_changes  = 0;
    std::vector<WPARAM> caption_activation;
    explicit FrameObserver(HWND hwnd) :
        window(hwnd)
    {
        SetPropW(window, L"LiltViewFrameObserver", this);
        previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&procedure)));
    }
    ~FrameObserver()
    {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
        RemovePropW(window, L"LiltViewFrameObserver");
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        auto *self = static_cast<FrameObserver *>(GetPropW(hwnd, L"LiltViewFrameObserver"));
        if (message == WM_NCCALCSIZE)
            ++self->recalculations;
        if (message == WM_NCPAINT)
            ++self->nonclient_paints;
        if (message == WM_NCACTIVATE)
        {
            self->caption_activation.push_back(wparam);
            self->activation_paint_parameters.push_back(lparam);
        }
        if (message == WM_ACTIVATE || message == WM_SETFOCUS || message == WM_KILLFOCUS)
            ++self->focus_changes;
        return CallWindowProcW(self->previous, hwnd, message, wparam, lparam);
    }
};

struct CompositionAttributeData
{
    int attribute;
    void *data;
    SIZE_T size;
};
using GetCompositionAttribute = BOOL(WINAPI *)(HWND, CompositionAttributeData *);

} // namespace

TEST_CASE("Windows native frame has no custom region and keeps DWM rendering", "[windows][chrome]")
{
    NativeWindow native;
    INFO(SDL_GetError());
    REQUIRE(native.window);
    REQUIRE(native.hwnd());
    liltview::WindowShape shape;
    REQUIRE(shape.apply(native.window, 0, true));
    // A rectangular HRGN is NOT equivalent to having no application-defined region.
    CHECK(window_region(native.hwnd()) == ERROR);
    BOOL composition = FALSE;
    REQUIRE(SUCCEEDED(DwmIsCompositionEnabled(&composition)));
    if (composition)
    {
        BOOL rendering = FALSE;
        REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
        CHECK(rendering == TRUE);
    }
}

TEST_CASE("Windows native frame restores DWM after rounded borderless mode", "[windows][chrome]")
{
    NativeWindow native;
    INFO(SDL_GetError());
    REQUIRE(native.window);
    liltview::WindowShape shape;
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    REQUIRE(shape.apply(native.window, 10));
    CHECK(window_region(native.hwnd()) == COMPLEXREGION);
    const auto id = SDL_GetWindowID(native.window);
    REQUIRE(shape.apply(native.window, 0, true));
    REQUIRE(SDL_SetWindowBordered(native.window, true));
    REQUIRE(shape.apply(native.window, 0, true));
    CHECK(SDL_GetWindowID(native.window) == id);
    CHECK(window_region(native.hwnd()) == ERROR);
    REQUIRE(SDL_SetWindowSize(native.window, 800, 600));
    SDL_SyncWindow(native.window);
    CHECK(window_region(native.hwnd()) == ERROR);
    CHECK(SDL_GetPointerProperty(SDL_GetWindowProperties(native.window), SDL_PROP_WINDOW_SHAPE_POINTER, nullptr) == nullptr);
}

TEST_CASE("Windows native titlebar follows explicit dark and light application themes", "[windows][chrome]")
{
    NativeWindow native;
    REQUIRE(native.window);
    BOOL enabled = FALSE;
    if (FAILED(DwmGetWindowAttribute(native.hwnd(), DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled))))
        SKIP("This Windows session does not support the DWM dark titlebar attribute");
    FrameObserver observer(native.hwnd());
    REQUIRE(observer.previous);
    for (const bool dark : {true, false, true})
    {
        CAPTURE(dark);
        const int before = observer.recalculations;
        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
        CHECK(observer.recalculations > before);
        REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled))));
        CHECK((enabled != FALSE) == dark);
    }
}

TEST_CASE("Windows square custom corners stay complete through theme resize and decoration changes",
          "[windows][chrome][rounding]")
{
    for (const bool transparent : {false, true})
    {
        CAPTURE(transparent);
        NativeWindow native(transparent);
        REQUIRE(native.window);
        liltview::WindowShape shape;
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            CAPTURE(cycle);
            REQUIRE(SDL_SetWindowBordered(native.window, false));
            for (const float radius : {0.0f, 12.0f, 0.0f})
                for (const bool dark : {false, true})
                    for (const int width : {640, 853, 640})
                    {
                        CAPTURE(radius, dark, width);
                        REQUIRE(SDL_SetWindowSize(native.window, width, 480));
                        SDL_SyncWindow(native.window);
                        REQUIRE(shape.apply(native.window, radius));
                        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
                        SendMessageW(native.hwnd(), WM_NCACTIVATE, TRUE, 0);
                        SendMessageW(native.hwnd(), WM_NCPAINT, 1, 0);
                        SendMessageW(native.hwnd(), WM_NCACTIVATE, FALSE, 0);
                        const auto region = CreateRectRgn(0, 0, 0, 0);
                        REQUIRE(region);
                        const int kind = GetWindowRgn(native.hwnd(), region);
                        RECT bounds{};
                        GetRgnBox(region, &bounds);
                        int pixels_w = 0, pixels_h = 0;
                        const bool have_size = SDL_GetWindowSizeInPixels(native.window, &pixels_w, &pixels_h);
                        const std::array corners{POINT{0, 0}, POINT{pixels_w - 1, 0}, POINT{0, pixels_h - 1},
                                                 POINT{pixels_w - 1, pixels_h - 1}};
                        std::array<bool, 4> contains{};
                        for (std::size_t i = 0; i < corners.size(); ++i)
                            contains[i] = PtInRegion(region, corners[i].x, corners[i].y) != FALSE;
                        DeleteObject(region);
                        REQUIRE(have_size);
                        CHECK(kind == (radius == 0 ? SIMPLEREGION : COMPLEXREGION));
                        CHECK(bounds.left == 0);
                        CHECK(bounds.top == 0);
                        CHECK(bounds.right == pixels_w);
                        CHECK(bounds.bottom == pixels_h);
                        for (std::size_t i = 0; i < corners.size(); ++i)
                        {
                            CAPTURE(i);
                            CHECK(contains[i] == (radius == 0));
                        }
                    }
            // Native captions must not inherit even a rectangular custom mask.
            REQUIRE(shape.apply(native.window, 0, true));
            REQUIRE(SDL_SetWindowBordered(native.window, true));
            REQUIRE(liltview::apply_native_window_theme(native.window, false));
            CHECK(window_region(native.hwnd()) == ERROR);
            BOOL composition = FALSE;
            REQUIRE(SUCCEEDED(DwmIsCompositionEnabled(&composition)));
            if (composition)
            {
                BOOL rendering = FALSE;
                REQUIRE(
                    SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
                CHECK(rendering);
            }
        }
    }
}

TEST_CASE("Windows native caption color policy agrees with app theme after frame changes", "[windows][chrome]")
{
    NativeWindow native;
    REQUIRE(native.window);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    REQUIRE(user32);
    const auto get_attribute = reinterpret_cast<GetCompositionAttribute>(GetProcAddress(user32, "GetWindowCompositionAttribute"));
    if (!get_attribute)
        SKIP("This Windows session cannot query the native caption color policy");
    BOOL enabled = FALSE;
    CompositionAttributeData data{26, &enabled, sizeof(enabled)};
    if (!get_attribute(native.hwnd(), &data))
        SKIP("This Windows session does not support the native caption color policy");
    liltview::WindowShape shape;
    const auto id = SDL_GetWindowID(native.window);
    for (const bool dark : {true, false, true, false})
    {
        CAPTURE(dark);
        REQUIRE(SDL_SetWindowBordered(native.window, false));
        REQUIRE(shape.apply(native.window, 10));
        REQUIRE(SDL_SetWindowBordered(native.window, true));
        REQUIRE(shape.apply(native.window, 0, true));
        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
        REQUIRE(get_attribute(native.hwnd(), &data));
        CHECK((enabled != FALSE) == dark);
        CHECK(window_region(native.hwnd()) == ERROR);
        CHECK(SDL_GetWindowID(native.window) == id);
    }
}

TEST_CASE("Windows theme switches refresh caption activation without moving or focusing window", "[windows][chrome]")
{
    NativeWindow native;
    REQUIRE(native.window);
    liltview::WindowShape shape;
    REQUIRE(shape.apply(native.window, 0, true));
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    FrameObserver observer(native.hwnd());
    REQUIRE(observer.previous);
    const auto foreground     = GetForegroundWindow();
    const auto active         = GetActiveWindow();
    const auto focus          = GetFocus();
    const auto keyboard_focus = SDL_GetKeyboardFocus();
    const bool caption_active = foreground == native.hwnd();
    const auto flags          = SDL_GetWindowFlags(native.window);
    RECT original_frame{}, original_client{};
    REQUIRE(GetWindowRect(native.hwnd(), &original_frame));
    REQUIRE(GetClientRect(native.hwnd(), &original_client));
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    REQUIRE(GetWindowPlacement(native.hwnd(), &placement));
    for (const bool dark : {true, false, true})
    {
        CAPTURE(dark);
        observer.caption_activation.clear();
        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
        // Merely changing DWM flags / recalculating geometry does not evict
        // Windows 10's cached active/inactive caption. Repaint both states,
        // ending with the REAL foreground state, without changing actual focus.
        REQUIRE(observer.caption_activation.size() == 2);
        CHECK((observer.caption_activation.front() != FALSE) == !caption_active);
        CHECK((observer.caption_activation.back() != FALSE) == caption_active);
        CHECK(observer.focus_changes == 0);
        CHECK(GetForegroundWindow() == foreground);
        CHECK(GetActiveWindow() == active);
        CHECK(GetFocus() == focus);
        CHECK(SDL_GetKeyboardFocus() == keyboard_focus);
        CHECK(SDL_GetWindowFlags(native.window) == flags);
        RECT frame{}, client{};
        REQUIRE(GetWindowRect(native.hwnd(), &frame));
        REQUIRE(GetClientRect(native.hwnd(), &client));
        CHECK(EqualRect(&frame, &original_frame));
        CHECK(EqualRect(&client, &original_client));
        WINDOWPLACEMENT current{};
        current.length = sizeof(current);
        REQUIRE(GetWindowPlacement(native.hwnd(), &current));
        CHECK(current.showCmd == placement.showCmd);
        CHECK(EqualRect(&current.rcNormalPosition, &placement.rcNormalPosition));
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            CHECK(event.type != SDL_EVENT_WINDOW_FOCUS_GAINED);
            CHECK(event.type != SDL_EVENT_WINDOW_FOCUS_LOST);
            CHECK(event.type != SDL_EVENT_WINDOW_RESIZED);
            CHECK(event.type != SDL_EVENT_WINDOW_MOVED);
        }
    }
}

TEST_CASE("Windows borderless theme update does not synthesize native caption activation", "[windows][chrome]")
{
    NativeWindow native;
    REQUIRE(native.window);
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    FrameObserver observer(native.hwnd());
    REQUIRE(observer.previous);
    REQUIRE(liltview::apply_native_window_theme(native.window, true));
    CHECK(observer.caption_activation.empty());
    CHECK(observer.focus_changes == 0);
}

TEST_CASE("Windows custom frame blocks classic painting on first activation and after native resize", "[windows][chrome][resize]")
{
    NativeWindow native;
    REQUIRE(native.window);
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    FrameObserver downstream(native.hwnd());
    REQUIRE(downstream.previous);
    // Without the guard, the same paint message reaches SDL/DefWindowProc.
    SendMessageW(native.hwnd(), WM_NCPAINT, 1, 0);
    CHECK(downstream.nonclient_paints > 0);
    liltview::WindowShape shape;
    const auto window_id = SDL_GetWindowID(native.window);
    for (const float radius : {12.0f, 0.0f, 12.0f})
        for (const bool dark : {false, true})
        {
            CAPTURE(radius, dark);
            REQUIRE(shape.apply(native.window, radius));
            REQUIRE(liltview::apply_native_window_theme(native.window, dark));
            for (const int width : {640, 800, 640})
            {
                CAPTURE(width);
                REQUIRE(SDL_SetWindowSize(native.window, width, 480));
                SDL_SyncWindow(native.window);
                REQUIRE(shape.apply(native.window, radius));
                downstream.nonclient_paints = 0;
                downstream.activation_paint_parameters.clear();
                // Model the first activation/drag repaint and a subsequent
                // deactivation. SDL must see activation, but must not repaint NC.
                SendMessageW(native.hwnd(), WM_NCACTIVATE, TRUE, 0);
                SendMessageW(native.hwnd(), WM_NCPAINT, 1, 0);
                SendMessageW(native.hwnd(), WM_NCACTIVATE, FALSE, 0);
                CHECK(downstream.nonclient_paints == 0);
                REQUIRE(downstream.activation_paint_parameters.size() == 2);
                CHECK(downstream.activation_paint_parameters[0] == -1);
                CHECK(downstream.activation_paint_parameters[1] == -1);
                CHECK(SDL_GetWindowID(native.window) == window_id);
                CHECK((GetWindowLongPtrW(native.hwnd(), GWL_STYLE) & WS_THICKFRAME) != 0);
                RECT frame{}, client{};
                REQUIRE(GetWindowRect(native.hwnd(), &frame));
                REQUIRE(GetClientRect(native.hwnd(), &client));
                CHECK(frame.right - frame.left == client.right);
                CHECK(frame.bottom - frame.top == client.bottom);
            }
        }
    // Switching back to a system caption must restore native painting, rather
    // than leaving either a rectangular custom region or a paint suppression.
    REQUIRE(SDL_SetWindowBordered(native.window, true));
    REQUIRE(shape.apply(native.window, 0, true));
    REQUIRE(liltview::apply_native_window_theme(native.window, true));
    downstream.nonclient_paints = 0;
    downstream.activation_paint_parameters.clear();
    SendMessageW(native.hwnd(), WM_NCPAINT, 1, 0);
    SendMessageW(native.hwnd(), WM_NCACTIVATE, TRUE, 0);
    CHECK(downstream.nonclient_paints > 0);
    REQUIRE(downstream.activation_paint_parameters.size() == 1);
    CHECK(downstream.activation_paint_parameters[0] == 0);
    CHECK(window_region(native.hwnd()) == ERROR);
    BOOL composition = FALSE;
    REQUIRE(SUCCEEDED(DwmIsCompositionEnabled(&composition)));
    if (composition)
    {
        BOOL rendering = FALSE;
        REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
        CHECK(rendering == TRUE);
    }
}

TEST_CASE("Windows frame guard preserves native move and all resize hit results and detaches cleanly", "[windows][chrome][resize]")
{
    NativeWindow native;
    REQUIRE(native.window);
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    const auto original_proc = GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC);
    SDL_HitTestResult requested = SDL_HITTEST_NORMAL;
    REQUIRE(SDL_SetWindowHitTest(native.window,
        [](SDL_Window *, const SDL_Point *, void *value) -> SDL_HitTestResult
        {
            return *static_cast<SDL_HitTestResult *>(value);
        }, &requested));
    {
        liltview::WindowShape shape;
        REQUIRE(shape.apply(native.window, 12));
        POINT point{100, 100};
        REQUIRE(ClientToScreen(native.hwnd(), &point));
        const auto position = MAKELPARAM(static_cast<WORD>(point.x), static_cast<WORD>(point.y));
        const SDL_HitTestResult requests[]{SDL_HITTEST_NORMAL, SDL_HITTEST_DRAGGABLE, SDL_HITTEST_RESIZE_TOPLEFT,
            SDL_HITTEST_RESIZE_TOP, SDL_HITTEST_RESIZE_TOPRIGHT, SDL_HITTEST_RESIZE_RIGHT, SDL_HITTEST_RESIZE_BOTTOMRIGHT,
            SDL_HITTEST_RESIZE_BOTTOM, SDL_HITTEST_RESIZE_BOTTOMLEFT, SDL_HITTEST_RESIZE_LEFT};
        const LRESULT expected[]{HTCLIENT, HTCAPTION, HTTOPLEFT, HTTOP, HTTOPRIGHT, HTRIGHT, HTBOTTOMRIGHT,
            HTBOTTOM, HTBOTTOMLEFT, HTLEFT};
        for (unsigned i = 0; i < 10; ++i)
        {
            CAPTURE(i);
            requested = requests[i];
            CHECK(SendMessageW(native.hwnd(), WM_NCHITTEST, 0, position) == expected[i]);
        }
    }
    CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
    REQUIRE(SDL_SetWindowHitTest(native.window, nullptr, nullptr));
}

TEST_CASE("Windows system frames never retain the custom painting subclass", "[windows][chrome][dwm]")
{
    NativeWindow native;
    REQUIRE(native.window);
    SECTION("hidden native startup") {}
    SECTION("shown native frame")
    {
        // Exercise the real shown-window NC lifecycle without stealing focus.
        ShowWindow(native.hwnd(), SW_SHOWNOACTIVATE);
        SDL_PumpEvents();
        REQUIRE(IsWindowVisible(native.hwnd()));
    }
    const auto original_proc = GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC);
    const auto id = SDL_GetWindowID(native.window);
    liltview::WindowShape shape;
    // A native startup must not install even a pass-through custom subclass.
    REQUIRE(shape.apply(native.window, 0, true));
    CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        CAPTURE(cycle);
        REQUIRE(SDL_SetWindowBordered(native.window, false));
        REQUIRE(shape.apply(native.window, 12));
        CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) != original_proc);
        // Application removes custom decoration BEFORE changing SDL's flags.
        REQUIRE(shape.apply(native.window, 0, true));
        CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
        REQUIRE(SDL_SetWindowBordered(native.window, true));
        REQUIRE(shape.apply(native.window, 0, true));
        CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
        CHECK(window_region(native.hwnd()) == ERROR);
        CHECK(SDL_GetWindowID(native.window) == id);
        REQUIRE(liltview::apply_native_window_theme(native.window, cycle % 2 == 0));
        CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
        BOOL composition = FALSE;
        REQUIRE(SUCCEEDED(DwmIsCompositionEnabled(&composition)));
        if (composition)
        {
            BOOL rendering = FALSE;
            REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
            CHECK(rendering == TRUE);
        }
        for (const int width : {800, 640})
        {
            REQUIRE(SDL_SetWindowSize(native.window, width, 480));
            SDL_SyncWindow(native.window);
            SDL_PumpEvents();
            REQUIRE(shape.apply(native.window, 0, true));
            CHECK(GetWindowLongPtrW(native.hwnd(), GWLP_WNDPROC) == original_proc);
            CHECK(window_region(native.hwnd()) == ERROR);
            RECT frame{}, client{};
            REQUIRE(GetWindowRect(native.hwnd(), &frame));
            REQUIRE(GetClientRect(native.hwnd(), &client));
            CHECK(frame.right - frame.left > client.right);
            CHECK(frame.bottom - frame.top > client.bottom);
        }
    }
}

TEST_CASE("Windows native theme restores DWM rendering after a disabled nonclient policy", "[windows][chrome][dwm]")
{
    NativeWindow native;
    REQUIRE(native.window);
    BOOL composition = FALSE;
    REQUIRE(SUCCEEDED(DwmIsCompositionEnabled(&composition)));
    if (!composition)
        SKIP("This Windows session has no DWM composition");
    liltview::WindowShape shape;
    for (const bool dark : {true, false})
    {
        CAPTURE(dark);
        REQUIRE(SDL_SetWindowBordered(native.window, false));
        REQUIRE(shape.apply(native.window, 12));
        // Model a disabled NC policy: a theme-color attribute alone cannot
        // restore DWM's ownership of the caption and border in this state.
        const auto policy = DWMNCRP_DISABLED;
        REQUIRE(SUCCEEDED(DwmSetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy))));
        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
        BOOL rendering = TRUE;
        REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
        CHECK(rendering == FALSE); // Custom theme updates must not force DWM on.
        REQUIRE(shape.apply(native.window, 0, true));
        REQUIRE(SDL_SetWindowBordered(native.window, true));
        REQUIRE(shape.apply(native.window, 0, true));
        REQUIRE(liltview::apply_native_window_theme(native.window, dark));
        rendering = FALSE;
        REQUIRE(SUCCEEDED(DwmGetWindowAttribute(native.hwnd(), DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering))));
        CHECK(rendering == TRUE);
        CHECK(window_region(native.hwnd()) == ERROR);
        FrameObserver observer(native.hwnd());
        REQUIRE(observer.previous);
        SendMessageW(native.hwnd(), WM_NCACTIVATE, TRUE, 0);
        SendMessageW(native.hwnd(), WM_NCPAINT, 1, 0);
        REQUIRE(observer.activation_paint_parameters.size() == 1);
        CHECK(observer.activation_paint_parameters.front() == 0);
        CHECK(observer.nonclient_paints > 0);
    }
}

TEST_CASE("Windows native sizing timer updates custom regions before SDL polling resumes", "[windows][chrome][live-resize]")
{
    NativeWindow native;
    REQUIRE(native.window);
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    liltview::WindowShape shape;
    REQUIRE(shape.apply(native.window, 12));
    struct NativeSizingLoop
    {
        HWND window;
        explicit NativeSizingLoop(HWND hwnd) : window(hwnd) { SendMessageW(window, WM_ENTERSIZEMOVE, 0, 0); }
        ~NativeSizingLoop() { SendMessageW(window, WM_EXITSIZEMOVE, 0, 0); }
    } sizing(native.hwnd());
    int frames = 0;
    int painted_width = 0, painted_height = 0;
    bool region_updated = false;
    liltview::LiveResizeRedraw redraw(SDL_GetWindowID(native.window), [&]
    {
        region_updated = shape.apply(native.window, 12);
        SDL_GetWindowSizeInPixels(native.window, &painted_width, &painted_height);
        ++frames;
    });
    for (const auto size : {SIZE{960, 640}, SIZE{480, 320}, SIZE{800, 600}, SIZE{640, 480}})
    {
        CAPTURE(size.cx, size.cy);
        REQUIRE(SDL_SetWindowSize(native.window, size.cx, size.cy));
        const int before = frames;
        // Pump only native timer messages, as DefWindowProc's modal loop does.
        // There is deliberately NO SDL_PollEvent or application frame here.
        const auto deadline = SDL_GetTicks() + 2000;
        while (frames == before && SDL_GetTicks() < deadline)
        {
            MSG message{};
            if (PeekMessageW(&message, native.hwnd(), WM_TIMER, WM_TIMER, PM_REMOVE))
                DispatchMessageW(&message);
            else
                SDL_Delay(1);
        }
        redraw.rethrow_if_failed();
        REQUIRE(frames > before);
        REQUIRE(region_updated);
        int width = 0, height = 0;
        REQUIRE(SDL_GetWindowSizeInPixels(native.window, &width, &height));
        CHECK(painted_width == width);
        CHECK(painted_height == height);
        const auto region = CreateRectRgn(0, 0, 0, 0);
        REQUIRE(region);
        const auto kind = GetWindowRgn(native.hwnd(), region);
        RECT bounds{};
        GetRgnBox(region, &bounds);
        DeleteObject(region);
        CHECK(kind == COMPLEXREGION);
        CHECK(bounds.left == 0);
        CHECK(bounds.top == 0);
        CHECK(bounds.right == width);
        CHECK(bounds.bottom == height);
    }
}

TEST_CASE("Windows alpha visual regions preserve AA padding without expanding opaque shapes", "[windows][chrome][rounding]")
{
    for (const bool transparent : {false, true})
    {
        CAPTURE(transparent);
        NativeWindow native(transparent);
        REQUIRE(native.window);
        REQUIRE(SDL_SetWindowBordered(native.window, false));
        liltview::WindowShape shape;
        REQUIRE(shape.apply(native.window, 12));
        int width = 0, height = 0;
        REQUIRE(SDL_GetWindowSizeInPixels(native.window, &width, &height));
        const float density = SDL_GetWindowPixelDensity(native.window);
        const auto spans = liltview::rounded_window_spans(width, height, 12 * density, transparent ? density : 0);
        const auto region = CreateRectRgn(0, 0, 0, 0);
        REQUIRE(region);
        const auto kind = GetWindowRgn(native.hwnd(), region);
        CHECK(kind == COMPLEXREGION);
        const int extent = static_cast<int>(std::ceil(12 * density));
        for (int y = 0; y < extent; ++y)
            for (int x = 0; x < extent; ++x)
            {
                const bool covered = std::any_of(spans.begin(), spans.end(), [&](const liltview::WindowSpan &span)
                {
                    return x >= span.x && x < span.x + span.width && y >= span.y && y < span.y + span.height;
                });
                CHECK((PtInRegion(region, x, y) != 0) == covered);
                CHECK((PtInRegion(region, width - 1 - x, y) != 0) == covered);
                CHECK((PtInRegion(region, x, height - 1 - y) != 0) == covered);
                CHECK((PtInRegion(region, width - 1 - x, height - 1 - y) != 0) == covered);
            }
        DeleteObject(region);
    }
}
