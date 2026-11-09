#ifndef LILTVIEW_GLOBAL_MENU_HPP
#define LILTVIEW_GLOBAL_MENU_HPP

#include "menu/menu_model.hpp"

#include <memory>
#include <vector>

struct SDL_Window;

namespace liltview
{
struct GlobalMenuAction
{
    MenuAction action = MenuAction::none;
    // SDL_GetTicksNS at application receipt of the D-Bus activation, not the
    // panel's timestamp (which is in a different clock domain).
    std::uint64_t received_ns = 0;
};

// Desktop-menu adapter. All public methods are called on the SDL/UI thread.
// The D-Bus worker only queues actions and wakes SDL; it never accesses ImGui.
class GlobalMenu
{
public:
    GlobalMenu(SDL_Window *window, const MenuItem &menu);
    ~GlobalMenu();
    GlobalMenu(const GlobalMenu &)            = delete;
    GlobalMenu &operator=(const GlobalMenu &) = delete;

    bool active();
    void update(const MenuItem &menu);
    std::vector<GlobalMenuAction> take_actions();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace liltview

#endif
