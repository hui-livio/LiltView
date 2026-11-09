// Reviewed UT scenarios before implementation:
// 1. No selection, scanning, directory errors, and shutdown never submit work.
// 2. First/last images schedule one neighbor, middle schedules two, duplicate paths only once.
// 3. Foreground work prevents new submissions; readiness permits low-priority work.
// 4. Repeated update/pump never duplicates running/completed/failed targets; cancellation retries need quiet edges.
// 5. BUSY with no ID can retry later; other synchronous failures count once without touching display state.
// 6. Generation/neighbor changes cancel and retire old IDs; active plus retired is always at most two.
// 7. Only the newest plan starts after retired terminals; old INFO/errors/completion cannot replace new metadata.
// 8. INFO becomes queryable only after successful COMPLETE; cache-hit statistics use source_bytes_read==0.
// 9. Foreign/main request IDs are never consumed; unexpected pixel events remain caller-owned and cause no upload.
// 10. Terminal processing releases a slot but never submits recursively before its event can be released.
// 11. Shutdown cancels once, accepts late terminal cleanup, and never submits again.
// 12. Static GIF first-frame metadata is reported honestly; it is not an animation session or playback cache.
// 13. Observed GIF format survives INFO->budget ERROR without claiming a successful cached result.
// 14. Observed format is only a current-plan content hint: retired/foreign IDs never overwrite it; refresh/close clear it.
// 15. Current cancellation retries only after a later foreground false->true edge, at most three accepted tries per target/plan.
// 16. Repeated idle pumps, ERROR, and retired terminals never grant retries; new generations reset accepted-try budgets.
// 17. BUSY during an eligible retry does not consume its bounded accepted-try budget or create an ID.

#include "viewer/prefetch.hpp"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace liltview;

namespace
{
DirectoryNavigation::Snapshot neighbors(const char *selected = "/images/2.png", const char *previous = "/images/1.png",
                                        const char *next = "/images/3.png")
{
    DirectoryNavigation::Snapshot value;
    value.selected_path = selected;
    if (previous)
    {
        value.previous_path = previous;
        value.can_previous  = true;
    }
    if (next)
    {
        value.next_path = next;
        value.can_next  = true;
    }
    return value;
}

lv_event_view terminal(lv_request_id id, lv_event_type type = LV_EVENT_COMPLETE, std::uint64_t bytes = 100)
{
    lv_event_view event{};
    event.type       = type;
    event.request_id = id;
    if (type == LV_EVENT_COMPLETE)
        event.as.completion.source_bytes_read = bytes;
    if (type == LV_EVENT_ERROR)
        event.as.error.code = LV_ERROR_MEMORY_BUDGET;
    return event;
}

lv_event_view information(lv_request_id id, lv_format format = LV_FORMAT_PNG, unsigned width = 37)
{
    lv_event_view event{};
    event.type                     = LV_EVENT_IMAGE_INFO;
    event.request_id               = id;
    event.as.image.format          = format;
    event.as.image.source_size     = {width, 29};
    event.as.image.canvas          = {width, 29};
    event.as.image.source_revision = width;
    return event;
}

struct Call
{
    std::filesystem::path path;
    std::uint64_t generation;
    lv_request_id id;
};

struct Fake
{
    lv_status submit(const std::filesystem::path &path, std::uint64_t generation, lv_request_id &id)
    {
        const auto status = responses.empty() ? static_cast<lv_status>(LV_OK) : responses.front();
        if (!responses.empty())
            responses.pop_front();
        id = status == LV_OK ? next++ : 0;
        calls.push_back({path, generation, id});
        return status;
    }
    PrefetchController controller()
    {
        return {[this](const auto &path, auto generation, auto &id) { return submit(path, generation, id); },
                [this](auto id) { cancelled.push_back(id); }};
    }
    lv_request_id next = 100;
    std::deque<lv_status> responses;
    std::vector<Call> calls;
    std::vector<lv_request_id> cancelled;
};
} // namespace

TEST_CASE("adjacent prefetch requires a usable directory and foreground readiness", "[prefetch]")
{
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        Fake fake;
        auto controller = fake.controller();
        auto value      = neighbors();
        if (mode == 0)
            value.selected_path.clear();
        if (mode == 1)
            value.scanning = true;
        if (mode == 2)
            value.error = std::make_error_code(std::errc::io_error);
        controller.update(1, value);
        controller.pump(true);
        CHECK(fake.calls.empty());
        CHECK_FALSE(controller.pending());
    }
    Fake fake;
    auto controller = fake.controller();
    controller.update(7, neighbors());
    CHECK(controller.pending());
    controller.pump(false);
    CHECK(fake.calls.empty());
    controller.pump(true);
    REQUIRE(fake.calls.size() == 2);
    CHECK(fake.calls[0].path == "/images/1.png");
    CHECK(fake.calls[1].path == "/images/3.png");
    CHECK(fake.calls[0].generation == 7);
    CHECK(controller.stats().active == 2);
    CHECK(controller.stats().submitted == 2);
    for (unsigned i = 0; i < 5; ++i)
    {
        controller.update(7, neighbors());
        controller.pump(true);
    }
    CHECK(fake.calls.size() == 2);
    CHECK(fake.cancelled.empty());
}

TEST_CASE("adjacent prefetch honors boundaries and deduplicates neighbor paths", "[prefetch]")
{
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        Fake fake;
        auto controller = fake.controller();
        auto value      = mode == 0 ? neighbors("/images/2.png", nullptr, "/images/3.png") :
            mode == 1               ? neighbors("/images/2.png", "/images/1.png", nullptr) :
            mode == 2               ? neighbors("/images/2.png", nullptr, nullptr) :
            mode == 3               ? neighbors("/images/2.png", "/images/1.png", "/images/1.png") :
                                      neighbors("/images/2.png", "/images/2.png", "/images/2.png");
        controller.update(1, value);
        controller.pump(true);
        const unsigned expected = mode == 2 || mode == 4 ? 0 : 1;
        CHECK(fake.calls.size() == expected);
        CHECK(controller.stats().active == expected);
    }
}

TEST_CASE("BUSY prefetch admission retries later while other synchronous failures are attempted once", "[prefetch]")
{
    Fake fake;
    fake.responses  = {LV_BUSY, LV_OK, LV_OUT_OF_MEMORY};
    auto controller = fake.controller();
    controller.update(1, neighbors());
    controller.pump(true);
    REQUIRE(fake.calls.size() == 1);
    CHECK(controller.stats().active == 0);
    CHECK(controller.stats().submitted == 0);
    CHECK(controller.stats().failed == 0);
    CHECK(controller.pending());
    controller.pump(false);
    CHECK(fake.calls.size() == 1);
    controller.pump(true);
    REQUIRE(fake.calls.size() == 3);
    CHECK(fake.calls[1].path == fake.calls[0].path);
    CHECK(controller.stats().active == 1);
    CHECK(controller.stats().submitted == 1);
    CHECK(controller.stats().failed == 1);
    CHECK(controller.consume(terminal(fake.calls[1].id)));
    CHECK_FALSE(controller.pending());
    controller.pump(true);
    CHECK(fake.calls.size() == 3);
}

TEST_CASE("successful prefetch metadata and cache hits belong only to completed current targets", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(9, neighbors());
    controller.pump(true);
    const auto first = fake.calls[0].id, second = fake.calls[1].id;
    CHECK_FALSE(controller.prefetched_info(fake.calls[0].path));
    CHECK_FALSE(controller.observed_format(fake.calls[0].path));
    CHECK(controller.consume(information(first)));
    CHECK_FALSE(controller.prefetched_info(fake.calls[0].path));
    CHECK(controller.observed_format(fake.calls[0].path) == LV_FORMAT_PNG);
    CHECK(controller.consume(terminal(first, LV_EVENT_COMPLETE, 0)));
    const auto info = controller.prefetched_info(fake.calls[0].path);
    REQUIRE(info);
    CHECK(info->format == LV_FORMAT_PNG);
    CHECK(info->source_size.width == 37);
    CHECK(controller.observed_format(fake.calls[0].path) == LV_FORMAT_PNG);
    CHECK_FALSE(controller.prefetched_info("/unrelated.png"));
    CHECK(controller.stats().completed == 1);
    CHECK(controller.stats().cache_hits == 1);
    CHECK(controller.stats().active == 1);
    CHECK(controller.consume(terminal(second, LV_EVENT_ERROR)));
    CHECK(controller.stats().failed == 1);
    CHECK_FALSE(controller.prefetched_info(fake.calls[1].path));
    CHECK_FALSE(controller.pending());
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    CHECK_FALSE(controller.owns(first));
    CHECK_FALSE(controller.consume(terminal(first)));
}

TEST_CASE("old prefetch IDs remain bounded and recognized until their terminal events", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors());
    controller.pump(true);
    const auto old_first = fake.calls[0].id, old_second = fake.calls[1].id;
    controller.update(2, neighbors("/new/2.png", "/new/1.png", "/new/3.png"));
    controller.update(3, neighbors("/latest/2.png", "/latest/1.png", "/latest/3.png"));
    controller.pump(true);
    CHECK(fake.cancelled == std::vector<lv_request_id>{old_first, old_second});
    CHECK(fake.calls.size() == 2);
    CHECK(controller.stats().active == 2);
    CHECK(controller.stats().retired == 2);
    CHECK(controller.owns(old_first));
    CHECK(controller.owns(old_second));
    CHECK(controller.consume(information(old_first, LV_FORMAT_GIF)));
    CHECK(controller.consume(terminal(old_first)));
    CHECK_FALSE(controller.prefetched_info("/images/1.png"));
    CHECK_FALSE(controller.prefetched_info("/latest/1.png"));
    CHECK(fake.calls.size() == 2); // consume never submits before its event is released by the owner.
    controller.pump(false);
    CHECK(fake.calls.size() == 2);
    controller.pump(true);
    REQUIRE(fake.calls.size() == 3);
    CHECK(fake.calls.back().path == "/latest/1.png");
    CHECK(fake.calls.back().generation == 3);
    CHECK(controller.stats().active == 2);
    CHECK(controller.consume(terminal(old_second, LV_EVENT_ERROR)));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 4);
    CHECK(fake.calls.back().path == "/latest/3.png");
    CHECK(controller.stats().active == 2);
    CHECK(controller.stats().retired == 0);
    CHECK_FALSE(controller.prefetched_info("/latest/3.png"));
}

TEST_CASE("directory refresh invalidates plans but identical snapshots do not restart work", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    auto value      = neighbors();
    controller.update(1, value);
    controller.pump(true);
    const auto first = fake.calls[0].id, second = fake.calls[1].id;
    controller.consume(information(first));
    controller.consume(terminal(first));
    REQUIRE(controller.prefetched_info("/images/1.png"));
    controller.update(1, value);
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    REQUIRE(controller.prefetched_info("/images/1.png"));
    value.scanning = true;
    controller.update(1, value);
    CHECK_FALSE(controller.prefetched_info("/images/1.png"));
    CHECK(fake.cancelled == std::vector<lv_request_id>{second});
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    controller.consume(terminal(second, LV_EVENT_CANCELLED));
    CHECK_FALSE(controller.pending());
    value.scanning = false;
    value.error    = std::make_error_code(std::errc::io_error);
    controller.update(1, value);
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    value.error.clear();
    controller.update(1, value);
    controller.pump(true);
    CHECK(fake.calls.size() == 4);
}

TEST_CASE("foreign foreground events and unexpected pixels cannot become prefetch display state", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors());
    controller.pump(true);
    CHECK_FALSE(controller.owns(0));
    CHECK_FALSE(controller.owns(999));
    CHECK_FALSE(controller.consume(information(999)));
    CHECK_FALSE(controller.consume(terminal(999, LV_EVENT_ERROR)));
    CHECK(controller.stats().failed == 0);
    CHECK(controller.stats().active == 2);
    lv_event_view pixels{};
    pixels.type       = LV_EVENT_REGION_READY;
    pixels.request_id = fake.calls[0].id;
    CHECK(controller.consume(pixels)); // Ownership remains with the caller; the controller has no upload callback.
    CHECK(controller.stats().active == 2);
    CHECK_FALSE(controller.prefetched_info(fake.calls[0].path));
    CHECK(controller.consume(terminal(fake.calls[0].id, LV_EVENT_CANCELLED)));
    CHECK(controller.stats().cancelled == 1);
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
}

TEST_CASE("prefetch shutdown cancels once and only cleans up late events", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors());
    controller.pump(true);
    const auto first = fake.calls[0].id, second = fake.calls[1].id;
    controller.begin_shutdown();
    controller.begin_shutdown();
    CHECK(fake.cancelled == std::vector<lv_request_id>{first, second});
    CHECK(controller.stats().active == 2);
    CHECK(controller.stats().retired == 2);
    CHECK(controller.pending());
    controller.update(2, neighbors("/other/2.png", "/other/1.png", "/other/3.png"));
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    CHECK(controller.consume(information(first)));
    CHECK(controller.consume(terminal(first, LV_EVENT_COMPLETE, 0)));
    CHECK_FALSE(controller.prefetched_info("/images/1.png"));
    CHECK(controller.consume(terminal(second, LV_EVENT_CANCELLED)));
    CHECK_FALSE(controller.pending());
    controller.pump(true);
    CHECK(fake.calls.size() == 2);
    CHECK(controller.stats().active == 0);

    Fake empty_fake;
    auto empty = empty_fake.controller();
    empty.begin_shutdown();
    empty.update(3, neighbors());
    empty.pump(true);
    CHECK(empty_fake.calls.empty());
    CHECK_FALSE(empty.pending());
}

TEST_CASE("static GIF prefetch reports first-frame image metadata without fabricating playback", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors("/images/2.png", "/images/1.gif", nullptr));
    controller.pump(true);
    const auto id = fake.calls.front().id;
    controller.consume(information(id, LV_FORMAT_GIF));
    CHECK_FALSE(controller.prefetched_info("/images/1.gif"));
    controller.consume(terminal(id));
    const auto info = controller.prefetched_info("/images/1.gif");
    REQUIRE(info);
    CHECK(info->format == LV_FORMAT_GIF);
    CHECK_FALSE(controller.pending());
    CHECK(controller.stats().completed == 1);
}

TEST_CASE("observed GIF format survives a failed prefetch without implying cached pixels", "[prefetch]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors("/images/2.png", "/images/1.data", nullptr));
    controller.pump(true);
    const auto id = fake.calls.front().id;
    CHECK_FALSE(controller.observed_format("/images/1.data"));
    CHECK(controller.consume(information(id, LV_FORMAT_GIF)));
    CHECK(controller.observed_format("/images/1.data") == LV_FORMAT_GIF);
    CHECK_FALSE(controller.prefetched_info("/images/1.data"));
    CHECK(controller.consume(terminal(id, LV_EVENT_ERROR)));
    CHECK(controller.observed_format("/images/1.data") == LV_FORMAT_GIF);
    CHECK_FALSE(controller.prefetched_info("/images/1.data"));
    CHECK_FALSE(controller.observed_format("/images/other.gif"));
    CHECK(controller.stats().completed == 0);
    CHECK(controller.stats().failed == 1);
    CHECK_FALSE(controller.pending());
    controller.pump(true);
    CHECK(fake.calls.size() == 1);
}

TEST_CASE("format observations never cross plans or accept retired and foreign metadata", "[prefetch]")
{
    Fake fake;
    auto controller  = fake.controller();
    const auto value = neighbors("/images/2.png", "/images/1.data", nullptr);
    controller.update(1, value);
    controller.pump(true);
    const auto old = fake.calls.front().id;
    CHECK(controller.consume(information(old, LV_FORMAT_GIF)));
    REQUIRE(controller.observed_format("/images/1.data") == LV_FORMAT_GIF);
    controller.update(2, value); // Same path, new generation: the observation is not a source/version anchor.
    CHECK_FALSE(controller.observed_format("/images/1.data"));
    CHECK(controller.consume(information(old, LV_FORMAT_GIF)));
    CHECK_FALSE(controller.observed_format("/images/1.data"));
    CHECK(controller.consume(terminal(old)));
    CHECK_FALSE(controller.prefetched_info("/images/1.data"));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 2);
    const auto current = fake.calls.back().id;
    CHECK_FALSE(controller.consume(information(9999, LV_FORMAT_GIF)));
    CHECK(controller.consume(information(current, LV_FORMAT_PNG)));
    CHECK(controller.observed_format("/images/1.data") == LV_FORMAT_PNG);
    CHECK_FALSE(controller.consume(information(old, LV_FORMAT_GIF)));
    CHECK(controller.observed_format("/images/1.data") == LV_FORMAT_PNG);
    CHECK(controller.consume(terminal(current)));
    REQUIRE(controller.prefetched_info("/images/1.data"));
    auto scanning     = value;
    scanning.scanning = true;
    controller.update(2, scanning);
    CHECK_FALSE(controller.observed_format("/images/1.data"));
    controller.update(2, value);
    controller.pump(true);
    CHECK(controller.consume(information(fake.calls.back().id, LV_FORMAT_GIF)));
    REQUIRE(controller.observed_format("/images/1.data") == LV_FORMAT_GIF);
    controller.begin_shutdown();
    CHECK_FALSE(controller.observed_format("/images/1.data"));
}

TEST_CASE("background callback failures cannot escape into foreground error handling", "[prefetch]")
{
    unsigned attempts = 0;
    PrefetchController controller(
        [&](const auto &, auto, auto &) -> lv_status
        {
            ++attempts;
            throw std::runtime_error("background submit failure");
        },
        [](auto) { throw std::runtime_error("background cancel failure"); });
    controller.update(1, neighbors());
    CHECK_NOTHROW(controller.pump(true));
    CHECK(attempts == 2);
    CHECK(controller.stats().failed == 2);
    CHECK_FALSE(controller.pending());
    controller.pump(true);
    CHECK(attempts == 2);

    lv_request_id assigned = 21;
    PrefetchController malformed(
        [&](const auto &, auto, auto &out)
        {
            out = assigned++;
            return LV_OUT_OF_MEMORY;
        },
        [](auto) { throw std::runtime_error("cancel failure"); });
    malformed.update(1, neighbors());
    CHECK_NOTHROW(malformed.pump(true));
    CHECK(malformed.stats().active == 2);
    CHECK(malformed.stats().retired == 2);
    CHECK(malformed.owns(21));
    CHECK(malformed.owns(22));
    CHECK_NOTHROW(malformed.begin_shutdown());
}

TEST_CASE("cancelled neighbor prefetch retries only on quiet edges with a per-plan cap", "[prefetch][retry]")
{
    Fake fake;
    auto controller  = fake.controller();
    const auto value = neighbors("/images/2.png", "/images/1.png", nullptr);
    controller.update(1, value);
    controller.pump(true);
    REQUIRE(fake.calls.size() == 1);
    CHECK(controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED)));
    CHECK(controller.pending());
    for (unsigned i = 0; i < 5; ++i)
        controller.pump(true); // No observed busy phase: this is not a new quiet opportunity.
    CHECK(fake.calls.size() == 1);
    controller.pump(false);
    controller.pump(true);
    REQUIRE(fake.calls.size() == 2);
    CHECK(controller.stats().submitted == 2);
    controller.pump(false);
    CHECK(controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED)));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 3);
    CHECK(controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED)));
    CHECK_FALSE(controller.pending());
    for (unsigned i = 0; i < 5; ++i)
    {
        controller.pump(false);
        controller.pump(true);
    }
    CHECK(fake.calls.size() == 3);
    CHECK(controller.stats().submitted == 3);
    CHECK(controller.stats().cancelled == 3);
    CHECK(controller.stats().active == 0);

    controller.update(2, value); // Same path in a new generation receives a fresh attempt budget.
    controller.pump(true);
    REQUIRE(fake.calls.size() == 4);
    CHECK(fake.calls.back().generation == 2);
    controller.pump(false);
    controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED));
    controller.pump(true);
    CHECK(fake.calls.size() == 5);
}

TEST_CASE("prefetch errors and retired cancellations do not grant quiet-edge retries", "[prefetch][retry]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors("/images/2.png", "/images/1.png", nullptr));
    controller.pump(true);
    const auto errored = fake.calls.front().id;
    controller.pump(false);
    controller.consume(terminal(errored, LV_EVENT_ERROR));
    controller.pump(true);
    CHECK(fake.calls.size() == 1);
    CHECK_FALSE(controller.pending());
    controller.update(2, neighbors("/old/2.png", "/old/1.png", nullptr));
    controller.pump(true);
    const auto retired = fake.calls.back().id;
    controller.update(3, neighbors("/new/2.png", "/new/1.png", nullptr));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 3);
    const auto current = fake.calls.back().id;
    controller.consume(information(retired, LV_FORMAT_GIF));
    controller.pump(false);
    controller.consume(terminal(retired, LV_EVENT_CANCELLED));
    controller.pump(true);
    CHECK(fake.calls.size() == 3);
    CHECK_FALSE(controller.observed_format("/new/1.png"));
    controller.consume(terminal(current));
    for (unsigned i = 0; i < 3; ++i)
    {
        controller.pump(false);
        controller.pump(true);
    }
    CHECK(fake.calls.size() == 3);
    CHECK_FALSE(controller.pending());
}

TEST_CASE("each adjacent target has its own bounded cancellation retry budget", "[prefetch][retry]")
{
    Fake fake;
    auto controller = fake.controller();
    controller.update(1, neighbors());
    controller.pump(true);
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        REQUIRE(fake.calls.size() == (attempt + 1) * 2);
        const auto first = fake.calls[attempt * 2].id, second = fake.calls[attempt * 2 + 1].id;
        controller.pump(false);
        controller.consume(terminal(first, LV_EVENT_CANCELLED));
        controller.consume(terminal(second, LV_EVENT_CANCELLED));
        controller.pump(true);
        CHECK(controller.stats().active <= 2);
    }
    CHECK(fake.calls.size() == 6);
    CHECK(controller.stats().submitted == 6);
    CHECK(controller.stats().cancelled == 6);
    CHECK_FALSE(controller.pending());
}

TEST_CASE("BUSY does not consume an eligible prefetch retry attempt", "[prefetch][retry]")
{
    Fake fake;
    fake.responses  = {LV_OK, LV_BUSY, LV_OK, LV_BUSY, LV_OK};
    auto controller = fake.controller();
    controller.update(1, neighbors("/images/2.png", "/images/1.png", nullptr));
    controller.pump(true);
    controller.pump(false);
    controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 2);
    CHECK(fake.calls.back().id == 0);
    CHECK(controller.stats().submitted == 1);
    CHECK(controller.pending());
    controller.pump(true);
    REQUIRE(fake.calls.size() == 3);
    CHECK(controller.stats().submitted == 2);
    controller.pump(false);
    controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED));
    controller.pump(true);
    REQUIRE(fake.calls.size() == 4);
    CHECK(fake.calls.back().id == 0);
    controller.pump(true);
    REQUIRE(fake.calls.size() == 5);
    CHECK(controller.stats().submitted == 3);
    controller.pump(false);
    controller.consume(terminal(fake.calls.back().id, LV_EVENT_CANCELLED));
    controller.pump(true);
    CHECK(fake.calls.size() == 5);
    CHECK_FALSE(controller.pending());
}
