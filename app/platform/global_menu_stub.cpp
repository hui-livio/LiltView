#include "global_menu.hpp"

namespace liltview
{
struct GlobalMenu::Impl
{
};
GlobalMenu::GlobalMenu(SDL_Window *, const MenuItem &)
{
}
GlobalMenu::~GlobalMenu() = default;
bool GlobalMenu::active()
{
    return false;
}
void GlobalMenu::update(const MenuItem &)
{
}
std::vector<GlobalMenuAction> GlobalMenu::take_actions()
{
    return {};
}
} // namespace liltview
