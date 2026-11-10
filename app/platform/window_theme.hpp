#ifndef LILTVIEW_WINDOW_THEME_HPP
#define LILTVIEW_WINDOW_THEME_HPP

struct SDL_Window;
namespace liltview
{
// Apply the resolved application theme to the native frame, on the SDL/UI thread.
// Refresh native caption activation visuals without moving/resizing/focusing.
// Other backends are a no-op. Unsupported DWM attributes are non-fatal to callers.
bool apply_native_window_theme(SDL_Window *window, bool dark);
} // namespace liltview
#endif
