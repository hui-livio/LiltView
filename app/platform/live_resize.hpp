#ifndef LILTVIEW_LIVE_RESIZE_HPP
#define LILTVIEW_LIVE_RESIZE_HPP

#include <SDL3/SDL_events.h>
#include <exception>
#include <functional>

namespace liltview
{
// Scope this watch to event pumping, with no ImGui frame open. SDL sends live
// expose events from inside native modal resize loops, before polling returns.
// Redraw only: callbacks must not join workers that may be posting SDL events.
class LiveResizeRedraw
{
public:
    LiveResizeRedraw(SDL_WindowID window, std::function<void()> redraw);
    ~LiveResizeRedraw();
    LiveResizeRedraw(const LiveResizeRedraw &)            = delete;
    LiveResizeRedraw &operator=(const LiveResizeRedraw &) = delete;
    void rethrow_if_failed() const;

private:
    static bool SDLCALL watch(void *userdata, SDL_Event *event);
    SDL_WindowID window_;
    std::function<void()> redraw_;
    std::exception_ptr failure_;
    bool drawing_ = false;
};
} // namespace liltview
#endif
