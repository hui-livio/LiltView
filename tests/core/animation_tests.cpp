#include <gif_lib.h>
#include "support.hpp"

namespace
{
using namespace liltview::test;
using Bytes = std::vector<unsigned char>;
struct Frame
{
    unsigned x = 0, y = 0, width = 4, height = 3, index = 0;
    unsigned disposal = DISPOSE_DO_NOT, delay = 3;
    int transparent = NO_TRANSPARENT_COLOR;
    bool interlace = false, local = false;
};
Bytes animation(const std::vector<Frame> &frames, int repeat = -1)
{
    Bytes bytes;
    int error        = 0;
    GifFileType *gif = EGifOpen(
        &bytes,
        [](GifFileType *file, const GifByteType *data, int size)
        {
            auto &out = *static_cast<Bytes *>(file->UserData);
            out.insert(out.end(), data, data + size);
            return size;
        },
        &error);
    REQUIRE(gif);
    EGifSetGifVersion(gif, true);
    const GifColorType colors[]{{255, 0, 0}, {0, 0, 255}, {0, 255, 0}, {255, 255, 255}};
    ColorMapObject *map = GifMakeMapObject(4, colors);
    REQUIRE(map);
    REQUIRE(EGifPutScreenDesc(gif, 4, 3, 8, 2, map) == GIF_OK);
    if (repeat >= 0)
    {
        const unsigned char loop[]{1, static_cast<unsigned char>(repeat), static_cast<unsigned char>(repeat >> 8)};
        REQUIRE(EGifPutExtensionLeader(gif, APPLICATION_EXT_FUNC_CODE) == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(gif, 11, "NETSCAPE2.0") == GIF_OK);
        REQUIRE(EGifPutExtensionBlock(gif, 3, loop) == GIF_OK);
        REQUIRE(EGifPutExtensionTrailer(gif) == GIF_OK);
    }
    for (const auto &frame : frames)
    {
        GraphicsControlBlock control{};
        control.DisposalMode     = static_cast<int>(frame.disposal);
        control.DelayTime        = static_cast<int>(frame.delay);
        control.TransparentColor = frame.transparent;
        GifByteType block[4];
        REQUIRE(EGifGCBToExtension(&control, block) == 4);
        REQUIRE(EGifPutExtension(gif, GRAPHICS_EXT_FUNC_CODE, 4, block) == GIF_OK);
        REQUIRE(EGifPutImageDesc(gif, static_cast<int>(frame.x), static_cast<int>(frame.y), static_cast<int>(frame.width),
                                 static_cast<int>(frame.height), frame.interlace, frame.local ? map : nullptr) == GIF_OK);
        std::vector<GifPixelType> row(frame.width, static_cast<GifPixelType>(frame.index));
        for (unsigned y = 0; y < frame.height; ++y)
            REQUIRE(EGifPutLine(gif, row.data(), static_cast<int>(row.size())) == GIF_OK);
    }
    REQUIRE(EGifCloseFile(gif, &error) == GIF_OK);
    GifFreeMapObject(map);
    return bytes;
}
struct Job
{
    lv_event_type terminal = 0;
    lv_error_info error{};
    lv_animation_info info{};
    lv_animation_frame_info frame{};
    bool end = false;
    Bytes pixels;
    unsigned tiles = 0;
};
Job drain(Engine &engine, lv_size_u32 canvas = {4, 3})
{
    Job job;
    std::uint64_t sequence = 0;
    for (;;)
    {
        Event event(engine.next());
        const auto &v = event.view();
        CHECK(v.sequence > sequence);
        sequence = v.sequence;
        if (v.type == LV_EVENT_ANIMATION_INFO)
            job.info = v.as.animation;
        else if (v.type == LV_EVENT_ANIMATION_END)
        {
            job.end  = true;
            job.info = v.as.animation_end.final_info;
        }
        else if (v.type == LV_EVENT_ANIMATION_FRAME_INFO)
        {
            job.frame = v.as.frame;
            job.pixels.resize(static_cast<std::size_t>(canvas.width) * canvas.height * 4);
        }
        else if (v.type == LV_EVENT_ANIMATION_REGION_READY)
        {
            REQUIRE_FALSE(job.pixels.empty());
            CHECK(v.as.frame_region.key.animation_id == job.frame.key.animation_id);
            CHECK(v.as.frame_region.key.epoch == job.frame.key.epoch);
            CHECK(v.as.frame_region.key.frame_index == job.frame.key.frame_index);
            const auto &r = v.as.frame_region.region;
            const auto &p = *lv_pixels_get_view(r.pixels);
            REQUIRE(r.rect.x + r.rect.width <= canvas.width);
            REQUIRE(r.rect.y + r.rect.height <= canvas.height);
            for (unsigned y = 0; y < r.rect.height; ++y)
                std::copy_n(p.data + y * p.stride_bytes, r.rect.width * 4,
                            job.pixels.data() + (static_cast<std::size_t>(r.rect.y + y) * canvas.width + r.rect.x) * 4);
            ++job.tiles;
        }
        else
        {
            job.terminal = v.type;
            if (v.type == LV_EVENT_ERROR)
                job.error = v.as.error;
            if (v.type == LV_EVENT_COMPLETE)
                CHECK(v.as.completion.final_tile_count == job.tiles);
            return job;
        }
    }
}
std::pair<lv_animation_id, lv_request_id> open(Engine &engine, const std::string &path)
{
    lv_animation_open_desc desc;
    lv_animation_open_desc_init(&desc);
    desc.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    desc.target.as.fit_bounds = {100, 100};
    desc.user_tag             = 11;
    lv_animation_id id        = 0;
    lv_request_id request     = 0;
    REQUIRE(lv_core_open_animation(engine.core, &desc, &id, &request) == LV_OK);
    REQUIRE(id);
    return {id, request};
}
lv_status submit(Engine &engine, lv_animation_id id, lv_animation_action action = LV_ANIMATION_NEXT)
{
    lv_animation_request_desc request;
    lv_animation_request_desc_init(&request);
    request.animation_id = id;
    request.action       = action;
    lv_request_id out    = 999;
    auto status          = lv_core_submit_animation(engine.core, &request, &out);
    if (status == LV_OK)
        CHECK(out != 0);
    else
        CHECK(out == 0);
    return status;
}
Job next(Engine &engine, lv_animation_id id)
{
    REQUIRE(submit(engine, id) == LV_OK);
    return drain(engine);
}
} // namespace

TEST_CASE("animation sessions pull one frame with timing then END and reset epoch", "[animation]")
{
    Files files;
    Engine engine;
    const auto [id, request] = open(engine, files.write("no-extension", animation({Frame{}, Frame{.index = 1}}, 2)));
    (void)request;
    auto opened = drain(engine);
    REQUIRE(opened.terminal == LV_EVENT_COMPLETE);
    CHECK(opened.info.epoch == 1);
    CHECK(opened.info.image.capabilities == LV_CAP_ANIMATION);
    CHECK(opened.info.image.source_revision != 0);
    auto first = next(engine, id);
    REQUIRE(first.terminal == LV_EVENT_COMPLETE);
    CHECK(first.frame.key.frame_index == 0);
    CHECK(first.frame.source_duration_ns == 30000000);
    CHECK(first.frame.source_pts_ns == 0);
    CHECK(first.pixels[0] == 255);
    auto second = next(engine, id);
    REQUIRE(second.terminal == LV_EVENT_COMPLETE);
    CHECK(second.frame.key.frame_index == 1);
    CHECK(second.frame.source_pts_ns == 30000000);
    CHECK(second.pixels[2] == 255);
    auto end = next(engine, id);
    REQUIRE(end.terminal == LV_EVENT_COMPLETE);
    CHECK(end.end);
    CHECK(end.info.frame_count == 2);
    CHECK(end.info.total_plays == 3);
    CHECK(end.info.duration_ns == 60000000);
    CHECK(submit(engine, id) == LV_INVALID_STATE);
    REQUIRE(submit(engine, id, LV_ANIMATION_RESET) == LV_OK);
    auto reset = drain(engine);
    REQUIRE(reset.terminal == LV_EVENT_COMPLETE);
    CHECK(reset.info.epoch == 2);
    CHECK(reset.info.image.source_revision == opened.info.image.source_revision);
    auto again = next(engine, id);
    CHECK(again.frame.key.epoch == 2);
    CHECK(again.frame.key.frame_index == 0);
    CHECK(again.pixels == first.pixels);
    REQUIRE(lv_core_close_animation(engine.core, id) == LV_OK);
}

TEST_CASE("animation GIF disposal background previous transparency interlace and local palettes", "[animation][composition]")
{
    for (bool transparent : {false, true})
    {
        Files files;
        Engine engine;
        Frame red;
        Frame blue{.x = 1, .y = 1, .width = 2, .height = 1, .index = 1, .disposal = DISPOSE_BACKGROUND};
        blue.transparent = transparent ? 3 : NO_TRANSPARENT_COLOR;
        Frame white{.width       = 1,
                    .height      = 1,
                    .index       = 3,
                    .disposal    = DISPOSE_PREVIOUS,
                    .delay       = 0,
                    .transparent = transparent ? 2 : NO_TRANSPARENT_COLOR};
        Frame overlay{.x = 3, .y = 2, .width = 1, .height = 1, .index = 1, .interlace = true, .local = true};
        auto [id, request] = open(engine, files.write("test", animation({red, blue, white, overlay}, 0)));
        (void)request;
        REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
        auto a = next(engine, id), b = next(engine, id), c = next(engine, id), d = next(engine, id);
        REQUIRE(d.terminal == LV_EVENT_COMPLETE);
        CHECK(a.pixels[0] == 255);
        CHECK(b.pixels[(1 * 4 + 1) * 4 + 2] == 255);
        CHECK(c.pixels[0] == 255);
        CHECK(c.pixels[1] == 255);
        CHECK(c.frame.source_duration_ns == 0);
        CHECK(c.frame.flags & LV_ANIMATION_FRAME_DURATION_KNOWN);
        const auto offset = (1 * 4 + 1) * 4;
        CHECK(c.pixels[offset + 3] == (transparent ? 0 : 255));
        if (!transparent)
            CHECK(c.pixels[offset + 1] == 255);
        CHECK(d.pixels[0] == 255);
        CHECK(d.pixels[1] == 0); // restore previous removes the white pixel
        CHECK(d.pixels[(2 * 4 + 3) * 4 + 2] == 255);
        auto end = next(engine, id);
        CHECK(end.info.total_plays == 0);
    }
}

TEST_CASE("animation busy terminal lifetime cancel and reset preserve one-slot ownership", "[animation][cancel]")
{
    Files files;
    Engine engine;
    auto [id, request] = open(engine, files.write("g", animation({Frame{}, Frame{.index = 1}})));
    (void)request;
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_ANIMATION_INFO);
        CHECK(submit(engine, id) == LV_BUSY);
        Event terminal(engine.next());
        REQUIRE(terminal.view().type == LV_EVENT_COMPLETE);
        CHECK(submit(engine, id) == LV_BUSY);
    }
    {
        std::lock_guard lock(engine.mutex);
        engine.gate    = true;
        engine.entered = false;
        engine.proceed = false;
    }
    lv_animation_request_desc desc;
    lv_animation_request_desc_init(&desc);
    desc.animation_id   = id;
    lv_request_id frame = 0;
    REQUIRE(lv_core_submit_animation(engine.core, &desc, &frame) == LV_OK);
    {
        std::unique_lock lock(engine.mutex);
        REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
    }
    REQUIRE(lv_core_cancel(engine.core, frame) == LV_OK);
    {
        std::lock_guard lock(engine.mutex);
        engine.proceed = true;
        engine.changed.notify_all();
    }
    auto cancelled = drain(engine);
    CHECK(cancelled.terminal == LV_EVENT_CANCELLED);
    CHECK(submit(engine, id) == LV_INVALID_STATE);
    REQUIRE(submit(engine, id, LV_ANIMATION_RESET) == LV_OK);
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    CHECK(next(engine, id).frame.key.frame_index == 0);
}

TEST_CASE("animation borrowed frame pixels survive close and core join", "[animation][lifetime]")
{
    Files files;
    Engine engine;
    auto [id, request] = open(engine, files.write("g", animation({Frame{}})));
    (void)request;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    REQUIRE(submit(engine, id) == LV_OK);
    lv_pixels *held = nullptr;
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_ANIMATION_FRAME_INFO);
        Event region(engine.next());
        REQUIRE(region.view().type == LV_EVENT_ANIMATION_REGION_READY);
        held = region.view().as.frame_region.region.pixels;
        lv_pixels_retain(held);
        Event complete(engine.next());
        REQUIRE(complete.view().type == LV_EVENT_COMPLETE);
        REQUIRE(lv_core_close_animation(engine.core, id) == LV_OK);
        CHECK(info.view().as.frame.key.animation_id == id);
    }
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    CHECK(lv_core_destroy(&engine.core) == LV_BUSY);
    CHECK(lv_pixels_get_view(held)->data[0] == 255);
    lv_pixels_release(held);
    REQUIRE(lv_core_destroy(&engine.core) == LV_OK);
}

TEST_CASE("next animation frame cannot mutate retained earlier pixels", "[animation][lifetime]")
{
    Files files;
    Engine engine;
    auto [id, request] = open(engine, files.write("two", animation({Frame{}, Frame{.index = 1}})));
    (void)request;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    REQUIRE(submit(engine, id) == LV_OK);
    lv_pixels *held = nullptr;
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_ANIMATION_FRAME_INFO);
        Event pixels(engine.next());
        REQUIRE(pixels.view().type == LV_EVENT_ANIMATION_REGION_READY);
        held = pixels.view().as.frame_region.region.pixels;
        lv_pixels_retain(held);
        Event complete(engine.next());
        REQUIRE(complete.view().type == LV_EVENT_COMPLETE);
    }
    auto blue = next(engine, id);
    REQUIRE(blue.terminal == LV_EVENT_COMPLETE);
    CHECK(blue.pixels[2] == 255);
    CHECK(lv_pixels_get_view(held)->data[0] == 255);
    CHECK(lv_pixels_get_view(held)->data[2] == 0);
    lv_pixels_release(held);
}

TEST_CASE("animation source replacement invalidates session and corrupt later frames do not become END", "[animation][source]")
{
    Files files;
    Engine engine;
    auto data          = animation({Frame{}, Frame{.index = 1}});
    auto path          = files.write("changed", data);
    auto [id, request] = open(engine, path);
    (void)request;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    REQUIRE(next(engine, id).terminal == LV_EVENT_COMPLETE);
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.put('x');
    }
    auto changed = next(engine, id);
    REQUIRE(changed.terminal == LV_EVENT_ERROR);
    CHECK(changed.error.code == LV_ERROR_SOURCE_CHANGED);
    CHECK(submit(engine, id, LV_ANIMATION_RESET) == LV_INVALID_STATE);
    lv_core_close_animation(engine.core, id);
    data.pop_back(); // missing GIF trailer
    auto [other, second] = open(engine, files.write("truncated", data));
    (void)second;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    REQUIRE(next(engine, other).terminal == LV_EVENT_COMPLETE);
    REQUIRE(next(engine, other).terminal == LV_EVENT_COMPLETE);
    auto bad = next(engine, other);
    CHECK(bad.terminal == LV_EVENT_ERROR);
    CHECK(bad.error.code == LV_ERROR_CORRUPT_DATA);
    CHECK_FALSE(bad.end);
}

TEST_CASE("animation limits disabled ABI mismatch and resident admission reject cleanly", "[animation][budget]")
{
    Files files;
    const auto path = files.write("g", animation({Frame{}}));
    {
        Engine engine(1, 64 * 1024 * 1024, 0);
        lv_animation_open_desc desc;
        lv_animation_open_desc_init(&desc);
        desc.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
        desc.target.as.fit_bounds = {4, 3};
        lv_animation_id id        = 123;
        lv_request_id request     = 123;
        CHECK(lv_core_open_animation(engine.core, &desc, &id, &request) == LV_UNSUPPORTED);
        CHECK(id == 0);
        CHECK(request == 0);
        desc.struct_size = 0;
        CHECK(lv_core_open_animation(engine.core, &desc, &id, &request) == LV_ABI_MISMATCH);
    }
    {
        Engine engine(1, 64 * 1024 * 1024, 2, 512 * 1024);
        auto [id, request] = open(engine, path);
        (void)request;
        auto failed = drain(engine);
        REQUIRE(failed.terminal == LV_EVENT_ERROR);
        CHECK(failed.error.code == LV_ERROR_MEMORY_BUDGET);
        CHECK(submit(engine, id) == LV_INVALID_STATE);
        CHECK(lv_core_close_animation(engine.core, id) == LV_OK);
    }
}

TEST_CASE("animation cleanup wakes BUSY open even when no request produces an event", "[animation][wakeup]")
{
    Files files;
    Engine engine(1, 64 * 1024 * 1024, 1);
    const auto path    = files.write("g", animation({Frame{}}));
    auto [id, request] = open(engine, path);
    (void)request;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    lv_animation_open_desc desc;
    lv_animation_open_desc_init(&desc);
    desc.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    desc.target.as.fit_bounds = {4, 3};
    lv_animation_id other     = 0;
    lv_request_id job         = 0;
    REQUIRE(lv_core_open_animation(engine.core, &desc, &other, &job) == LV_BUSY);
    REQUIRE(lv_core_close_animation(engine.core, id) == LV_OK);
    // Arm by checking resources after sampling notifications. A missed wake cannot
    // be masked by polling in a sleep loop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        std::unique_lock lock(engine.mutex);
        auto before = engine.notifications;
        lock.unlock();
        auto status = lv_core_open_animation(engine.core, &desc, &other, &job);
        if (status == LV_OK)
            break;
        REQUIRE(status == LV_BUSY);
        lock.lock();
        REQUIRE(engine.changed.wait_until(lock, deadline, [&] { return engine.notifications != before; }));
    }
    CHECK(other != id);
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
}

TEST_CASE("animation shutdown releases full tile queues without polling", "[animation][shutdown]")
{
    Files files;
    Engine engine;
    // Tiny output is enough to fill the one-tile queue; terminal still reserves
    // its independent control record. begin_shutdown never needs UI draining.
    auto [id, request] = open(engine, files.write("g", animation({Frame{}})));
    (void)request;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    REQUIRE(submit(engine, id) == LV_OK);
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_ANIMATION_FRAME_INFO);
    }
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    lv_core_stats stats{};
    REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.accepted_requests == 0);
    CHECK(stats.queued_pixel_bytes == 0);
}

TEST_CASE("idle animation residents cannot deadlock single-worker static admission", "[animation][budget]")
{
    Files files;
    Engine engine(1, 3 * 1024 * 1024, 2, 3 * 1024 * 1024);
    const auto path = files.write("resident", animation({Frame{}}));
    auto [first, a] = open(engine, path);
    (void)a;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    auto [second, b] = open(engine, path);
    (void)b;
    REQUIRE(drain(engine).terminal == LV_EVENT_COMPLETE);
    engine.submit(path);
    auto rejected = collect(engine);
    REQUIRE(rejected.terminal == LV_EVENT_ERROR);
    CHECK(rejected.error.code == LV_ERROR_MEMORY_BUDGET);
    REQUIRE(lv_core_close_animation(engine.core, first) == LV_OK);
    engine.submit(path); // Cleanup must run before this worker's next admission.
    CHECK(collect(engine).terminal == LV_EVENT_COMPLETE);
    CHECK(lv_core_close_animation(engine.core, second) == LV_OK);
}
