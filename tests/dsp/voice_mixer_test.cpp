#include "somike/dsp/voice_mixer.h"

#include <gtest/gtest.h>

#include <vector>

namespace somike::dsp {
namespace {

ClipPtr ConstantClip(std::size_t frames, float value) {
    auto clip = std::make_shared<Clip>();
    clip->samples.assign(frames * 2, value);
    return clip;
}

TEST(VoiceMixer, PlaysAClipOnceAndEnds) {
    VoiceMixer mixer;
    mixer.Play(1, ConstantClip(100, 0.5f), 1.0f);
    std::vector<float> out(64 * 2, 0.0f);
    EXPECT_TRUE(mixer.MixInto(out.data(), 64));
    EXPECT_FLOAT_EQ(out[0], 0.5f);
    EXPECT_FLOAT_EQ(out[127], 0.5f);

    std::fill(out.begin(), out.end(), 0.0f);
    EXPECT_TRUE(mixer.MixInto(out.data(), 64));
    EXPECT_FLOAT_EQ(out[2 * 35], 0.5f);  // frame 99 of the clip
    EXPECT_FLOAT_EQ(out[2 * 36], 0.0f);  // past the end: untouched
    EXPECT_FALSE(mixer.active());
    EXPECT_FALSE(mixer.MixInto(out.data(), 64));
}

TEST(VoiceMixer, AddsIntoTheBufferAndOverlapsSounds) {
    VoiceMixer mixer;
    mixer.Play(1, ConstantClip(10, 0.25f), 1.0f);
    mixer.Play(2, ConstantClip(10, 0.5f), 0.5f);
    std::vector<float> out(10 * 2, 0.1f);
    mixer.MixInto(out.data(), 10);
    EXPECT_FLOAT_EQ(out[0], 0.1f + 0.25f + 0.25f);
}

TEST(VoiceMixer, PlayingTheSameSoundAgainRestartsIt) {
    VoiceMixer mixer;
    mixer.Play(7, ConstantClip(1000, 0.5f), 1.0f);
    std::vector<float> out(300 * 2, 0.0f);
    mixer.MixInto(out.data(), 300);
    ASSERT_EQ(mixer.Snapshot().at(0).position, 300u);
    mixer.Play(7, ConstantClip(1000, 0.5f), 1.0f);
    const auto voices = mixer.Snapshot();
    ASSERT_EQ(voices.size(), 1u);
    EXPECT_EQ(voices[0].position, 0u);
    EXPECT_EQ(voices[0].length, 1000u);
}

TEST(VoiceMixer, StopFadesOutInsteadOfClicking) {
    VoiceMixer mixer;
    mixer.Play(1, ConstantClip(48000, 1.0f), 1.0f);
    std::vector<float> out(100 * 2, 0.0f);
    mixer.MixInto(out.data(), 100);
    mixer.Stop(1);
    EXPECT_FALSE(mixer.IsPlaying(1));  // stopping counts as stopped right away

    std::vector<float> tail(1000 * 2, 0.0f);
    mixer.MixInto(tail.data(), 1000);
    EXPECT_GT(tail[0], 0.9f);
    for (std::size_t f = 1; f < VoiceMixer::kFadeFrames; ++f) {
        EXPECT_LE(tail[2 * f], tail[2 * (f - 1)]);
    }
    EXPECT_FLOAT_EQ(tail[2 * VoiceMixer::kFadeFrames], 0.0f);
    EXPECT_FALSE(mixer.active());
}

TEST(VoiceMixer, MasterGainIsRampedAcrossTheBlock) {
    VoiceMixer mixer;
    mixer.Play(1, ConstantClip(1000, 1.0f), 1.0f);
    mixer.SetMasterGain(0.0f);
    std::vector<float> out(100 * 2, 0.0f);
    mixer.MixInto(out.data(), 100);
    EXPECT_NEAR(out[0], 1.0f, 1e-6f);
    EXPECT_LT(out[2 * 99], 0.02f);
    for (std::size_t f = 1; f < 100; ++f) {
        EXPECT_LE(out[2 * f], out[2 * (f - 1)]);
    }
    std::fill(out.begin(), out.end(), 0.0f);
    mixer.MixInto(out.data(), 100);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
}

TEST(VoiceMixer, DropsTheOldestVoiceWhenFull) {
    VoiceMixer mixer;
    for (std::uint32_t id = 1; id <= VoiceMixer::kMaxVoices + 1; ++id) {
        mixer.Play(id, ConstantClip(100, 0.1f), 1.0f);
    }
    EXPECT_EQ(mixer.Snapshot().size(), VoiceMixer::kMaxVoices);
    EXPECT_FALSE(mixer.IsPlaying(1));
    EXPECT_TRUE(mixer.IsPlaying(VoiceMixer::kMaxVoices + 1));
}

TEST(VoiceMixer, IgnoresEmptyClipsAndBadGains) {
    VoiceMixer mixer;
    mixer.Play(1, nullptr, 1.0f);
    mixer.Play(2, std::make_shared<Clip>(), 1.0f);
    EXPECT_FALSE(mixer.active());
    mixer.Play(3, ConstantClip(10, 1.0f), std::numeric_limits<float>::quiet_NaN());
    std::vector<float> out(10 * 2, 0.0f);
    mixer.MixInto(out.data(), 10);
    EXPECT_FLOAT_EQ(out[0], 0.0f);
}

}  // namespace
}  // namespace somike::dsp
