#include "somike/dsp/pump_scheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>

namespace somike::dsp {
namespace {

using Action = PumpStep::Action;

// Plays the router's part: executes the steps the scheduler asks for, with a
// sound of `sound_frames` that is "active" until all of it was written.
struct FakeRouter {
    PumpScheduler scheduler;
    bool session_open = false;
    bool capture_running = false;
    std::size_t sound_left = 0;
    std::size_t silence_written = 0;
    std::size_t sound_written = 0;
    int sessions_begun = 0;
    int sessions_ended = 0;

    PumpStep Tick(double now_ms) {
        const PumpStep step = scheduler.Next(now_ms, sound_left > 0, capture_running, session_open);
        if (step.begin_session) {
            session_open = true;
            ++sessions_begun;
        }
        if (step.end_session) {
            session_open = false;
            ++sessions_ended;
        }
        if (step.action == Action::Write) {
            silence_written += step.silence_frames;
            sound_written += step.sound_frames;
            sound_left -= std::min(sound_left, step.sound_frames);
            scheduler.Wrote(now_ms, step);
        }
        return step;
    }
};

TEST(PumpScheduler, FirstSoundAfterIdleIsWrittenRightAway) {
    // Regression: the gap check used to re-anchor the pacer on every tick
    // until the first write, so nothing ever became due and the sound hung.
    FakeRouter router;
    router.sound_left = 48000;
    router.scheduler.Kick(1000.0);

    PumpStep step = router.Tick(1000.0);
    EXPECT_EQ(step.action, Action::Tick);
    EXPECT_TRUE(step.begin_session);

    step = router.Tick(1010.0);
    ASSERT_EQ(step.action, Action::Write);
    EXPECT_EQ(step.silence_frames, 480u);
    EXPECT_EQ(step.sound_frames, 0u);

    router.Tick(1020.0);
    router.Tick(1030.0);
    EXPECT_EQ(router.silence_written, 1440u);  // the 30 ms lead-in
    step = router.Tick(1040.0);
    ASSERT_EQ(step.action, Action::Write);
    EXPECT_EQ(step.silence_frames, 0u);
    EXPECT_EQ(step.sound_frames, 480u);
    EXPECT_EQ(router.sessions_begun, 1);
}

TEST(PumpScheduler, StreamsInRealTimeThenSleepsAfterTheTail) {
    FakeRouter router;
    router.sound_left = 24000;  // 500 ms
    router.scheduler.Kick(0.0);

    double now = 0.0;
    double sound_done_ms = -1.0;
    PumpStep step;
    do {
        step = router.Tick(now);
        if (sound_done_ms < 0.0 && router.sound_left == 0) {
            sound_done_ms = now;
        }
        now += 10.0;
    } while (step.action != Action::Sleep && now < 10000.0);

    EXPECT_GE(router.sound_written, 24000u);
    // Lead-in + sound took real time: 30 ms + 500 ms, give or take one tick.
    EXPECT_NEAR(sound_done_ms, 530.0, 10.0);
    // Then the tail of silence (written as mix frames: the voices are empty).
    EXPECT_NEAR(now - 10.0 - sound_done_ms, 1500.0, 20.0);
    const double written_ms = static_cast<double>(router.silence_written + router.sound_written) / 48.0;
    EXPECT_NEAR(written_ms, now - 10.0 - 10.0, 15.0);  // paced by the clock, from the first write
    EXPECT_TRUE(step.end_session);
    EXPECT_FALSE(router.session_open);
}

TEST(PumpScheduler, YieldsToTheCaptureThreadAndBackfillsWhenItStops) {
    FakeRouter router;
    router.capture_running = true;
    router.session_open = true;
    router.sound_left = 48000;
    router.scheduler.OnCaptureStart(0.0);
    router.scheduler.Kick(0.0);

    EXPECT_EQ(router.Tick(10.0).action, Action::Tick);  // waiting for the first block
    router.scheduler.OnCaptureBlock(20.0);
    EXPECT_EQ(router.Tick(40.0).action, Action::Tick);  // 20 ms since the block: capture is the clock

    // The app fell silent: 35 ms after its last block the pump fills exactly
    // the gap, with no lead-in (the reader has not run dry).
    const PumpStep step = router.Tick(55.0);
    ASSERT_EQ(step.action, Action::Write);
    EXPECT_FALSE(step.begin_session);
    EXPECT_EQ(step.silence_frames, 0u);
    EXPECT_EQ(step.sound_frames, 35u * 48u);
}

TEST(PumpScheduler, LeadsInAgainAfterAStall) {
    FakeRouter router;
    router.sound_left = 480000;
    router.scheduler.Kick(0.0);
    for (double now = 0.0; now <= 100.0; now += 10.0) {
        router.Tick(now);
    }
    const std::size_t silence_before = router.silence_written;

    // The pump thread stalled for 200 ms: the reader ran dry and re-primes,
    // so the pump starts a fresh timeline with a lead-in instead of a burst.
    EXPECT_EQ(router.Tick(300.0).action, Action::Tick);
    const PumpStep step = router.Tick(310.0);
    ASSERT_EQ(step.action, Action::Write);
    EXPECT_EQ(step.silence_frames, 480u);
    EXPECT_EQ(router.silence_written - silence_before, 480u);
    EXPECT_EQ(router.sessions_begun, 1);  // same session
}

TEST(PumpScheduler, KeepsTheSessionWhileCaptureRuns) {
    FakeRouter router;
    router.capture_running = true;
    router.session_open = true;
    router.scheduler.Kick(0.0);
    PumpStep step = router.Tick(2000.0);  // tail over, no sound
    EXPECT_EQ(step.action, Action::Sleep);
    EXPECT_FALSE(step.end_session);  // the capture owns the session

    router.capture_running = false;
    step = router.Tick(2010.0);
    EXPECT_EQ(step.action, Action::Sleep);
    EXPECT_TRUE(step.end_session);
}

TEST(PumpScheduler, SleepsWithoutTouchingTheRingWhenNothingPlays) {
    FakeRouter router;
    const PumpStep step = router.Tick(5000.0);
    EXPECT_EQ(step.action, Action::Sleep);
    EXPECT_FALSE(step.begin_session);
    EXPECT_FALSE(step.end_session);
    EXPECT_FALSE(router.scheduler.Sounding(5000.0));
}

}  // namespace
}  // namespace somike::dsp
