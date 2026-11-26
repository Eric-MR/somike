#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include "somike/dsp/gain.h"
#include "somike/dsp/ring_reader.h"
#include "somike/dsp/ring_writer.h"
#include "somike/protocol/shared_ring.h"
#include "support/test_region.h"

namespace somike::dsp {
namespace {

namespace proto = somike::protocol;

constexpr float kUntouched = 12345.0f;

// Frame i carries (i, -i): positions can be checked exactly (integers below
// 2^24 are exact in float).
std::vector<float> Ramp(std::uint64_t first, std::size_t frames) {
    std::vector<float> data(frames * 2);
    for (std::size_t f = 0; f < frames; ++f) {
        const auto value = static_cast<float>(first + f);
        data[2 * f] = value;
        data[2 * f + 1] = -value;
    }
    return data;
}

class RingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(writer.valid());
        ASSERT_TRUE(reader.Attach(region.span()));
    }

    void Write(std::size_t frames) {
        const std::vector<float> data = Ramp(written, frames);
        writer.Write(data.data(), frames, now);
        written += frames;
    }

    ReadResult Read(std::size_t frames) {
        out.assign(frames * 2, kUntouched);
        return reader.Read(out.data(), frames, now);
    }

    // Primes and starts a running reader that has consumed one 480 block.
    void StartRunning() {
        writer.BeginSession(now);
        ASSERT_EQ(Read(480).frames, 0u);  // primes at the current write position
        Write(1920);
        ASSERT_EQ(Read(480).frames, 480u);
        ASSERT_TRUE(reader.running());
    }

    test::TestRegion region;
    RingWriter writer{region.span()};
    RingReader reader;
    std::vector<float> out;
    std::uint64_t written = 0;
    std::uint64_t now = 1'000'000;
};

TEST(RingRegion, WriterInitialisesAFreshHeader) {
    test::TestRegion region;
    const RingWriter writer(region.span());
    ASSERT_TRUE(writer.valid());

    const proto::RegionHeader* header = proto::HeaderOf(region.data());
    EXPECT_TRUE(proto::HeaderIsCompatible(*header));
    EXPECT_EQ(header->producer.state, static_cast<std::uint32_t>(proto::ProducerState::Idle));
    EXPECT_EQ(std::bit_cast<float>(header->producer.inject_gain_bits), 1.0f);
    EXPECT_EQ(std::bit_cast<float>(header->producer.mic_gain_bits), 1.0f);
}

TEST(RingRegion, RejectsTooSmallOrMisalignedRegions) {
    test::TestRegion region;
    EXPECT_FALSE(RingWriter(region.span().first(proto::kRegionBytes - 1)).valid());
    EXPECT_FALSE(RingWriter(region.raw().subspan(8, proto::kRegionBytes)).valid());

    RingReader reader;
    EXPECT_FALSE(reader.Attach(region.raw().subspan(8, proto::kRegionBytes)));
    EXPECT_FALSE(reader.attached());
}

TEST(RingRegion, InspectRejectsUninitialisedRegion) {
    test::TestRegion region;
    EXPECT_FALSE(InspectRing(region.span()).compatible);
}

TEST_F(RingTest, InactiveUntilASessionBegins) {
    Write(4000);
    const ReadResult result = Read(480);
    EXPECT_FALSE(result.active);
    EXPECT_EQ(result.frames, 0u);
    EXPECT_EQ(result.mic_gain, 1.0f);
}

TEST_F(RingTest, PrimesToTargetLatencyBeforeDelivering) {
    writer.BeginSession(now);
    ReadResult result = Read(480);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.frames, 0u);

    Write(1000);
    EXPECT_EQ(Read(480).frames, 0u);  // 1000 < 1920 target

    Write(1000);
    result = Read(480);
    ASSERT_EQ(result.frames, 480u);
    // Playback starts exactly `target` frames behind the writer: 2000 - 1920.
    EXPECT_EQ(out[0], 80.0f);
    EXPECT_EQ(out[1], -80.0f);
    EXPECT_EQ(out[2 * 479], 559.0f);
    EXPECT_EQ(reader.backlog(), 2000u - 560u);
}

TEST_F(RingTest, DeliversContinuousAudioAcrossWrapAround) {
    StartRunning();
    EXPECT_EQ(out[0], 0.0f);

    std::uint64_t expected = 480;
    // 200 blocks of 480 frames = ~3 trips around the 32768-frame ring.
    for (int block = 0; block < 200; ++block) {
        Write(480);
        ASSERT_EQ(Read(480).frames, 480u);
        for (std::size_t f = 0; f < 480; ++f) {
            ASSERT_EQ(out[2 * f], static_cast<float>(expected + f)) << "block " << block;
        }
        expected += 480;
    }
    EXPECT_EQ(reader.underruns(), 0u);
    EXPECT_EQ(reader.resyncs(), 0u);
}

TEST_F(RingTest, UnderrunDeliversWhatIsThereAndReprimes) {
    StartRunning();  // 1440 frames left in the ring

    const ReadResult result = Read(2000);
    EXPECT_EQ(result.frames, 1440u);
    EXPECT_EQ(out[2 * 1440], kUntouched);  // nothing written past the delivered frames
    EXPECT_EQ(reader.underruns(), 1u);
    EXPECT_FALSE(reader.running());

    Write(1000);
    EXPECT_EQ(Read(480).frames, 0u);  // priming again
    Write(1000);
    EXPECT_EQ(Read(480).frames, 480u);
}

TEST_F(RingTest, ResyncsWhenFallingTooFarBehind) {
    StartRunning();
    Write(10000);  // backlog 11440 > 7200 max latency

    ASSERT_EQ(Read(480).frames, 480u);
    EXPECT_EQ(reader.resyncs(), 1u);
    EXPECT_EQ(out[0], static_cast<float>(written - 1920));  // jumped back to target latency
}

TEST_F(RingTest, ProducerHeartbeatTimeoutDeactivates) {
    StartRunning();
    now += 501;  // last heartbeat is older than producer_timeout_ms

    const ReadResult result = Read(480);
    EXPECT_FALSE(result.active);
    EXPECT_EQ(result.mic_gain, 1.0f);
    EXPECT_FALSE(reader.running());
}

TEST_F(RingTest, EndSessionDeactivates) {
    StartRunning();
    writer.EndSession(now);
    EXPECT_FALSE(Read(480).active);
}

TEST_F(RingTest, NewSessionNeverReplaysStaleAudio) {
    StartRunning();
    Write(3000);
    writer.EndSession(now);
    EXPECT_FALSE(Read(480).active);

    writer.BeginSession(now);
    EXPECT_EQ(Read(480).frames, 0u);
    const std::uint64_t restart = written;
    Write(1920);
    ASSERT_EQ(Read(480).frames, 480u);
    EXPECT_GE(out[0], static_cast<float>(restart));
}

TEST_F(RingTest, SessionChangeWhileStreamingReprimes) {
    StartRunning();
    writer.BeginSession(now);  // producer restarted without an idle gap
    EXPECT_EQ(Read(480).frames, 0u);
}

TEST_F(RingTest, UntrustedGainsAreSanitised) {
    writer.BeginSession(now);
    writer.SetGains(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
    ReadResult result = Read(480);
    EXPECT_TRUE(result.active);
    EXPECT_EQ(result.inject_gain, 0.0f);  // garbage never turns routed audio on
    EXPECT_EQ(result.mic_gain, 1.0f);     // ...and never mutes the microphone

    writer.SetGains(100.0f, -3.0f);
    result = Read(480);
    EXPECT_EQ(result.inject_gain, kMaxGain);
    EXPECT_EQ(result.mic_gain, 0.0f);
}

TEST_F(RingTest, OversizedWriteKeepsTheNewestFrames) {
    const std::size_t frames = proto::kCapacityFrames + 100;
    Write(frames);
    EXPECT_EQ(writer.write_frame(), frames);

    const float* samples = proto::SamplesOf(region.data());
    EXPECT_EQ(samples[2 * 99], static_cast<float>(frames - 1));  // newest frame
    EXPECT_EQ(samples[2 * 100], 100.0f);                          // oldest survivor
}

TEST_F(RingTest, InspectRingReportsProducerState) {
    writer.BeginSession(now);
    Write(480);

    const RingSnapshot snapshot = InspectRing(region.span());
    EXPECT_TRUE(snapshot.compatible);
    EXPECT_TRUE(snapshot.producer_streaming);
    EXPECT_EQ(snapshot.write_frame, 480u);
    EXPECT_EQ(snapshot.producer_heartbeat_ms, now);
    EXPECT_EQ(snapshot.session, 1u);
}

TEST_F(RingTest, IndependentReadersDoNotInterfere) {
    StartRunning();
    RingReader second;
    ASSERT_TRUE(second.Attach(region.span()));
    std::vector<float> other(960);
    EXPECT_EQ(second.Read(other.data(), 480, now).frames, 0u);  // primes on its own

    Write(1920);
    ASSERT_EQ(Read(480).frames, 480u);
    EXPECT_EQ(out[0], 480.0f);  // the first reader continues where it was
    ASSERT_EQ(second.Read(other.data(), 480, now).frames, 480u);
}

}  // namespace
}  // namespace somike::dsp
