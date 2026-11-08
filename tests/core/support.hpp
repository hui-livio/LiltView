#pragma once
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <liltview/core.h>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace liltview::test
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

struct Engine
{
    lv_core *core = nullptr;
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t notifications = 0;
    bool gate = false, entered = false, proceed = false;
    std::chrono::seconds timeout{5};
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
    explicit Engine(std::uint32_t max = 1, std::uint64_t memory = 64ULL * 1024 * 1024, std::uint32_t sessions = 2,
                    std::uint64_t resident = 0, std::uint32_t tile_edge = 16)
    {
        lv_core_config config;
        lv_core_config_init(&config);
        config.cpu_budget_bytes                = memory;
        config.tile_edge                       = tile_edge;
        config.queue_pixel_bytes               = static_cast<std::uint64_t>(tile_edge) * tile_edge * 4;
        config.max_requests                    = max;
        config.wakeup                          = wake;
        config.wakeup_userdata                 = this;
        config.animation.max_sessions          = sessions;
        config.animation.resident_budget_bytes = resident;
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
        const auto deadline = std::chrono::steady_clock::now() + timeout;
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
struct Decoded
{
    lv_image_info info{};
    lv_event_type terminal = 0;
    lv_error_info error{};
    std::vector<unsigned char> pixels;
};
inline Decoded collect(Engine &engine)
{
    Decoded result;
    std::uint64_t area = 0, sequence = 0;
    for (;;)
    {
        Event event(engine.next());
        const auto &v = event.view();
        CHECK(v.sequence > sequence);
        sequence = v.sequence;
        if (v.type == LV_EVENT_IMAGE_INFO)
        {
            result.info = v.as.image;
            result.pixels.resize(static_cast<std::size_t>(result.info.canvas.width) * result.info.canvas.height * 4);
        }
        else if (v.type == LV_EVENT_REGION_READY)
        {
            REQUIRE_FALSE(result.pixels.empty());
            const auto &region = v.as.region;
            const auto &p      = *lv_pixels_get_view(region.pixels);
            REQUIRE(region.rect.x + region.rect.width <= result.info.canvas.width);
            REQUIRE(region.rect.y + region.rect.height <= result.info.canvas.height);
            for (unsigned y = 0; y < region.rect.height; ++y)
                std::copy_n(p.data + y * p.stride_bytes, region.rect.width * 4,
                            result.pixels.data() +
                                (static_cast<std::size_t>(region.rect.y + y) * result.info.canvas.width + region.rect.x) * 4);
            area += static_cast<std::uint64_t>(region.rect.width) * region.rect.height;
        }
        else
        {
            result.terminal = v.type;
            if (v.type == LV_EVENT_ERROR)
                result.error = v.as.error;
            if (v.type == LV_EVENT_COMPLETE)
                CHECK(area == static_cast<std::uint64_t>(result.info.canvas.width) * result.info.canvas.height);
            return result;
        }
    }
}
inline Decoded decode(const std::vector<unsigned char> &bytes, lv_size_u32 bounds = {100, 100})
{
    Files files;
    Engine engine;
    engine.submit(files.write("图片.wrong-extension", bytes), bounds);
    return collect(engine);
}
} // namespace liltview::test
