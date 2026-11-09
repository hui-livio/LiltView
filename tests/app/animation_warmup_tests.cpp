// API / UT design recorded before implementation for independent review.
// Use the real ABI 2 Core and red/blue GIF fixtures, no renderer or GL calls.
//
// 1. Warm OPEN -> NEXT(frame 0) -> complete tiles -> COMPLETE -> released event
//    -> READY. Never advance to READY/NEXT inside consume(), never upload to GPU.
// 2. Exact READY adoption submits foreground CURRENT NEXT, validating the file
//    version. Only its released successful terminal yields frame 0 plus the real
//    frame 1 (same session/epoch, indices 0/1), with original pixels and duration.
// 3. One-frame GIF validation reaches END; transfer first once, ended=true and
//    final known metadata. No RESET/relabeling, repeated frame 0 or fabricated 1.
// 4. Modify/replace/delete the ready source or redirect its symlink before
//    adoption: no stale first frame, one fallback failure, session closed.
// 5. Changed FIT bounds never adopt an incompatible ready result; new generation
//    retires obsolete work/adoptions and cannot leak an old failure into the new.
// 6. interrupt_background cancels/closes only unfinished warm jobs, preserves
//    READY, and keeps cancelled IDs tracked until terminal release/pump.
// 7. Busy request/session slots retry after real cleanup/wakeup; no fake READY,
//    no spin loop and no more than two globally quota-permitted warm IDs.
// 8. Foreground=false permits no new warm work, while pending foreground adoption
//    can progress. Background jobs use VISIBLE_DETAIL; verification uses CURRENT.
// 9. Retained budget bills the entire frame backing from the first partial tile,
//    plus bounded descriptor capacity, and includes both first and validation
//    second frames. Exhaustion closes/falls back and releases every pixel ref.
// 10. Foreign request IDs do not get consumed. Wrong frame/session/epoch, missing
//     coverage or unexpected terminals cannot yield an adoption.
// 11. Fast generation churn, cancellation, and begin_shutdown release all retained
//     references. A moved PreparedAnimation owns pixels/session independently;
//     release_session transfers it without destructor closing the displayed one.
// 12. PreparedAnimation destruction closes an unclaimed session. Retained pixels
//     remain valid through session close; explicit module shutdown before Core
//     destroy prevents stale borrowed Core calls or outside-reference leaks.
// 13. A target/plan accepts at most three warm OPEN jobs interrupted by foreground
//     work. BUSY attempts do not consume this limit, ERROR does not retry, and a
//     changed generation/bounds/neighbor plan starts a fresh admission allowance.
// 14. Unclassified real PNG and damaged candidates are classified by Core OPEN;
//     their errors release reservations and never create READY/adoption or a
//     foreground fallback failure. No filename-based classifier is introduced.
// 15. pending includes unsubmitted/BUSY phases, adoption/result and retired IDs;
//     only READY/failed/exhausted targets are idle. Shutdown uses Core join,
//     since Core shutdown may suppress the terminals that clear tracked IDs.

#include "../core/support.hpp"
#include "viewer/animation_warmup.hpp"

#include <functional>
#include <gif_lib.h>
#include <iterator>

using namespace liltview;
using namespace liltview::test;

namespace
{
std::vector<unsigned char> gif(unsigned frames = 2, unsigned width = 4, unsigned height = 3, int repeat = -1)
{
    std::vector<unsigned char> bytes;
    int error     = 0;
    auto *encoder = EGifOpen(
        &bytes,
        [](GifFileType *file, const GifByteType *data, int size)
        {
            auto &output = *static_cast<std::vector<unsigned char> *>(file->UserData);
            output.insert(output.end(), data, data + size);
            return size;
        },
        &error);
    REQUIRE(encoder);
    EGifSetGifVersion(encoder, true);
    const GifColorType colors[]{{255, 0, 0}, {0, 0, 255}};
    auto *map = GifMakeMapObject(2, colors);
    REQUIRE(map);
    REQUIRE(EGifPutScreenDesc(encoder, static_cast<int>(width), static_cast<int>(height), 8, 0, map) == GIF_OK);
    if (repeat >= 0)
    {
        const unsigned char loop[]{1, static_cast<unsigned char>(repeat), static_cast<unsigned char>(repeat >> 8)};
        REQUIRE(EGifPutExtensionLeader(encoder, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(encoder, 11, "NETSCAPE2.0") == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(encoder, 3, loop) == GIF_OK);
        REQUIRE(EGifPutExtensionTrailer(encoder) == GIF_OK);
    }
    for (unsigned frame = 0; frame < frames; ++frame)
    {
        GraphicsControlBlock control{};
        control.DisposalMode     = DISPOSE_DO_NOT;
        control.DelayTime        = static_cast<int>(3 + frame);
        control.TransparentColor = NO_TRANSPARENT_COLOR;
        GifByteType block[4];
        REQUIRE(EGifGCBToExtension(&control, block) == 4);
        REQUIRE(EGifPutExtension(encoder, GRAPHICS_EXT_FUNC_CODE, 4, block) == GIF_OK);
        REQUIRE(EGifPutImageDesc(encoder, 0, 0, static_cast<int>(width), static_cast<int>(height), false, nullptr) == GIF_OK);
        std::vector<GifPixelType> row(width, static_cast<GifPixelType>(frame % 2));
        for (unsigned y = 0; y < height; ++y)
            REQUIRE(EGifPutLine(encoder, row.data(), static_cast<int>(width)) == GIF_OK);
    }
    REQUIRE(EGifCloseFile(encoder, &error) == GIF_OK);
    GifFreeMapObject(map);
    return bytes;
}

struct Driver
{
    Engine &engine;
    AnimationWarmup &warm;
    bool foreground_busy = false;
    std::size_t quota    = 2;

    void cycle(bool idle = true, std::size_t slots = 2, const std::function<void(const lv_event_view &)> &observe = {})
    {
        const auto available = [&]
        {
            return std::min(slots, quota - std::min(quota, warm.stats().outstanding_requests));
        };
        warm.pump(idle && !foreground_busy, available());
        lv_event *raw = nullptr;
        while (lv_core_poll_event(engine.core, &raw) == LV_OK)
        {
            Event event(raw);
            REQUIRE(warm.owns(event.view().request_id));
            REQUIRE(warm.consume(event.view()));
            if (observe)
                observe(event.view());
        }
        warm.pump(idle && !foreground_busy, available()); // Every event owner above has released its terminal.
        CHECK(warm.stats().outstanding_requests <= quota);
    }

    void until(const std::function<bool()> &done, bool idle = true,
               const std::function<void(const lv_event_view &)> &observe = {})
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done())
        {
            std::unique_lock lock(engine.mutex);
            const auto before = engine.notifications;
            lock.unlock();
            cycle(idle, 2, observe);
            if (done())
                return;
            lock.lock();
            engine.changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(50)),
                                      [&] { return engine.notifications != before; });
            REQUIRE(std::chrono::steady_clock::now() < deadline);
        }
    }

    void ready(const std::filesystem::path &path, lv_size_u32 bounds = {100, 100}, std::uint64_t generation = 1)
    {
        warm.update(generation, {path}, bounds);
        until([&] { return warm.stats().ready == 1; });
    }

    std::optional<PreparedAnimation> adopt(const std::filesystem::path &path, lv_size_u32 bounds = {100, 100},
                                           std::uint64_t tag = 2)
    {
        REQUIRE(warm.request_adoption(path, bounds, tag));
        CHECK(warm.pending());
        warm.update(tag, {}, bounds); // Current selection is no longer its own neighbor.
        std::optional<PreparedAnimation> prepared;
        until(
            [&]
            {
                prepared = warm.take_adoption();
                return prepared.has_value() || warm.stats().failed != 0;
            },
            false);
        return prepared;
    }
};

void color(const PreparedAnimationFrame &frame, unsigned red, unsigned blue)
{
    REQUIRE_FALSE(frame.tiles.empty());
    for (const auto &tile : frame.tiles)
    {
        const auto *pixels = lv_pixels_get_view(tile.region.pixels);
        REQUIRE(pixels);
        for (unsigned y = 0; y < pixels->size.height; ++y)
            for (unsigned x = 0; x < pixels->size.width; ++x)
            {
                const auto *p = pixels->data + y * pixels->stride_bytes + x * 4;
                CHECK(p[0] == red);
                CHECK(p[1] == 0);
                CHECK(p[2] == blue);
                CHECK(p[3] == 255);
            }
    }
}

lv_animation_id open_foreign(Engine &engine, const std::string &path, lv_request_id &request)
{
    lv_animation_open_desc desc;
    lv_animation_open_desc_init(&desc);
    desc.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    desc.target.as.fit_bounds = {100, 100};
    lv_animation_id session   = 0;
    REQUIRE(lv_core_open_animation(engine.core, &desc, &session, &request) == LV_OK);
    return session;
}

bool next_is_end(Engine &engine, lv_animation_id session)
{
    lv_animation_request_desc next;
    lv_animation_request_desc_init(&next);
    next.animation_id = session;
    lv_request_id id  = 0;
    REQUIRE(lv_core_submit_animation(engine.core, &next, &id) == LV_OK);
    bool ended = false;
    for (;;)
    {
        Event event(engine.next());
        REQUIRE(event.view().request_id == id);
        if (event.view().type == LV_EVENT_ANIMATION_END)
            ended = true;
        else if (event.view().type == LV_EVENT_COMPLETE)
            return ended;
        else
            REQUIRE(event.view().type != LV_EVENT_ERROR);
    }
}

void mutate_same_length(const std::filesystem::path &path, std::vector<unsigned char> bytes)
{
    const auto size = std::filesystem::file_size(path);
    const auto time = std::filesystem::last_write_time(path);
    REQUIRE(bytes.size() == size);
    REQUIRE(bytes.size() > 13);
    bytes[13] ^= 0x7f; // First global-palette red component; dimensions and encoded length unchanged.
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(output.good());
    }
    std::filesystem::last_write_time(path, time + std::chrono::seconds(1)); // Deterministic version change, no sleep.
    CHECK(std::filesystem::file_size(path) == size);
}

void enable_cache(Engine &engine)
{
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    REQUIRE(lv_core_destroy(&engine.core) == LV_OK);
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes       = 64ULL * 1024 * 1024;
    config.cache_budget_bytes     = 4ULL * 1024 * 1024;
    config.queue_pixel_bytes      = 16 * 16 * 4;
    config.tile_edge              = 16;
    config.max_requests           = 6;
    config.animation.max_sessions = 3;
    config.wakeup                 = Engine::wake;
    config.wakeup_userdata        = &engine;
    REQUIRE(lv_core_create(&config, &engine.core) == LV_OK);
}
} // namespace

TEST_CASE("GIF warmup retains frame zero only after complete coverage and released terminal", "[animation_warmup]")
{
    Files files;
    const auto path = files.write("content-is-gif.wrong", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    CHECK_FALSE(warm.pending());
    warm.update(1, {path}, {100, 100});
    CHECK(warm.pending());
    bool checked_terminal = false;
    driver.until([&] { return warm.stats().ready == 1; }, true,
                 [&](const auto &view)
                 {
                     if (view.type == LV_EVENT_COMPLETE && view.as.completion.final_tile_count)
                     {
                         checked_terminal = true;
                         CHECK(warm.stats().ready == 0);
                         CHECK(warm.stats().outstanding_requests == 1);
                         CHECK(warm.pending());
                         CHECK_FALSE(warm.take_adoption());
                     }
                 });
    CHECK(checked_terminal);
    CHECK(warm.stats().completed == 1);
    CHECK(warm.stats().retained_bytes >= 4 * 3 * 4);
    CHECK(warm.stats().outstanding_requests == 0);
    CHECK_FALSE(warm.pending());
    warm.interrupt_background();
    CHECK(warm.stats().ready == 1);
}

TEST_CASE("GIF adoption validates with real NEXT and transfers red zero then blue one without resetting epoch",
          "[animation_warmup][adoption]")
{
    Files files;
    const auto path = files.write("two.gif", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    auto prepared = driver.adopt(path);
    REQUIRE(prepared);
    REQUIRE(prepared->second);
    CHECK_FALSE(prepared->ended);
    CHECK(prepared->first.info.key.animation_id == prepared->session);
    CHECK(prepared->first.info.key.epoch == 1);
    CHECK(prepared->first.info.key.frame_index == 0);
    CHECK(prepared->second->info.key.animation_id == prepared->session);
    CHECK(prepared->second->info.key.epoch == 1);
    CHECK(prepared->second->info.key.frame_index == 1);
    CHECK(prepared->first.info.source_duration_ns == 30000000);
    CHECK(prepared->second->info.source_duration_ns == 40000000);
    color(prepared->first, 255, 0);
    color(*prepared->second, 0, 255);
    CHECK(warm.stats().ready == 0);
    CHECK(warm.stats().retained_bytes == 0);
    warm.begin_shutdown();
    const auto session = prepared->release_session();
    REQUIRE(next_is_end(engine, session)); // Decoder already consumed real frame 1.
    CHECK(lv_core_close_animation(engine.core, session) == LV_OK);
    prepared.reset();
    lv_core_stats stats{};
    REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.external_pixel_refs == 0);
}

TEST_CASE("single-frame finite GIF adoption returns END metadata without inventing a second frame", "[animation_warmup][end]")
{
    Files files;
    const auto path = files.write("one.gif", gif(1));
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    auto prepared = driver.adopt(path);
    REQUIRE(prepared);
    CHECK(prepared->ended);
    CHECK_FALSE(prepared->second);
    CHECK(prepared->first.info.key.frame_index == 0);
    CHECK(prepared->info.flags & LV_ANIMATION_FRAME_COUNT_KNOWN);
    CHECK(prepared->info.flags & LV_ANIMATION_DURATION_KNOWN);
    CHECK(prepared->info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN);
    CHECK(prepared->info.frame_count == 1);
    CHECK(prepared->info.total_plays == 1);
    CHECK(prepared->info.duration_ns == 30000000);
    color(prepared->first, 255, 0);
}

TEST_CASE("ready GIF mutation replacement deletion and symlink redirection always fall back before showing stale zero",
          "[animation_warmup][source]")
{
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        CAPTURE(mode);
        Files files;
        const auto source    = files.write("source", gif());
        const auto alternate = files.write("alternate", gif(1));
        auto path            = std::filesystem::path(source);
        if (mode == 3)
        {
            path = files.directory / "alias";
            std::filesystem::create_symlink(source, path);
        }
        Engine engine(6, 64 * 1024 * 1024, 3);
        AnimationWarmup warm(engine.core);
        Driver driver{engine, warm};
        driver.ready(path);
        if (mode == 0)
            std::ofstream(source, std::ios::binary | std::ios::app).put('\0');
        else if (mode == 1)
            std::filesystem::rename(alternate, path);
        else if (mode == 2)
            std::filesystem::remove(path);
        else if (mode == 3)
        {
            std::filesystem::remove(path);
            std::filesystem::create_symlink(alternate, path);
        }
        else
            mutate_same_length(path, gif());
        REQUIRE(warm.request_adoption(path, {100, 100}, 2));
        warm.update(2, {}, {100, 100});
        driver.until([&] { return warm.stats().failed && !warm.stats().outstanding_requests; }, false);
        CHECK_FALSE(warm.take_adoption());
        CHECK(warm.take_adoption_failure());
        CHECK_FALSE(warm.take_adoption_failure());
        CHECK(warm.stats().ready == 0);
        CHECK(warm.stats().retained_bytes == 0);
        lv_core_stats stats{};
        REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
        CHECK(stats.external_pixel_refs == 0);
    }
}

TEST_CASE("GIF warmup rejects mismatched bounds and preserves a matching new-generation adoption",
          "[animation_warmup][generation]")
{
    Files files;
    const auto path = files.write("bounds", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path, {4, 3});
    CHECK_FALSE(warm.request_adoption(path, {2, 2}, 2));
    warm.update(2, {path}, {2, 2});
    CHECK(warm.stats().ready == 0);
    driver.until([&] { return warm.stats().ready == 1; });
    REQUIRE(warm.request_adoption(path, {2, 2}, 3));
    warm.update(3, {}, {2, 2});
    std::optional<PreparedAnimation> prepared;
    driver.until(
        [&]
        {
            prepared = warm.take_adoption();
            return prepared.has_value();
        },
        false);
    CHECK(prepared->info.image.canvas.width == 2);
    CHECK(prepared->info.image.canvas.height == 1);
    CHECK(prepared->first.info.key.frame_index == 0);
}

TEST_CASE("foreground gating and cancellation track old GIF IDs until released while preserving ready results",
          "[animation_warmup][cancel]")
{
    Files files;
    const auto path = files.write("partial", gif(2, 64, 64));
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    warm.update(1, {path}, {100, 100});
    driver.cycle(false);
    CHECK(warm.stats().outstanding_requests == 0);
    warm.pump(true, 0);
    CHECK(warm.stats().outstanding_requests == 0);
    bool interrupted = false;
    driver.until([&] { return interrupted && !warm.stats().outstanding_requests; }, true,
                 [&](const auto &view)
                 {
                     if (!interrupted && view.type == LV_EVENT_ANIMATION_REGION_READY)
                     {
                         CHECK(warm.stats().retained_bytes >= 64 * 64 * 4);
                         warm.interrupt_background();
                         driver.foreground_busy = true;
                         CHECK(warm.owns(view.request_id));
                         interrupted = true;
                     }
                 });
    CHECK(warm.stats().ready == 0);
    CHECK_FALSE(warm.take_adoption_failure());
    driver.foreground_busy = false;
    driver.until([&] { return warm.stats().ready == 1; });
    warm.interrupt_background();
    driver.cycle(false);
    CHECK(warm.stats().ready == 1);
}

TEST_CASE("GIF warmup retries true session BUSY after asynchronous close cleanup", "[animation_warmup][busy]")
{
    Files files;
    const auto first  = files.write("foreground", gif());
    const auto second = files.write("neighbor", gif());
    Engine engine(6, 64 * 1024 * 1024, 1);
    lv_request_id request = 0;
    const auto session    = open_foreign(engine, first, request);
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_ANIMATION_INFO);
        Event terminal(engine.next());
        REQUIRE(terminal.view().type == LV_EVENT_COMPLETE);
    }
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    warm.update(1, {second}, {100, 100});
    warm.pump(true);
    CHECK(warm.stats().outstanding_requests == 0);
    CHECK(warm.stats().ready == 0);
    CHECK(warm.stats().failed == 0);
    CHECK(warm.pending()); // Session admission is BUSY even though no request ID exists.
    REQUIRE(lv_core_close_animation(engine.core, session) == LV_OK);
    driver.until([&] { return warm.stats().ready == 1; });
    CHECK_FALSE(warm.pending());
}

TEST_CASE("unreleased foreign terminals keep request BUSY without fake GIF success", "[animation_warmup][busy]")
{
    Files files;
    const auto path = files.write("request-busy", gif());
    Engine engine(1, 64 * 1024 * 1024, 3);
    lv_request_id request = 0;
    const auto session    = open_foreign(engine, path, request);
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_ANIMATION_INFO);
    Event terminal(engine.next());
    REQUIRE(terminal.view().type == LV_EVENT_COMPLETE);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    CHECK_FALSE(warm.owns(request));
    CHECK_FALSE(warm.consume(terminal.view()));
    warm.update(1, {path}, {100, 100});
    warm.pump(true);
    CHECK(warm.stats().outstanding_requests == 0);
    CHECK(warm.stats().ready == 0);
    CHECK(warm.stats().failed == 0);
    for (unsigned attempt = 0; attempt < 8; ++attempt)
    {
        warm.interrupt_background();
        warm.pump(false, 0);
        warm.pump(true, 1); // Foreign terminal still holds the sole Core request slot.
        CHECK(warm.stats().outstanding_requests == 0);
        CHECK(warm.stats().failed == 0);
        CHECK(warm.pending());
    }
    lv_event_release(info.event);
    info.event = nullptr;
    lv_event_release(terminal.event);
    terminal.event = nullptr;
    REQUIRE(lv_core_close_animation(engine.core, session) == LV_OK);
    driver.until([&] { return warm.stats().ready == 1; });
    CHECK_FALSE(warm.pending());
}

TEST_CASE("GIF retained budget includes the validation frame and drops all references on fallback", "[animation_warmup][budget]")
{
    Files files;
    const auto path   = files.write("budget", gif());
    std::uint64_t one = 0;
    {
        Engine engine(6, 64 * 1024 * 1024, 3);
        AnimationWarmup warm(engine.core);
        Driver driver{engine, warm};
        driver.ready(path);
        one = warm.stats().retained_bytes;
    }
    REQUIRE(one > 0);
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core, one);
    Driver driver{engine, warm};
    driver.ready(path);
    CHECK(warm.stats().retained_bytes == one);
    REQUIRE(warm.request_adoption(path, {100, 100}, 2));
    warm.update(2, {}, {100, 100});
    driver.until([&] { return warm.stats().failed && !warm.stats().outstanding_requests; }, false);
    CHECK(warm.take_adoption_failure());
    CHECK_FALSE(warm.take_adoption());
    CHECK(warm.stats().retained_bytes == 0);
    lv_core_stats stats{};
    REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.external_pixel_refs == 0);
}

TEST_CASE("GIF warmup generation churn remains bounded including retired request IDs", "[animation_warmup][generation]")
{
    Files files;
    const auto a = files.write("a", gif());
    const auto b = files.write("b", gif());
    const auto c = files.write("c", gif());
    const auto d = files.write("d", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    for (unsigned generation = 1; generation <= 30; ++generation)
    {
        warm.update(generation,
                    generation % 2 ? std::vector<std::filesystem::path>{a, b} : std::vector<std::filesystem::path>{c, d},
                    {100, 100});
        warm.pump(true, 2);
        CHECK(warm.stats().outstanding_requests <= 2);
    }
    driver.until([&] { return warm.stats().ready == 2; });
    CHECK_FALSE(warm.request_adoption(a, {100, 100}, 31));
    auto prepared = driver.adopt(c, {100, 100}, 31);
    REQUIRE(prepared);
    CHECK_FALSE(warm.take_adoption_failure());
}

TEST_CASE("obsolete GIF validation never delivers an old result or poisons the new generation", "[animation_warmup][generation]")
{
    Files files;
    const auto path = files.write("obsolete", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    REQUIRE(warm.request_adoption(path, {100, 100}, 2));
    warm.pump(false);
    warm.update(3, {}, {100, 100});
    driver.until([&] { return !warm.stats().outstanding_requests; }, false);
    CHECK_FALSE(warm.take_adoption());
    CHECK_FALSE(warm.take_adoption_failure());
}

TEST_CASE("moved GIF preparation owns its session and pixels independently of the warm collection",
          "[animation_warmup][lifetime]")
{
    Files files;
    const auto path = files.write("move", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    auto prepared = driver.adopt(path);
    REQUIRE(prepared);
    const auto id = prepared->session;
    {
        PreparedAnimation moved(std::move(*prepared));
        prepared.reset();
        CHECK(moved.session == id);
        warm.begin_shutdown();
        CHECK_FALSE(warm.pending()); // The moved result is independent and no longer warm work.
        color(moved.first, 255, 0);
        color(*moved.second, 0, 255);
        CHECK(next_is_end(engine, id));
        // No release_session(): this object's destructor must close the unclaimed session.
    }
    lv_animation_request_desc next;
    lv_animation_request_desc_init(&next);
    next.animation_id     = id;
    lv_request_id request = 0;
    CHECK(lv_core_submit_animation(engine.core, &next, &request) == LV_NOT_FOUND);
}

TEST_CASE("GIF warmup shutdown returns retained partial pixels and never submits again", "[animation_warmup][shutdown]")
{
    Files files;
    const auto path = files.write("shutdown", gif(2, 64, 64));
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    warm.update(1, {path}, {100, 100});
    bool stopped = false;
    driver.until([&] { return stopped && !warm.stats().outstanding_requests; }, true,
                 [&](const auto &view)
                 {
                     if (!stopped && view.type == LV_EVENT_ANIMATION_REGION_READY)
                     {
                         warm.begin_shutdown();
                         CHECK(warm.pending()); // Its in-flight ID is retained until the terminal is released.
                         stopped = true;
                     }
                 });
    CHECK(warm.stats().retained_bytes == 0);
    CHECK_FALSE(warm.pending());
    warm.update(2, {path}, {100, 100});
    driver.cycle(true);
    CHECK(warm.stats().outstanding_requests == 0);
    CHECK(warm.stats().ready == 0);
    lv_core_stats stats{};
    REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.external_pixel_refs == 0);
}

TEST_CASE("GIF plans deduplicate content-classified paths and retain at most two sessions", "[animation_warmup][limits]")
{
    Files files;
    const auto a = files.write("a.png", gif());
    const auto b = files.write("b.bin", gif());
    const auto c = files.write("c", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    warm.update(1, {a, a, b, c}, {100, 100});
    driver.until([&] { return warm.stats().ready == 2; });
    CHECK(warm.stats().completed == 2);
    CHECK_FALSE(warm.request_adoption(c, {100, 100}, 2));
    auto prepared = driver.adopt(b);
    REQUIRE(prepared);
}

TEST_CASE("GIF validation failures from a completed old generation are cleared by the new plan", "[animation_warmup][generation]")
{
    Files files;
    const auto path = files.write("failed-generation", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    std::ofstream(path, std::ios::binary | std::ios::app).put('\0');
    REQUIRE(warm.request_adoption(path, {100, 100}, 2));
    warm.update(2, {}, {100, 100});
    driver.until([&] { return warm.stats().failed && !warm.stats().outstanding_requests; }, false);
    warm.update(3, {}, {100, 100});
    CHECK_FALSE(warm.take_adoption_failure());
    CHECK_FALSE(warm.take_adoption());
}

TEST_CASE("GIF retained reservation covers larger pooled frame allocations before retaining a partial tile",
          "[animation_warmup][budget][pool]")
{
    constexpr std::uint64_t payload = 512ULL * 512 * 4;
    for (const bool enough : {false, true})
    {
        CAPTURE(enough);
        Files files;
        const std::string svg =
            "<svg xmlns='http://www.w3.org/2000/svg' width='512' height='560'><rect width='512' height='560' fill='red'/></svg>";
        const auto seed = files.write("larger.svg", {svg.begin(), svg.end()});
        const auto path = files.write("smaller.gif", gif(2, 512, 512));
        Engine engine(6, 64ULL * 1024 * 1024, 3, 0, 512);
        lv_request_desc request;
        lv_request_desc_init(&request);
        request.path        = {LV_PATH_POSIX_BYTES, seed.data(), seed.size()};
        request.target.kind = LV_TARGET_NATIVE;
        lv_request_id id    = 0;
        REQUIRE(lv_core_submit(engine.core, &request, &id) == LV_OK);
        const unsigned char *pooled = nullptr;
        for (;;)
        {
            Event event(engine.next());
            if (event.view().type == LV_EVENT_REGION_READY && !pooled)
                pooled = lv_pixels_get_view(event.view().as.region.pixels)->data;
            if (event.view().type == LV_EVENT_COMPLETE)
                break;
            REQUIRE(event.view().type != LV_EVENT_ERROR);
        }
        REQUIRE(pooled);
        lv_core_stats seeded{};
        REQUIRE(lv_core_get_stats(engine.core, &seeded) == LV_OK);
        // 512x560 is within the Core pool's 25% fit window for 512x512. A
        // payload-only quota would accept a backing larger than this low budget.
        const auto budget = enough ? 4ULL * 1024 * 1024 : payload + 64 * 1024;
        AnimationWarmup warm(engine.core, budget);
        Driver driver{engine, warm};
        warm.update(1, {path}, {512, 512});
        bool reused = false;
        driver.until([&] { return warm.stats().ready || (warm.stats().failed && !warm.stats().outstanding_requests); }, true,
                     [&](const auto &view)
                     {
                         if (view.type == LV_EVENT_ANIMATION_REGION_READY)
                             reused = reused || lv_pixels_get_view(view.as.frame_region.region.pixels)->data == pooled;
                     });
        if (enough)
        {
            CHECK(warm.stats().ready == 1);
            CHECK(reused);
            CHECK(warm.stats().retained_bytes >= payload + payload / 4);
            CHECK(warm.stats().retained_bytes <= budget);
            lv_core_stats held{};
            REQUIRE(lv_core_get_stats(engine.core, &held) == LV_OK);
            // OPEN adds canvas+backup but must preserve the larger seeded
            // backing capacity. Fresh-malloc at the same address is insufficient.
            CHECK(held.accounted_cpu_bytes >= seeded.accounted_cpu_bytes + 2 * payload);
        }
        else
        {
            CHECK(warm.stats().ready == 0);
            CHECK(warm.stats().failed == 1);
            CHECK(warm.stats().retained_bytes == 0);
        }
    }
}

TEST_CASE("stopped GIF warmup may outlive Core shutdown and destruction without stale Core calls",
          "[animation_warmup][shutdown][lifetime]")
{
    Files files;
    const auto path = files.write("outlive-core", gif(2, 64, 64));
    Engine engine(6, 64 * 1024 * 1024, 3);
    auto warm = std::make_unique<AnimationWarmup>(engine.core);
    warm->update(1, {path}, {100, 100});
    bool stopped = false;
    while (!stopped)
    {
        warm->pump(true);
        Event event(engine.next());
        REQUIRE(warm->consume(event.view()));
        if (event.view().type == LV_EVENT_ANIMATION_REGION_READY)
        {
            warm->begin_shutdown();
            CHECK(warm->pending());
            REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
            stopped = true;
        }
    }
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    REQUIRE(lv_core_destroy(&engine.core) == LV_OK);
    CHECK(warm->pending()); // Suppressed terminal: safe observation, never a Core-join predicate.
    warm.reset();           // ASan must not observe a call through the old borrowed Core.
}

TEST_CASE("single-frame GIF warmup preserves infinite and finite total-play metadata", "[animation_warmup][end][loop]")
{
    Files files;
    for (const int repeat : {0, 2})
    {
        CAPTURE(repeat);
        const auto path = files.write(repeat ? "finite" : "infinite", gif(1, 4, 3, repeat));
        Engine engine(6, 64 * 1024 * 1024, 3);
        AnimationWarmup warm(engine.core);
        Driver driver{engine, warm};
        driver.ready(path);
        auto prepared = driver.adopt(path);
        REQUIRE(prepared);
        CHECK(prepared->ended);
        CHECK_FALSE(prepared->second);
        CHECK(prepared->info.flags & LV_ANIMATION_PLAY_COUNT_KNOWN);
        CHECK(prepared->info.total_plays == (repeat ? 3ULL : 0ULL));
        CHECK(prepared->info.frame_count == 1);
        CHECK(prepared->first.info.key.frame_index == 0);
        CHECK(prepared->first.info.key.epoch == 1);
    }
}

TEST_CASE("external static PREFETCH and retired GIF work respect additional global quotas of zero and one",
          "[animation_warmup][quota]")
{
    Files files;
    const auto external = files.write("static-prefetch", gif());
    const auto a        = files.write("warm-a", gif());
    const auto b        = files.write("warm-b", gif());
    Engine engine(6, 64 * 1024 * 1024, 3);
    enable_cache(engine);
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path        = {LV_PATH_POSIX_BYTES, external.data(), external.size()};
    request.kind        = LV_REQUEST_PREFETCH;
    request.priority    = LV_PRIORITY_PREFETCH;
    request.target.kind = LV_TARGET_NATIVE;
    lv_request_id id    = 0;
    REQUIRE(lv_core_submit(engine.core, &request, &id) == LV_OK);
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    Event terminal(engine.next());
    REQUIRE(terminal.view().type == LV_EVENT_COMPLETE);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.quota = 1; // The unreleased external terminal reserves the other global slot.
    warm.update(1, {a, b}, {100, 100});
    warm.pump(true, 0);
    CHECK(warm.stats().outstanding_requests == 0);
    warm.pump(true, 1);
    CHECK(warm.stats().outstanding_requests == 1);
    CHECK(warm.pending());
    warm.update(2, {b, a}, {100, 100});
    warm.pump(true, 0);
    CHECK(warm.stats().outstanding_requests == 1);
    CHECK(warm.pending()); // Retired IDs still occupy quota before their released terminals are pumped.
    driver.until([&] { return warm.stats().ready == 2; }, true,
                 [&](const auto &)
                 {
                     lv_core_stats stats{};
                     REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
                     CHECK(stats.accepted_requests <= 2);
                 });
    CHECK_FALSE(warm.pending());
    CHECK_FALSE(warm.owns(id));
    lv_event_release(info.event);
    info.event = nullptr;
    lv_event_release(terminal.event);
    terminal.event = nullptr;
}

TEST_CASE("GIF validation rejects a same-length rewrite while the NEXT tile queue is backpressured",
          "[animation_warmup][source][backpressure]")
{
    Files files;
    const auto bytes = gif(2, 64, 64);
    const auto path  = files.write("during-validation", bytes);
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    driver.ready(path);
    REQUIRE(warm.request_adoption(path, {100, 100}, 2));
    warm.update(2, {}, {100, 100});
    warm.pump(false);
    bool got_tile = false;
    while (!got_tile)
    {
        Event event(engine.next());
        REQUIRE(warm.consume(event.view()));
        if (event.view().type == LV_EVENT_ANIMATION_REGION_READY)
        {
            got_tile = true;
            lv_core_stats stats{};
            REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
            REQUIRE(stats.running_requests == 1);
            // Fifteen remaining tiles cannot finish through the one-tile queue.
            mutate_same_length(path, bytes);
        }
        REQUIRE(event.view().type != LV_EVENT_ERROR);
    }
    std::optional<PreparedAnimation> prepared;
    driver.until(
        [&]
        {
            prepared = warm.take_adoption();
            return prepared.has_value() || (warm.stats().failed && !warm.stats().outstanding_requests);
        },
        false);
    CHECK_FALSE(prepared);
    CHECK(warm.take_adoption_failure());
    CHECK(warm.stats().retained_bytes == 0);
}

TEST_CASE("foreground interruptions admit no more than three warm OPEN jobs per target and plan", "[animation_warmup][attempts]")
{
    for (unsigned reset = 0; reset < 3; ++reset)
    {
        CAPTURE(reset);
        Files files;
        const auto path  = files.write("bounded-restart", gif(2, 64, 64));
        const auto other = files.write("new-neighbor", gif());
        Engine engine(6, 64 * 1024 * 1024, 3);
        AnimationWarmup warm(engine.core);
        Driver driver{engine, warm};
        warm.update(1, {path}, {100, 100});
        for (unsigned accepted = 0; accepted < 3; ++accepted)
        {
            warm.pump(true, 1);
            REQUIRE(warm.stats().outstanding_requests == 1);
            warm.interrupt_background();
            driver.until([&] { return warm.stats().outstanding_requests == 0; }, false);
        }
        for (unsigned frame = 0; frame < 10; ++frame)
        {
            warm.pump(true, 1);
            CHECK(warm.stats().outstanding_requests == 0);
            CHECK_FALSE(warm.pending());
        }
        warm.update(reset == 0 ? 2 : 1,
                    reset == 2 ? std::vector<std::filesystem::path>{path, other} : std::vector<std::filesystem::path>{path},
                    reset == 1 ? lv_size_u32{99, 100} : lv_size_u32{100, 100});
        warm.pump(true, 1);
        CHECK(warm.stats().outstanding_requests == 1);
        warm.begin_shutdown();
    }
}

TEST_CASE("a failed warm GIF target never retries without a new plan", "[animation_warmup][attempts][errors]")
{
    Files files;
    const auto path = files.write("invalid-gif", {'G', 'I', 'F', '8', '9', 'a'});
    Engine engine(6, 64 * 1024 * 1024, 3);
    AnimationWarmup warm(engine.core);
    Driver driver{engine, warm};
    warm.update(1, {path}, {100, 100});
    driver.until([&] { return warm.stats().failed == 1 && !warm.stats().outstanding_requests; });
    for (unsigned frame = 0; frame < 10; ++frame)
    {
        warm.pump(true, 2);
        CHECK(warm.stats().outstanding_requests == 0);
        CHECK(warm.stats().failed == 1);
    }
    warm.update(2, {path}, {100, 100});
    driver.until([&] { return warm.stats().failed == 2 && !warm.stats().outstanding_requests; });
}

TEST_CASE("unclassified PNG and damaged neighbors fail only in the background and return all reservations",
          "[animation_warmup][classification]")
{
    const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures" / "red-alpha.png";
    std::ifstream input(fixture, std::ios::binary);
    REQUIRE(input.good());
    const std::vector<unsigned char> png((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    for (const auto &bytes : {png, std::vector<unsigned char>{'G', 'I', 'F', '8', '9', 'a'}})
    {
        Files files;
        const auto path = files.write("unclassified-neighbor.bin", bytes);
        Engine engine(6, 64 * 1024 * 1024, 3);
        AnimationWarmup warm(engine.core);
        Driver driver{engine, warm};
        warm.update(1, {path}, {100, 100});
        driver.until([&] { return warm.stats().failed == 1 && !warm.stats().outstanding_requests; });
        CHECK(warm.stats().ready == 0);
        CHECK(warm.stats().retained_bytes == 0);
        CHECK_FALSE(warm.request_adoption(path, {100, 100}, 2));
        CHECK_FALSE(warm.take_adoption());
        CHECK_FALSE(warm.take_adoption_failure());
        lv_core_stats stats{};
        REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
        CHECK(stats.external_pixel_refs == 0);
    }
}
