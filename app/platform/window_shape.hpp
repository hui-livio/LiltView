#ifndef LILTVIEW_WINDOW_SHAPE_HPP
#define LILTVIEW_WINDOW_SHAPE_HPP
#include <memory>
struct SDL_Window;
namespace liltview
{
// Native regions are O(radius), not full-window RGBA masks on Linux.
// Must be destroyed before SDL tears down its display connection.
class WindowShape
{
public:
    WindowShape();
    ~WindowShape();
    bool apply(SDL_Window *, float radius, bool opaque = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Unlike SDL's cached event position, query actual X11 client coordinates.
// Wayland does not expose global toplevel positioning; reuse its surface instead.
bool query_window_position(SDL_Window *, int &x, int &y);
bool position_window_client(SDL_Window *, int x, int y);
} // namespace liltview
#endif
