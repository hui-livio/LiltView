#include "platform/live_resize.hpp"

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <stdexcept>
#include <thread>

namespace
{
struct Events
{
    Events()
    {
        REQUIRE(SDL_Init(SDL_INIT_EVENTS));
    }
    ~Events()
    {
        SDL_QuitSubSystem(SDL_INIT_EVENTS);
    }
};

SDL_Event expose(SDL_WindowID window = 42, int live = 1)
{
    SDL_Event event{};
    event.type            = SDL_EVENT_WINDOW_EXPOSED;
    event.window.windowID = window;
    event.window.data1    = live;
    return event;
}
} // namespace

TEST_CASE("Live resize redraw is synchronous and leaves events queued", "[chrome][live-resize]")
{
    Events events;
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    int frames = 0;
    liltview::LiveResizeRedraw watch(42, [&] { ++frames; });
    auto event = expose();
    REQUIRE(SDL_PushEvent(&event));
    // No SDL_PollEvent / outer application frame has run.
    CHECK(frames == 1);
    SDL_Event queued{};
    REQUIRE(SDL_PeepEvents(&queued, 1, SDL_GETEVENT, SDL_EVENT_WINDOW_EXPOSED, SDL_EVENT_WINDOW_EXPOSED) == 1);
    CHECK(queued.window.windowID == 42);
    CHECK(queued.window.data1 == 1);
    watch.rethrow_if_failed();
}

TEST_CASE("Live resize redraw ignores other windows ordinary exposes and worker threads", "[chrome][live-resize]")
{
    Events events;
    int frames = 0;
    liltview::LiveResizeRedraw watch(42, [&] { ++frames; });
    auto other    = expose(43);
    auto ordinary = expose(42, 0);
    SDL_Event resize{};
    resize.type            = SDL_EVENT_WINDOW_RESIZED;
    resize.window.windowID = 42;
    REQUIRE(SDL_PushEvent(&other));
    REQUIRE(SDL_PushEvent(&ordinary));
    REQUIRE(SDL_PushEvent(&resize));
    bool pushed = false;
    std::thread worker(
        [&]
        {
            auto event = expose();
            pushed     = SDL_PushEvent(&event);
        });
    worker.join();
    REQUIRE(pushed);
    CHECK(frames == 0);
    auto event = expose();
    REQUIRE(SDL_PushEvent(&event));
    CHECK(frames == 1);
}

TEST_CASE("Live resize redraw cannot reenter and unregisters at the event pump boundary", "[chrome][live-resize]")
{
    Events events;
    int frames         = 0;
    bool nested_queued = false;
    auto event         = expose();
    {
        liltview::LiveResizeRedraw watch(42,
                                         [&]
                                         {
                                             ++frames;
                                             auto nested   = expose();
                                             nested_queued = SDL_PushEvent(&nested);
                                         });
        REQUIRE(SDL_PushEvent(&event));
        REQUIRE(nested_queued);
        CHECK(frames == 1);
        watch.rethrow_if_failed();
    }
    REQUIRE(SDL_PushEvent(&event));
    CHECK(frames == 1); // No callback remains during normal rendering/shutdown.
}

TEST_CASE("Live resize redraw defers callback exceptions outside SDL native dispatch", "[chrome][live-resize]")
{
    Events events;
    int calls = 0;
    liltview::LiveResizeRedraw watch(42,
                                     [&]
                                     {
                                         ++calls;
                                         throw std::runtime_error("resize draw failed");
                                     });
    auto event = expose();
    CHECK_NOTHROW(SDL_PushEvent(&event));
    CHECK(calls == 1);
    CHECK_THROWS_WITH(watch.rethrow_if_failed(), "resize draw failed");
    REQUIRE(SDL_PushEvent(&event));
    CHECK(calls == 1); // Do not render again with a partially failed frame.
}
