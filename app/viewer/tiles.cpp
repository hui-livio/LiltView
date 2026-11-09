#include "tiles.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <zlib.h>

namespace liltview
{
namespace
{
constexpr std::uint64_t max_cache_bytes = 16ULL * 1024 * 1024 * 1024;
void cache_error(const char *message)
{
    throw std::system_error(errno ? errno : EIO, std::generic_category(), message);
}
void seek(std::FILE *file, std::uint64_t offset)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0)
        cache_error("Cannot seek native tile cache");
}
std::optional<std::uint32_t> uniform_color(const std::vector<std::uint8_t> &data)
{
    std::uint32_t first;
    std::memcpy(&first, data.data(), 4);
    for (std::size_t at = 4; at < data.size(); at += 4)
    {
        std::uint32_t pixel;
        std::memcpy(&pixel, data.data() + at, 4);
        if (pixel != first)
            return {};
    }
    return first;
}
std::vector<std::uint8_t> solid(render::Size size, std::uint32_t color)
{
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.width) * size.height * 4);
    std::memcpy(data.data(), &color, 4);
    for (std::size_t at = 4; at < data.size();)
    {
        const auto count = std::min(at, data.size() - at);
        std::memcpy(data.data() + at, data.data(), count);
        at += count;
    }
    return data;
}

// Built once during native decode, not on zoom. Alpha-correct box filtering
// supplies texture LODs without keeping a separate full-image thumbnail.
std::vector<std::uint8_t> reduce(const std::vector<std::uint8_t> &in, render::Size size)
{
    const render::Size next{(size.width + 1) / 2, (size.height + 1) / 2};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(next.width) * next.height * 4);
    for (unsigned y = 0; y < next.height; ++y)
        for (unsigned x = 0; x < next.width; ++x)
        {
            unsigned alpha = 0, color[3]{}, count = 0;
            for (unsigned dy = 0; dy < 2 && y * 2 + dy < size.height; ++dy)
                for (unsigned dx = 0; dx < 2 && x * 2 + dx < size.width; ++dx)
                {
                    const auto at  = (static_cast<std::size_t>(y * 2 + dy) * size.width + x * 2 + dx) * 4;
                    alpha         += in[at + 3];
                    for (unsigned c = 0; c < 3; ++c)
                        color[c] += static_cast<unsigned>(in[at + c]) * in[at + 3];
                    ++count;
                }
            const auto at = (static_cast<std::size_t>(y) * next.width + x) * 4;
            for (unsigned c = 0; c < 3; ++c)
                out[at + c] = static_cast<std::uint8_t>(alpha ? (color[c] + alpha / 2) / alpha : 0);
            out[at + 3] = static_cast<std::uint8_t>((alpha + count / 2) / count);
        }
    return out;
}
} // namespace
void Tiles::begin(const lv_image_info &info)
{
    clear();
    info_              = info;
    const auto columns = (static_cast<std::uint64_t>(info.canvas.width) + info.tile_edge - 1) / info.tile_edge;
    const auto rows    = (static_cast<std::uint64_t>(info.canvas.height) + info.tile_edge - 1) / info.tile_edge;
    if (columns * rows > 128ULL * 1024 * 1024 / sizeof(Tile))
        throw render::Error(render::ErrorCode::memory_budget, "Native scene tile index exceeds budget");
    tiles_.resize(static_cast<std::size_t>(columns * rows));
    const auto budget    = std::max<std::uint64_t>(1, renderer_.capabilities().image_budget_bytes / 4);
    long double estimate = static_cast<long double>(info.canvas.width) * info.canvas.height * 4 * 4 / 3;
    while (estimate > budget && (info.tile_edge >> overview_level_) > 1)
    {
        ++overview_level_;
        estimate /= 4;
    }
    if (estimate > budget)
        throw render::Error(render::ErrorCode::memory_budget, "Native scene texture index exceeds GPU budget");
    if (overview_level_)
    {
        open_cache();
        const unsigned factor = 1u << overview_level_;
        const unsigned width  = (info.canvas.width - 1) / factor + 1;
        const unsigned height = (info.canvas.height - 1) / factor + 1;
        atlas_edge_           = std::bit_floor(std::min(1024u, renderer_.capabilities().max_texture_edge));
        atlas_columns_        = (width - 1) / atlas_edge_ + 1;
        for (unsigned y = 0; y < height; y += atlas_edge_)
            for (unsigned x = 0; x < width; x += atlas_edge_)
                atlases_.push_back({{x, y, std::min(atlas_edge_, width - x), std::min(atlas_edge_, height - y)}, {}});
    }
}
void Tiles::open_cache()
{
    std::filesystem::path directory;
    if (const auto *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        directory = xdg;
    else if (const auto *home = std::getenv("HOME"); home && *home)
        directory = std::filesystem::path(home) / ".cache";
    else
        directory = std::filesystem::temp_directory_path();
    directory /= "liltview";
    std::filesystem::create_directories(directory);
    auto name    = (directory / "native-XXXXXX").string();
    const int fd = mkstemp(name.data());
    if (fd < 0)
        cache_error("Cannot create native tile cache");
    // Anonymous after opening: reclaimed on close, errors, cancellation or crash.
    if (unlink(name.c_str()) != 0)
    {
        close(fd);
        cache_error("Cannot unlink native tile cache");
    }
    cache_ = fdopen(fd, "w+b");
    if (!cache_)
    {
        close(fd);
        cache_error("Cannot open native tile cache");
    }
}
void Tiles::release_details() noexcept
{
    for (auto &tile : tiles_)
    {
        renderer_.destroy_image(tile.detail);
        tile.detail = {};
    }
}
void Tiles::clear() noexcept
{
    for (const auto &tile : tiles_)
    {
        if (!cache_)
            renderer_.destroy_image(tile.overview);
        renderer_.destroy_image(tile.detail);
    }
    tiles_.clear();
    for (const auto &atlas : atlases_)
        renderer_.destroy_image(atlas.image);
    atlases_.clear();
    uniform_levels_.clear();
    atlas_edge_ = atlas_columns_ = 0;
    if (cache_)
        std::fclose(cache_);
    cache_       = nullptr;
    info_        = {};
    ready_       = 0;
    cache_bytes_ = clock_ = 0;
    overview_level_       = 0;
}
void Tiles::swap(Tiles &other)
{
    tiles_.swap(other.tiles_);
    atlases_.swap(other.atlases_);
    uniform_levels_.swap(other.uniform_levels_);
    std::swap(atlas_edge_, other.atlas_edge_);
    std::swap(atlas_columns_, other.atlas_columns_);
    std::swap(info_, other.info_);
    std::swap(cache_, other.cache_);
    std::swap(cache_bytes_, other.cache_bytes_);
    std::swap(clock_, other.clock_);
    std::swap(ready_, other.ready_);
    std::swap(overview_level_, other.overview_level_);
}
void Tiles::make_room(std::uint64_t bytes)
{
    auto caps = renderer_.capabilities();
    while (bytes > caps.image_budget_bytes - caps.image_bytes)
    {
        Tile *oldest = nullptr;
        for (auto &tile : tiles_)
            if (tile.detail && tile.touched != clock_ && (!oldest || tile.touched < oldest->touched))
                oldest = &tile;
        if (!oldest)
            throw render::Error(render::ErrorCode::memory_budget, "Visible native textures exceed GPU budget");
        renderer_.destroy_image(oldest->detail);
        oldest->detail = {};
        caps           = renderer_.capabilities();
    }
}
render::Image Tiles::upload_texture(render::Size size, unsigned level, const std::uint8_t *data, std::uint64_t stride,
                                    std::uint64_t bytes)
{
    const render::ImageDesc desc{
        size, true, level == 0 ? render::ImageDesc::Magnification::nearest : render::ImageDesc::Magnification::linear};
    make_room(render::image_storage_bytes(desc));
    if (!stride)
        stride = static_cast<std::uint64_t>(size.width) * 4;
    if (!bytes)
        bytes = stride * (size.height - 1) + static_cast<std::uint64_t>(size.width) * 4;
    auto image = renderer_.create_image(desc);
    try
    {
        renderer_.update_image(image, {0, 0, size.width, size.height}, {data, bytes, stride, size});
    } catch (...)
    {
        renderer_.destroy_image(image);
        throw;
    }
    return image;
}
void Tiles::upload_overview(Tile &tile, render::Size size, unsigned level, const std::uint8_t *data)
{
    // Shared texture atlases let ImGui batch thousands of native tiles in FIT.
    // Without atlases a 32K scene would cause tens of thousands of GL draws.
    const unsigned x = tile.rect.x >> overview_level_, y = tile.rect.y >> overview_level_;
    auto &atlas = atlases_.at(static_cast<std::size_t>(y / atlas_edge_) * atlas_columns_ + x / atlas_edge_);
    if (!atlas.image)
    {
        const render::ImageDesc desc{{atlas.rect.width, atlas.rect.height}, false};
        make_room(render::image_storage_bytes(desc));
        atlas.image = renderer_.create_image(desc);
    }
    const render::Rect destination{x - atlas.rect.x, y - atlas.rect.y, size.width, size.height};
    renderer_.update_image(
        atlas.image, destination,
        {data, static_cast<std::uint64_t>(size.width) * size.height * 4, static_cast<std::uint64_t>(size.width) * 4, size});
    tile.overview       = atlas.image;
    tile.overview_level = level;
    const float u0      = static_cast<float>(destination.x) / static_cast<float>(atlas.rect.width);
    const float v0      = static_cast<float>(destination.y) / static_cast<float>(atlas.rect.height);
    const float u1      = static_cast<float>(destination.x + size.width) / static_cast<float>(atlas.rect.width);
    const float v1      = static_cast<float>(destination.y + size.height) / static_cast<float>(atlas.rect.height);
    tile.overview_uv    = {{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
}

void Tiles::upload(const lv_region_info &region)
{
    if (region.tile_id >= tiles_.size())
        throw render::Error(render::ErrorCode::invalid_argument, "Invalid native tile ID");
    auto &tile = tiles_[static_cast<std::size_t>(region.tile_id)];
    if (tile.overview)
        return;
    const auto &pixels = *lv_pixels_get_view(region.pixels);
    tile.rect          = region.rect;
    render::Size size{region.rect.width, region.rect.height};
    if (!cache_)
    {
        // Borrow final core pixels; packing is only needed for cache generation.
        tile.overview    = upload_texture(size, 0, pixels.data, pixels.stride_bytes, pixels.byte_count);
        tile.overview_uv = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        ++ready_;
        return;
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.width) * size.height * 4);
    for (unsigned y = 0; y < size.height; ++y)
        std::copy_n(pixels.data + y * pixels.stride_bytes, static_cast<std::size_t>(size.width) * 4,
                    data.data() + static_cast<std::size_t>(y) * size.width * 4);
    unsigned level = 0;
    {
        const auto color = uniform_color(data);
        const auto key   = std::make_tuple(color.value_or(0), size.width, size.height);
        if (color)
        {
            if (const auto found = uniform_levels_.find(key); found != uniform_levels_.end())
            {
                tile.levels       = found->second;
                const auto index  = std::min(overview_level_, static_cast<unsigned>(tile.levels.size() - 1));
                const auto pixels = solid(tile.levels[index].size, *color);
                upload_overview(tile, tile.levels[index].size, index, pixels.data());
                ++ready_;
                return;
            }
        }
        // Native pixels and all LODs are generated/stored once. Subsequent view
        // changes only upload these immutable cached bytes, never reopen source.
        for (;; ++level)
        {
            std::vector<std::uint8_t> compressed(compressBound(static_cast<uLong>(data.size())));
            uLongf bytes = compressed.size();
            if (compress2(compressed.data(), &bytes, data.data(), static_cast<uLong>(data.size()), Z_BEST_SPEED) != Z_OK)
                throw std::runtime_error("Cannot compress native tile cache");
            if (cache_bytes_ + bytes > max_cache_bytes)
                throw render::Error(render::ErrorCode::memory_budget, "Native tile disk cache exceeds 16 GiB limit");
            seek(cache_, cache_bytes_);
            if (std::fwrite(compressed.data(), 1, bytes, cache_) != bytes)
                cache_error("Cannot write native tile cache");
            tile.levels.push_back({size, cache_bytes_, static_cast<std::uint32_t>(bytes)});
            cache_bytes_ += bytes;
            if (level == overview_level_ || (size.width == 1 && size.height == 1 && !tile.overview))
            {
                upload_overview(tile, size, level, data.data());
            }
            if (size.width == 1 && size.height == 1)
                break;
            const render::Size next{(size.width + 1) / 2, (size.height + 1) / 2};
            data = color ? solid(next, *color) : reduce(data, size);
            size = next;
        }
        if (color && uniform_levels_.size() < 4096)
            uniform_levels_.emplace(key, tile.levels);
    }
    ++ready_;
}
unsigned Tiles::level_for(const Viewport &view) const
{
    return static_cast<unsigned>(std::max(0.0, std::floor(-std::log2(view.scale()))));
}
bool Tiles::visible(const Tile &tile, lv_rect_u32 visible) const
{
    if (!tile.overview)
        return false;
    return tile.rect.x < static_cast<std::uint64_t>(visible.x) + visible.width &&
        tile.rect.y < static_cast<std::uint64_t>(visible.y) + visible.height &&
        visible.x < static_cast<std::uint64_t>(tile.rect.x) + tile.rect.width &&
        visible.y < static_cast<std::uint64_t>(tile.rect.y) + tile.rect.height;
}
render::Image Tiles::texture(const Tile &tile, unsigned level) const
{
    if (tile.overview_level <= level)
        return tile.overview;
    if (tile.detail && tile.detail_level <= level)
        return tile.detail;
    return {};
}
bool Tiles::prepare(const Viewport &view)
{
    ++clock_;
    const unsigned level = level_for(view);
    if (cache_ && level >= overview_level_)
        return true; // FIT is already resident and batched in a few atlas textures.
    const auto region = view.target(1, true).rect;
    for (auto &tile : tiles_)
        if (visible(tile, region))
            tile.touched = clock_;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    bool ready          = true;
    for (auto &tile : tiles_)
    {
        if (tile.touched != clock_ || texture(tile, level))
            continue;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            ready = false;
            continue;
        }
        const auto &stored = tile.levels.at(level);
        std::vector<std::uint8_t> compressed(stored.bytes);
        seek(cache_, stored.offset);
        if (std::fread(compressed.data(), 1, compressed.size(), cache_) != compressed.size())
            cache_error("Cannot read native tile cache");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(stored.size.width) * stored.size.height * 4);
        uLongf bytes = data.size();
        if (uncompress(data.data(), &bytes, compressed.data(), compressed.size()) != Z_OK || bytes != data.size())
            throw std::runtime_error("Invalid native tile cache");
        renderer_.destroy_image(tile.detail);
        tile.detail       = {};
        tile.detail       = upload_texture(stored.size, level, data.data());
        tile.detail_level = level;
    }
    return ready;
}
void Tiles::draw(const Viewport &viewport, ImDrawList *list) const
{
    constexpr std::array<render::Point, 4> uv{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    const auto level = level_for(viewport);
    if (cache_ && level >= overview_level_)
    {
        const auto factor = 1u << overview_level_;
        for (const auto &atlas : atlases_)
        {
            if (!atlas.image)
                continue;
            const lv_rect_u32 rect{atlas.rect.x * factor, atlas.rect.y * factor,
                                   std::min(atlas.rect.width * factor, info_.canvas.width - atlas.rect.x * factor),
                                   std::min(atlas.rect.height * factor, info_.canvas.height - atlas.rect.y * factor)};
            const float u = static_cast<float>(rect.width) / static_cast<float>(atlas.rect.width * factor);
            const float v = static_cast<float>(rect.height) / static_cast<float>(atlas.rect.height * factor);
            const std::array<render::Point, 4> atlas_uv{{{0, 0}, {u, 0}, {u, v}, {0, v}}};
            renderer_.add_image_quad(list, atlas.image, viewport.quad(rect, info_.canvas), atlas_uv);
        }
        return;
    }
    const auto region = viewport.target(1, true).rect;
    for (const auto &tile : tiles_)
        if (visible(tile, region))
            if (const auto image = texture(tile, level))
                renderer_.add_image_quad(list, image, viewport.quad(tile.rect, info_.canvas),
                                         image.slot == tile.overview.slot ? tile.overview_uv : uv);
}
} // namespace liltview
