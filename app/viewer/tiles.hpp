#ifndef LILTVIEW_TILES_HPP
#define LILTVIEW_TILES_HPP
#include <cstdio>
#include <liltview/core.h>
#include <map>
#include <tuple>
#include <unordered_set>
#include <vector>
#include "tile_reader.hpp"
#include "tile_preparation.hpp"
#include "viewport.hpp"

namespace liltview
{
// One immutable native-resolution scene. Small scenes stay on the GPU; large
// scenes keep compressed native tiles/mip levels in an anonymous local cache.
// Camera changes never submit a decode or resample image contents.
class Tiles
{
public:
    explicit Tiles(render::Renderer &renderer, lv_wakeup_fn wake = nullptr, void *userdata = nullptr) :
        renderer_(renderer),
        wake_(wake),
        userdata_(userdata)
    {
    }
    ~Tiles()
    {
        clear();
    }
    Tiles(const Tiles &)            = delete;
    Tiles &operator=(const Tiles &) = delete;
    void begin(const lv_image_info &, bool defer_cache = false);
    unsigned overview_level() const { return overview_level_; }
    void install(PreparedTile);
    void adopt_cache(std::FILE *file, std::uint64_t bytes) { cache_ = file; cache_bytes_ = bytes; cache_dirty_ = false; }
    void upload(const lv_region_info &);
    void clear() noexcept;
    bool retire_ready() noexcept { if (!reader_) return true; reader_->cancel(); return reader_->done(); }
    void release_details() noexcept;
    void retry_reads()
    {
        if (reader_)
            reader_->retry_errors();
    }
    void commit_restore() noexcept;
    void cancel_restore() noexcept;
    void swap(Tiles &);
    struct UploadBudget
    {
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        std::uint64_t bytes = 0;
        unsigned tiles = 0;
        bool unlimited = false;
        bool available() const { return unlimited || !tiles || (tiles < 4 && bytes < 4ULL*1024*1024 && std::chrono::steady_clock::now() < deadline); }
    };
    bool prepare(const Viewport &,
                 const Viewport *preserve = nullptr, UploadBudget *shared = nullptr); // Upload cached visible textures before building draw commands.
    void draw(const Viewport &, ImDrawList *, bool preserving = false) const;
    bool covered(const Viewport &) const; // GPU-only readiness: no cache I/O or upload.
    struct Stats
    {
        TileReader::Stats ram;
        std::uint64_t gpu_restores = 0;
        std::size_t indexed_tiles  = 0;
        bool upload_pending        = false;
    };
    Stats stats() const
    {
        return {reader_ ? reader_->stats() : TileReader::Stats{}, gpu_restores_, indexed_tiles_, upload_pending_};
    }
    bool upload_pending() const
    {
        return upload_pending_;
    }
    bool empty() const
    {
        return ready_ == 0;
    }
    std::size_t loaded_tiles() const
    {
        return ready_;
    }
    bool complete() const
    {
        return ready_ == tiles_.size() && ready_ != 0;
    }

private:
    using Level = PreparedLevel;
    struct Tile
    {
        lv_rect_u32 rect{};
        render::Image overview{}, detail{}, preserved{};
        std::vector<Level> levels;
        unsigned overview_level = 0, detail_level = 0, preserved_level = 0;
        std::uint64_t touched = 0;
        std::array<render::Point, 4> overview_uv{};
    };
    struct Atlas
    {
        lv_rect_u32 rect{};
        render::Image image{};
    };
    bool decoded(const Viewport &) const;
    unsigned level_for(const Viewport &) const;
    std::vector<std::size_t> indices(const Viewport &, unsigned margin = 0) const;
    render::Image texture(const Tile &, unsigned level) const;
    void make_room(std::uint64_t bytes);
    render::Image upload_texture(render::Size, unsigned level, const std::uint8_t *, std::uint64_t stride = 0,
                                 std::uint64_t bytes = 0);
    void open_cache();
    void request_reads(const std::vector<TileReader::Request> &);
    std::vector<TileReader::Request> requests(const std::vector<std::size_t> &, unsigned) const;
    bool restore(std::size_t, unsigned, const Viewport *preserve);
    void warm(const std::vector<std::size_t> &, unsigned);
    void upload_overview(Tile &, render::Size, unsigned, const std::uint8_t *);
    render::Renderer &renderer_;
    lv_wakeup_fn wake_;
    void *userdata_;
    std::unique_ptr<TileReader> reader_;
    std::unordered_set<std::size_t> resident_, preserved_;
    bool cache_dirty_ = false, upload_pending_ = false;
    std::uint64_t gpu_restores_ = 0;
    std::size_t indexed_tiles_  = 0;
    lv_image_info info_{};
    std::vector<Tile> tiles_;
    std::vector<Atlas> atlases_;
    std::map<std::tuple<std::uint32_t, unsigned, unsigned>, std::vector<Level>> uniform_levels_;
    unsigned atlas_edge_ = 0, atlas_columns_ = 0;
    std::FILE *cache_          = nullptr;
    std::uint64_t cache_bytes_ = 0, clock_ = 0;
    std::size_t ready_       = 0;
    unsigned overview_level_ = 0;
};
} // namespace liltview
#endif
