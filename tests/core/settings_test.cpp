#include <gtest/gtest.h>

#include <string>

#include "somike/core/settings.h"

namespace somike::core {
namespace {

TEST(Settings, RoundTripsThroughText) {
    Settings original;
    original.source_executable = L"Spotify Ü.exe";  // non-ASCII survives UTF-8
    original.microphone_id = L"{0.0.1.00000000}.{a1b2c3d4-1111-2222-3333-444455556666}";
    original.inject_gain = 1.25f;
    original.mute_microphone = true;

    EXPECT_EQ(ParseSettings(SerializeSettings(original)), original);
}

TEST(Settings, ParsesHandWrittenFiles) {
    const std::string text =
        "\xEF\xBB\xBF# comment\r\n"
        "  source = Discord.exe  \r\n"
        "; another comment\r\n"
        "inject_gain=0.5\r\n"
        "mute_microphone=yes\r\n"
        "unknown_key=whatever\r\n";
    const Settings settings = ParseSettings(text);
    EXPECT_EQ(settings.source_executable, L"Discord.exe");
    EXPECT_FLOAT_EQ(settings.inject_gain, 0.5f);
    EXPECT_TRUE(settings.mute_microphone);
    EXPECT_TRUE(settings.microphone_id.empty());
}

TEST(Settings, InvalidValuesKeepDefaults) {
    const Settings settings = ParseSettings("inject_gain=loud\nmute_microphone=maybe\n");
    EXPECT_FLOAT_EQ(settings.inject_gain, 1.0f);
    EXPECT_FALSE(settings.mute_microphone);
}

TEST(Settings, GainIsClamped) {
    EXPECT_FLOAT_EQ(ParseSettings("inject_gain=7.5").inject_gain, 2.0f);
    EXPECT_FLOAT_EQ(ParseSettings("inject_gain=-1").inject_gain, 0.0f);
}

TEST(Settings, EmptyTextYieldsDefaults) {
    EXPECT_EQ(ParseSettings(""), Settings{});
}

}  // namespace
}  // namespace somike::core
