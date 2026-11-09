#include "close_timing.hpp"

#include <algorithm>
#include <imgui.h>
#include <imgui_internal.h>

namespace liltview
{
namespace
{
const char *activation_origin(const ImGuiInputEvent &event)
{
    if (event.Type == ImGuiInputEventType_MouseButton && event.MouseButton.Button == ImGuiMouseButton_Left &&
        !event.MouseButton.Down)
        return "sdl_mouse_release";
    if (event.Type == ImGuiInputEventType_Key && event.Key.Down &&
        (event.Key.Key == ImGuiKey_Enter || event.Key.Key == ImGuiKey_KeypadEnter || event.Key.Key == ImGuiKey_Space))
        return "sdl_key_down";
    return nullptr;
}
} // namespace

void UiCloseTiming::begin_event(const ImGuiContext &context)
{
    first_input_ = context.InputEventsQueue.Size;
}

void UiCloseTiming::end_event(const ImGuiContext &context, std::uint64_t event_ns, std::uint64_t handled_ns)
{
    for (int i = first_input_; i < context.InputEventsQueue.Size; ++i)
    {
        const auto &event = context.InputEventsQueue[i];
        if (const auto *origin = activation_origin(event))
        {
            // Bound diagnostics even while the window is minimized/not rendering.
            if (inputs_.size() == 64)
                inputs_.erase(inputs_.begin());
            inputs_.push_back({event.EventId, {"window_menu", origin, event_ns, handled_ns}});
        }
    }
}

CloseTiming UiCloseTiming::activation(const ImGuiContext &context) const
{
    const CloseTiming unavailable{"window_menu", "unavailable"};
    const Input *selected = nullptr;
    for (const auto &event : context.InputEventsTrail)
    {
        if (!activation_origin(event))
            continue;
        const auto input =
            std::find_if(inputs_.begin(), inputs_.end(), [&](const Input &value) { return value.id == event.EventId; });
        // Missing samples or simultaneous activation candidates are ambiguous:
        // report unavailable instead of silently assigning another input's time.
        if (input == inputs_.end() || selected != nullptr)
            return unavailable;
        selected = &*input;
    }
    return selected ? selected->timing : unavailable;
}

void UiCloseTiming::discard_processed(const ImGuiContext &context)
{
    std::erase_if(inputs_,
                  [&](const Input &input)
                  {
                      return std::none_of(context.InputEventsQueue.begin(), context.InputEventsQueue.end(),
                                          [&](const ImGuiInputEvent &event) { return input.id == event.EventId; });
                  });
}
} // namespace liltview
