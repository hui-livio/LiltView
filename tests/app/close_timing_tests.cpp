#include "ui/close_timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <string_view>

using namespace liltview;

namespace
{
struct Context
{
    ImGuiContext *value = ImGui::CreateContext();
    ~Context()
    {
        ImGui::DestroyContext(value);
    }
};

ImGuiInputEvent release(unsigned id)
{
    ImGuiInputEvent event;
    event.EventId            = id;
    event.Type               = ImGuiInputEventType_MouseButton;
    event.MouseButton.Button = ImGuiMouseButton_Left;
    event.MouseButton.Down   = false;
    return event;
}

ImGuiInputEvent key_down(unsigned id, ImGuiKey key)
{
    ImGuiInputEvent event;
    event.EventId  = id;
    event.Type     = ImGuiInputEventType_Key;
    event.Key.Key  = key;
    event.Key.Down = true;
    return event;
}

void record(UiCloseTiming &trace, ImGuiContext &context, const ImGuiInputEvent &event, std::uint64_t received,
            std::uint64_t handled)
{
    trace.begin_event(context);
    context.InputEventsQueue.push_back(event);
    trace.end_event(context, received, handled);
}

void consume(ImGuiContext &context, unsigned id)
{
    context.InputEventsTrail.clear();
    for (int i = 0; i < context.InputEventsQueue.Size; ++i)
        if (context.InputEventsQueue[i].EventId == id)
        {
            context.InputEventsTrail.push_back(context.InputEventsQueue[i]);
            context.InputEventsQueue.erase(context.InputEventsQueue.begin() + i);
            return;
        }
    FAIL("Unknown input EventId");
}
} // namespace

TEST_CASE("close latency rejects absent or incompatible clock samples", "[close][timing]")
{
    CHECK(std::abs(close_elapsed_ms(1000000, 2500000) - 1.5) < 1e-12);
    CHECK(close_elapsed_ms(1000000, 1000000) == 0);
    CHECK(close_elapsed_ms(0, 2500000) == -1);
    CHECK(close_elapsed_ms(2500000, 1000000) == -1);
}

TEST_CASE("window menu timing follows consumed EventIds instead of the latest SDL input", "[close][timing]")
{
    Context owner;
    auto &context = *owner.value;
    UiCloseTiming trace;
    record(trace, context, release(1), 1000000, 2000000);
    record(trace, context, release(2), 3000000, 4000000);
    consume(context, 1); // NewFrame deferred input 2 to a later frame.
    const auto first = trace.activation(context);
    CHECK(std::string_view(first.source) == "window_menu");
    CHECK(std::string_view(first.origin) == "sdl_mouse_release");
    CHECK(first.event_ns == 1000000);
    CHECK(first.handled_ns == 2000000);
    trace.discard_processed(context);
    consume(context, 2);
    const auto second = trace.activation(context);
    CHECK(second.event_ns == 3000000);
    CHECK(second.handled_ns == 4000000);
}

TEST_CASE("window menu keyboard activation retains its original SDL timing", "[close][timing]")
{
    Context owner;
    auto &context = *owner.value;
    UiCloseTiming trace;
    unsigned id = 1;
    for (const auto key : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space})
    {
        record(trace, context, key_down(id, key), 1000000, 2500000);
        consume(context, id++);
        const auto timing = trace.activation(context);
        CHECK(std::string_view(timing.origin) == "sdl_key_down");
        CHECK(timing.event_ns == 1000000);
        CHECK(timing.handled_ns == 2500000);
        trace.discard_processed(context);
    }
}

TEST_CASE("ambiguous missing and discarded menu inputs are unavailable rather than guessed", "[close][timing]")
{
    Context owner;
    auto &context = *owner.value;
    UiCloseTiming trace;
    CHECK(trace.activation(context).event_ns == 0);
    record(trace, context, release(1), 1000000, 2000000);
    record(trace, context, key_down(2, ImGuiKey_Enter), 3000000, 4000000);
    context.InputEventsTrail = context.InputEventsQueue;
    context.InputEventsQueue.clear();
    CHECK(trace.activation(context).event_ns == 0); // Two possible activators in one frame.
    trace.discard_processed(context);
    context.InputEventsTrail.clear();
    context.InputEventsTrail.push_back(release(1));
    CHECK(trace.activation(context).event_ns == 0); // Already discarded, never reuse stale timing.
    context.InputEventsTrail.clear();
    context.InputEventsTrail.push_back(release(3));
    CHECK(trace.activation(context).event_ns == 0); // No corresponding SDL sample.
    context.InputEventsTrail.clear();
    record(trace, context, key_down(4, ImGuiKey_DownArrow), 5000000, 6000000);
    consume(context, 4);
    CHECK(trace.activation(context).event_ns == 0); // Navigation is not menu activation.
}

TEST_CASE("diagnostic input storage is bounded while UI frames are suspended", "[close][timing]")
{
    Context owner;
    auto &context = *owner.value;
    UiCloseTiming trace;
    for (unsigned id = 1; id <= 65; ++id)
        record(trace, context, release(id), id * 1000000, id * 1000000 + 500000);
    consume(context, 1);
    CHECK(trace.activation(context).event_ns == 0); // Oldest sample evicted, not misattributed.
    consume(context, 65);
    CHECK(trace.activation(context).event_ns == 65000000);
}
