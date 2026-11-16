#ifndef LILTVIEW_TILE_READER_HPP
#define LILTVIEW_TILE_READER_HPP
#include <cstddef>
#include <cstdint>
#include <functional>
#include <liltview/core.h>
#include <memory>
#include <vector>
#include "renderer/renderer.hpp"
#include "memory_tile_cache.hpp"

namespace liltview
{
// In-memory cache lookup and decompression only. No source codec or viewer state is
// accessed by this worker. Jobs refer to immutable, already-generated LODs.
class TileReader
{
public:
    struct Key
    {
        std::uint64_t tile                 = 0;
        unsigned level                     = 0;
        bool operator==(const Key &) const = default;
    };
    struct Request
    {
        Key key;
        render::Size size;
        std::uint64_t offset;
        std::uint32_t bytes;
    };
    struct Stats
    {
        std::uint64_t reads = 0, hits = 0, cached_bytes = 0;
        std::size_t queued = 0, entries = 0;
        bool active = false;
        double read_ms = 0, decode_ms = 0, max_decode_ms = 0;
    };
    using Data                                    = MemoryBlock;
    using Loader                                  = std::function<std::vector<std::uint8_t>(const Request &)>;
    static constexpr std::size_t max_requests     = 64;
    static constexpr std::uint64_t default_budget = 32ULL * 1024 * 1024;

    TileReader(std::shared_ptr<const MemoryTileCache> cache, lv_wakeup_fn = nullptr, void *userdata = nullptr, std::uint64_t budget = default_budget, Loader = {});
    ~TileReader();
    TileReader(const TileReader &)            = delete;
    TileReader &operator=(const TileReader &) = delete;
    void cancel() noexcept;
    bool done() const noexcept;
    void request(const std::vector<Request> &); // Replace obsolete pending jobs, visible first.
    void retry_errors();                        // Explicit user retry; keep successful decoded entries.
    Data find(Key);                             // Never performs I/O or waits for a job; may rethrow its error.
    Stats stats() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace liltview
#endif
