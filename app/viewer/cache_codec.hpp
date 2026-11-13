#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include "memory_block.hpp"

namespace liltview
{
// Independent LZ4 blocks with a magic and XXH32 checksum. No persistent cache ABI:
// immutable memory cache blocks belong to one scene/process. Scratch is reused per worker.
class CacheEncoder
{
public:
    CacheEncoder();
    MemoryBlock encode(std::span<const std::uint8_t>);
private:
    std::vector<std::max_align_t> state_;
};
std::vector<std::uint8_t> decode_cache(std::span<const std::uint8_t>, std::size_t expected);
} // namespace liltview
