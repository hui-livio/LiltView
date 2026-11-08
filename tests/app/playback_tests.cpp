#include <catch2/catch_test_macros.hpp>
#include "viewer/playback.hpp"
using namespace std::chrono_literals;
using liltview::Playback;
namespace
{
lv_animation_frame_info frame(std::uint64_t index, std::uint64_t duration, bool known = true)
{
    return {{1, 1, index}, duration, 0, known ? static_cast<lv_animation_frame_flags>(LV_ANIMATION_FRAME_DURATION_KNOWN) : 0u, 0};
}
} // namespace

TEST_CASE("playback respects complete candidate deadlines and clamps zero or tiny durations", "[playback]")
{
    Playback player;
    const Playback::Time start{100s};
    player.candidate(frame(0, 0), start);
    CHECK(player.due(start));
    player.presented(start);
    player.candidate(frame(1, 2000000), start + 5ms);
    CHECK_FALSE(player.due(start + 99ms));
    CHECK(player.wait_ms(start + 95ms, 200) == 5);
    CHECK(player.due(start + 100ms));
    player.presented(start + 100ms);
    CHECK(player.frame_index() == 1);
    player.candidate(frame(2, 30000000), start + 101ms);
    CHECK_FALSE(player.due(start + 109ms));
    CHECK(player.due(start + 110ms));
    player.presented(start + 110ms);
    player.candidate(frame(3, 0, false), start + 111ms);
    CHECK_FALSE(player.due(start + 139ms));
    CHECK(player.due(start + 140ms));
}
TEST_CASE("late decode rebases playback while timely uploads keep scheduled cadence", "[playback]")
{
    Playback player;
    const Playback::Time start{100s};
    player.candidate(frame(0, 20000000), start);
    player.presented(start);
    player.candidate(frame(1, 30000000), start + 40ms);
    player.presented(start + 45ms);
    player.candidate(frame(2, 10000000), start + 46ms);
    CHECK_FALSE(player.due(start + 74ms));
    CHECK(player.due(start + 75ms));
    player.presented(start + 77ms); // prepared on time; scheduler jitter is not accumulated
    player.candidate(frame(3, 10000000), start + 78ms);
    CHECK_FALSE(player.due(start + 84ms));
    CHECK(player.due(start + 85ms));
}
TEST_CASE("suspension keeps remaining frame time and avoids background catch-up", "[playback]")
{
    Playback player;
    const Playback::Time start{100s};
    player.candidate(frame(0, 20000000), start);
    player.presented(start);
    player.candidate(frame(1, 20000000), start + 1ms);
    player.suspend(true, start + 5ms);
    CHECK_FALSE(player.due(start + 1000ms));
    CHECK(player.wait_ms(start + 1000ms, 200) == 200);
    player.suspend(false, start + 1000ms);
    CHECK_FALSE(player.due(start + 1014ms));
    CHECK(player.due(start + 1015ms));
    player.presented(start + 1015ms);
    player.reset();
    CHECK_FALSE(player.started());
    CHECK_FALSE(player.ready());
}
