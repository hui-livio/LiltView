#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <jpeglib.h>
#include <liltview/core.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>

namespace
{
struct Files
{
    std::filesystem::path directory;
    Files()
    {
        std::array<char, 64> name{};
        std::strcpy(name.data(), "/tmp/liltview-jpeg-test-XXXXXX");
        auto *value = mkdtemp(name.data());
        if (!value)
            throw std::runtime_error("mkdtemp");
        directory = value;
    }
    ~Files()
    {
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
    }
    std::string write(const char *name, const std::vector<unsigned char> &bytes)
    {
        auto path = directory / name;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path.string();
    }
};

std::vector<unsigned char> jpeg(unsigned width = 40, unsigned height = 24, bool progressive = false, unsigned orientation = 1,
                                bool gray = false)
{
    jpeg_compress_struct encoder{};
    jpeg_error_mgr error{};
    encoder.err = jpeg_std_error(&error);
    jpeg_create_compress(&encoder);
    unsigned char *data = nullptr;
    unsigned long size  = 0;
    jpeg_mem_dest(&encoder, &data, &size);
    encoder.image_width      = width;
    encoder.image_height     = height;
    encoder.input_components = gray ? 1 : 3;
    encoder.in_color_space   = gray ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&encoder);
    jpeg_set_quality(&encoder, 95, TRUE);
    if (progressive)
        jpeg_simple_progression(&encoder);
    jpeg_start_compress(&encoder, TRUE);
    const std::array<unsigned char, 32> exif{{'E', 'x',  'i', 'f', 0,
                                              0,   'I',  'I', 42,  0,
                                              8,   0,    0,   0,   1,
                                              0,   0x12, 1,   3,   0,
                                              1,   0,    0,   0,   static_cast<unsigned char>(orientation),
                                              0,   0,    0,   0,   0,
                                              0,   0}};
    jpeg_write_marker(&encoder, JPEG_APP0 + 1, exif.data(), static_cast<unsigned>(exif.size()));
    std::vector<unsigned char> row(static_cast<std::size_t>(width) * (gray ? 1u : 3u), 0);
    for (unsigned x = 0; x < width; ++x)
    {
        if (gray)
            row[x] = 120;
        else
        {
            row[x * 3]     = 220;
            row[x * 3 + 1] = 40;
            row[x * 3 + 2] = 30;
        }
    }
    while (encoder.next_scanline < encoder.image_height)
    {
        JSAMPROW pointer = row.data();
        jpeg_write_scanlines(&encoder, &pointer, 1);
    }
    jpeg_finish_compress(&encoder);
    std::vector<unsigned char> result(data, data + size);
    free(data);
    jpeg_destroy_compress(&encoder);
    return result;
}

struct Engine
{
    lv_core *core = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t notifications = 0;
    bool gate = false, entered = false, proceed = false;
    static void LV_CALL wake(void *userdata)
    {
        auto &self = *static_cast<Engine *>(userdata);
        std::unique_lock lock(self.mutex);
        ++self.notifications;
        self.changed.notify_all();
        if (self.gate && !self.entered)
        {
            self.entered = true;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return self.proceed; });
        }
    }
    explicit Engine(std::uint32_t max = 1, std::uint64_t memory = 64ULL * 1024 * 1024)
    {
        lv_core_config config;
        lv_core_config_init(&config);
        config.cpu_budget_bytes  = memory;
        config.tile_edge         = 16;
        config.queue_pixel_bytes = 16 * 16 * 4;
        config.max_requests      = max;
        config.wakeup            = wake;
        config.wakeup_userdata   = this;
        if (lv_core_create(&config, &core) != LV_OK)
            throw std::runtime_error("core creation");
    }
    ~Engine()
    {
        {
            std::lock_guard lock(mutex);
            proceed = true;
            changed.notify_all();
        }
        lv_core_begin_shutdown(core);
        lv_core_join(core, 5000);
        lv_core_destroy(&core);
    }
    lv_request_id submit(const std::string &path, lv_size_u32 bounds = {100, 100})
    {
        lv_request_desc request;
        lv_request_desc_init(&request);
        request.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
        request.user_tag             = 789;
        request.target.as.fit_bounds = bounds;
        lv_request_id id             = 0;
        REQUIRE(lv_core_submit(core, &request, &id) == LV_OK);
        REQUIRE(id != 0);
        return id;
    }
    lv_event *next()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            std::unique_lock lock(mutex);
            const auto before = notifications;
            lock.unlock();
            lv_event *event   = nullptr;
            const auto status = lv_core_poll_event(core, &event);
            if (status == LV_OK)
                return event;
            REQUIRE(status == LV_EMPTY);
            lock.lock();
            if (!changed.wait_until(lock, deadline, [&] { return notifications != before; }))
                throw std::runtime_error("Timed out waiting for core event");
        }
    }
};
struct Event
{
    lv_event *event;
    explicit Event(lv_event *e) :
        event(e)
    {
    }
    ~Event()
    {
        lv_event_release(event);
    }
    const lv_event_view &view() const
    {
        return *lv_event_get_view(event);
    }
};
} // namespace

TEST_CASE("JPEG FIT tiles cover the image and retained pixels outlive terminal release", "[jpeg][lifetime]")
{
    Files files;
    Engine engine;
    const auto id = engine.submit(files.write("普通图片.JPG", jpeg()));
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    CHECK(info.view().as.image.canvas.width == 40);
    CHECK(info.view().as.image.canvas.height == 24);
    std::uint64_t sequence = info.view().sequence, area = 0, tiles = 0;
    lv_pixels *held = nullptr;
    for (;;)
    {
        Event event(engine.next());
        const auto &v = event.view();
        CHECK(v.request_id == id);
        CHECK(v.user_tag == 789);
        CHECK(v.sequence > sequence);
        sequence = v.sequence;
        if (v.type == LV_EVENT_COMPLETE)
        {
            CHECK(v.as.completion.final_tile_count == tiles);
            break;
        }
        REQUIRE(v.type == LV_EVENT_REGION_READY);
        ++tiles;
        const auto &r  = v.as.region;
        area          += static_cast<std::uint64_t>(r.rect.width) * r.rect.height;
        CHECK(r.flags == LV_REGION_FINAL);
        const auto &p = *lv_pixels_get_view(r.pixels);
        CHECK(p.data[0] > 200);
        CHECK(p.data[1] < 60);
        CHECK(p.data[3] == 255);
        if (!held)
        {
            held = r.pixels;
            lv_pixels_retain(held);
        }
    }
    CHECK(area == 40 * 24);
    CHECK(tiles == 6);
    CHECK(lv_core_cancel(engine.core, id) == LV_NOT_FOUND);
    REQUIRE(lv_core_begin_shutdown(engine.core) == LV_OK);
    REQUIRE(lv_core_join(engine.core, 5000) == LV_OK);
    CHECK(lv_core_destroy(&engine.core) == LV_BUSY);
    CHECK(lv_pixels_get_view(held)->data[3] == 255);
    lv_pixels_release(held);
    // INFO is still alive and valid, independently keeping the core busy.
    CHECK(info.view().as.image.source_revision != 0);
    CHECK(lv_core_destroy(&engine.core) == LV_BUSY);
}

TEST_CASE("JPEG supports progressive grayscale and EXIF FIT orientation", "[jpeg]")
{
    Files files;
    for (const bool progressive : {false, true})
    {
        Engine engine;
        engine.submit(files.write("gray.jpg", jpeg(40, 24, progressive, 6, true)), {10, 20});
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
        CHECK(info.view().as.image.orientation == 6);
        CHECK(info.view().as.image.canvas.width == 16);
        CHECK(info.view().as.image.canvas.height == 10);
        for (;;)
        {
            Event event(engine.next());
            if (event.view().type == LV_EVENT_COMPLETE)
                break;
            REQUIRE(event.view().type == LV_EVENT_REGION_READY);
            const auto &p = *lv_pixels_get_view(event.view().as.region.pixels);
            CHECK(p.data[0] == p.data[1]);
            CHECK(p.data[1] == p.data[2]);
            CHECK(p.data[3] == 255);
        }
    }
}

TEST_CASE("JPEG errors are asynchronous and do not produce a success terminal", "[jpeg][errors]")
{
    Files files;
    const auto good = jpeg();
    auto truncated  = good;
    truncated.resize(good.size() / 2);
    const std::array<std::pair<std::string, lv_error_code>, 4> cases{
        {{files.write("bad.jpg", truncated), LV_ERROR_CORRUPT_DATA},
         {files.write("empty.jpg", {}), LV_ERROR_CORRUPT_DATA},
         {files.write("other.jpg", {'a', 'b', 'c', 'd'}), LV_ERROR_FORMAT_UNSUPPORTED},
         {(files.directory / "missing.jpg").string(), LV_ERROR_IO}}};
    for (const auto &[path, expected] : cases)
    {
        Engine engine;
        engine.submit(path);
        for (;;)
        {
            Event e(engine.next());
            if (e.view().type == LV_EVENT_IMAGE_INFO)
                continue;
            REQUIRE(e.view().type == LV_EVENT_ERROR);
            CHECK(e.view().as.error.code == expected);
            break;
        }
    }
}

TEST_CASE("cancel wins before decode and shutdown releases a full pixel queue", "[jpeg][cancel]")
{
    Files files;
    Engine engine;
    engine.gate = true;
    auto id     = engine.submit(files.write("cancel.jpg", jpeg(128, 96)));
    {
        std::unique_lock lock(engine.mutex);
        REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
    }
    REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
    {
        std::lock_guard lock(engine.mutex);
        engine.proceed = true;
        engine.changed.notify_all();
    }
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    Event terminal(engine.next());
    CHECK(terminal.view().type == LV_EVENT_CANCELLED);
    CHECK(lv_core_begin_shutdown(engine.core) == LV_OK);
    CHECK(lv_core_join(engine.core, 5000) == LV_OK);
}

TEST_CASE("a one-slot engine preserves source identity while INFO is borrowed", "[jpeg][source]")
{
    Files files;
    Engine engine;
    auto path = files.write("same.jpg", jpeg(8, 8));
    engine.submit(path);
    Event first(engine.next());
    REQUIRE(first.view().type == LV_EVENT_IMAGE_INFO);
    for (;;)
    {
        Event e(engine.next());
        if (e.view().type == LV_EVENT_COMPLETE)
            break;
        REQUIRE(e.view().type == LV_EVENT_REGION_READY);
    }
    engine.submit(path);
    Event second(engine.next());
    REQUIRE(second.view().type == LV_EVENT_IMAGE_INFO);
    CHECK(second.view().as.image.source_revision == first.view().as.image.source_revision);
    CHECK(lv_core_begin_shutdown(engine.core) == LV_OK);
    CHECK(lv_core_join(engine.core, 5000) == LV_OK);
}

TEST_CASE("small CPU budget reports a permanent JPEG memory error", "[jpeg][budget]")
{
    Files files;
    Engine engine(1, 1024 * 1024);
    engine.submit(files.write("budget.jpg", jpeg()));
    Event e(engine.next());
    REQUIRE(e.view().type == LV_EVENT_ERROR);
    CHECK(e.view().as.error.code == LV_ERROR_MEMORY_BUDGET);
}

TEST_CASE("shutdown joins a worker blocked on a full one-tile queue", "[jpeg][shutdown]")
{
    Files files;
    Engine engine;
    engine.submit(files.write("full.jpg", jpeg(128, 96)));
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    }
    {
        std::unique_lock lock(engine.mutex);
        REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.notifications >= 2; }));
    }
    lv_core_stats stats{};
    REQUIRE(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.queued_pixel_bytes == 16 * 16 * 4);
    CHECK(lv_core_begin_shutdown(engine.core) == LV_OK);
    CHECK(lv_core_join(engine.core, 5000) == LV_OK);
    CHECK(lv_core_get_stats(engine.core, &stats) == LV_OK);
    CHECK(stats.queued_pixel_bytes == 0);
    CHECK(stats.borrowed_events == 0);
    CHECK(stats.accepted_requests == 0);
}

TEST_CASE("cancel after terminal commit preserves every final tile", "[jpeg][cancel]")
{
    Files files;
    Engine engine;
    const auto id = engine.submit(files.write("final.jpg", jpeg(8, 8)));
    {
        Event info(engine.next());
        REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    }
    {
        std::unique_lock lock(engine.mutex);
        REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.notifications >= 3; }));
    }
    REQUIRE(lv_core_cancel(engine.core, id) == LV_OK);
    Event tile(engine.next());
    REQUIRE(tile.view().type == LV_EVENT_REGION_READY);
    Event end(engine.next());
    CHECK(end.view().type == LV_EVENT_COMPLETE);
    CHECK(end.view().as.completion.final_tile_count == 1);
}

TEST_CASE("source mutation after INFO fails without publishing old pixels", "[jpeg][source]")
{
    Files files;
    Engine engine;
    engine.gate     = true;
    const auto path = files.write("changed.jpg", jpeg());
    engine.submit(path);
    {
        std::unique_lock lock(engine.mutex);
        REQUIRE(engine.changed.wait_for(lock, std::chrono::seconds(5), [&] { return engine.entered; }));
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output.put('\0');
    }
    {
        std::lock_guard lock(engine.mutex);
        engine.proceed = true;
        engine.changed.notify_all();
    }
    Event info(engine.next());
    REQUIRE(info.view().type == LV_EVENT_IMAGE_INFO);
    Event terminal(engine.next());
    REQUIRE(terminal.view().type == LV_EVENT_ERROR);
    CHECK(terminal.view().as.error.code == LV_ERROR_SOURCE_CHANGED);
    CHECK(terminal.view().as.error.has_partial_output == 0);
}

TEST_CASE("unsupported features reject synchronously and clear the request ID", "[jpeg][api]")
{
    Files files;
    Engine engine;
    std::string path = files.write("input.jpg", jpeg());
    lv_request_desc request;
    lv_request_desc_init(&request);
    request.path                 = {LV_PATH_POSIX_BYTES, path.data(), path.size()};
    request.target.as.fit_bounds = {40, 24};
    request.kind                 = LV_REQUEST_PREFETCH;
    request.priority             = LV_PRIORITY_PREFETCH;
    lv_request_id id             = 99;
    CHECK(lv_core_submit(engine.core, &request, &id) == LV_UNSUPPORTED);
    CHECK(id == 0);
    request.kind                 = LV_REQUEST_DISPLAY;
    request.priority             = LV_PRIORITY_CURRENT;
    request.target.as.fit_bounds = {0, 24};
    id                           = 99;
    CHECK(lv_core_submit(engine.core, &request, &id) == LV_INVALID_ARGUMENT);
    CHECK(id == 0);
    auto fifo = files.directory / "named-pipe.jpg";
    REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);
    engine.submit(fifo.string());
    Event e(engine.next());
    REQUIRE(e.view().type == LV_EVENT_ERROR);
    CHECK(e.view().as.error.code == LV_ERROR_IO);
}

TEST_CASE("oversized JPEG source is rejected even for a tiny FIT target", "[jpeg][limits]")
{
    Files files;
    auto data    = jpeg();
    bool changed = false;
    for (std::size_t i = 0; i + 8 < data.size(); ++i)
    {
        if (data[i] == 0xff && data[i + 1] == 0xc0)
        {
            data[i + 5] = data[i + 6] = data[i + 7] = data[i + 8] = 0xff;
            changed                                               = true;
            break;
        }
    }
    REQUIRE(changed);
    // The codec itself rejects 65535; also exercise our lower original-RGBA cap.
    for (std::size_t i = 0; i + 8 < data.size(); ++i)
    {
        if (data[i] == 0xff && data[i + 1] == 0xc0)
        {
            data[i + 5] = data[i + 7] = 0x40;
            data[i + 6] = data[i + 8] = 0;
            break;
        }
    }
    {
        Engine engine;
        engine.submit(files.write("huge-dimensions.jpg", data), {1, 1});
        Event error(engine.next());
        REQUIRE(error.view().type == LV_EVENT_ERROR);
        CHECK(error.view().as.error.code == LV_ERROR_DIMENSION_LIMIT);
    }
    const auto path = files.write("huge-file.jpg", jpeg());
    std::filesystem::resize_file(path, 64ULL * 1024 * 1024 + 1);
    Engine engine;
    engine.submit(path, {1, 1});
    Event error(engine.next());
    REQUIRE(error.view().type == LV_EVENT_ERROR);
    CHECK(error.view().as.error.code == LV_ERROR_DIMENSION_LIMIT);
}
