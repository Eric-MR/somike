#include "somike/dsp/canonical_convert.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace somike::dsp {
namespace {

std::vector<float> Sine(double frequency, std::uint32_t rate, std::size_t frames, std::uint32_t channels) {
    std::vector<float> out(frames * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * static_cast<double>(f) / rate));
        for (std::uint32_t c = 0; c < channels; ++c) {
            out[f * channels + c] = v;
        }
    }
    return out;
}

double Rms(const std::vector<float>& stereo, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t f = from; f < to; ++f) {
        sum += static_cast<double>(stereo[2 * f]) * stereo[2 * f];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

TEST(CanonicalConvert, MonoGoesToBothSides) {
    const std::vector<float> mono = {0.1f, -0.2f, 0.3f};
    const std::vector<float> out = ConvertToCanonical(mono, 1, 48000);
    ASSERT_EQ(out.size(), 6u);
    EXPECT_FLOAT_EQ(out[2], -0.2f);
    EXPECT_FLOAT_EQ(out[3], -0.2f);
}

TEST(CanonicalConvert, SurroundKeepsFrontAndCentre) {
    // FL FR FC LFE BL BR
    const std::vector<float> surround = {0.1f, 0.2f, 0.5f, 0.9f, 0.9f, 0.9f};
    const std::vector<float> out = ToStereo(surround, 6);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_NEAR(out[0], 0.1f + 0.5f * 0.7071f, 1e-4f);
    EXPECT_NEAR(out[1], 0.2f + 0.5f * 0.7071f, 1e-4f);
}

TEST(CanonicalConvert, CanonicalInputPassesThrough) {
    const std::vector<float> in = Sine(440.0, 48000, 480, 2);
    EXPECT_EQ(ConvertToCanonical(in, 2, 48000), in);
}

TEST(CanonicalConvert, UpsamplingKeepsPitchAndLevel) {
    const std::vector<float> in = Sine(1000.0, 44100, 44100, 1);  // one second
    const std::vector<float> out = ConvertToCanonical(in, 1, 44100);
    const std::size_t frames = out.size() / 2;
    EXPECT_NEAR(static_cast<double>(frames), 48000.0, 2.0);

    // Away from the edges: full level, and 1000 periods per second.
    EXPECT_NEAR(Rms(out, 4800, 43200), 1.0 / std::sqrt(2.0), 0.01);
    int crossings = 0;
    for (std::size_t f = 4801; f < 43200; ++f) {
        if ((out[2 * (f - 1)] < 0.0f) != (out[2 * f] < 0.0f)) {
            ++crossings;
        }
    }
    EXPECT_NEAR(crossings, 2 * 1000 * 0.8, 3);
}

TEST(CanonicalConvert, DownsamplingFiltersWhatCannotBeRepresented) {
    // 30 kHz is above the 24 kHz Nyquist limit of 48 kHz: it must not fold back.
    const std::vector<float> in = Sine(30000.0, 96000, 96000, 2);
    const std::vector<float> out = ConvertToCanonical(in, 2, 96000);
    EXPECT_NEAR(static_cast<double>(out.size() / 2), 48000.0, 2.0);
    EXPECT_LT(Rms(out, 4800, 43200), 0.01);

    // ...while 1 kHz goes through untouched.
    const std::vector<float> low = ConvertToCanonical(Sine(1000.0, 96000, 96000, 2), 2, 96000);
    EXPECT_NEAR(Rms(low, 4800, 43200), 1.0 / std::sqrt(2.0), 0.01);
}

TEST(CanonicalConvert, NonFiniteSamplesBecomeSilence) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<float> in = {nan, inf, 0.5f, 0.5f};
    const std::vector<float> out = ConvertToCanonical(in, 2, 48000);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
    EXPECT_FLOAT_EQ(out[2], 0.5f);
}

TEST(CanonicalConvert, DegenerateInputGivesNothing) {
    const std::vector<float> in = {0.1f, 0.2f};
    EXPECT_TRUE(ConvertToCanonical({}, 2, 44100).empty());
    EXPECT_TRUE(ConvertToCanonical(in, 0, 44100).empty());
    EXPECT_TRUE(ConvertToCanonical(in, 2, 0).empty());
}

}  // namespace
}  // namespace somike::dsp
