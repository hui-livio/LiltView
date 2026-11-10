#include "window_theme.hpp"

#include <SDL3/SDL.h>
#ifdef _WIN32
    #include <windows.h>

    #include <dwmapi.h>
#endif

namespace liltview
{
bool apply_native_window_theme(SDL_Window *window, bool dark)
{
    if (!window)
        return SDL_SetError("Cannot theme a null window");
#ifdef _WIN32
    const auto hwnd =
        static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd) // e.g. SDL's offscreen backend has no native caption.
        return true;
    BOOL enabled      = dark ? TRUE : FALSE;
    const auto result = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled));
    if (FAILED(result))
        return SDL_SetError("DWM titlebar theme unavailable (0x%08lx)", static_cast<unsigned long>(result));

    // SDL 3.4 also sets this per-window policy, but derives it from the SYSTEM
    // theme. On Windows 10 the caption and its button glyphs need this policy to
    // agree with the explicit APP theme, not just the DWM dark-mode attribute.
    // Mirror SDL's dynamically loaded User32 bridge; do not force a process-wide
    // theme or override the caption background independently of its controls.
    struct CompositionAttributeData
    {
        int attribute;
        void *data;
        SIZE_T size;
    };
    using SetCompositionAttribute = BOOL(WINAPI *)(HWND, const CompositionAttributeData *);
    if (const auto user32 = GetModuleHandleW(L"user32.dll"))
    {
        const auto set_attribute =
            reinterpret_cast<SetCompositionAttribute>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
        if (set_attribute)
        {
            constexpr int use_dark_mode_colors = 26;
            const CompositionAttributeData data{use_dark_mode_colors, &enabled, sizeof(enabled)};
            set_attribute(hwnd, &data); // Optional bridge; DWM remains the supported fallback.
        }
    }

    // Recalculate and repaint the non-client frame synchronously. Invalidating
    // alone can leave the old caption/buttons visible until activation changes.
    if (!SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
        return SDL_SetError("Cannot refresh Win32 window frame: %lu", GetLastError());
    // SDL retains WS_CAPTION in borderless mode for taskbar animations; use
    // SDL flags rather than Win32 style bits to identify a real native frame.
    if ((SDL_GetWindowFlags(window) & (SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN)) == 0)
    {
        // Windows 10 can keep the old active/inactive caption cached even after
        // SWP_FRAMECHANGED + RDW_FRAME. Refresh that visual state explicitly,
        // then restore the REAL state. WM_NCACTIVATE only redraws the caption;
        // do not steal focus with SetActiveWindow/SetForegroundWindow, resize,
        // or hide/show the window as a repaint workaround.
        const BOOL active = GetForegroundWindow() == hwnd ? TRUE : FALSE;
        SendMessageW(hwnd, WM_NCACTIVATE, active ? FALSE : TRUE, 0);
        SendMessageW(hwnd, WM_NCACTIVATE, active, 0);
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
#else
    (void)dark;
#endif
    return true;
}
} // namespace liltview
