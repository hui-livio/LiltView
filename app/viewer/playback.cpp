#include "playback.hpp"
#include <algorithm>
#include <limits>

namespace liltview
{
void Playback::reset()
{
    *this = {};
}
std::chrono::nanoseconds Playback::duration(const lv_animation_frame_info &frame)
{
    constexpr std::uint64_t millisecond = 1000000;
    std::uint64_t ns                    = !(frame.flags & LV_ANIMATION_FRAME_DURATION_KNOWN) || !frame.source_duration_ns ?
        100 * millisecond :
        std::max(frame.source_duration_ns, 10 * millisecond);
    // Saturate before converting to the signed chrono representation.
    ns = std::min(ns, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    return std::chrono::nanoseconds(static_cast<std::int64_t>(ns));
}
void Playback::candidate(lv_animation_frame_info frame, Time ready)
{
    candidate_ = frame;
    ready_at_  = ready;
    ready_     = true;
}
bool Playback::due(Time now) const
{
    return ready_ && !suspended_ && (!started_ || now >= deadline_);
}
void Playback::presented(Time now)
{
    const auto base  = !started_ || ready_at_ > deadline_ ? now : deadline_;
    const auto span  = std::chrono::duration_cast<Clock::duration>(duration(candidate_));
    deadline_        = span > Time::max() - base ? Time::max() : base + span;
    displayed_index_ = candidate_.key.frame_index;
    ready_           = false;
    started_         = true;
}
void Playback::suspend(bool suspended, Time now)
{
    if (suspended == suspended_)
        return;
    if (suspended && started_)
        remaining_ = deadline_ > now ? deadline_ - now : Clock::duration::zero();
    if (!suspended && started_)
    {
        deadline_ = remaining_ > Time::max() - now ? Time::max() : now + remaining_;
        // A prepared candidate may now be shown at the resumed deadline.
        ready_at_ = std::min(ready_at_, deadline_);
    }
    suspended_ = suspended;
}
int Playback::wait_ms(Time now, int maximum) const
{
    if (suspended_ || !ready_)
        return maximum;
    if (!started_ || now >= deadline_)
        return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now).count();
    return static_cast<int>(std::min<std::int64_t>(ms, maximum));
}
} // namespace liltview
