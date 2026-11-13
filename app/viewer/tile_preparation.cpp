#include "tile_preparation.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <zlib.h>
#include "platform/file_io.hpp"
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
constexpr std::uint64_t max_cache = 16ULL * 1024 * MiB;
std::atomic<std::uint64_t> all_cache_bytes{0};
std::uint64_t output_cost(const PreparedTile &tile)
{
    return tile.pixels.capacity() + tile.levels.capacity() * sizeof(PreparedLevel) + sizeof(PreparedTile);
}
using tile_pixels::reduce;
using tile_pixels::solid;
using tile_pixels::uniform_color;
} // namespace
void reserve_tile_cache(std::uint64_t bytes)
{
    auto used = all_cache_bytes.load();
    do
    {
        if (bytes > max_cache - used)
            throw render::Error(render::ErrorCode::memory_budget, "Active/retiring tile caches exceed 16 GiB");
    } while (!all_cache_bytes.compare_exchange_weak(used, used + bytes));
}
void release_tile_cache(std::uint64_t bytes) noexcept
{
    all_cache_bytes.fetch_sub(bytes);
}
TilePreparation::TilePreparation(unsigned level, Mode mode, lv_wakeup_fn wake, void *userdata, std::uint64_t generation) :
    overview_level_(level),
    generation_(generation),
    mode_(mode),
    wake_(wake),
    userdata_(userdata),
    worker_([this] { run(); })
{
}
TilePreparation::~TilePreparation()
{
    cancel();
    if (worker_.joinable())
        worker_.join();
    if (cache_)
        std::fclose(cache_);
    release_tile_cache(cache_reserved_);
}
void TilePreparation::wake() noexcept
{
    if (wake_)
        wake_(userdata_);
}
bool TilePreparation::can_accept() const
{
    std::lock_guard lock(mutex_);
    return !finishing_ && !cancelled_ && !done_ && retained_ < 2;
}
void TilePreparation::submit(const lv_region_info &region)
{
    // Retain before event release; the original backing may be larger than this view.
    lv_pixels_retain(region.pixels);
    std::shared_ptr<lv_pixels> pixels(region.pixels, lv_pixels_release);
    {
        std::lock_guard lock(mutex_);
        if (cancelled_ || finishing_ || retained_ >= 2)
            throw std::logic_error("Preparation input admission");
        inputs_.push_back({region.tile_id, region.rect, std::move(pixels)});
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
    }
    changed_.notify_all();
}
void TilePreparation::cancel() noexcept
{
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
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
std::FILE *TilePreparation::take_cache()
{
    std::lock_guard lock(mutex_);
    if (!done_ || !outputs_.empty() || error_)
        throw std::logic_error("Cache transfer before preparation completed");
    cache_reserved_ = 0; // The receiving Tiles now owns this reservation.
    return std::exchange(cache_, nullptr);
}
void TilePreparation::run() noexcept
{
    const auto started = Clock::now();
    Stats measured;
    std::map<std::tuple<std::uint32_t, unsigned, unsigned>, std::vector<PreparedLevel>> uniform;
    try
    {
        if (overview_level_)
        {
            const auto start = Clock::now();
            std::filesystem::path directory;
            if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
                directory = xdg;
            else if (const char *home = std::getenv("HOME"); home && *home)
                directory = std::filesystem::path(home) / ".cache";
            else
                directory = std::filesystem::temp_directory_path();
            directory /= "liltview";
            std::filesystem::create_directories(directory);
            cache_             = platform::temporary_file(directory);
            measured.write_ms += elapsed(start);
        }
        for (;;)
        {
            Input input;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return cancelled_ || !inputs_.empty() || finishing_; });
                if (cancelled_ || (inputs_.empty() && finishing_))
                    break;
                input = std::move(inputs_.front());
                inputs_.pop_front();
            }
            PreparedTile result;
            result.id         = input.id;
            result.generation = generation_;
            result.rect       = input.rect;
            render::Size size{input.rect.width, input.rect.height};
            auto start         = Clock::now();
            const auto &pixels = *lv_pixels_get_view(input.pixels.get());
            // A Core tile must fit this fixed scratch bound; never allocate an unbounded full-image copy.
            const auto data_bytes = static_cast<std::uint64_t>(size.width) * size.height * 4;
            if (data_bytes > MiB)
                throw render::Error(render::ErrorCode::memory_budget, "Preparation tile exceeds 512px RGBA scratch");
            std::vector<std::uint8_t> data(static_cast<std::size_t>(data_bytes));
            for (unsigned y = 0; y < size.height; ++y)
                std::copy_n(pixels.data + y * pixels.stride_bytes, static_cast<std::size_t>(size.width) * 4,
                            data.data() + static_cast<std::size_t>(y) * size.width * 4);
            input.pixels.reset(); // Release Core backing before waiting for output space or COMPLETE.
            {
                std::lock_guard lock(mutex_);
                --retained_;
            }
            wake();
            measured.pack_ms += elapsed(start);
            if (!cache_)
            {
                result.size   = size;
                result.pixels = std::move(data);
            }
            else
            {
                start             = Clock::now();
                const auto color  = uniform_color(data);
                measured.lod_ms  += elapsed(start);
                const auto key    = std::make_tuple(color.value_or(0), size.width, size.height);
                const auto found  = color ? uniform.find(key) : uniform.end();
                if (found != uniform.end())
                {
                    result.levels = found->second;
                    result.level  = std::min(overview_level_, static_cast<unsigned>(result.levels.size() - 1));
                    result.size   = result.levels[result.level].size;
                    result.pixels = solid(result.size, *color);
                }
                else
                {
                    for (unsigned level = 0;; ++level)
                    {
                        {
                            std::lock_guard lock(mutex_);
                            if (cancelled_)
                                break;
                        }
                        start = Clock::now();
                        std::vector<std::uint8_t> compressed(compressBound(static_cast<uLong>(data.size())));
                        uLongf bytes = static_cast<uLongf>(compressed.size());
                        if (compress2(compressed.data(), &bytes, data.data(), static_cast<uLong>(data.size()), Z_BEST_SPEED) !=
                            Z_OK)
                            throw std::runtime_error("Cannot compress prepared tile");
                        measured.compress_ms += elapsed(start);
                        if (measured.cache_bytes + bytes > max_cache)
                            throw render::Error(render::ErrorCode::memory_budget, "Prepared cache exceeds 16 GiB");
                        reserve_tile_cache(bytes);
                        cache_reserved_ += bytes;
                        start            = Clock::now();
                        if (std::fwrite(compressed.data(), 1, bytes, cache_) != bytes)
                            throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot write prepared cache");
                        measured.write_ms += elapsed(start);
                        result.levels.push_back({size, measured.cache_bytes, static_cast<std::uint32_t>(bytes)});
                        measured.cache_bytes += bytes;
                        if (level == overview_level_ || (size.width == 1 && size.height == 1 && result.pixels.empty()))
                        {
                            result.level  = level;
                            result.size   = size;
                            result.pixels = data;
                        }
                        if (size.width == 1 && size.height == 1)
                            break;
                        start = Clock::now();
                        const render::Size next{(size.width + 1) / 2, (size.height + 1) / 2};
                        data             = color ? solid(next, *color) : reduce(data, size);
                        size             = next;
                        measured.lod_ms += elapsed(start);
                    }
                    if (color && uniform.size() < 256)
                        uniform.emplace(key, result.levels);
                }
            }
            {
                std::unique_lock lock(mutex_);
                const auto limit = mode_ == Mode::batch ? 64 * MiB : 8 * MiB;
                if (output_cost(result) > limit || (mode_ == Mode::batch && output_bytes_ + output_cost(result) > limit))
                    throw render::Error(render::ErrorCode::memory_budget, "Batch diagnostic staging exceeds 64 MiB");
                changed_.wait(lock, [&] { return cancelled_ || output_bytes_ + output_cost(result) <= limit; });
                if (cancelled_)
                    break;
                output_bytes_ += output_cost(result);
                outputs_.push_back(std::move(result));
                ++measured.tiles;
                const auto input_peak = stats_.input_peak;
                measured.output_peak  = std::max(measured.output_peak, output_bytes_);
                stats_                = measured;
                stats_.input_peak     = input_peak;
            }
            wake();
        }
        if (cache_)
        {
            const auto start = Clock::now();
            if (std::fflush(cache_) != 0)
                throw std::system_error(errno ? errno : EIO, std::generic_category(), "Cannot flush prepared cache");
            measured.write_ms += elapsed(start);
        }
    } catch (...)
    {
        std::lock_guard lock(mutex_);
        error_ = std::current_exception();
    }
    bool discard = false;
    {
        std::lock_guard lock(mutex_);
        discard = cancelled_ || static_cast<bool>(error_);
    }
    if (discard && cache_)
    {
        std::fclose(cache_);
        cache_ = nullptr;
        release_tile_cache(cache_reserved_);
        cache_reserved_ = 0;
    }
    uniform.clear(); // Finish potentially large CPU destruction before publishing done.
    {
        std::lock_guard lock(mutex_);
        inputs_.clear();
        retained_ = 0;
        if (cancelled_ || error_)
        {
            outputs_.clear();
            output_bytes_ = 0;
        }
        measured.input_peak  = stats_.input_peak;
        measured.worker_ms   = elapsed(started);
        measured.finished_at = Clock::now();
        stats_               = measured;
        done_.store(true, std::memory_order_release);
    }
    wake();
}
} // namespace liltview
