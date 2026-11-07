#include "sdl_render_host.hpp"
#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>
#include <stdexcept>

namespace liltview
{
SDL_WindowFlags prepare_render_window()
{
    if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) || !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3) ||
        !SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1))
        throw std::runtime_error(SDL_GetError());
    return SDL_WINDOW_OPENGL;
}

bool initialize_imgui_platform(SDL_Window *window)
{
    return ImGui_ImplSDL3_InitForOpenGL(window, SDL_GL_GetCurrentContext());
}
} // namespace liltview
