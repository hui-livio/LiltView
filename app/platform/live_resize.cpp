#include "live_resize.hpp"

#include <SDL3/SDL.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace liltview
{
LiveResizeRedraw::LiveResizeRedraw(SDL_WindowID window, std::function<void()> redraw) :
    window_(window),
    redraw_(std::move(redraw))
{
    if (!SDL_IsMainThread() || !window_ || !redraw_)
        throw std::invalid_argument("Live resize redraw requires a main-thread window and callback");
    if (!SDL_AddEventWatch(watch, this))
        throw std::runtime_error(std::string("Cannot watch live resize: ") + SDL_GetError());
}

LiveResizeRedraw::~LiveResizeRedraw()
{
    SDL_RemoveEventWatch(watch, this);
}

bool SDLCALL LiveResizeRedraw::watch(void *userdata, SDL_Event *event)
{
    // Watches may run on event-producing worker threads. Do not even read
    // mutable watch/application state before checking the main thread.
    if (!SDL_IsMainThread() || event->type != SDL_EVENT_WINDOW_EXPOSED || event->window.data1 != 1)
        return true;
    auto &self = *static_cast<LiveResizeRedraw *>(userdata);
    if (event->window.windowID != self.window_ || self.drawing_ || self.failure_)
        return true;
    self.drawing_ = true;
    try
    {
        self.redraw_();
    } catch (...)
    {
        // No C++ exception may unwind through SDL / a native window procedure.
        // Propagate it at the outer event-loop boundary instead.
        self.failure_ = std::current_exception();
    }
    self.drawing_ = false;
    return true; // Observes events; never consumes native move/resize input.
}

void LiveResizeRedraw::rethrow_if_failed() const
{
    if (failure_)
        std::rethrow_exception(failure_);
}
} // namespace liltview
