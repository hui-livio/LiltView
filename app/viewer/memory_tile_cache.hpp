#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include "memory_block.hpp"
namespace liltview
{
// Shared admission across current, pending and retiring scenes. Payload capacity
// plus conservative per-block metadata are charged; no cache file or OS paging API.
void reserve_tile_cache(std::uint64_t);
void release_tile_cache(std::uint64_t) noexcept;
class MemoryTileCache
{
public:
    static constexpr std::uint64_t budget = 2ULL * 1024 * 1024 * 1024;
    struct Block
    {
        explicit Block(MemoryBlock, bool compressed);
        ~Block();
        MemoryBlock data;
        bool compressed; // Business metadata, not part of the generic C memory block.
        std::uint64_t charge = 0;
    };
    std::uint64_t append(MemoryBlock, bool compressed = true);
    std::shared_ptr<const Block> find(std::uint64_t offset, std::uint32_t bytes) const;
private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<const Block>> blocks_;
    std::uint64_t next_ = 0;
};
} // namespace liltview
