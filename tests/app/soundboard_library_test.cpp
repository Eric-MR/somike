// The soundboard's library file (library.json) and its validation.
#include "soundboard_library.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace somike::app {
namespace {

using nlohmann::json;

SoundLibrary Sample() {
    SoundLibrary library;
    library.settings.mic_volume = 1.25f;
    library.settings.monitor = false;
    library.settings.monitor_volume = 0.4f;
    library.settings.stop_hotkey = {0x7B, 0, "F12"};
    SoundEntry horn;
    horn.id = "0123456789ab";
    horn.name = "Air horn";
    horn.file = "air-horn-0123456789ab.mp3";
    horn.emoji = "\xF0\x9F\x93\xAF";  // 📯
    horn.color = "#ec4899";
    horn.volume = 0.75f;
    horn.hotkey = {0x61, 0, "Num 1"};
    horn.duration_ms = 2571;
    horn.credit = SoundCredit{"Industrial Air Horn", "someone", "cc0", "https://freesound.org/s/1/"};
    library.sounds.push_back(horn);
    SoundEntry clap;
    clap.id = "ba9876543210";
    clap.name = "Applause";
    clap.file = "applause-ba9876543210.wav";
    clap.color = "#22c55e";
    clap.hotkey = {'A', kModControl | kModAlt, "Ctrl+Alt+A"};
    library.sounds.push_back(clap);
    return library;
}

TEST(SoundboardLibrary, RoundTrips) {
    const SoundLibrary library = Sample();
    EXPECT_EQ(ParseLibrary(SerializeLibrary(library)), library);
}

TEST(SoundboardLibrary, MalformedFilesGiveAnEmptyLibrary) {
    EXPECT_EQ(ParseLibrary(""), SoundLibrary{});
    EXPECT_EQ(ParseLibrary("not json"), SoundLibrary{});
    EXPECT_EQ(ParseLibrary("[1,2,3]"), SoundLibrary{});
    EXPECT_TRUE(ParseLibrary(R"({"sounds":"nope"})").sounds.empty());
}

TEST(SoundboardLibrary, FileNamesCannotLeaveTheSoundsFolder) {
    for (const char* bad : {"..\\secret.mp3", "../x.mp3", "C:\\x.mp3", "sub/x.mp3", "x:y.mp3", "CON.mp3", "nul",
                            "", ".", "..", "trailing.", " lead.mp3"}) {
        json root = {{"sounds", {{{"id", "0123456789ab"}, {"name", "x"}, {"file", bad}}}}};
        EXPECT_TRUE(ParseLibrary(root.dump()).sounds.empty()) << bad;
        EXPECT_FALSE(IsSafeFileName(bad)) << bad;
    }
    EXPECT_TRUE(IsSafeFileName("air horn (1)-0123456789ab.mp3"));
}

TEST(SoundboardLibrary, InvalidValuesAreDroppedOrClamped) {
    const json root = {
        {"settings", {{"micVolume", 9.0}, {"monitorVolume", -1.0}, {"monitor", "yes"}}},
        {"sounds",
         {{{"id", "0123456789ab"},
           {"name", "  Boom\x01\tbox  "},
           {"file", "boom.mp3"},
           {"color", "red"},
           {"volume", 5.0},
           {"hotkey", {{"vk", 'A'}, {"mods", 0}, {"label", "A"}}}},  // plain letter: not allowed
          {{"id", "0123456789ab"}, {"name", "duplicate id"}, {"file", "dup.mp3"}},
          {{"id", "NOT-AN-ID"}, {"name", "bad id"}, {"file", "bad.mp3"}},
          {{"id", "aaaaaaaaaaaa"}, {"name", ""}, {"file", "x.mp3"}, {"credit", {{"url", "javascript:alert(1)"}}}}}},
    };
    const SoundLibrary library = ParseLibrary(root.dump());
    EXPECT_FLOAT_EQ(library.settings.mic_volume, 2.0f);
    EXPECT_FLOAT_EQ(library.settings.monitor_volume, 0.0f);
    EXPECT_TRUE(library.settings.monitor);  // default kept
    ASSERT_EQ(library.sounds.size(), 2u);
    const SoundEntry& boom = library.sounds[0];
    EXPECT_EQ(boom.name, "Boom box");
    EXPECT_TRUE(IsValidColor(boom.color));
    EXPECT_FLOAT_EQ(boom.volume, 2.0f);
    EXPECT_TRUE(boom.hotkey.empty());
    EXPECT_EQ(library.sounds[1].name, "Sound");
    ASSERT_TRUE(library.sounds[1].credit.has_value());
    EXPECT_TRUE(library.sounds[1].credit->url.empty());
}

TEST(SoundboardLibrary, AKeyCombinationTriggersOnlyOneThing) {
    const json hotkey = {{"vk", 0x70}, {"mods", 0}, {"label", "F1"}};
    const json root = {
        {"settings", {{"stopHotkey", hotkey}}},
        {"sounds",
         {{{"id", "aaaaaaaaaaaa"}, {"name", "a"}, {"file", "a.mp3"}, {"hotkey", hotkey}},
          {{"id", "bbbbbbbbbbbb"}, {"name", "b"}, {"file", "b.mp3"}, {"hotkey", {{"vk", 0x71}, {"mods", 0}}}},
          {{"id", "cccccccccccc"}, {"name", "c"}, {"file", "c.mp3"}, {"hotkey", {{"vk", 0x71}, {"mods", 0}}}}}},
    };
    const SoundLibrary library = ParseLibrary(root.dump());
    EXPECT_EQ(library.settings.stop_hotkey.vk, 0x70u);
    EXPECT_TRUE(library.sounds[0].hotkey.empty());
    EXPECT_EQ(library.sounds[1].hotkey.vk, 0x71u);
    EXPECT_TRUE(library.sounds[2].hotkey.empty());
}

TEST(SoundboardLibrary, HotkeysThatWouldBreakTypingAreRefused) {
    EXPECT_FALSE(IsAllowedHotkey('A', 0));
    EXPECT_FALSE(IsAllowedHotkey('A', kModShift));
    EXPECT_FALSE(IsAllowedHotkey(0x20, 0));                      // Space
    EXPECT_TRUE(IsAllowedHotkey('A', kModControl));
    EXPECT_TRUE(IsAllowedHotkey('1', kModAlt | kModShift));
    EXPECT_TRUE(IsAllowedHotkey(0x7C, 0));                       // F13
    EXPECT_TRUE(IsAllowedHotkey(0x61, 0));                       // Num 1
    EXPECT_TRUE(IsAllowedHotkey(0x13, 0));                       // Pause
    EXPECT_FALSE(IsAllowedHotkey('M', kModControl | kModAlt));   // Somike's go-live key
    EXPECT_FALSE(IsAllowedHotkey(0x11, kModControl));            // Ctrl alone
    EXPECT_FALSE(IsAllowedHotkey(0x01, kModControl));            // mouse button
    EXPECT_FALSE(IsAllowedHotkey('A', 0x10));                    // unknown modifier bit
}

TEST(SoundboardLibrary, CleanTextIsUtf8Aware) {
    EXPECT_EQ(CleanText("  a\x01 b\n c  ", 60), "a b c");
    // Four emoji = four characters, never cut in the middle of one.
    const std::string emoji = "\xF0\x9F\x8E\xBA";  // 🎺
    EXPECT_EQ(CleanText(emoji + emoji + emoji + emoji, 3), emoji + emoji + emoji);
    EXPECT_EQ(CleanText("ok\xFF", 10), "ok\xEF\xBF\xBD");
}

TEST(SoundboardLibrary, NamesFromTitlesLookTidy) {
    EXPECT_EQ(NameFromTitle("Air_Horn-01.WAV"), "Air Horn-01");
    EXPECT_EQ(NameFromTitle("drum roll.mp3"), "drum roll");
    EXPECT_EQ(NameFromTitle("   "), "Sound");
}

TEST(SoundboardLibrary, DefaultColoursAreStableAndValid) {
    EXPECT_EQ(DefaultColorFor("0123456789ab"), DefaultColorFor("0123456789ab"));
    EXPECT_TRUE(IsValidColor(DefaultColorFor("anything")));
}

}  // namespace
}  // namespace somike::app
