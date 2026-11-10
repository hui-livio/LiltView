#include "platform/window_shape.hpp"
#include "platform/window_theme.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <windows.h>

#include <dwmapi.h>

namespace
{
struct NativeWindow
{
    SDL_Window *window = nullptr;
    NativeWindow()
    {
        if (SDL_Init(SDL_INIT_VIDEO))
            window = SDL_CreateWindow("LiltView native frame test", 640, 480,
                                      SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_TRANSPARENT);
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
        if (message == WM_NCACTIVATE)
            self->caption_activation.push_back(wparam);
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

TEST_CASE("Windows square custom windows clear regions and can become rounded again", "[windows][chrome]")
{
    NativeWindow native;
    REQUIRE(native.window);
    REQUIRE(SDL_SetWindowBordered(native.window, false));
    liltview::WindowShape shape;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        CAPTURE(cycle);
        REQUIRE(shape.apply(native.window, 10));
        CHECK(window_region(native.hwnd()) == COMPLEXREGION);
        REQUIRE(shape.apply(native.window, 0));
        CHECK(window_region(native.hwnd()) == ERROR);
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
