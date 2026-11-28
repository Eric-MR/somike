// Decoding real files with Media Foundation (part of every desktop Windows;
// skipped where it is missing, e.g. Windows Server without the feature).
#include "somike/core/audio_file.h"

#include <gtest/gtest.h>
#include <mfapi.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "somike/core/com_init.h"

namespace somike::core {
namespace {

std::wstring TempPath(const wchar_t* name) {
    wchar_t folder[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, folder);
    return std::wstring(folder) + name;
}

template <class T>
void Put(std::string& out, T value) {
    char bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.append(bytes, sizeof(T));
}

// 16-bit PCM WAV with a 1 kHz sine at half scale.
void WriteWav(const std::wstring& path, std::uint32_t rate, std::uint16_t channels, double seconds) {
    const auto frames = static_cast<std::uint32_t>(rate * seconds);
    const std::uint32_t data_bytes = frames * channels * 2;
    std::string file = "RIFF";
    Put<std::uint32_t>(file, 36 + data_bytes);
    file += "WAVEfmt ";
    Put<std::uint32_t>(file, 16);
    Put<std::uint16_t>(file, 1);  // PCM
    Put<std::uint16_t>(file, channels);
    Put<std::uint32_t>(file, rate);
    Put<std::uint32_t>(file, rate * channels * 2);
    Put<std::uint16_t>(file, static_cast<std::uint16_t>(channels * 2));
    Put<std::uint16_t>(file, 16);
    file += "data";
    Put<std::uint32_t>(file, data_bytes);
    for (std::uint32_t f = 0; f < frames; ++f) {
        const double v = 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * f / rate);
        for (std::uint16_t c = 0; c < channels; ++c) {
            Put<std::int16_t>(file, static_cast<std::int16_t>(std::lround(v * 32767.0)));
        }
    }
    std::ofstream(path, std::ios::binary).write(file.data(), static_cast<std::streamsize>(file.size()));
}

class AudioFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (FAILED(::MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            GTEST_SKIP() << "Media Foundation is not available";
        }
        ::MFShutdown();
    }
    ComInit com_{COINIT_MULTITHREADED};
};

TEST_F(AudioFileTest, DecodesAWavFileToTheCanonicalFormat) {
    const std::wstring path = TempPath(L"somike-test-44k-mono.wav");
    WriteWav(path, 44100, 1, 0.5);
    DecodedAudio decoded;
    ASSERT_EQ(DecodeAudioFile(path, 60.0, decoded), S_OK);
    ::DeleteFileW(path.c_str());

    ASSERT_TRUE(decoded.clip);
    EXPECT_NEAR(static_cast<double>(decoded.clip->frames()), 24000.0, 4.0);
    EXPECT_NEAR(decoded.source_seconds, 0.5, 0.01);
    EXPECT_FALSE(decoded.truncated);
    float peak = 0.0f;
    const auto& s = decoded.clip->samples;
    for (std::size_t f = 2400; f < 21600; ++f) {
        EXPECT_FLOAT_EQ(s[2 * f], s[2 * f + 1]);  // mono -> identical sides
        peak = std::max(peak, std::abs(s[2 * f]));
    }
    EXPECT_NEAR(peak, 0.5f, 0.02f);
}

TEST_F(AudioFileTest, DecodesOnlyUpToTheLimit) {
    const std::wstring path = TempPath(L"somike-test-48k-stereo.wav");
    WriteWav(path, 48000, 2, 2.0);
    DecodedAudio decoded;
    ASSERT_EQ(DecodeAudioFile(path, 0.5, decoded), S_OK);
    ::DeleteFileW(path.c_str());
    EXPECT_TRUE(decoded.truncated);
    EXPECT_EQ(decoded.clip->frames(), 24000u);
    EXPECT_NEAR(decoded.source_seconds, 2.0, 0.01);
}

TEST_F(AudioFileTest, RejectsMissingAndNonAudioFiles) {
    DecodedAudio decoded;
    EXPECT_TRUE(FAILED(DecodeAudioFile(TempPath(L"somike-test-does-not-exist.mp3"), 10.0, decoded)));

    const std::wstring text = TempPath(L"somike-test-not-audio.mp3");
    std::ofstream(text, std::ios::binary) << "this is not an mp3 file, just text pretending to be one";
    EXPECT_TRUE(FAILED(DecodeAudioFile(text, 10.0, decoded)));
    ::DeleteFileW(text.c_str());
    EXPECT_FALSE(decoded.clip);
}

}  // namespace
}  // namespace somike::core
