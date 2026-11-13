#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
    std::vector<std::uint8_t> pixels;
};
// CPU-only first-load pipeline. All SDL/ImGui/texture work stays on the main thread.
// At most two retained Core inputs (including the active input), bounded output,
// and one tile's scratch. Cancellation wakes both queue waits; no UI-thread join.
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
        double pack_ms = 0, lod_ms = 0, compress_ms = 0, write_ms = 0, worker_ms = 0;
        std::uint64_t tiles = 0, cache_bytes = 0, output_peak = 0;
        std::size_t input_peak = 0;
        std::chrono::steady_clock::time_point finished_at{};
    };
    TilePreparation(unsigned overview_level, Mode, lv_wakeup_fn = nullptr, void * = nullptr, std::uint64_t generation = 0);
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
    std::FILE *take_cache(); // Only after successful done + drained; transfers ownership.
private:
    struct Input
    {
        std::uint64_t id;
        lv_rect_u32 rect;
        std::shared_ptr<lv_pixels> pixels;
    };
    void run() noexcept;
    void wake() noexcept;
    unsigned overview_level_;
    std::uint64_t generation_;
    Mode mode_;
    lv_wakeup_fn wake_;
    void *userdata_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Input> inputs_;
    std::deque<PreparedTile> outputs_;
    std::size_t retained_       = 0;
    std::uint64_t output_bytes_ = 0;
    bool finishing_ = false, cancelled_ = false;
    std::atomic<bool> done_{false};
    std::exception_ptr error_;
    Stats stats_;
    std::FILE *cache_             = nullptr;
    std::uint64_t cache_reserved_ = 0;
    std::thread worker_;
};
} // namespace liltview
