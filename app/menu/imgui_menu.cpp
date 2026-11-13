#include "imgui_menu.hpp"
#include <imgui.h>
namespace liltview
{
std::string menu_shortcut_label(const std::vector<std::string> &keys)
{
    std::string shortcut;
    for (const auto &key : keys)
    {
        if (!shortcut.empty())
            shortcut += "+";
        if (key == "Control")
            shortcut += "Ctrl";
        else if (key == "Up")
            shortcut += "↑";
        else if (key == "Down")
            shortcut += "↓";
        else if (key == "Left")
            shortcut += "←";
        else if (key == "Right")
            shortcut += "→";
        else if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
            shortcut += static_cast<char>(key[0] - 'a' + 'A');
        else
            shortcut += key;
    }
    return shortcut;
}
MenuAction draw_menu(const MenuItem &root)
{
    MenuAction result = MenuAction::none;
    for (const auto &item : root.children)
    {
        if (item.separator)
        {
            ImGui::Separator();
            continue;
        }
        const auto title = item.label + "###menu_" + std::to_string(item.id);
        if (!item.children.empty())
        {
            if (ImGui::BeginMenu(title.c_str(), item.enabled))
            {
                const auto selected = draw_menu(item);
                if (selected != MenuAction::none)
                    result = selected;
                ImGui::EndMenu();
            }
        }
        else
        {
            const auto shortcut = menu_shortcut_label(item.shortcut);
            if (ImGui::MenuItem(title.c_str(), shortcut.empty() ? nullptr : shortcut.c_str(), item.checked, item.enabled))
                result = item.action;
        }
    }
    return result;
}

} // namespace liltview
