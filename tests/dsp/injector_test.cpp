// End-to-end tests of the APO's processing core: RingWriter (app side) ->
// shared region -> Injector (APO side), with simulated engine periods.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "somike/dsp/injector.h"
#include "somike/dsp/ring_writer.h"
#include "support/test_region.h"

namespace somike::dsp {
namespace {

class InjectorTest : public ::testing::Test {
protected:
    bool Prepare(EndpointFormat format, std::uint32_t max_frames) {
        if (!injector.Configure(format, max_frames)) {
            return false;
        }
        injector.Attach(region.span(), true);
        return injector.attached();
    }

    void WriteConstant(std::size_t frames, float left, float right) {
        std::vector<float> data(frames * 2);
        for (std::size_t f = 0; f < frames; ++f) {
            data[2 * f] = left;
            data[2 * f + 1] = right;
        }
        writer.Write(data.data(), frames, now);
    }

    // Simulates `blocks` 10 ms engine periods: the app writes `produced`
    // frames, then the APO processes `consumed` frames of microphone audio
    // whose samples all equal `mic_level`. Returns the last processed block.
    std::vector<float> RunBlocks(int blocks, std::size_t produced, std::uint32_t consumed,
                                 std::uint32_t channels, float left, float right,
                                 float mic_level = 0.0f) {
        std::vector<float> buffer;
        for (int b = 0; b < blocks; ++b) {
            now += 10;
            WriteConstant(produced, left, right);
            buffer.assign(std::size_t{consumed} * channels, mic_level);
            injector.Process(buffer.data(), consumed, false, now);
        }
        return buffer;
    }

    test::TestRegion region;
    RingWriter writer{region.span()};
    Injector injector;
    std::uint64_t now = 5'000'000;
};

TEST_F(InjectorTest, RejectsUnsupportedConfigurations) {
    EXPECT_FALSE(injector.Configure({0, 2}, 480));
    EXPECT_FALSE(injector.Configure({48000, 0}, 480));
    EXPECT_FALSE(injector.Configure({48000, 2}, 0));
    EXPECT_FALSE(injector.Configure({48000, 2}, kMaxBlockFrames + 1));
    EXPECT_TRUE(injector.Configure({48000, 2}, 480));
}

TEST_F(InjectorTest, PassesThroughWhenNotAttached) {
    ASSERT_TRUE(injector.Configure({48000, 2}, 480));
    std::vector<float> mic(960, 0.5f);
    EXPECT_FALSE(injector.Process(mic.data(), 480, false, now));
    EXPECT_EQ(mic, std::vector<float>(960, 0.5f));
}

TEST_F(InjectorTest, PassesThroughWhileProducerIsIdle) {
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    WriteConstant(4800, 0.25f, 0.25f);  // data, but no session

    std::vector<float> mic(960, 0.5f);
    EXPECT_FALSE(injector.Process(mic.data(), 480, false, now));
    EXPECT_EQ(mic, std::vector<float>(960, 0.5f));
}

TEST_F(InjectorTest, InjectsIntoStereo48k) {
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    writer.BeginSession(now);

    const std::vector<float> out = RunBlocks(50, 480, 480, 2, 0.25f, -0.25f);
    for (std::size_t f = 0; f < 480; ++f) {
        ASSERT_NEAR(out[2 * f], 0.25f, 1e-5f);
        ASSERT_NEAR(out[2 * f + 1], -0.25f, 1e-5f);
    }
    EXPECT_TRUE(injector.active());

    const ConsumerSnapshot diagnostics = writer.ReadConsumer();
    EXPECT_EQ(diagnostics.sample_rate, 48000u);
    EXPECT_EQ(diagnostics.channels, 2u);
    EXPECT_GT(diagnostics.frames_injected, 0u);
    EXPECT_EQ(diagnostics.heartbeat_ms, now);
}

TEST_F(InjectorTest, ConvertsTo44100Mono) {
    ASSERT_TRUE(Prepare({44100, 1}, 441));
    writer.BeginSession(now);

    const std::vector<float> out = RunBlocks(60, 480, 441, 1, 0.2f, 0.4f);
    for (float sample : out) {
        ASSERT_NEAR(sample, 0.3f, 1e-5f);  // (0.2 + 0.4) / 2
    }
    EXPECT_EQ(writer.ReadConsumer().underruns, 0u);
}

TEST_F(InjectorTest, MixesMicrophoneAndRoutedAudio) {
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    writer.SetGains(0.5f, 1.0f);
    writer.BeginSession(now);

    const std::vector<float> out = RunBlocks(50, 480, 480, 2, 0.4f, 0.4f, 0.2f);
    EXPECT_NEAR(out[0], 0.4f, 1e-5f);  // 0.2 * 1.0 + 0.4 * 0.5
}

TEST_F(InjectorTest, MutesTheRealMicrophoneWhenRequested) {
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    writer.SetGains(1.0f, 0.0f);
    writer.BeginSession(now);

    const std::vector<float> out = RunBlocks(50, 480, 480, 2, 0.1f, 0.1f, 0.5f);
    EXPECT_NEAR(out[0], 0.1f, 1e-5f);
}

TEST_F(InjectorTest, RestoresTheMicrophoneWhenTheProducerVanishes) {
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    writer.SetGains(1.0f, 0.0f);  // "mute my voice" was on...
    writer.BeginSession(now);
    RunBlocks(30, 480, 480, 2, 0.1f, 0.1f, 0.5f);
    ASSERT_TRUE(injector.active());

    now += 1000;  // ...and the app crashed: no heartbeat any more
    std::vector<float> mic(960, 0.5f);
    EXPECT_FALSE(injector.Process(mic.data(), 480, false, now));
    EXPECT_EQ(mic, std::vector<float>(960, 0.5f));  // untouched, full volume
    EXPECT_FALSE(injector.active());
}

// The app's capture clock and the microphone's clock are never identical.
// The drift controller must absorb a 0.2 % mismatch in either direction for
// a long time without a single underrun or resync.
class InjectorDriftTest : public InjectorTest, public ::testing::WithParamInterface<std::size_t> {};

TEST_P(InjectorDriftTest, CompensatesClockDrift) {
    const std::size_t produced_per_block = GetParam();
    ASSERT_TRUE(Prepare({48000, 2}, 480));
    writer.BeginSession(now);

    RunBlocks(3000, produced_per_block, 480, 2, 0.1f, 0.1f);  // 30 s of audio

    const ConsumerSnapshot diagnostics = writer.ReadConsumer();
    EXPECT_EQ(diagnostics.underruns, 0u);
    EXPECT_EQ(diagnostics.resyncs, 0u);
    if (produced_per_block > 480) {
        EXPECT_GT(injector.last_step(), 1.0);  // consuming faster
    } else {
        EXPECT_LT(injector.last_step(), 1.0);  // consuming slower
    }
}

INSTANTIATE_TEST_SUITE_P(ProducerRates, InjectorDriftTest, ::testing::Values(479u, 481u));

}  // namespace
}  // namespace somike::dsp
