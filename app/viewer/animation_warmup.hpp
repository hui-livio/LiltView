#ifndef LILTVIEW_ANIMATION_WARMUP_HPP
#define LILTVIEW_ANIMATION_WARMUP_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <liltview/core.h>
#include <memory>
#include <optional>
#include <vector>

namespace liltview
{
// Owned Core references, with the original unmodified tile/frame identity.
// The Core must outlive these references; no GPU object is created here.
struct PreparedAnimationTile
{
    lv_region_info region{};
    explicit PreparedAnimationTile(const lv_region_info &) noexcept;
    ~PreparedAnimationTile();
    PreparedAnimationTile(PreparedAnimationTile &&) noexcept;
    PreparedAnimationTile &operator=(PreparedAnimationTile &&) noexcept;
    PreparedAnimationTile(const PreparedAnimationTile &)            = delete;
    PreparedAnimationTile &operator=(const PreparedAnimationTile &) = delete;
};

struct PreparedAnimationFrame
{
    lv_animation_frame_info info{};
    std::vector<PreparedAnimationTile> tiles;
};

// Transfer result after foreground NEXT has revalidated the source and the
// terminal event has been released. First is frame 0; second, when present, is
// the real frame 1 in that same epoch. No RESET, seeking or key relabeling.
// A discarded result closes its session. Viewer takes ownership using
// release_session(), presents first once, and schedules second/END normally.
struct PreparedAnimation
{
    lv_animation_id session = 0;
    lv_animation_info info{};
    PreparedAnimationFrame first;
    std::optional<PreparedAnimationFrame> second;
    bool ended = false; // Validation NEXT returned END after the one-frame GIF.

    PreparedAnimation() = default;
    ~PreparedAnimation();
    PreparedAnimation(PreparedAnimation &&) noexcept;
    PreparedAnimation &operator=(PreparedAnimation &&) noexcept;
    PreparedAnimation(const PreparedAnimation &)            = delete;
    PreparedAnimation &operator=(const PreparedAnimation &) = delete;
    lv_animation_id release_session() noexcept;

private:
    friend class AnimationWarmup;
    lv_core *core_ = nullptr;
};

// UI/control-thread helper on a borrowed Core, limited to two warm sessions.
// Input paths are GIF hints or unclassified candidates; Core OPEN is the final
// content classifier. Non-GIF/corrupt candidates fail only in the background.
// This class does not inspect extensions, open files directly, upload pixels,
// drive a playback clock or change the foreground image.
class AnimationWarmup
{
public:
    struct Stats
    {
        std::size_t ready = 0, outstanding_requests = 0;
        // Conservative retained reservation, including pooled backing capacity
        // and descriptors; not an exact allocation/RSS measurement.
        std::uint64_t retained_bytes = 0, completed = 0, adopted = 0, failed = 0;
        bool adoption_pending = false;
    };

    explicit AnimationWarmup(lv_core *, std::uint64_t retained_budget = 64ULL * 1024 * 1024);
    ~AnimationWarmup();
    AnimationWarmup(const AnimationWarmup &)            = delete;
    AnimationWarmup &operator=(const AnimationWarmup &) = delete;

    // Only the first two distinct paths are desired. Changed bounds retire old
    // warm results; obsolete generations never yield an adoption/failure.
    // At most three successfully accepted warm OPENs per target/plan; BUSY does
    // not count. Generation/bounds/neighbor changes create a fresh allowance.
    void update(std::uint64_t generation, const std::vector<std::filesystem::path> &, lv_size_u32 fit_bounds);

    // Invoke only outside event consumption, after terminal events are released.
    // available_requests means additional free slots in the caller's GLOBAL
    // two-request background quota, not a per-module allowance. Existing IDs,
    // including cancelled IDs awaiting terminals, remain counted by stats().
    // Warm OPEN/NEXT use VISIBLE_DETAIL only while foreground_idle; foreground
    // adoption verification uses CURRENT and can advance while it is false.
    void pump(bool foreground_idle, std::size_t available_requests = 2);
    // Includes unsubmitted/BUSY stages and tracked retired IDs, not just live
    // requests. READY-only is idle. After shutdown, use Core join instead:
    // suppressed terminal events can leave an ID tracked until destruction.
    bool pending() const noexcept;
    bool owns(lv_request_id) const noexcept;
    bool consume(const lv_event_view &); // Borrowed view; caller still releases the event.

    // Cancel/close unfinished background work and preserve complete READY work.
    // The caller invokes this before every ordinary foreground OPEN/NEXT/RESET.
    void interrupt_background();

    // True means an exact READY path/bounds match was accepted for foreground
    // NEXT validation; it does not mean the pixels are safe to display yet.
    // pump handles BUSY retry. Any error/cancel closes the session and latches
    // one failure for the caller's ordinary-open fallback, never stale frame 0.
    bool request_adoption(const std::filesystem::path &, lv_size_u32 bounds, std::uint64_t new_tag);
    std::optional<PreparedAnimation> take_adoption();
    bool take_adoption_failure() noexcept;

    // Stop submissions and immediately return retained CPU refs while Core is
    // still alive. Caller then drains/joins Core; destruction after this point
    // performs no further Core calls, even if shutdown suppressed terminals.
    void begin_shutdown();
    Stats stats() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace liltview

#endif
