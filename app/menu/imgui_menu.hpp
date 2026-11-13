#ifndef LILTVIEW_IMGUI_MENU_HPP
#define LILTVIEW_IMGUI_MENU_HPP
#include "menu_model.hpp"
namespace liltview
{
std::string menu_shortcut_label(const std::vector<std::string> &keys);
MenuAction draw_menu(const MenuItem &root);
} // namespace liltview
#endif
