#ifndef LILTVIEW_PREFETCH_HPP
#define LILTVIEW_PREFETCH_HPP

#include "navigation.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <liltview/core.h>
#include <optional>

namespace liltview
{
// Control-thread scheduler only. The owner submits NATIVE/PREFETCH and releases
// every event after consume; this model never owns events, pixels, or GPU state.
class PrefetchController
{
public:
    using Submit = std::function<lv_status(const std::filesystem::path &, std::uint64_t generation, lv_request_id &)>;
    using Cancel = std::function<void(lv_request_id)>;
    struct Stats
    {
        // Cumulative accepted/outcome counts include retired requests. Active includes retired IDs.
        std::uint64_t submitted = 0, completed = 0, failed = 0, cancelled = 0, cache_hits = 0;
        std::uint32_t active = 0, retired = 0;
    };

    PrefetchController(Submit, Cancel);
    PrefetchController(const PrefetchController &)            = delete;
    PrefetchController &operator=(const PrefetchController &) = delete;
    void update(std::uint64_t generation, const DirectoryNavigation::Snapshot &);
    // Called by the owner's event/resource loop. BUSY may retry on a later call.
    // A cancelled target needs a subsequent false->true edge, with at most three
    // accepted attempts per target/plan. No self-wakeup or foreground waiting.
    void pump(bool foreground_idle);
    bool owns(lv_request_id) const noexcept;
    // true means a background ID was recognized; the caller must still release its event.
    bool consume(const lv_event_view &);
    void begin_shutdown();
    // Includes retired work until its terminal. Never use this as a substitute for core join.
    bool pending() const noexcept;
    Stats stats() const noexcept;
    // Only successful results belonging to the current plan; GIF means static first-frame metadata only.
    std::optional<lv_image_info> prefetched_info(const std::filesystem::path &) const;
    // Last current-plan content observation, even if prefetch subsequently fails.
    // A hint only: no successful cache, retained source identity, or animation capability is implied.
    std::optional<lv_format> observed_format(const std::filesystem::path &) const;

private:
    struct Target
    {
        std::filesystem::path path;
        bool attempted             = false;
        bool retryable             = false;
        unsigned accepted_attempts = 0;
        std::optional<lv_image_info> info;
        std::optional<lv_format> observed;
    };
    struct Slot
    {
        lv_request_id id = 0;
        std::filesystem::path path;
        std::uint64_t plan = 0;
        std::size_t target = 0;
        bool retired       = false;
        std::optional<lv_image_info> info;
    };
    void retire();
    void cancel(lv_request_id) noexcept;
    bool current(const Slot &) const;
    Submit submit_;
    Cancel cancel_;
    std::array<Target, 2> targets_;
    std::array<Slot, 2> slots_;
    std::size_t count_ = 0;
    std::filesystem::path selected_;
    std::uint64_t generation_ = 0, plan_ = 0;
    bool closing_ = false, foreground_idle_ = false;
    Stats stats_;
};
} // namespace liltview

#endif
