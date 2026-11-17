#include "sdl_render_host.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
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
void new_imgui_platform_frame(SDL_Window *window)
{
    auto &io                      = ImGui::GetIO();
    const auto now                = SDL_GetTicksNS();
    const auto properties         = SDL_GetWindowProperties(window);
    constexpr auto last_frame_key = "liltview.imgui.last_frame_ns";
    const auto previous           = static_cast<Uint64>(SDL_GetNumberProperty(properties, last_frame_key, 0));
    SDL_SetNumberProperty(properties, last_frame_key, static_cast<Sint64>(now));
    if (SDL_GetKeyboardFocus() == window)
    {
        // The backend caches cursor identity per context, while SDL keeps one
        // process-wide cursor. Restore this context's last cursor before its
        // cache decides whether a new binding is necessary.
        constexpr auto cursor_key = "liltview.imgui.cursor";
        const bool cursor_changes = (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0;
        if (cursor_changes)
            if (auto *cursor = static_cast<SDL_Cursor *>(SDL_GetPointerProperty(properties, cursor_key, nullptr)))
                SDL_SetCursor(cursor);
        ImGui_ImplSDL3_NewFrame();
        if (cursor_changes)
            SDL_SetPointerProperty(properties, cursor_key, SDL_GetCursor());
    }
    else
    {
        // The bundled SDL backend unconditionally updates global mouse capture
        // and cursor state. Calling it for a background context would cancel a
        // drag in the other window. Its remaining per-window updates are small.
        int width = 0, height = 0, pixels_x = 0, pixels_y = 0;
        SDL_GetWindowSize(window, &width, &height);
        SDL_GetWindowSizeInPixels(window, &pixels_x, &pixels_y);
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0)
            width = height = 0;
        io.DisplaySize             = {static_cast<float>(width), static_cast<float>(height)};
        io.DisplayFramebufferScale = width > 0 && height > 0 ?
            ImVec2{static_cast<float>(pixels_x) / width, static_cast<float>(pixels_y) / height} :
            ImVec2{1, 1};
        if (SDL_GetMouseFocus() != window && !ImGui::IsAnyMouseDown())
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    // Keep timing continuous even while the backend was skipped out of focus.
    io.DeltaTime = previous && now > previous ? std::max(0.000001f, static_cast<float>((now - previous) / 1e9)) : 1.0f / 60;
}
} // namespace liltview
