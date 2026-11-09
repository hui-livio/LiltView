// Reviewed ABI 2 contract: cache_budget_bytes bounds complete immutable static
// results. The key is the opened file version plus active target fields, never
// request kind, priority, tag or union padding. DISPLAY may bypass caching;
// PREFETCH must decode and retain a complete result or report a budget error.
// PREFETCH publishes INFO and one terminal, never pixel events. Cache hits open
// and revalidate the source, replay with fresh request identity and bytes_read=0.
// Only finish_request's locked COMPLETE decision commits a capture. Cancel and
// ERROR discard it. Internal cache references do not count as external/pinned.
//
// Reviewed UT matrix: configuration/disabled prefetch, display and prefetch
// hits, active target fields versus padding/tag, exact replay coverage, changed
// and replaced sources, LRU/real backing accounting, retained-pixel lifetime,
// oversized DISPLAY bypass versus PREFETCH failure, NATIVE capture pressure,
// source-slot exhaustion, successful/failed foreground admission, deterministic
// cancel/terminal races and backpressured replay mutation/cancel, shutdown.

#include <array>
#include <map>
#include <memory>
#include <set>
#include "region_support.hpp"

namespace
{
using namespace liltview::test;
constexpr std::uint64_t mib = 1024ULL * 1024;

struct CachedEngine : Engine
{
    std::uint64_t gate_notification = 0;

    static void LV_CALL wake(void *userdata)
    {
        auto &self = *static_cast<CachedEngine *>(userdata);
        std::unique_lock lock(self.mutex);
        ++self.notifications;
        self.changed.notify_all();
        if (self.gate && !self.entered && (!self.gate_notification || self.notifications == self.gate_notification))
        {
            self.entered = true;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.proceed; });
        }
    }

    explicit CachedEngine(std::uint64_t cache = mib, std::uint64_t memory = 32 * mib, unsigned max = 4, unsigned edge = 16,
                          unsigned prefetch = 1) :
        Engine(max, memory, 0, 0, edge)
    {
        REQUIRE(lv_core_begin_shutdown(core) == LV_OK);
        REQUIRE(lv_core_join(core, 5000) == LV_OK);
        REQUIRE(lv_core_destroy(&core) == LV_OK);
        notifications = 0; // The original base Engine has joined; only the new worker follows.
        lv_core_config config;
        lv_core_config_init(&config);
        config.cpu_budget_bytes       = memory;
        config.cache_budget_bytes     = cache;
        config.queue_pixel_bytes      = static_cast<std::uint64_t>(edge) * edge * 4;
        config.tile_edge              = edge;
        config.max_requests           = max;
        config.animation.max_sessions = 0;
        config.prefetch_workers       = prefetch;
        config.wakeup                 = CachedEngine::wake;
        config.wakeup_userdata        = this;
        REQUIRE(lv_core_create(&config, &core) == LV_OK);
    }

    ~CachedEngine()
    {
        release_gate();
        if (core)
        {
            lv_core_begin_shutdown(core);
            while (lv_core_join(core, 100) == LV_TIMEOUT)
            {
            }
        }
        // The callback uses derived state, so it must finish before base destruction.
    }

    lv_request_desc descriptor(const std::string &path, bool prefetch = false, std::uint64_t tag = 1)
    {
        lv_request_desc result;
        lv_request_desc_init(&result);
        result.path        = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
        result.kind        = prefetch ? LV_REQUEST_PREFETCH : LV_REQUEST_DISPLAY;
        result.priority    = prefetch ? LV_PRIORITY_PREFETCH : LV_PRIORITY_CURRENT;
        result.target.kind = LV_TARGET_NATIVE;
        result.user_tag    = tag;
        return result;
    }

    lv_request_id request(const lv_request_desc &desc)
    {
        lv_request_id id = 0;
        REQUIRE(lv_core_submit(core, &desc, &id) == LV_OK);
        REQUIRE(id != 0);
        return id;
    }

    lv_core_stats stats()
    {
        lv_core_stats value{};
        REQUIRE(lv_core_get_stats(core, &value) == LV_OK);
        return value;
    }

    void wait_gate()
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }

    void release_gate()
    {
        std::lock_guard lock(mutex);
        proceed = true;
        changed.notify_all();
    }
};

struct Result
{
    lv_event_type terminal = 0;
    lv_image_info info{};
    lv_error_info error{};
    lv_completion_info completion{};
    std::vector<unsigned char> data;
    std::vector<std::array<std::uint64_t, 6>> coverage;
    lv_pixels *held = nullptr;
};

Result receive(CachedEngine &engine, lv_request_id id, std::uint64_t tag = 1, bool retain = false)
{
    Result result;
    std::uint64_t sequence = 0;
    for (;;)
    {
        Event event(engine.next());
        const auto &view = event.view();
        CHECK(view.request_id == id);
        CHECK(view.user_tag == tag);
        CHECK(view.sequence > sequence);
        sequence = view.sequence;
        if (view.type == LV_EVENT_IMAGE_INFO)
            result.info = view.as.image;
        else if (view.type == LV_EVENT_REGION_READY)
        {
            const auto &region = view.as.region;
            const auto &pixels = *lv_pixels_get_view(region.pixels);
            result.coverage.push_back(
                {region.tile_id, region.revision, region.rect.x, region.rect.y, region.rect.width, region.rect.height});
            for (unsigned y = 0; y < pixels.size.height; ++y)
                result.data.insert(result.data.end(), pixels.data + y * pixels.stride_bytes,
                                   pixels.data + y * pixels.stride_bytes + pixels.size.width * 4);
            if (retain && !result.held)
            {
                result.held = region.pixels;
                lv_pixels_retain(result.held);
            }
        }
        else
        {
            result.terminal = view.type;
            if (view.type == LV_EVENT_COMPLETE)
                result.completion = view.as.completion;
            if (view.type == LV_EVENT_ERROR)
                result.error = view.as.error;
            return result;
        }
    }
}

std::string image(Files &files, const char *name = "cache.bmp", unsigned width = 49, unsigned height = 35)
{
    const auto path = files.directory / name;
    checkerboard(path, width, height, 7);
    return path.string();
}
} // namespace

TEST_CASE("content cache replays immutable complete results with new identity and no decode reads", "[cache]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine;
    auto desc           = engine.descriptor(path);
    const auto first_id = engine.request(desc);
    auto first          = receive(engine, first_id, 1, true);
    REQUIRE(first.terminal == LV_EVENT_COMPLETE);
    CHECK(first.completion.source_bytes_read > 0);
    const auto after_first = engine.stats();
    CHECK(after_first.cache_bytes > 0);
    CHECK(after_first.cache_bytes <= mib);
    CHECK(after_first.cache_bytes <= after_first.accounted_cpu_bytes);
    CHECK(after_first.external_pixel_refs == 1);
    desc.user_tag        = 922;
    desc.priority        = LV_PRIORITY_VISIBLE_DETAIL;
    const auto second_id = engine.request(desc);
    auto second          = receive(engine, second_id, 922, true);
    REQUIRE(second.terminal == LV_EVENT_COMPLETE);
    CHECK(second_id != first_id);
    CHECK(second.completion.source_bytes_read == 0);
    CHECK(second.info.source_revision == first.info.source_revision);
    CHECK(second.coverage == first.coverage);
    CHECK(second.data == first.data);
    CHECK(second.held == first.held);
    CHECK(second.completion.final_tile_count == second.coverage.size());
    lv_pixels_release(first.held);
    lv_pixels_release(second.held);
    CHECK(engine.stats().external_pixel_refs == 0);
    CHECK(engine.stats().pinned_pixel_bytes == 0);
}

TEST_CASE("PREFETCH decodes complete content without pixel events and DISPLAY reuses it", "[cache][prefetch]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine;
    const auto id   = engine.request(engine.descriptor(path, true, 44));
    auto prefetched = receive(engine, id, 44);
    REQUIRE(prefetched.terminal == LV_EVENT_COMPLETE);
    CHECK(prefetched.info.source_size.width == 49);
    CHECK(prefetched.coverage.empty());
    CHECK(prefetched.completion.final_tile_count == 0);
    CHECK(prefetched.completion.source_bytes_read > 0);
    CHECK(engine.stats().cache_bytes > 0);
    CHECK(engine.stats().external_pixel_refs == 0);
    CHECK(engine.stats().pinned_pixel_bytes == 0);
    auto shown = receive(engine, engine.request(engine.descriptor(path)));
    REQUIRE(shown.terminal == LV_EVENT_COMPLETE);
    CHECK(shown.completion.source_bytes_read == 0);
    CHECK(shown.data.size() == 49 * 35 * 4);
    CHECK(shown.info.source_revision == prefetched.info.source_revision);
    auto again = receive(engine, engine.request(engine.descriptor(path, true)));
    CHECK(again.terminal == LV_EVENT_COMPLETE);
    CHECK(again.completion.source_bytes_read == 0);
    CHECK(again.coverage.empty());
}

TEST_CASE("content keys compare active target fields rather than union padding or request tags", "[cache][target]")
{
    Files files;
    const auto path = image(files, "target.bmp", 80, 48);
    CachedEngine engine;
    auto desc                 = engine.descriptor(path, true, 20);
    desc.target.kind          = LV_TARGET_FIT;
    desc.target.as.fit_bounds = {40, 24};
    REQUIRE(receive(engine, engine.request(desc), 20).terminal == LV_EVENT_COMPLETE);
    desc = engine.descriptor(path, false, 21);
    std::memset(&desc.target.as, 0xa5, sizeof desc.target.as);
    desc.target.kind          = LV_TARGET_FIT;
    desc.target.as.fit_bounds = {40, 24};
    auto same                 = receive(engine, engine.request(desc), 21);
    REQUIRE(same.terminal == LV_EVENT_COMPLETE);
    CHECK(same.completion.source_bytes_read == 0);
    desc.target.as.fit_bounds = {20, 12};
    auto changed              = receive(engine, engine.request(desc), 21);
    CHECK(changed.completion.source_bytes_read > 0);
    desc.target.kind      = LV_TARGET_REGION;
    desc.target.as.region = {{80, 48}, {9, 7, 25, 21}};
    auto region           = receive(engine, engine.request(desc), 21);
    CHECK(region.completion.source_bytes_read > 0);
    auto replay = receive(engine, engine.request(desc), 21);
    CHECK(replay.completion.source_bytes_read == 0);
    CHECK(replay.coverage == region.coverage);
    CHECK(replay.data == region.data);
    desc.target.as.region.rect.x++;
    CHECK(receive(engine, engine.request(desc), 21).completion.source_bytes_read > 0);
    desc.target.kind = LV_TARGET_NATIVE;
    REQUIRE(receive(engine, engine.request(desc), 21).completion.source_bytes_read > 0);
    std::memset(&desc.target.as, 0xcc, sizeof desc.target.as);
    CHECK(receive(engine, engine.request(desc), 21).completion.source_bytes_read == 0);
}

TEST_CASE("cache validates modified replaced deleted and aliased source versions", "[cache][source]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine;
    auto desc        = engine.descriptor(path);
    const auto first = receive(engine, engine.request(desc));
    const auto alias = (files.directory / "alias.bmp").string();
    std::filesystem::create_symlink(path, alias);
    auto aliased = receive(engine, engine.request(engine.descriptor(alias)));
    CHECK(aliased.completion.source_bytes_read == 0);
    CHECK(aliased.info.source_revision == first.info.source_revision);
    checkerboard(path, 51, 37, 5);
    auto modified = receive(engine, engine.request(desc));
    REQUIRE(modified.terminal == LV_EVENT_COMPLETE);
    CHECK(modified.completion.source_bytes_read > 0);
    CHECK(modified.info.source_revision != first.info.source_revision);
    const auto replacement = image(files, "replacement.bmp", 53, 39);
    std::filesystem::rename(replacement, path);
    auto replaced = receive(engine, engine.request(desc));
    CHECK(replaced.completion.source_bytes_read > 0);
    CHECK(replaced.info.source_size.width == 53);
    std::filesystem::remove(path);
    const auto missing = receive(engine, engine.request(desc));
    CHECK(missing.terminal == LV_EVENT_ERROR);
    CHECK(missing.error.code == LV_ERROR_IO);
    CHECK(missing.coverage.empty());
}

TEST_CASE("fresh publication rechecks source after queue backpressure before committing content", "[cache][source][publish]")
{
    for (const bool vector : {false, true})
    {
        Files files;
        const auto path =
            vector ? files.write("publishing.svg", std::vector<unsigned char>{}) : image(files, "publishing.bmp", 96, 80);
        if (vector)
        {
            std::ofstream file(path);
            file << "<svg xmlns='http://www.w3.org/2000/svg' width='96' height='80'>"
                    "<rect width='96' height='80' fill='red'/></svg>";
        }
        CachedEngine engine;
        auto desc                 = engine.descriptor(path);
        desc.target.kind          = LV_TARGET_FIT;
        desc.target.as.fit_bounds = {96, 80};
        const auto id             = engine.request(desc);
        std::unique_ptr<lv_pixels, decltype(&lv_pixels_release)> held(nullptr, lv_pixels_release);
        std::vector<std::uint8_t> original;
        {
            Event info(engine.next());
            REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
            Event tile(engine.next());
            REQUIRE(tile.view().type == LV_EVENT_REGION_READY);
            const auto *pixels = lv_pixels_get_view(tile.view().as.region.pixels);
            const auto alpha   = pixels->data[3];
            lv_pixels_retain(tile.view().as.region.pixels);
            held.reset(tile.view().as.region.pixels);
            original.assign(pixels->data, pixels->data + pixels->byte_count);
            {
                // Decode and the original version check have finished. The
                // remaining tiles cannot all fit in this one-tile queue.
                std::ofstream changed(path, std::ios::binary | std::ios::app);
                changed << '\n';
                REQUIRE(changed.good());
            }
            const auto changed = receive(engine, id);
            CHECK(changed.terminal == LV_EVENT_ERROR);
            CHECK(changed.error.code == LV_ERROR_SOURCE_CHANGED);
            CHECK(changed.error.has_partial_output == 1);
            CHECK(engine.stats().cache_bytes == 0);
            CHECK(pixels->data[3] == alpha);
        }
        lv_event *extra = nullptr;
        CHECK(lv_core_poll_event(engine.core, &extra) == LV_EMPTY);
        CHECK(extra == nullptr);
        if (vector)
            std::ofstream(path) << "<svg xmlns='http://www.w3.org/2000/svg' width='96' height='80'>"
                                   "<rect width='96' height='80' fill='blue'/></svg>";
        else
            checkerboard(path, 96, 80, 3);
        const auto retried = receive(engine, engine.request(desc));
        REQUIRE(retried.terminal == LV_EVENT_COMPLETE);
        CHECK(retried.completion.source_bytes_read > 0);
        const auto *pixels = lv_pixels_get_view(held.get());
        CHECK(std::vector<std::uint8_t>(pixels->data, pixels->data + pixels->byte_count) == original);
    }
}

TEST_CASE("content LRU accounts real allocations and evicts the least recently used result", "[cache][budget]")
{
    Files files;
    const auto a      = image(files, "a.bmp", 16, 16);
    const auto b      = image(files, "b.bmp", 16, 16);
    const auto c      = image(files, "c.bmp", 16, 16);
    std::uint64_t one = 0;
    {
        CachedEngine probe;
        REQUIRE(receive(probe, probe.request(probe.descriptor(a))).terminal == LV_EVENT_COMPLETE);
        one = probe.stats().cache_bytes;
    }
    CachedEngine engine(one * 2 + one / 4);
    REQUIRE(receive(engine, engine.request(engine.descriptor(a))).terminal == LV_EVENT_COMPLETE);
    REQUIRE(receive(engine, engine.request(engine.descriptor(b))).terminal == LV_EVENT_COMPLETE);
    CHECK(receive(engine, engine.request(engine.descriptor(a))).completion.source_bytes_read == 0);
    REQUIRE(receive(engine, engine.request(engine.descriptor(c))).terminal == LV_EVENT_COMPLETE);
    CHECK(engine.stats().cache_bytes <= one * 2 + one / 4);
    CHECK(receive(engine, engine.request(engine.descriptor(a))).completion.source_bytes_read == 0);
    CHECK(receive(engine, engine.request(engine.descriptor(b))).completion.source_bytes_read > 0);
}

TEST_CASE("shared backing cache costs scale with pixels and tile descriptors rather than stride spans", "[cache][stride]")
{
    Files files;
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg' width='129' height='65'><rect width='129' height='65' fill='red'/></svg>";
    const auto path = files.write("slices.svg", {svg.begin(), svg.end()});
    CachedEngine engine;
    auto first = receive(engine, engine.request(engine.descriptor(path)));
    REQUIRE(first.terminal == LV_EVENT_COMPLETE);
    const auto cached = engine.stats().cache_bytes;
    CHECK(cached > 129 * 65 * 4);
    CHECK(cached < 129 * 65 * 4 + first.coverage.size() * 256 + 4096);
    CHECK(engine.stats().external_pixel_refs == 0);
    CHECK(engine.stats().pinned_pixel_bytes == 0);
    auto replay = receive(engine, engine.request(engine.descriptor(path)));
    CHECK(replay.completion.source_bytes_read == 0);
    CHECK(replay.data == first.data);
}

TEST_CASE("oversized DISPLAY bypasses caching while PREFETCH explicitly fails without partial pixels",
          "[cache][prefetch][budget]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine(1024);
    auto shown = receive(engine, engine.request(engine.descriptor(path)));
    REQUIRE(shown.terminal == LV_EVENT_COMPLETE);
    CHECK(engine.stats().cache_bytes == 0);
    auto rejected = receive(engine, engine.request(engine.descriptor(path, true)));
    CHECK(rejected.terminal == LV_EVENT_ERROR);
    CHECK(rejected.error.code == LV_ERROR_MEMORY_BUDGET);
    CHECK(rejected.error.has_partial_output == 0);
    CHECK(rejected.coverage.empty());
    CHECK(engine.stats().cache_bytes == 0);
}

TEST_CASE("NATIVE capture pressure abandons internal retained rows instead of blocking DISPLAY", "[cache][native][budget]")
{
    Files files;
    const auto path = image(files, "pressure.bmp", 1024, 1016);
    CachedEngine engine(4 * mib, 4 * mib, 1, 64);
    auto shown = receive(engine, engine.request(engine.descriptor(path)));
    REQUIRE(shown.terminal == LV_EVENT_COMPLETE);
    CHECK(shown.data.size() == 1024 * 1016 * 4);
    CHECK(engine.stats().cache_bytes == 0);
    CHECK(engine.stats().accounted_cpu_bytes <= 4 * mib);
    const auto prefetched = receive(engine, engine.request(engine.descriptor(path, true)));
    CHECK(prefetched.terminal == LV_EVENT_ERROR);
    CHECK(prefetched.error.code == LV_ERROR_MEMORY_BUDGET);
    CHECK(prefetched.error.has_partial_output == 0);
}

TEST_CASE("many cached source identities never exhaust the bounded source table", "[cache][source][budget]")
{
    Files files;
    CachedEngine engine(mib, 8 * mib, 1);
    for (unsigned i = 0; i < 40; ++i)
    {
        const auto name  = "micro-" + std::to_string(i) + ".bmp";
        const auto path  = image(files, name.c_str(), 2, 2);
        const auto shown = receive(engine, engine.request(engine.descriptor(path)));
        REQUIRE(shown.terminal == LV_EVENT_COMPLETE);
        CHECK(shown.info.source_revision != 0);
        CHECK(engine.stats().accepted_requests == 0);
    }
    CHECK(engine.stats().cache_bytes > 0);
}

TEST_CASE("cancel before terminal discards capture while committed success survives cancel", "[cache][cancel]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine;
    engine.gate   = true;
    const auto id = engine.request(engine.descriptor(path));
    engine.wait_gate(); // Fresh engine: the first notification is IMAGE_INFO.
    REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
    engine.release_gate();
    auto cancelled = receive(engine, id);
    CHECK(cancelled.terminal == LV_EVENT_CANCELLED);
    CHECK(engine.stats().cache_bytes == 0);
    const auto retry_id = engine.request(engine.descriptor(path));
    auto retried        = receive(engine, retry_id);
    REQUIRE(retried.terminal == LV_EVENT_COMPLETE);
    CHECK(retried.completion.source_bytes_read > 0);
    const auto hit_id = engine.request(engine.descriptor(path));
    for (;;)
    {
        Event event(engine.next());
        if (event.view().type == LV_EVENT_COMPLETE)
        {
            CHECK(event.view().as.completion.source_bytes_read == 0);
            CHECK(lv_core_cancel(engine.core, hit_id) == LV_OK);
            CHECK(engine.stats().cache_bytes > 0);
            break;
        }
    }
}

TEST_CASE("cancelling a partially captured fresh NATIVE result leaves no reusable content", "[cache][cancel][native]")
{
    Files files;
    const auto path = image(files, "partial.bmp", 96, 80);
    CachedEngine engine;
    const auto id = engine.request(engine.descriptor(path));
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
        Event tile(engine.next());
        REQUIRE(tile.view().type == LV_EVENT_REGION_READY);
        // A one-tile queue prevents the remaining 29 tiles reaching COMPLETE.
        REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
        CHECK(receive(engine, id).terminal == LV_EVENT_CANCELLED);
        CHECK(engine.stats().cache_bytes == 0);
        CHECK(lv_pixels_get_view(tile.view().as.region.pixels)->data[3] == 255);
    }
    const auto retry = receive(engine, engine.request(engine.descriptor(path)));
    REQUIRE(retry.terminal == LV_EVENT_COMPLETE);
    CHECK(retry.completion.source_bytes_read > 0);
}

TEST_CASE("complete capture commits only when terminal wins the deterministic cancellation race", "[cache][cancel][terminal]")
{
    Files files;
    const auto path = image(files, "one-tile.bmp", 16, 16);
    for (const bool terminal_first : {false, true})
    {
        CAPTURE(terminal_first);
        CachedEngine engine;
        engine.gate = true;
        // Fresh worker: INFO is notify 1, captured tile is 2, committed COMPLETE is 3.
        // No event is released before this gate, so resource notifications cannot interfere.
        engine.gate_notification = terminal_first ? 3 : 2;
        const auto id            = engine.request(engine.descriptor(path));
        engine.wait_gate();
        CHECK((engine.stats().cache_bytes != 0) == terminal_first);
        REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
        engine.release_gate();
        const auto result = receive(engine, id);
        CHECK(result.terminal == (terminal_first ? LV_EVENT_COMPLETE : LV_EVENT_CANCELLED));
        CHECK(result.coverage.size() == (terminal_first ? 1 : 0));
        CHECK((engine.stats().cache_bytes != 0) == terminal_first);
        const auto next = receive(engine, engine.request(engine.descriptor(path)));
        REQUIRE(next.terminal == LV_EVENT_COMPLETE);
        CHECK((next.completion.source_bytes_read == 0) == terminal_first);
    }
}

TEST_CASE("accepted foreground work cancels running PREFETCH and rejected work does not", "[cache][prefetch][priority]")
{
    Files files;
    const auto path = image(files);
    for (const unsigned maximum : {1u, 2u})
    {
        CAPTURE(maximum);
        CachedEngine engine(mib, 32 * mib, maximum);
        engine.gate           = true;
        const auto background = engine.request(engine.descriptor(path, true));
        engine.wait_gate();
        auto foreground   = engine.descriptor(path, false, 2);
        lv_request_id id  = 999;
        const auto status = lv_core_submit(engine.core, &foreground, &id);
        CHECK(status == (maximum == 1 ? LV_BUSY : LV_OK));
        CHECK((id != 0) == (status == LV_OK));
        engine.release_gate();
        auto prefetched = receive(engine, background);
        if (maximum == 1)
        {
            CHECK(prefetched.terminal == LV_EVENT_COMPLETE);
            CHECK(engine.stats().cache_bytes > 0);
        }
        else
        {
            CHECK(prefetched.terminal == LV_EVENT_CANCELLED);
            const auto shown = receive(engine, id, 2);
            CHECK(shown.terminal == LV_EVENT_COMPLETE);
            CHECK(shown.completion.source_bytes_read > 0);
        }
    }
}

TEST_CASE("replay rechecks source after queue backpressure and preserves borrowed pixels on cancel", "[cache][source][cancel]")
{
    Files files;
    for (const bool cancel : {false, true})
    {
        CAPTURE(cancel);
        const auto path = image(files, cancel ? "cancel.bmp" : "mutate.bmp", 96, 80);
        CachedEngine engine;
        REQUIRE(receive(engine, engine.request(engine.descriptor(path))).terminal == LV_EVENT_COMPLETE);
        const auto id = engine.request(engine.descriptor(path));
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
        Event tile(engine.next());
        REQUIRE(tile.view().type == LV_EVENT_REGION_READY);
        auto *held = tile.view().as.region.pixels;
        lv_pixels_retain(held);
        const auto first = lv_pixels_get_view(held)->data[0];
        if (cancel)
            REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
        else
            checkerboard(path, 96, 80, 3);
        auto result = receive(engine, id);
        CHECK(result.terminal == (cancel ? LV_EVENT_CANCELLED : LV_EVENT_ERROR));
        if (!cancel)
        {
            CHECK(result.error.code == LV_ERROR_SOURCE_CHANGED);
            CHECK(engine.stats().cache_bytes == 0);
        }
        CHECK(lv_pixels_get_view(held)->data[0] == first);
        lv_pixels_release(held);
    }
}

TEST_CASE("shutdown drops cache references but retained pixels remain valid until returned", "[cache][shutdown][lifetime]")
{
    Files files;
    const auto path = image(files);
    CachedEngine engine;
    auto shown = receive(engine, engine.request(engine.descriptor(path)), 1, true);
    REQUIRE(shown.held);
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    CHECK(engine.stats().cache_bytes == 0);
    CHECK(lv_core_destroy(&engine.core) == LV_BUSY);
    CHECK(lv_pixels_get_view(shown.held)->data[3] == 255);
    lv_pixels_release(shown.held);
    CHECK(lv_core_destroy(&engine.core) == LV_OK);
}

TEST_CASE("shutdown reclaims an in-use replay entry while retained pixels remain readable", "[cache][shutdown][replay]")
{
    Files files;
    const auto path = image(files, "replay-shutdown.bmp", 96, 80);
    CachedEngine engine;
    auto cached = receive(engine, engine.request(engine.descriptor(path)), 1, true);
    REQUIRE(cached.terminal == LV_EVENT_COMPLETE);
    REQUIRE(cached.held);
    engine.request(engine.descriptor(path));
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    Event tile(engine.next());
    REQUIRE(tile.view().type == LV_EVENT_REGION_READY);
    auto *held = tile.view().as.region.pixels;
    REQUIRE(held == cached.held); // This is a cache replay, not a second decode.
    lv_pixels_retain(held);
    lv_pixels_release(cached.held);
    const auto first = lv_pixels_get_view(held)->data[0];
    lv_event_release(info.event);
    info.event = nullptr;
    lv_event_release(tile.event);
    tile.event = nullptr;
    // At most two further tiles can enter the one-tile queue; replay remains in use.
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    CHECK(engine.stats().cache_bytes == 0);
    CHECK(engine.stats().external_pixel_refs == 1);
    CHECK(lv_pixels_get_view(held)->data[0] == first);
    CHECK(lv_core_destroy(&engine.core) == LV_BUSY);
    lv_pixels_release(held);
    CHECK(lv_core_destroy(&engine.core) == LV_OK);
}

TEST_CASE("PREFETCH configuration preserves ABI and rejects disabled or unimplemented workers", "[cache][config]")
{
    Files files;
    const auto path = image(files);
    for (const auto &options : {std::pair<std::uint64_t, unsigned>{0, 1}, {mib, 0}})
    {
        CachedEngine engine(options.first, 32 * mib, 1, 16, options.second);
        auto desc        = engine.descriptor(path, true);
        lv_request_id id = 999;
        CHECK(lv_core_submit(engine.core, &desc, &id) == LV_UNSUPPORTED);
        CHECK(id == 0);
    }
    lv_core_config config;
    lv_core_config_init(&config);
    config.cpu_budget_bytes   = 32 * mib;
    config.queue_pixel_bytes  = mib;
    config.cache_budget_bytes = mib;
    config.prefetch_workers   = 2;
    lv_core *core             = nullptr;
    CHECK(lv_core_create(&config, &core) == LV_UNSUPPORTED);
    CHECK(core == nullptr);
    config.prefetch_workers        = 1;
    config.disk_cache_budget_bytes = mib;
    CHECK(lv_core_create(&config, &core) == LV_UNSUPPORTED);
    CHECK(core == nullptr);
    CHECK(lv_core_abi_version() == 2);
}

TEST_CASE("queued foreground requests run before earlier queued PREFETCH", "[cache][prefetch][priority]")
{
    Files files;
    const auto initial    = image(files, "initial.bmp");
    const auto adjacent   = image(files, "adjacent.bmp");
    const auto foreground = image(files, "foreground.bmp");
    CachedEngine engine(mib, 32 * mib, 3);
    engine.gate      = true;
    const auto first = engine.request(engine.descriptor(initial));
    engine.wait_gate();
    const auto background = engine.request(engine.descriptor(adjacent, true));
    const auto current    = engine.request(engine.descriptor(foreground));
    engine.release_gate();
    std::vector<lv_request_id> order;
    while (order.size() != 3)
    {
        Event event(engine.next());
        const auto &view = event.view();
        CHECK(view.type != LV_EVENT_ERROR);
        CHECK(view.type != LV_EVENT_CANCELLED);
        if (view.request_id == background)
            CHECK(view.type != LV_EVENT_REGION_READY);
        if (view.type == LV_EVENT_COMPLETE)
            order.push_back(view.request_id);
    }
    CHECK(order == std::vector<lv_request_id>{first, current, background});
    CHECK(receive(engine, engine.request(engine.descriptor(adjacent))).completion.source_bytes_read == 0);
}

TEST_CASE("source mutation and decoder errors discard unfinished captures", "[cache][source][errors]")
{
    Files files;
    const auto path = image(files);
    {
        CachedEngine engine;
        engine.gate   = true;
        const auto id = engine.request(engine.descriptor(path, true));
        engine.wait_gate();
        checkerboard(path, 49, 35, 3);
        engine.release_gate();
        const auto changed = receive(engine, id);
        CHECK(changed.terminal == LV_EVENT_ERROR);
        CHECK(changed.error.code == LV_ERROR_SOURCE_CHANGED);
        CHECK(changed.error.has_partial_output == 0);
        CHECK(engine.stats().cache_bytes == 0);
        CHECK(receive(engine, engine.request(engine.descriptor(path))).completion.source_bytes_read > 0);
    }
    const auto png = files.directory / "truncated.png";
    checkerboard(png, 64, 64, 7);
    std::filesystem::resize_file(png, std::filesystem::file_size(png) / 2);
    CachedEngine engine;
    const auto broken = receive(engine, engine.request(engine.descriptor(png.string(), true)));
    CHECK(broken.terminal == LV_EVENT_ERROR);
    CHECK(broken.coverage.empty());
    CHECK(engine.stats().cache_bytes == 0);
}

TEST_CASE("existing static codec outputs all participate in content caching", "[cache][formats]")
{
    Files files;
    const auto fixtures = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures";
    std::vector<std::string> paths{image(files, "cached.jpg")};
    for (const auto *name : {"red.bmp", "red-alpha.png", "red-alpha.webp", "red-alpha.svg", "red-first-frame.gif", "red.tiff"})
        paths.push_back((fixtures / name).string());
    CachedEngine engine(4 * mib, 48 * mib);
    for (const auto &path : paths)
    {
        CAPTURE(path);
        const auto first = receive(engine, engine.request(engine.descriptor(path)));
        REQUIRE(first.terminal == LV_EVENT_COMPLETE);
        const auto hit = receive(engine, engine.request(engine.descriptor(path)));
        REQUIRE(hit.terminal == LV_EVENT_COMPLETE);
        CHECK(hit.completion.source_bytes_read == 0);
        CHECK(hit.data == first.data);
    }
}
