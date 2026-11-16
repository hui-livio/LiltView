#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <spdlog/spdlog.h>
#include <system_error>
#include "viewer.hpp"

namespace liltview
{
namespace
{
void LV_CALL wake_static(void *)
{
    SDL_Event event{};
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}
} // namespace
double Viewer::load_elapsed_ms() const
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_started_).count();
}
void Viewer::advance_static()
{
    static_budget_           = {};
    static_budget_.unlimited = static_stats_.batch;
    if (!preparation_ || closing_)
        return;
    try
    {
        if (auto error = preparation_->error())
            std::rethrow_exception(error);
        const bool batch         = static_mode_ == StaticUploadMode::batch;
        const auto start         = std::chrono::steady_clock::now();
        static_budget_           = {};
        static_budget_.unlimited = batch;
        std::uint64_t bytes      = 0;
        unsigned count           = 0;
        PreparedTile tile;
        while ((batch ||
                (!count ||
                 (count < 4 && bytes < 4ULL * 1024 * 1024 &&
                  std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2)))) &&
               preparation_->pop(tile))
        {
            if (static_stats_.first_upload_ms < 0)
                static_stats_.first_upload_ms = load_elapsed_ms();
            if (tile.generation != generation_)
                throw std::logic_error("Stale preparation generation");
            bytes += static_cast<std::uint64_t>(tile.size.width) * tile.size.height * 4;
            pending_scene_.install(std::move(tile));
            ++count;
            ++static_stats_.tiles;
            static_stats_.last_upload_ms = load_elapsed_ms();
        }
        static_budget_.bytes = bytes;
        static_budget_.tiles = count;
        if (count)
        {
            const auto cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            static_stats_.upload_cpu_ms       += cost;
            static_stats_.uploaded_bytes      += bytes;
            static_stats_.max_upload_slice_ms  = std::max(static_stats_.max_upload_slice_ms, cost);
        }
        if (preparation_->done())
        {
            if (auto error = preparation_->error())
                std::rethrow_exception(error);
            static_stats_.preparation = preparation_->stats();
            if (static_stats_.prepare_done_ms < 0)
                static_stats_.prepare_done_ms =
                    std::chrono::duration<double, std::milli>(static_stats_.preparation.finished_at - load_started_).count();
            if (preparation_->drained())
            {
                if (!decode_complete_ || !pending_scene_.complete() || static_stats_.tiles != expected_tiles_)
                    throw render::Error(render::ErrorCode::invalid_argument, "Prepared coverage differs from Core completion");
                pending_scene_.adopt_cache(preparation_->take_cache(), static_stats_.preparation.cache_bytes);
                cpu_complete_ = true;
                preparation_.reset(); // done is published after releasing all inputs; no running worker join.
            }
            else
                wake_static(nullptr);
        }
        else if (count)
            wake_static(nullptr);
    } catch (const std::exception &error)
    {
        spdlog::error("static_load_failed generation={} elapsed_ms={:.3f} reason={}", generation_, load_elapsed_ms(),
                      error.what());
        if (request_)
            lv_core_cancel(core_, request_);
        fail(dynamic_cast<const std::system_error *>(&error) ? LV_ERROR_IO : LV_ERROR_MEMORY_BUDGET);
    }
}
void Viewer::commit_static_if_ready()
{
    if (!loading_ || work_ != Work::static_image || !decode_complete_ || !cpu_complete_ || failed())
        return;
    try
    {
        // Called with this frame's actual canvas, including DPI and information-panel layout.
        // Initial FIT normally needs no restore; larger/100% views remain hidden until covered.
        const auto start        = std::chrono::steady_clock::now();
        const auto before_bytes = static_budget_.bytes;
        const auto before_tiles = static_budget_.tiles;
        const bool ready        = pending_scene_.prepare(viewport_, nullptr, &static_budget_);
        if (static_budget_.tiles != before_tiles)
        {
            const auto cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            static_stats_.upload_cpu_ms       += cost;
            static_stats_.uploaded_bytes      += static_budget_.bytes - before_bytes;
            static_stats_.last_upload_ms       = load_elapsed_ms();
            static_stats_.max_upload_slice_ms  = std::max(static_stats_.max_upload_slice_ms, cost);
        }
        if (!ready)
        {
            if (pending_scene_.upload_pending())
                wake_static(nullptr);
            return; // TileReader wakes on completed memory cache work; no polling loop on the UI thread.
        }
        static_stats_.ready_ms = load_elapsed_ms();
        accept_completed();
        first_commit_this_frame_ = true;
        spdlog::info(
            "static_load_ready generation={} mode={} cache_compression={} ready_ms={:.3f} core_ms={:.3f} first_region_ms={:.3f} decode_done_ms={:.3f} prepare_done_ms={:.3f} first_upload_ms={:.3f} last_upload_ms={:.3f} upload_cpu_ms={:.3f} max_upload_slice_ms={:.3f} max_pump_ms={:.3f} tiles={} upload_bytes={} pack_ms={:.3f} lod_ms={:.3f} compress_ms={:.3f} cache_store_ms={:.3f} cache_bytes={} input_peak={} output_peak={}",
            generation_, static_stats_.batch ? "batch" : "pipeline", static_cache_compression_, static_stats_.ready_ms, static_stats_.core_ms,
            static_stats_.first_region_ms, static_stats_.decode_done_ms, static_stats_.prepare_done_ms,
            static_stats_.first_upload_ms, static_stats_.last_upload_ms, static_stats_.upload_cpu_ms,
            static_stats_.max_upload_slice_ms, static_stats_.max_pump_ms, static_stats_.tiles, static_stats_.uploaded_bytes,
            static_stats_.preparation.pack_ms, static_stats_.preparation.lod_ms, static_stats_.preparation.compress_ms,
            static_stats_.preparation.store_ms, static_stats_.preparation.cache_bytes, static_stats_.preparation.input_peak,
            static_stats_.preparation.output_peak);
        const auto ram = scene_.stats().ram;
        spdlog::info("static_load_cache generation={} codec={} workers={} worker_peak={} scratch_limit_bytes={} restore_reads={} restore_read_ms={:.3f} restore_decode_ms={:.3f} restore_max_decode_ms={:.3f}",
            generation_, static_stats_.preparation.cache_bytes ? (static_cache_compression_ ? "lz4" : "raw-rgba") : "none/direct-rgba",
            static_stats_.preparation.workers, static_stats_.preparation.worker_peak,
            static_stats_.preparation.scratch_limit_bytes, ram.reads, ram.read_ms, ram.decode_ms, ram.max_decode_ms);
    } catch (const std::exception &error)
    {
        spdlog::error("static_load_first_view_failed generation={} reason={}", generation_, error.what());
        fail(dynamic_cast<const std::system_error *>(&error) ? LV_ERROR_IO : LV_ERROR_MEMORY_BUDGET);
    }
}
void Viewer::note_frame_submitted()
{
    loading_frame_submitted_ = loading_;
    static_frame_submitted_  = !loading_ && static_stats_.ready_ms >= 0 && static_stats_.submitted_ms < 0;
    if (static_frame_submitted_)
        static_stats_.submitted_ms = load_elapsed_ms();
}
void Viewer::note_frame_presented()
{
    if (loading_frame_submitted_ && static_stats_.loading_presented_ms < 0)
        static_stats_.loading_presented_ms = load_elapsed_ms();
    if (static_frame_submitted_ && static_stats_.presented_ms < 0)
    {
        static_stats_.presented_ms = load_elapsed_ms();
        spdlog::info(
            "static_load_present generation={} mode={} loading_presented_ms={:.3f} submitted_ms={:.3f} presented_ms={:.3f}",
            generation_, static_stats_.batch ? "batch" : "pipeline", static_stats_.loading_presented_ms,
            static_stats_.submitted_ms, static_stats_.presented_ms);
    }
    loading_frame_submitted_ = static_frame_submitted_ = false;
}
} // namespace liltview
