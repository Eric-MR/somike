#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "somike/dsp/mixer.h"

namespace somike::dsp {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

TEST(Mixer, StereoAddsChannelWise) {
    std::vector<float> io = {0.1f, 0.2f};
    const std::vector<float> inject = {0.3f, 0.4f};
    MixInto(io.data(), 1, 2, inject.data(), 1.0f, 1.0f, false);
    EXPECT_FLOAT_EQ(io[0], 0.4f);
    EXPECT_FLOAT_EQ(io[1], 0.6f);
}

TEST(Mixer, MonoReceivesDownmix) {
    std::vector<float> io = {0.1f};
    const std::vector<float> inject = {0.2f, 0.4f};
    MixInto(io.data(), 1, 1, inject.data(), 1.0f, 1.0f, false);
    EXPECT_FLOAT_EQ(io[0], 0.4f);  // 0.1 + (0.2 + 0.4) / 2
}

TEST(Mixer, MultiChannelArraysReceiveDownmixOnEveryChannel) {
    std::vector<float> io(4, 0.0f);
    const std::vector<float> inject = {0.2f, 0.4f};
    MixInto(io.data(), 1, 4, inject.data(), 1.0f, 1.0f, false);
    for (float sample : io) {
        EXPECT_FLOAT_EQ(sample, 0.3f);
    }
}

TEST(Mixer, AppliesBothGains) {
    std::vector<float> io = {0.4f, 0.4f};
    const std::vector<float> inject = {0.2f, 0.2f};
    MixInto(io.data(), 1, 2, inject.data(), 0.5f, 0.25f, false);
    EXPECT_FLOAT_EQ(io[0], 0.2f);  // 0.4 * 0.25 + 0.2 * 0.5
}

TEST(Mixer, SilentInputContributesNothingEvenIfGarbage) {
    std::vector<float> io = {kNaN, 7.0f};
    const std::vector<float> inject = {0.25f, -0.25f};
    MixInto(io.data(), 1, 2, inject.data(), 1.0f, 1.0f, true);
    EXPECT_FLOAT_EQ(io[0], 0.25f);
    EXPECT_FLOAT_EQ(io[1], -0.25f);
}

TEST(Mixer, LimitsAndSanitisesOutput) {
    std::vector<float> io = {0.9f, -0.9f, 0.0f, 0.0f};
    const std::vector<float> inject = {0.9f, -0.9f, kNaN, kNaN};
    MixInto(io.data(), 2, 2, inject.data(), 1.0f, 1.0f, false);
    EXPECT_EQ(io[0], 1.0f);
    EXPECT_EQ(io[1], -1.0f);
    EXPECT_EQ(io[2], 0.0f);
    EXPECT_EQ(io[3], 0.0f);
}

TEST(Mixer, SanitizeSample) {
    EXPECT_EQ(SanitizeSample(kNaN), 0.0f);
    EXPECT_EQ(SanitizeSample(std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(SanitizeSample(2.0f), 1.0f);
    EXPECT_EQ(SanitizeSample(-0.5f), -0.5f);
}

}  // namespace
}  // namespace somike::dsp
