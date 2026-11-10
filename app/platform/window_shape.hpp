#ifndef LILTVIEW_WINDOW_SHAPE_HPP
#define LILTVIEW_WINDOW_SHAPE_HPP
#include <memory>
struct SDL_Window;
namespace liltview
{
// Native regions are O(radius), not full-window RGBA masks on Linux / Windows.
// Native frames clear custom regions so DWM can draw the non-client area.
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
} // namespace liltview
#endif
