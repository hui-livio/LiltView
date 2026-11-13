#include "tile_reader.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include "cache_codec.hpp"

namespace liltview
{
namespace
{
constexpr std::uint64_t max_tile_bytes = 4ULL * 1024 * 1024;
std::uint64_t raw_bytes(const TileReader::Request &request)
{
    const auto size = static_cast<std::uint64_t>(request.size.width) * request.size.height;
    if (!size || size > max_tile_bytes / 4 || !request.bytes || request.bytes > max_tile_bytes * 2)
        throw std::invalid_argument("Invalid cached tile read size");
    return size * 4;
}
MemoryBlock read_tile(const MemoryTileCache &cache, const TileReader::Request &request, double &read_ms, double &decode_ms)
{
    const auto start = std::chrono::steady_clock::now();
    const auto bytes = raw_bytes(request);
    const auto compressed = cache.find(request.offset, request.bytes);
    const auto read_done = std::chrono::steady_clock::now();
    read_ms = std::chrono::duration<double, std::milli>(read_done - start).count();
    if (!compressed->compressed)
    {
        if (compressed->data.size() != bytes)
            throw std::runtime_error("Invalid raw tile cache data size");
        return compressed->data; // Retain the same C memory block, no decode or payload copy.
    }
    MemoryBlock data(decode_cache(compressed->data, static_cast<std::size_t>(bytes)));
    decode_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - read_done).count();
    return data;
}
struct KeyHash
{
    std::size_t operator()(TileReader::Key key) const
    {
        return std::hash<std::uint64_t>{}(key.tile) ^ (std::hash<unsigned>{}(key.level) << 1);
    }
};
} // namespace
struct TileReader::State
{
    struct Entry
    {
        Data data;
        std::exception_ptr error;
        std::uint64_t touched;
    };
    std::shared_ptr<const MemoryTileCache> source;
    lv_wakeup_fn wake;
    void *userdata;
    std::uint64_t budget, used = 0, clock = 0, reads = 0, hits = 0;
    Loader loader;
    double read_ms = 0, decode_ms = 0, max_decode_ms = 0;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::atomic<bool> finished{false};
    std::deque<Request> pending;
    std::optional<Key> active;
    std::unordered_map<Key, Entry, KeyHash> cache;
    std::exception_ptr fatal;
    std::thread worker;

    State(std::shared_ptr<const MemoryTileCache> cache, lv_wakeup_fn notify, void *user, std::uint64_t limit, Loader load) :
        source(std::move(cache)),
        wake(notify),
        userdata(user),
        budget(limit),
        loader(std::move(load))
    {
    }
    void run() noexcept
    {
        try
        {
            for (;;)
            {
                Request request;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || !pending.empty(); });
                    if (stopping)
                        return;
                    request = pending.front();
                    pending.pop_front();
                    active = request.key;
                }
                double io_elapsed = 0, decode_elapsed = 0;
                Data data;
                std::exception_ptr error;
                try
                {
                    auto loaded = loader ? MemoryBlock(loader(request)) : read_tile(*source, request, io_elapsed, decode_elapsed);
                    if (loaded.size() != raw_bytes(request))
                        throw std::runtime_error("Cached tile loader returned an invalid size");
                    data = MemoryBlock(std::move(loaded));
                } catch (...)
                {
                    error = std::current_exception();
                }
                {
                    std::lock_guard lock(mutex);
                    active.reset();
                    if (stopping)
                        return;
                    ++reads;
                    read_ms += io_elapsed;
                    decode_ms += decode_elapsed;
                    max_decode_ms = std::max(max_decode_ms, decode_elapsed);
                    const auto bytes = data ? data->size() : 0;
                    while (!cache.empty() && (used + bytes > budget || cache.size() >= 512))
                    {
                        const auto oldest = std::min_element(cache.begin(), cache.end(), [](const auto &a, const auto &b)
                                                             { return a.second.touched < b.second.touched; });
                        if (oldest->second.data)
                            used -= oldest->second.data->size();
                        cache.erase(oldest);
                    }
                    cache.insert_or_assign(request.key, Entry{std::move(data), error, ++clock});
                    used += bytes;
                }
                if (wake)
                    wake(userdata);
            }
        } catch (...)
        {
            {
                std::lock_guard lock(mutex);
                fatal = std::current_exception();
                active.reset();
                pending.clear();
            }
            if (wake)
                wake(userdata);
        }
    }
};
TileReader::TileReader(std::shared_ptr<const MemoryTileCache> cache, lv_wakeup_fn wake, void *userdata, std::uint64_t budget, Loader loader)
{
    if (!budget)
        throw std::invalid_argument("A native tile RAM cache needs a nonzero budget");
    if (!cache) throw std::invalid_argument("Missing memory tile cache");
    state_ = std::make_unique<State>(std::move(cache), wake, userdata, budget, std::move(loader));
    state_->worker = std::thread([state = state_.get()] {
        state->run();
        // Large CPU cleanup is part of worker retirement, before done is visible.
        state->source.reset();
        {
            std::lock_guard lock(state->mutex);
            state->cache.clear();
            state->used = 0;
        }
        state->finished.store(true, std::memory_order_release);
        if (state->wake) state->wake(state->userdata);
    });
}

void TileReader::cancel() noexcept
{
    { std::lock_guard lock(state_->mutex); state_->stopping = true; state_->pending.clear(); }
    state_->changed.notify_one();
}
bool TileReader::done() const noexcept { return state_->finished.load(std::memory_order_acquire); }
TileReader::~TileReader()
{
    cancel();
    state_->worker.join();
}
void TileReader::request(const std::vector<Request> &requests)
{
    auto &state = *state_;
    // Validate before changing the queue; requests are private generated cache metadata.
    for (std::size_t i = 0; i < std::min(requests.size(), max_requests); ++i)
        if (raw_bytes(requests[i]) > state.budget)
            throw std::invalid_argument("A cached tile exceeds its RAM cache budget");
    {
        std::lock_guard lock(state.mutex);
        if (state.fatal)
            std::rethrow_exception(state.fatal);
        state.pending.clear();
        for (const auto &request : requests)
        {
            if (state.cache.contains(request.key) || (state.active && *state.active == request.key))
                continue;
            if (std::any_of(state.pending.begin(), state.pending.end(),
                            [&](const Request &queued) { return queued.key == request.key; }))
                continue;
            if (state.pending.size() == max_requests)
                break;
            // All callers bound requests to max_requests; keep public input safe too.
            if (raw_bytes(request) > state.budget)
                throw std::invalid_argument("A cached tile exceeds its RAM cache budget");
            state.pending.push_back(request);
        }
    }
    state.changed.notify_one();
}
void TileReader::retry_errors()
{
    std::lock_guard lock(state_->mutex);
    for (auto at = state_->cache.begin(); at != state_->cache.end();)
        if (at->second.error)
            at = state_->cache.erase(at);
        else
            ++at;
}
TileReader::Data TileReader::find(Key key)
{
    std::lock_guard lock(state_->mutex);
    if (state_->fatal)
        std::rethrow_exception(state_->fatal);
    const auto found = state_->cache.find(key);
    if (found == state_->cache.end())
        return {};
    if (found->second.error)
        std::rethrow_exception(found->second.error);
    ++state_->hits;
    found->second.touched = ++state_->clock;
    return found->second.data;
}
TileReader::Stats TileReader::stats() const
{
    std::lock_guard lock(state_->mutex);
    return {state_->reads, state_->hits, state_->used, state_->pending.size(), state_->cache.size(), state_->active.has_value(),
        state_->read_ms, state_->decode_ms, state_->max_decode_ms};
}
} // namespace liltview
