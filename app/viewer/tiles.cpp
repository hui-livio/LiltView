#include "tiles.hpp"
#include "tile_pixels.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include "cache_codec.hpp"

namespace liltview
{
namespace
{
using tile_pixels::uniform_color;
using tile_pixels::solid;
using tile_pixels::reduce;
} // namespace
void Tiles::begin(const lv_image_info &info, bool defer_cache)
{
    clear();
    info_              = info;
    const auto columns = (static_cast<std::uint64_t>(info.canvas.width) + info.tile_edge - 1) / info.tile_edge;
    const auto rows    = (static_cast<std::uint64_t>(info.canvas.height) + info.tile_edge - 1) / info.tile_edge;
    const auto index_cost = sizeof(Tile) + std::bit_ceil(static_cast<unsigned>(std::bit_width(info.tile_edge))) * sizeof(Level);
    if (columns * rows > 128ULL * 1024 * 1024 / index_cost)
        throw render::Error(render::ErrorCode::memory_budget, "Native scene tile index exceeds budget");
    tiles_.resize(static_cast<std::size_t>(columns * rows));
    const auto caps = renderer_.capabilities();
    auto budget = std::max<std::uint64_t>(1, caps.image_budget_bytes / 4);
    // Admit the whole native scene only when its real tiled mip allocation fits.
    // This skips CPU cache compression/restoration for budget-friendly images,
    // including 4K on the default 128 MiB budget, without evicting the old scene.
    const auto edge = info.tile_edge;
    const auto full_columns = info.canvas.width / edge, full_rows = info.canvas.height / edge;
    const auto tail_width = info.canvas.width % edge, tail_height = info.canvas.height % edge;
    const auto cost = [](unsigned w, unsigned h) -> std::uint64_t {
        return w && h ? render::image_storage_bytes({{w, h}, true}) : 0;
    };
    const auto full_cost = static_cast<std::uint64_t>(full_columns) * full_rows * cost(edge, edge) +
        static_cast<std::uint64_t>(full_rows) * cost(tail_width, edge) +
        static_cast<std::uint64_t>(full_columns) * cost(edge, tail_height) + cost(tail_width, tail_height);
    const auto available = caps.image_bytes < caps.image_budget_bytes ? caps.image_budget_bytes - caps.image_bytes : 0;
    const auto raw = static_cast<std::uint64_t>(info.canvas.width) * info.canvas.height * 4;
    const bool direct = raw <= 64ULL * 1024 * 1024 && full_cost <= available &&
        full_cost <= caps.image_budget_bytes - caps.image_budget_bytes / 4;
    if (direct) budget = std::max(budget, full_cost);
    long double estimate = direct ? static_cast<long double>(full_cost) :
        static_cast<long double>(info.canvas.width) * info.canvas.height * 4 * 4 / 3;
    while (estimate > budget && (info.tile_edge >> overview_level_) > 1)
    {
        ++overview_level_;
        estimate /= 4;
    }
    if (estimate > budget)
        throw render::Error(render::ErrorCode::memory_budget, "Native scene texture index exceeds GPU budget");
    if (overview_level_)
    {
        if (!defer_cache) open_cache();
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
    cache_ = std::make_shared<MemoryTileCache>();
}
void Tiles::commit_restore() noexcept
{
    for (const auto index : preserved_)
    {
        auto &tile = tiles_[index];
        renderer_.destroy_image(tile.preserved);
        tile.preserved = {};
        if (!tile.detail)
            resident_.erase(index);
    }
    preserved_.clear();
}
void Tiles::cancel_restore() noexcept
{
    for (const auto index : preserved_)
    {
        auto &tile = tiles_[index];
        renderer_.destroy_image(tile.detail);
        tile.detail       = tile.preserved;
        tile.detail_level = tile.preserved_level;
        tile.preserved    = {};
    }
    preserved_.clear();
}
bool Tiles::retire_ready() noexcept
{
    if (cache_)
    {
        if (!reader_)
        {
            try { reader_ = std::make_unique<TileReader>(cache_, wake_, userdata_); }
            catch (...)
            {
                // Shutdown/destructors must remain safe if a cleanup thread cannot
                // be allocated. Rare fallback frees on the caller instead of leaking
                // or allowing an exception to escape a destructor.
                cache_.reset();
                return true;
            }
        }
        cache_.reset(); // Reader worker owns the final memory-cache reference.
    }
    if (!reader_) return true;
    reader_->cancel();
    return reader_->done();
}
void Tiles::release_details() noexcept
{
    commit_restore();
    for (const auto index : resident_)
    {
        renderer_.destroy_image(tiles_[index].detail);
        tiles_[index].detail = {};
    }
    resident_.clear();
}
void Tiles::clear() noexcept
{
    // Join the CPU-only reader before dropping its memory cache and metadata.
    reader_.reset();
    release_details();
    if (!overview_level_)
        for (const auto &tile : tiles_)
            renderer_.destroy_image(tile.overview);
    tiles_.clear();
    for (const auto &atlas : atlases_)
        renderer_.destroy_image(atlas.image);
    atlases_.clear();
    uniform_levels_.clear();
    atlas_edge_ = atlas_columns_ = 0;
    cache_.reset();
    info_        = {};
    ready_       = 0;
    cache_bytes_ = clock_ = gpu_restores_ = 0;
    overview_level_                       = 0;
    indexed_tiles_                        = 0;
    upload_pending_ = false;
}
void Tiles::swap(Tiles &other)
{
    tiles_.swap(other.tiles_);
    reader_.swap(other.reader_);
    resident_.swap(other.resident_);
    preserved_.swap(other.preserved_);
    std::swap(wake_, other.wake_);
    std::swap(userdata_, other.userdata_);
    std::swap(upload_pending_, other.upload_pending_);
    std::swap(gpu_restores_, other.gpu_restores_);
    std::swap(indexed_tiles_, other.indexed_tiles_);
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
        auto oldest = resident_.end();
        for (auto at = resident_.begin(); at != resident_.end(); ++at)
        {
            const auto &tile = tiles_[*at];
            if (tile.detail && tile.touched != clock_ && (oldest == resident_.end() || tile.touched < tiles_[*oldest].touched))
                oldest = at;
        }
        if (oldest == resident_.end())
            throw render::Error(render::ErrorCode::memory_budget, "Visible native textures exceed GPU budget");
        auto &tile = tiles_[*oldest];
        renderer_.destroy_image(tile.detail);
        tile.detail = {};
        if (!tile.preserved)
            resident_.erase(oldest);
        caps = renderer_.capabilities();
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
    return renderer_.create_uploaded_image(desc, {data, bytes, stride, size});
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

void Tiles::install(PreparedTile prepared)
{
    if (prepared.id >= tiles_.size())
        throw render::Error(render::ErrorCode::invalid_argument, "Invalid prepared tile ID");
    auto &tile = tiles_[static_cast<std::size_t>(prepared.id)];
    if (tile.overview) throw render::Error(render::ErrorCode::invalid_argument, "Duplicate prepared tile");
    tile.rect = prepared.rect;
    tile.levels = std::move(prepared.levels);
    if (overview_level_)
        upload_overview(tile, prepared.size, prepared.level, prepared.pixels.data());
    else
    {
        tile.overview = upload_texture(prepared.size, 0, prepared.pixels.data(), prepared.stride_bytes, prepared.pixels.size());
        tile.overview_uv = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    }
    ++ready_;
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
                const auto solid_pixels = solid(tile.levels[index].size, *color);
                upload_overview(tile, tile.levels[index].size, index, solid_pixels.data());
                ++ready_;
                return;
            }
        }
        // Native pixels and all LODs are generated/stored once. Subsequent view
        // changes only upload these immutable cached bytes, never reopen source.
        CacheEncoder encoder;
        for (;; ++level)
        {
            const auto &compressed = encoder.encode(data);
            const auto bytes = compressed.size();
            const auto offset = cache_->append(compressed);
            cache_bytes_ += bytes;
            tile.levels.push_back({size, offset, static_cast<std::uint32_t>(bytes)});
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
std::vector<std::size_t> Tiles::indices(const Viewport &view, unsigned margin) const
{
    const auto rect = view.target(1, true).rect;
    if (!rect.width || !rect.height)
        return {};
    const std::uint64_t edge    = info_.tile_edge;
    const std::uint64_t columns = (static_cast<std::uint64_t>(info_.canvas.width) + edge - 1) / edge;
    const std::uint64_t rows    = (static_cast<std::uint64_t>(info_.canvas.height) + edge - 1) / edge;
    const auto left             = rect.x / edge > margin ? rect.x / edge - margin : 0;
    const auto top              = rect.y / edge > margin ? rect.y / edge - margin : 0;
    const auto right            = std::min(columns, (static_cast<std::uint64_t>(rect.x) + rect.width - 1) / edge + 1 + margin);
    const auto bottom           = std::min(rows, (static_cast<std::uint64_t>(rect.y) + rect.height - 1) / edge + 1 + margin);
    std::vector<std::size_t> result;
    result.reserve(static_cast<std::size_t>((right - left) * (bottom - top)));
    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x)
        {
            const lv_rect_u32 cell{static_cast<std::uint32_t>(x * edge), static_cast<std::uint32_t>(y * edge),
                                   static_cast<std::uint32_t>(std::min(edge, info_.canvas.width - x * edge)),
                                   static_cast<std::uint32_t>(std::min(edge, info_.canvas.height - y * edge))};
            if (margin || view.visible(cell))
                result.push_back(static_cast<std::size_t>(y * columns + x));
        }
    return result;
}
render::Image Tiles::texture(const Tile &tile, unsigned level) const
{
    if (tile.overview_level <= level)
        return tile.overview;
    if (tile.detail && tile.detail_level <= level)
        return tile.detail;
    return {};
}
bool Tiles::covered(const Viewport &view) const
{
    const auto level = level_for(view);
    if (cache_ && level >= overview_level_)
        return decoded(view);
    for (const auto index : indices(view))
        if (!tiles_[index].overview || !texture(tiles_[index], level))
            return false;
    return true;
}
bool Tiles::decoded(const Viewport &view) const
{
    if (complete())
        return true;
    if (view.fitting())
        return false; // FIT covers the entire source; no scan of the whole tile index.
    for (const auto index : indices(view))
        if (!tiles_[index].overview)
            return false;
    return true;
}
std::vector<TileReader::Request> Tiles::requests(const std::vector<std::size_t> &order, unsigned level) const
{
    std::vector<TileReader::Request> result;
    result.reserve(std::min(order.size(), TileReader::max_requests));
    for (const auto index : order)
    {
        const auto &tile = tiles_[index];
        if (!tile.overview || texture(tile, level))
            continue;
        const auto &stored = tile.levels.at(level);
        result.push_back({{index, level}, stored.size, stored.offset, stored.bytes});
        if (result.size() == TileReader::max_requests)
            break;
    }
    return result;
}
void Tiles::request_reads(const std::vector<TileReader::Request> &jobs)
{
    if (jobs.empty() && !reader_)
        return;
    if (!reader_)
        reader_ = std::make_unique<TileReader>(cache_, wake_, userdata_);
    reader_->request(jobs);
}
bool Tiles::restore(std::size_t index, unsigned level, const Viewport *preserve)
{
    auto &tile      = tiles_[index];
    const auto data = reader_ ? reader_->find({index, level}) : TileReader::Data{};
    if (!data)
        return false; // The disk/decompression worker will wake the UI on completion.
    if (!renderer_.upload_ready())
    {
        upload_pending_ = true;
        return false; // Staging ring is busy: never wait for a GPU transfer on the UI thread.
    }
    const auto &stored   = tile.levels.at(level);
    bool retain_previous = false;
    if (preserve && tile.detail && !tile.preserved)
    {
        const auto rect    = preserve->target(1, true).rect;
        const bool overlap = tile.rect.x < static_cast<std::uint64_t>(rect.x) + rect.width &&
            tile.rect.y < static_cast<std::uint64_t>(rect.y) + rect.height &&
            rect.x < static_cast<std::uint64_t>(tile.rect.x) + tile.rect.width &&
            rect.y < static_cast<std::uint64_t>(tile.rect.y) + tile.rect.height;
        const auto previous = texture(tile, level_for(*preserve));
        retain_previous     = overlap && previous && previous.slot == tile.detail.slot;
    }
    const bool already_owned = resident_.contains(index);
    resident_.insert(index); // Bookkeeping allocation must succeed before creating GL resources.
    try
    {
        if (retain_previous)
            preserved_.insert(index);
        const auto replacement = upload_texture(stored.size, level, data->data());
        if (retain_previous)
        {
            tile.preserved       = tile.detail;
            tile.preserved_level = tile.detail_level;
        }
        else
            renderer_.destroy_image(tile.detail);
        tile.detail       = replacement;
        tile.detail_level = level;
        tile.touched      = clock_;
    } catch (...)
    {
        if (retain_previous)
            preserved_.erase(index);
        if (!already_owned)
            resident_.erase(index);
        throw;
    }
    ++gpu_restores_;
    return true;
}
void Tiles::warm(const std::vector<std::size_t> &order, unsigned level)
{
    if (!reader_)
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    unsigned uploaded   = 0;
    for (const auto index : order)
    {
        const auto &tile = tiles_[index];
        if (!tile.overview || texture(tile, level))
            continue;
        const auto caps    = renderer_.capabilities();
        const auto &size   = tile.levels.at(level).size;
        const auto bytes   = render::image_storage_bytes({size, true});
        const auto reserve = std::min<std::uint64_t>(8ULL * 1024 * 1024, caps.image_budget_bytes / 4);
        if (caps.image_bytes + bytes + reserve > caps.image_budget_bytes)
            continue; // Speculation must never evict visible data or exhaust admission headroom.
        try
        {
            const auto data = reader_->find({index, level});
            if (!data)
                continue;
            if (uploaded >= 2 || std::chrono::steady_clock::now() >= deadline)
            {
                upload_pending_ = true;
                break;
            }
            restore(index, level, nullptr);
            ++uploaded;
        } catch (const std::exception &)
        {
            // An offscreen prefetch failure must not invalidate a complete view.
            // A later visible request will report the actual error normally.
        }
    }
}
bool Tiles::prepare(const Viewport &view, const Viewport *preserve, UploadBudget *shared)
{
    if (!preserve)
        cancel_restore();
    ++clock_;
    upload_pending_      = false;
    indexed_tiles_       = 0;
    const unsigned level = level_for(view);
    if (cache_ && level >= overview_level_)
    {
        // Preheat the native center while FIT remains on screen. This is a
        // bounded cache restore, not another source decode or a full-image scan.
        if (complete())
        {
            auto native = view;
            native.actual_size();
            auto near      = indices(native, 1);
            indexed_tiles_ = near.size();
            try
            {
                // FIT only prefetches decoded CPU data. Do not change GPU LOD
                // residency before the user requests that scale (notably alpha).
                request_reads(requests(near, 0));
            } catch (const std::exception &)
            {
            }
        }
        return decoded(view);
    }
    const auto visible = indices(view);
    indexed_tiles_     = visible.size();
    for (const auto index : visible)
        tiles_[index].touched = clock_;
    if (preserve && cache_ && level_for(*preserve) < overview_level_)
    {
        const auto previous_level  = level_for(*preserve);
        const auto previous        = indices(*preserve);
        indexed_tiles_            += previous.size();
        for (const auto index : previous)
        {
            auto &tile = tiles_[index];
            if (tile.detail && !tile.preserved)
                if (const auto image = texture(tile, previous_level); image && image.slot == tile.detail.slot)
                    tile.touched = clock_;
        }
    }
    auto jobs        = cache_ ? requests(visible, level) : std::vector<TileReader::Request>{};
    const auto near  = cache_ ? indices(view, 1) : std::vector<std::size_t>{};
    indexed_tiles_  += near.size();
    for (const auto &job : cache_ ? requests(near, level) : std::vector<TileReader::Request>{})
        if (jobs.size() < TileReader::max_requests &&
            std::none_of(jobs.begin(), jobs.end(), [&](const auto &queued) { return queued.key == job.key; }))
            jobs.push_back(job);
    if (cache_)
        request_reads(jobs);
    UploadBudget local;
    auto &budget = shared ? *shared : local;
    bool ready = decoded(view);
    for (const auto index : visible)
    {
        const auto &tile = tiles_[index];
        if (!tile.overview || texture(tile, level))
            continue;
        if (!budget.available())
        {
            ready = false;
            if (reader_ && reader_->find({index, level}))
                upload_pending_ = true;
            continue;
        }
        if (!restore(index, level, preserve))
        {
            ready = false;
            continue;
        }
        ++budget.tiles;
        budget.bytes += render::image_storage_bytes({tile.levels.at(level).size, true});
    }
    if (ready && !preserve && cache_ && !shared)
        warm(near, level);
    return ready;
}
void Tiles::draw(const Viewport &viewport, ImDrawList *list, bool preserving) const
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
    for (const auto index : indices(viewport))
    {
        const auto &tile = tiles_[index];
        if (!tile.overview)
            continue;
        if (const auto image = preserving && tile.preserved ? tile.preserved : texture(tile, level))
            renderer_.add_image_quad(list, image, viewport.quad(tile.rect, info_.canvas),
                                     image.slot == tile.overview.slot ? tile.overview_uv : uv);
    }
}
} // namespace liltview
