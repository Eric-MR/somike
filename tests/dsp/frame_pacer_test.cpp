#include "somike/dsp/frame_pacer.h"

#include <gtest/gtest.h>

namespace somike::dsp {
namespace {

TEST(FramePacer, HandsOutFramesAtTheWallClockRate) {
    FramePacer pacer;
    pacer.Reset(1000.0);
    EXPECT_EQ(pacer.Due(1000.0, 4800), 0u);
    EXPECT_EQ(pacer.Due(1010.0, 4800), 480u);
    pacer.Produced(480);
    EXPECT_EQ(pacer.Due(1010.0, 4800), 0u);
    EXPECT_EQ(pacer.Due(1025.5, 4800), 744u);  // 15.5 ms later
}

TEST(FramePacer, ForgetsABacklogBeyondTheBurstLimit) {
    FramePacer pacer;
    pacer.Reset(0.0);
    EXPECT_EQ(pacer.Due(1000.0, 4800), 4800u);  // a second late: only 100 ms are made up
    pacer.Produced(4800);
    EXPECT_EQ(pacer.Due(1000.0, 4800), 0u);
    EXPECT_EQ(pacer.Due(1010.0, 4800), 480u);
}

TEST(FramePacer, ContinuesSeamlesslyAfterAnotherClockStops) {
    // The capture thread wrote its last block at t = 500 ms (and re-anchored
    // the pacer); the pump notices 30 ms later and fills exactly the gap.
    FramePacer pacer;
    pacer.Reset(500.0);
    EXPECT_EQ(pacer.Due(530.0, 4800), 1440u);
}

}  // namespace
}  // namespace somike::dsp
