#ifndef LILTVIEW_PLAYBACK_HPP
#define LILTVIEW_PLAYBACK_HPP
#include <chrono>
#include <cstdint>
#include <liltview/core.h>

namespace liltview
{
// Scheduling only: no decoder, platform clock calls, or renderer ownership.
// The injected time points let tests verify deadlines without sleeps.
class Playback
{
public:
    using Clock = std::chrono::steady_clock;
    using Time  = Clock::time_point;
    void reset();
    void candidate(lv_animation_frame_info frame, Time ready);
    bool due(Time now) const;
    void presented(Time now);
    void suspend(bool suspended, Time now);
    int wait_ms(Time now, int maximum) const;
    bool ready() const
    {
        return ready_;
    }
    bool started() const
    {
        return started_;
    }
    std::uint64_t frame_index() const
    {
        return displayed_index_;
    }

private:
    static std::chrono::nanoseconds duration(const lv_animation_frame_info &);
    lv_animation_frame_info candidate_{};
    Time deadline_{}, ready_at_{};
    Clock::duration remaining_{};
    bool ready_ = false, started_ = false, suspended_ = false;
    std::uint64_t displayed_index_ = 0;
};
} // namespace liltview
#endif
