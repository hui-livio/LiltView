#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <zlib.h>
#include "viewer/tile_reader.hpp"

namespace
{
using liltview::TileReader;
struct File
{
    std::FILE *file = std::tmpfile();
    File()
    {
        if (!file)
            throw std::runtime_error("tmpfile");
    }
    ~File()
    {
        if (file)
            std::fclose(file);
    }
};
TileReader::Request job(std::uint64_t id)
{
    return {{id, 0}, {4, 4}, 0, 1};
}
TileReader::Data wait_for(TileReader &reader, TileReader::Key key)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;)
    {
        if (auto data = reader.find(key))
            return data;
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Tile read timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void wait_reads(TileReader &reader, std::uint64_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (reader.stats().reads < count)
    {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Tile worker timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
std::vector<std::uint8_t> fake_read(int, const TileReader::Request &request)
{
    return std::vector<std::uint8_t>(request.size.width * request.size.height * 4, static_cast<std::uint8_t>(request.key.tile));
}
struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, proceed = false;
    std::thread::id thread;
    std::vector<std::uint64_t> loaded;
    void release()
    {
        std::lock_guard lock(mutex);
        proceed = true;
        changed.notify_all();
    }
};
struct Release
{
    Gate &gate;
    ~Release()
    {
        gate.release();
    }
};
void LV_CALL wake(void *userdata)
{
    static_cast<std::atomic<unsigned> *>(userdata)->fetch_add(1);
}
} // namespace

TEST_CASE("tile cache I/O cannot block request or lookup and latest viewport replaces queued work", "[tile_reader]")
{
    File file;
    Gate gate;
    std::atomic<unsigned> notifications{0};
    TileReader reader(fileno(file.file), wake, &notifications, 128,
                      [&](int fd, const TileReader::Request &request)
                      {
                          std::unique_lock lock(gate.mutex);
                          gate.thread = std::this_thread::get_id();
                          gate.loaded.push_back(request.key.tile);
                          gate.entered = true;
                          gate.changed.notify_all();
                          gate.changed.wait(lock, [&] { return gate.proceed; });
                          return fake_read(fd, request);
                      });
    Release release{gate}; // Also release a gated loader if a REQUIRE aborts this test.
    reader.request({job(1), job(2)});
    {
        std::unique_lock lock(gate.mutex);
        REQUIRE(gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.entered; }));
    }
    // These calls complete while the I/O worker is deliberately blocked.
    CHECK_FALSE(reader.find({1, 0}));
    reader.request({job(3)});
    CHECK(reader.stats().queued == 1);
    {
        std::lock_guard lock(gate.mutex);
        CHECK_FALSE(gate.proceed);
        CHECK(gate.thread != std::this_thread::get_id());
    }
    gate.release();
    CHECK(wait_for(reader, {1, 0})->front() == 1);
    CHECK(wait_for(reader, {3, 0})->front() == 3);
    {
        std::lock_guard lock(gate.mutex);
        CHECK(gate.loaded == std::vector<std::uint64_t>{1, 3});
    }
    const auto reads = reader.stats().reads;
    reader.request({job(1), job(3)});
    CHECK(reader.stats().queued == 0);
    CHECK(reader.stats().reads == reads);
    CHECK(reader.stats().cached_bytes <= 128);
}

TEST_CASE("decoded tile RAM cache is bounded LRU and queued work has a fixed limit", "[tile_reader]")
{
    File file;
    TileReader reader(fileno(file.file), nullptr, nullptr, 128, fake_read);
    reader.request({job(1), job(2)});
    wait_for(reader, {1, 0});
    wait_for(reader, {2, 0});
    wait_for(reader, {1, 0}); // Refresh tile 1 before admitting tile 3.
    reader.request({job(3)});
    wait_for(reader, {3, 0});
    CHECK_FALSE(reader.find({2, 0}));
    CHECK(reader.find({1, 0}));
    CHECK(reader.stats().cached_bytes == 128);
    std::vector<TileReader::Request> many;
    for (unsigned i = 10; i < 110; ++i)
        many.push_back(job(i));
    reader.request(many);
    CHECK(reader.stats().queued <= TileReader::max_requests);
    CHECK(reader.stats().cached_bytes <= 128);
}

TEST_CASE("background cache reads use an owned fd without disturbing the writer seek position", "[tile_reader]")
{
    File file;
    const std::vector<std::uint8_t> pixels(64, 213);
    std::vector<std::uint8_t> compressed(compressBound(pixels.size()));
    uLongf size = compressed.size();
    REQUIRE(compress2(compressed.data(), &size, pixels.data(), pixels.size(), 1) == Z_OK);
    REQUIRE(std::fseek(file.file, 1000, SEEK_SET) == 0);
    REQUIRE(std::fwrite(compressed.data(), 1, size, file.file) == size);
    REQUIRE(std::fflush(file.file) == 0);
    REQUIRE(std::fseek(file.file, 17, SEEK_SET) == 0);
    TileReader reader(fileno(file.file));
    auto request   = job(7);
    request.offset = 1000;
    request.bytes  = static_cast<std::uint32_t>(size);
    reader.request({request});
    CHECK(*wait_for(reader, request.key) == pixels);
    CHECK(std::ftell(file.file) == 17);
    const auto reads = reader.stats().reads;
    reader.request({request});
    CHECK(*reader.find(request.key) == pixels);
    CHECK(reader.stats().reads == reads);
    std::fclose(file.file);
    file.file        = nullptr;
    request.key.tile = 8;
    reader.request({request}); // The duplicated fd keeps the unlinked cache alive.
    CHECK(*wait_for(reader, request.key) == pixels);
}

TEST_CASE("cached tile errors are reported without poisoning unrelated entries", "[tile_reader]")
{
    File file;
    std::atomic<unsigned> attempts{0};
    TileReader reader(fileno(file.file), nullptr, nullptr, 128,
                      [&](int fd, const TileReader::Request &request)
                      {
                          if (request.key.tile == 1 && attempts.fetch_add(1) == 0)
                              throw std::runtime_error("Injected read error");
                          return fake_read(fd, request);
                      });
    reader.request({job(1)});
    wait_reads(reader, 1);
    CHECK_THROWS_AS(reader.find({1, 0}), std::runtime_error);
    CHECK(reader.stats().cached_bytes == 0);
    reader.request({job(2)});
    CHECK(wait_for(reader, {2, 0})->front() == 2);
    const auto before = reader.stats().reads;
    reader.retry_errors();
    CHECK(reader.find({2, 0})); // A retry does not discard unrelated hot data.
    reader.request({job(1)});
    CHECK(wait_for(reader, {1, 0})->front() == 1);
    CHECK(reader.stats().reads == before + 1);
    auto invalid = job(3);
    invalid.size = {1024, 1024};
    CHECK_THROWS_AS(reader.request({invalid}), std::invalid_argument);
    CHECK_THROWS_AS(TileReader(-1), std::system_error);
}
