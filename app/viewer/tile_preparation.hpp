#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include "memory_tile_cache.hpp"
#include <deque>
#include <exception>
#include <liltview/core.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "renderer/renderer.hpp"

namespace liltview
{
// Shared across displayed, pending and retiring scenes (including synchronous test adapters).
void reserve_tile_cache(std::uint64_t bytes);
void release_tile_cache(std::uint64_t bytes) noexcept;
struct PreparedLevel
{
    render::Size size{};
    std::uint64_t offset = 0;
    std::uint32_t bytes  = 0;
};
struct PreparedTile
{
    std::uint64_t id = 0, generation = 0;
    lv_rect_u32 rect{};
    unsigned level = 0;
    render::Size size{};
    std::vector<PreparedLevel> levels;
    MemoryBlock pixels;
    std::uint64_t stride_bytes = 0;
    bool borrowed = false;
};
// CPU-only first-load pipeline. All SDL/ImGui/texture work stays on the main thread.
// At most eight retained Core inputs (four for direct upload), bounded output,
// <=2 CPU workers, each with <=4 MiB conservative scratch allowance.
// Cancellation wakes every wait; joining the sibling happens on the root worker.
class TilePreparation
{
public:
    enum class Mode
    {
        pipeline,
        batch
    };
    struct Stats
    {
        double pack_ms = 0, lod_ms = 0, compress_ms = 0, store_ms = 0, worker_ms = 0;
        std::uint64_t tiles = 0, cache_bytes = 0, output_peak = 0;
        std::size_t input_peak = 0;
        unsigned workers = 1, worker_peak = 0;
        std::uint64_t scratch_limit_bytes = 4ULL * 1024 * 1024;
        std::chrono::steady_clock::time_point finished_at{};
    };
    TilePreparation(unsigned overview_level, Mode, lv_wakeup_fn = nullptr, void * = nullptr, std::uint64_t generation = 0, unsigned workers = 1, bool compression = true);
    static unsigned workers_for(const lv_image_info &);
    ~TilePreparation();
    std::uint64_t generation() const
    {
        return generation_;
    }
    bool can_accept() const;
    void submit(const lv_region_info &); // Caller checked admission; retains pixels, not event views.
    void finish();
    void cancel() noexcept;
    bool pop(PreparedTile &);
    bool done() const noexcept
    {
        return done_.load(std::memory_order_acquire);
    }
    bool drained() const;
    Stats stats() const;
    std::exception_ptr error() const;
    std::shared_ptr<MemoryTileCache> take_cache(); // Only after successful done + drained; transfers ownership.
private:
    struct Input
    {
        std::uint64_t id;
        lv_rect_u32 rect;
        MemoryBlock pixels;
        std::uint64_t stride_bytes;
    };
    void run() noexcept;
    void wake() noexcept;
    void consume() noexcept;
    void record_error(std::exception_ptr) noexcept;
    unsigned overview_level_;
    unsigned worker_limit_, active_workers_ = 0;
    bool direct_;
    bool compression_;
    std::size_t input_limit_;
    std::uint64_t generation_;
    Mode mode_;
    lv_wakeup_fn wake_;
    void *userdata_;
    mutable std::mutex mutex_;
    std::mutex cache_mutex_;
    std::condition_variable changed_;
    std::deque<Input> inputs_;
    std::deque<PreparedTile> outputs_;
    std::size_t retained_       = 0;
    std::uint64_t output_bytes_ = 0;
    bool finishing_ = false, cancelled_ = false;
    std::atomic<bool> done_{false};
    std::exception_ptr error_;
    Stats stats_;
    std::shared_ptr<MemoryTileCache> cache_;
    std::thread worker_;
};
} // namespace liltview
