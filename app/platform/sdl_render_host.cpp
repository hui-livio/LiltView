#include "sdl_render_host.hpp"
#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

namespace liltview
{
SDL_WindowFlags prepare_render_window()
{
    // No graphics API flags or context attributes: SDL_Render owns the device.
    return 0;
}

bool initialize_imgui_platform(SDL_Window *window)
{
    auto *renderer = SDL_GetRenderer(window);
    return renderer && ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
}
} // namespace liltview
