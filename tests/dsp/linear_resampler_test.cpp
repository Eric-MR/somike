#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "somike/dsp/linear_resampler.h"

namespace somike::dsp {
namespace {

TEST(LinearResampler, UnitStepIsATwoFrameDelay) {
    LinearResampler resampler(2);
    const std::vector<float> first = {1, -1, 2, -2, 3, -3, 4, -4};
    ASSERT_EQ(resampler.InputFramesRequired(1.0, 4), 4u);

    std::vector<float> out(8);
    resampler.Process(first.data(), 4, out.data(), 4, 1.0);
    EXPECT_EQ(out, (std::vector<float>{0, 0, 0, 0, 1, -1, 2, -2}));

    // The two held-back frames come out first in the next block.
    const std::vector<float> second = {5, -5, 6, -6};
    ASSERT_EQ(resampler.InputFramesRequired(1.0, 2), 2u);
    resampler.Process(second.data(), 2, out.data(), 2, 1.0);
    EXPECT_EQ(out[0], 3.0f);
    EXPECT_EQ(out[2], 4.0f);
}

TEST(LinearResampler, InterpolatesBetweenFrames) {
    LinearResampler resampler(1);
    const std::vector<float> warmup = {0, 0};
    std::vector<float> out(4);
    resampler.Process(warmup.data(), 2, out.data(), 2, 1.0);  // history is now (0, 0)

    const std::vector<float> input = {1, 2};
    ASSERT_EQ(resampler.InputFramesRequired(0.5, 4), 2u);
    resampler.Process(input.data(), 2, out.data(), 4, 0.5);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
    EXPECT_FLOAT_EQ(out[1], 0.0f);
    EXPECT_FLOAT_EQ(out[2], 0.0f);
    EXPECT_FLOAT_EQ(out[3], 0.5f);
}

TEST(LinearResampler, ShortInputNeverReadsOutOfBounds) {
    LinearResampler resampler(2);
    const std::vector<float> input = {0.5f, 0.5f};  // 1 frame, 480 required
    std::vector<float> out(2 * 480, -1.0f);
    resampler.Process(input.data(), 1, out.data(), 480, 1.0);
    for (float sample : out) {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_GE(sample, 0.0f);
        EXPECT_LE(sample, 0.5f);
    }
}

TEST(LinearResampler, ZeroOutputNeedsNoInput) {
    const LinearResampler resampler(2);
    EXPECT_EQ(resampler.InputFramesRequired(1.0, 0), 0u);
    EXPECT_EQ(resampler.InputFramesRequired(0.0, 480), 0u);
}

// Streaming a ramp x[n] = n block by block must yield y = P - 2 at every
// output position P (accumulated time in input frames; 2 = history delay),
// for any step and any sequence of block sizes. This checks interpolation
// and, more importantly, that InputFramesRequired() matches consumption
// exactly - a single lost or duplicated frame would show up as a jump.
class ResamplerStream : public ::testing::TestWithParam<double> {};

TEST_P(ResamplerStream, RampStaysContinuousAcrossBlocks) {
    const double step = GetParam();
    LinearResampler resampler(1);
    std::uint64_t next_input = 0;
    double position = 0.0;
    const std::size_t block_sizes[] = {480, 441, 1, 37, 1024, 160, 2};

    for (int round = 0; round < 8; ++round) {
        for (const std::size_t n : block_sizes) {
            const std::size_t needed = resampler.InputFramesRequired(step, n);
            std::vector<float> input(needed);
            for (float& value : input) {
                value = static_cast<float>(next_input++);
            }
            std::vector<float> out(n);
            resampler.Process(input.data(), needed, out.data(), n, step);

            for (std::size_t j = 0; j < n; ++j) {
                const double p = position + static_cast<double>(j) * step;
                const double expected = std::max(0.0, p - 2.0);
                ASSERT_NEAR(out[j], expected, 0.02)
                    << "step " << step << ", round " << round << ", block " << n << ", j " << j;
            }
            position += static_cast<double>(n) * step;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(Steps, ResamplerStream,
                         ::testing::Values(0.5, 0.9187, 0.995, 1.0, 1.0005, 1.0884, 2.0, 3.0));

}  // namespace
}  // namespace somike::dsp
