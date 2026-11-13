#include "cache_codec.hpp"
#include <cstring>
#include <lz4.h>
#include <xxhash.h>
#include <stdexcept>

namespace liltview
{
namespace
{
constexpr std::uint32_t magic = 0x344c564c; // LVL4, little endian
constexpr std::size_t header = 8;
constexpr std::size_t max_block = 4 * 1024 * 1024;
void store(std::uint8_t *p, std::uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(n >> (8 * i));
}
std::uint32_t load(const std::uint8_t *p)
{
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return n;
}
} // namespace
CacheEncoder::CacheEncoder() :
    state_((static_cast<std::size_t>(LZ4_sizeofState()) + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t))
{
}
MemoryBlock CacheEncoder::encode(std::span<const std::uint8_t> data)
{
    if (data.empty() || data.size() > max_block)
        throw std::invalid_argument("Invalid LZ4 cache block size");
    const auto bound = LZ4_compressBound(static_cast<int>(data.size()));
    std::vector<std::uint8_t> output_(header + static_cast<std::size_t>(bound));
    const auto bytes = LZ4_compress_fast_extState(state_.data(), reinterpret_cast<const char *>(data.data()),
        reinterpret_cast<char *>(output_.data() + header), static_cast<int>(data.size()), bound, 1);
    if (bytes <= 0) throw std::runtime_error("Cannot compress LZ4 cache block");
    store(output_.data(), magic);
    store(output_.data() + 4, XXH32(data.data(), data.size(), 0));
    output_.resize(header + static_cast<std::size_t>(bytes));
    return MemoryBlock(std::move(output_));
}
std::vector<std::uint8_t> decode_cache(std::span<const std::uint8_t> compressed, std::size_t expected)
{
    if (!expected || expected > max_block || compressed.size() <= header ||
        compressed.size() > header + static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(expected))) ||
        load(compressed.data()) != magic)
        throw std::runtime_error("Invalid LZ4 tile cache header");
    std::vector<std::uint8_t> data(expected);
    const auto bytes = LZ4_decompress_safe(reinterpret_cast<const char *>(compressed.data() + header),
        reinterpret_cast<char *>(data.data()), static_cast<int>(compressed.size() - header), static_cast<int>(expected));
    if (bytes != static_cast<int>(expected) || XXH32(data.data(), data.size(), 0) != load(compressed.data() + 4))
        throw std::runtime_error("Invalid LZ4 tile cache data/checksum");
    return data;
}
} // namespace liltview
