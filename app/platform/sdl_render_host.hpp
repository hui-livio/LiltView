#ifndef LILTVIEW_SDL_RENDER_HOST_HPP
#define LILTVIEW_SDL_RENDER_HOST_HPP
#include <SDL3/SDL_video.h>
namespace liltview
{
SDL_WindowFlags prepare_render_window();
bool initialize_imgui_platform(SDL_Window *window);
// Only the focused context may change SDL's process-wide cursor/capture state.
// Background windows still get current size, density, timing and queued input.
void new_imgui_platform_frame(SDL_Window *window);
} // namespace liltview
#endif
