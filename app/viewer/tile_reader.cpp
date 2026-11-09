#include "tile_reader.hpp"
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <zlib.h>

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
std::vector<std::uint8_t> read_tile(int fd, const TileReader::Request &request)
{
    const auto bytes = raw_bytes(request);
    if (request.offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - request.bytes)
        throw std::invalid_argument("Cached tile file offset overflow");
    std::vector<std::uint8_t> compressed(request.bytes);
    std::size_t at = 0;
    while (at < compressed.size())
    {
        const auto count = pread(fd, compressed.data() + at, std::min<std::size_t>(65536, compressed.size() - at),
                                 static_cast<off_t>(request.offset + at));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            throw std::system_error(count < 0 ? errno : EIO, std::generic_category(), "Cannot read cached native tile");
        at += static_cast<std::size_t>(count);
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));
    uLongf length = data.size();
    if (uncompress(data.data(), &length, compressed.data(), compressed.size()) != Z_OK || length != data.size())
        throw std::system_error(EIO, std::generic_category(), "Invalid compressed native tile cache");
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
    int fd;
    lv_wakeup_fn wake;
    void *userdata;
    std::uint64_t budget, used = 0, clock = 0, reads = 0, hits = 0;
    Loader loader;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::deque<Request> pending;
    std::optional<Key> active;
    std::unordered_map<Key, Entry, KeyHash> cache;
    std::exception_ptr fatal;
    std::thread worker;

    State(int file, lv_wakeup_fn notify, void *user, std::uint64_t limit, Loader load) :
        fd(file),
        wake(notify),
        userdata(user),
        budget(limit),
        loader(load ? std::move(load) : read_tile)
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
                Data data;
                std::exception_ptr error;
                try
                {
                    auto loaded = loader(fd, request);
                    if (loaded.size() != raw_bytes(request))
                        throw std::runtime_error("Cached tile loader returned an invalid size");
                    data = std::make_shared<const std::vector<std::uint8_t>>(std::move(loaded));
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
TileReader::TileReader(int fd, lv_wakeup_fn wake, void *userdata, std::uint64_t budget, Loader loader)
{
    if (!budget)
        throw std::invalid_argument("A native tile RAM cache needs a nonzero budget");
    const int copy = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (copy < 0)
        throw std::system_error(errno, std::generic_category(), "Cannot duplicate native tile cache fd");
    try
    {
        state_         = std::make_unique<State>(copy, wake, userdata, budget, std::move(loader));
        state_->worker = std::thread([state = state_.get()] { state->run(); });
    } catch (...)
    {
        close(copy);
        throw;
    }
}
TileReader::~TileReader()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pending.clear();
    }
    state_->changed.notify_one();
    state_->worker.join();
    close(state_->fd);
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
    return {state_->reads, state_->hits, state_->used, state_->pending.size(), state_->cache.size(), state_->active.has_value()};
}
} // namespace liltview
