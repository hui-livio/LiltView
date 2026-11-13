#include "tile_preparation.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include "cache_codec.hpp"
#include "tile_pixels.hpp"

namespace liltview
{
namespace
{
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
constexpr std::uint64_t MiB       = 1024 * 1024;
std::uint64_t output_cost(const PreparedTile &tile)
{
    return tile.pixels.capacity() + tile.levels.capacity() * sizeof(PreparedLevel) + sizeof(PreparedTile);
}
using tile_pixels::reduce;
using tile_pixels::solid;
using tile_pixels::uniform_color;
} // namespace
TilePreparation::TilePreparation(unsigned level, Mode mode, lv_wakeup_fn wake, void *userdata, std::uint64_t generation, unsigned workers, bool compression) :
    overview_level_(level),
    worker_limit_(std::clamp(workers, 1u, 2u)),
    direct_(level == 0 && mode == Mode::pipeline),
    compression_(compression),
    input_limit_(direct_ ? 4 : (level ? 8 : 2)),
    generation_(generation),
    mode_(mode),
    wake_(wake),
    userdata_(userdata)
{
    stats_.workers = direct_ ? 0 : worker_limit_;
    stats_.scratch_limit_bytes = direct_ ? 0 : worker_limit_ * 4 * MiB;
    if (!direct_) worker_ = std::thread([this] { run(); });
}
TilePreparation::~TilePreparation()
{
    cancel();
    if (worker_.joinable())
        worker_.join();
    cache_.reset();
}
void TilePreparation::wake() noexcept
{
    if (wake_)
        wake_(userdata_);
}
bool TilePreparation::can_accept() const
{
    std::lock_guard lock(mutex_);
    return !finishing_ && !cancelled_ && !done_ && retained_ < input_limit_;
}
void TilePreparation::submit(const lv_region_info &region)
{
    if (direct_)
    {
        try
        {
            const auto bytes = static_cast<std::uint64_t>(region.rect.width) * region.rect.height * 4;
            if (!bytes || bytes > MiB) throw render::Error(render::ErrorCode::memory_budget, "Invalid direct tile size");
            PreparedTile tile;
            tile.id = region.tile_id; tile.generation = generation_; tile.rect = region.rect;
            tile.size = {region.rect.width, region.rect.height};
            tile.stride_bytes = lv_pixels_get_view(region.pixels)->stride_bytes;
            tile.pixels = MemoryBlock::pixels(region.pixels);
            tile.borrowed = true;
            std::lock_guard lock(mutex_);
            if (cancelled_ || finishing_ || retained_ >= input_limit_) throw std::logic_error("Direct input admission");
            output_bytes_ += output_cost(tile);
            outputs_.push_back(std::move(tile));
            ++retained_; ++stats_.tiles;
            stats_.input_peak = std::max(stats_.input_peak, retained_);
            stats_.output_peak = std::max(stats_.output_peak, output_bytes_);
        }
        catch (...)
        {
            record_error(std::current_exception());
            std::lock_guard lock(mutex_);
            outputs_.clear(); output_bytes_ = 0; retained_ = 0;
            stats_.finished_at = Clock::now();
            done_.store(true, std::memory_order_release);
        }
        return;
    }
    // Retain before event release; the original backing may be larger than this view.
    auto pixels = MemoryBlock::pixels(region.pixels);
    const auto stride = lv_pixels_get_view(region.pixels)->stride_bytes;
    {
        std::lock_guard lock(mutex_);
        if (cancelled_ || finishing_ || retained_ >= input_limit_)
            throw std::logic_error("Preparation input admission");
        inputs_.push_back({region.tile_id, region.rect, std::move(pixels), stride});
        ++retained_;
        stats_.input_peak = std::max(stats_.input_peak, retained_);
    }
    changed_.notify_all();
}
void TilePreparation::finish()
{
    {
        std::lock_guard lock(mutex_);
        finishing_ = true;
        if (direct_) { stats_.finished_at = Clock::now(); done_.store(true, std::memory_order_release); }
    }
    changed_.notify_all();
}
void TilePreparation::cancel() noexcept
{
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
        if (direct_) {
            outputs_.clear(); output_bytes_ = 0; retained_ = 0;
            stats_.finished_at = Clock::now(); done_.store(true, std::memory_order_release);
        }
    }
    changed_.notify_all();
}
bool TilePreparation::pop(PreparedTile &result)
{
    {
        std::lock_guard lock(mutex_);
        if (outputs_.empty() || (mode_ == Mode::batch && !done_))
            return false;
        result = std::move(outputs_.front());
        outputs_.pop_front();
        if (result.borrowed) --retained_;
        output_bytes_ -= output_cost(result);
    }
    changed_.notify_all();
    return true;
}
bool TilePreparation::drained() const
{
    std::lock_guard lock(mutex_);
    return outputs_.empty();
}
TilePreparation::Stats TilePreparation::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}
std::exception_ptr TilePreparation::error() const
{
    std::lock_guard lock(mutex_);
    return error_;
}
std::shared_ptr<MemoryTileCache> TilePreparation::take_cache()
{
    std::lock_guard lock(mutex_);
    if (!done_ || !outputs_.empty() || error_)
        throw std::logic_error("Cache transfer before preparation completed");
    return std::exchange(cache_, {});
}
unsigned TilePreparation::workers_for(const lv_image_info &info)
{
    // Small jobs retain one worker. Work, not a 4K dimension cutoff, controls
    // concurrency: >32 MiB RGBA and more than one tile may use two CPU workers.
    const auto pixels = static_cast<std::uint64_t>(info.canvas.width) * info.canvas.height;
    if (!info.tile_edge || pixels <= 8ULL * 1024 * 1024 || std::thread::hardware_concurrency() == 1)
        return 1;
    return info.canvas.width > info.tile_edge || info.canvas.height > info.tile_edge ? 2 : 1;
}
void TilePreparation::record_error(std::exception_ptr error) noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!error_) error_ = error;
        cancelled_ = true;
    }
    changed_.notify_all();
    wake();
}
void TilePreparation::consume() noexcept
{
    bool active = false;
    try
    {
        CacheEncoder encoder;
        // Per-worker immutable-level deduplication. Both tables and their LOD
        // metadata are bounded; concurrent first occurrences may be duplicated.
        std::map<std::tuple<std::uint32_t, unsigned, unsigned>, std::vector<PreparedLevel>> uniform;
        for (;;)
        {
            Input input;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return cancelled_ || !inputs_.empty() || finishing_; });
                if (cancelled_ || (inputs_.empty() && finishing_)) break;
                input = std::move(inputs_.front());
                inputs_.pop_front();
                active = true;
                stats_.worker_peak = std::max(stats_.worker_peak, ++active_workers_);
            }
            Stats measured;
            PreparedTile result;
            result.id = input.id;
            result.generation = generation_;
            result.rect = input.rect;
            render::Size size{input.rect.width, input.rect.height};
            auto start = Clock::now();
            const auto stride = input.stride_bytes;
            const auto data_bytes = static_cast<std::uint64_t>(size.width) * size.height * 4;
            if (!data_bytes || data_bytes > MiB)
                throw render::Error(render::ErrorCode::memory_budget, "Preparation tile exceeds 512px RGBA scratch");
            MemoryBlock data;
            // Contiguous input is referenced directly. Only strided compression
            // input and batch diagnostics need a packed copy; UI accepts stride.
            const bool direct_reference = !cache_ && mode_ == Mode::pipeline;
            if (direct_reference || stride == static_cast<std::uint64_t>(size.width) * 4)
                data = input.pixels;
            else
            {
                std::vector<std::uint8_t> packed(static_cast<std::size_t>(data_bytes));
                for (unsigned y = 0; y < size.height; ++y)
                    std::copy_n(input.pixels.data() + y * stride, static_cast<std::size_t>(size.width) * 4,
                                packed.data() + static_cast<std::size_t>(y) * size.width * 4);
                data = MemoryBlock(std::move(packed));
            }
            // Batch waits for COMPLETE; it must not pin all Core backing buffers.
            if (!cache_ && mode_ == Mode::batch && data.capacity() == 0)
                data = MemoryBlock(std::vector<std::uint8_t>(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(data_bytes)));
            result.borrowed = direct_reference;
            result.stride_bytes = direct_reference ? stride : 0;
            input.pixels.reset();
            measured.pack_ms += elapsed(start);
            if (!cache_)
            {
                result.size = size;
                result.pixels = std::move(data);
            }
            else
            {
                start = Clock::now();
                const auto color = uniform_color(data);
                measured.lod_ms += elapsed(start);
                const auto key = std::make_tuple(color.value_or(0), size.width, size.height);
                const auto found = color ? uniform.find(key) : uniform.end();
                if (found != uniform.end())
                {
                    result.levels = found->second;
                    result.level = std::min(overview_level_, static_cast<unsigned>(result.levels.size() - 1));
                    result.size = result.levels[result.level].size;
                    result.pixels = solid(result.size, *color);
                }
                else
                {
                    for (unsigned level = 0;; ++level)
                    {
                        {
                            std::lock_guard lock(mutex_);
                            if (cancelled_) break;
                        }
                        start = Clock::now();
                        // A long-lived raw cache must not pin the bounded Core pool.
                        // Only borrowed contiguous Core data needs this ownership copy;
                        // packed input and generated LOD allocations are shared as-is.
                        MemoryBlock stored;
                        if (compression_)
                        {
                            stored = encoder.encode(data);
                            measured.compress_ms += elapsed(start);
                        }
                        else
                        {
                            if (data.capacity() == 0)
                                data = MemoryBlock(std::vector<std::uint8_t>(data.begin(), data.end()));
                            stored = data;
                            measured.pack_ms += elapsed(start);
                        }
                        {
                            // Exactly one writer at a time; levels may finish out of
                            // order and are addressed by immutable offsets, not tile order.
                            std::lock_guard write_lock(cache_mutex_);
                            const auto bytes = stored.size();
                            start = Clock::now();
                            const auto offset = cache_->append(stored, compression_);
                            measured.store_ms += elapsed(start); // RAM insertion, never file I/O.
                            result.levels.push_back({size, offset, static_cast<std::uint32_t>(bytes)});
                            measured.cache_bytes += bytes;
                        }
                        if (level == overview_level_ || (size.width == 1 && size.height == 1 && result.pixels.empty()))
                        {
                            result.level = level;
                            result.size = size;
                            result.pixels = data;
                        }
                        if (size.width == 1 && size.height == 1) break;
                        start = Clock::now();
                        const render::Size next{(size.width + 1) / 2, (size.height + 1) / 2};
                        data = color ? solid(next, *color) : reduce(data, size);
                        size = next;
                        measured.lod_ms += elapsed(start);
                    }
                    if (color && uniform.size() < 256) uniform.emplace(key, result.levels);
                }
            }
            data.reset();
            {
                std::unique_lock lock(mutex_);
                if (!result.borrowed) --retained_;
                changed_.notify_all();
                stats_.pack_ms += measured.pack_ms;
                stats_.lod_ms += measured.lod_ms;
                stats_.compress_ms += measured.compress_ms;
                stats_.store_ms += measured.store_ms;
                stats_.cache_bytes += measured.cache_bytes;
                const auto limit = mode_ == Mode::batch ? 80 * MiB : 8 * MiB;
                if (output_cost(result) > limit || (mode_ == Mode::batch && output_bytes_ + output_cost(result) > limit))
                    throw render::Error(render::ErrorCode::memory_budget, "Batch diagnostic staging exceeds 80 MiB");
                changed_.wait(lock, [&] { return cancelled_ || output_bytes_ + output_cost(result) <= limit; });
                if (cancelled_) break;
                output_bytes_ += output_cost(result);
                outputs_.push_back(std::move(result));
                ++stats_.tiles;
                stats_.output_peak = std::max(stats_.output_peak, output_bytes_);
                --active_workers_;
                active = false;
            }
            wake();
        }
    }
    catch (...) { record_error(std::current_exception()); }
    if (active)
    {
        std::lock_guard lock(mutex_);
        --active_workers_;
    }
}
void TilePreparation::run() noexcept
{
    const auto started = Clock::now();
    std::thread sibling;
    try
    {
        {
            std::lock_guard lock(mutex_);
            stats_.workers = worker_limit_;
            stats_.scratch_limit_bytes = worker_limit_ * 4 * MiB;
        }
        if (overview_level_) cache_ = std::make_shared<MemoryTileCache>();
        if (worker_limit_ > 1) sibling = std::thread([this] { consume(); });
        consume();
    }
    catch (...) { record_error(std::current_exception()); }
    // Never join on the UI thread; all CPU locals are destroyed before done.
    if (sibling.joinable()) sibling.join();
    bool discard;
    {
        std::lock_guard lock(mutex_);
        discard = cancelled_ || static_cast<bool>(error_);
    }
    if (discard) cache_.reset();
    {
        std::lock_guard lock(mutex_);
        inputs_.clear();
        retained_ = 0;
        if (cancelled_ || error_)
        {
            outputs_.clear();
            output_bytes_ = 0;
        }
        stats_.worker_ms = elapsed(started);
        stats_.finished_at = Clock::now();
        done_.store(true, std::memory_order_release);
    }
    wake();
}
} // namespace liltview
