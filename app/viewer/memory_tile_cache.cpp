#include "memory_tile_cache.hpp"
#include "renderer/renderer.hpp"
#include <atomic>
#include <stdexcept>
namespace liltview
{
namespace { std::atomic<std::uint64_t> all_cache_bytes{0}; }
void reserve_tile_cache(std::uint64_t bytes)
{
    auto used = all_cache_bytes.load();
    do
    {
        if (bytes > MemoryTileCache::budget - used)
            throw render::Error(render::ErrorCode::memory_budget, "Active/retiring memory tile caches exceed 2 GiB");
    } while (!all_cache_bytes.compare_exchange_weak(used, used + bytes));
}
void release_tile_cache(std::uint64_t bytes) noexcept { all_cache_bytes.fetch_sub(bytes); }
MemoryTileCache::Block::Block(MemoryBlock source, bool encoded) : data(std::move(source)), compressed(encoded)
{
    charge = data.capacity() + 320;
    reserve_tile_cache(charge);
}
MemoryTileCache::Block::~Block() { release_tile_cache(charge); }
std::uint64_t MemoryTileCache::append(MemoryBlock data, bool compressed)
{
    if (data.empty() || data.size() > 8ULL * 1024 * 1024)
        throw std::invalid_argument("Invalid memory tile cache block");
    auto block = std::make_shared<const Block>(data, compressed);
    std::lock_guard lock(mutex_);
    const auto offset = next_;
    blocks_.emplace(offset, std::move(block));
    next_ += data.size();
    return offset;
}
std::shared_ptr<const MemoryTileCache::Block> MemoryTileCache::find(std::uint64_t offset, std::uint32_t bytes) const
{
    std::lock_guard lock(mutex_);
    const auto at = blocks_.find(offset);
    if (at == blocks_.end() || at->second->data.size() != bytes)
        throw std::runtime_error("Invalid memory tile cache reference");
    return at->second;
}
} // namespace liltview
