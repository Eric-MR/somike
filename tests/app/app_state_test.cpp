// The JSON contract between the native app and src/app/ui/app.js.
#include "app_state.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace somike::app {
namespace {

using nlohmann::json;

UiState SampleState() {
    UiState s;
    s.installed = true;
    s.mics = {{"{0.0.1.00000000}.{abc}", "Mikrofon (Corsair HS80)", true}};
    s.mic_id = "{0.0.1.00000000}.{abc}";
    s.sources = {{"spotify.exe", "Spotify", "spotify.exe", true, true, false},
                 {kSystemSourceId, "All system audio", "", true, true, true}};
    s.source_id = "spotify.exe";
    s.live_on = true;
    s.live_phase = LivePhase::Waiting;
    s.level = 0.123456f;
    s.gain = 1.25f;
    return s;
}

TEST(AppState, StateMessageMatchesTheUiContract) {
    const json message = json::parse(SerializeStateMessage(SampleState()));
    ASSERT_EQ(message["type"], "state");
    const json& s = message["state"];
    EXPECT_EQ(s["view"], "main");
    EXPECT_EQ(s["micId"], "{0.0.1.00000000}.{abc}");
    EXPECT_EQ(s["mics"][0]["isDefault"], true);
    EXPECT_EQ(s["injector"]["installed"], true);
    EXPECT_EQ(s["injector"]["needsUpdate"], false);
    EXPECT_EQ(s["sources"][0]["id"], "spotify.exe");
    EXPECT_EQ(s["sources"][0]["iconKey"], "spotify.exe");
    EXPECT_EQ(s["sources"][1]["system"], true);
    EXPECT_EQ(s["sourceId"], "spotify.exe");
    EXPECT_EQ(s["live"]["on"], true);
    EXPECT_EQ(s["live"]["phase"], "waiting");
    EXPECT_DOUBLE_EQ(s["level"].get<double>(), 0.12);
    EXPECT_DOUBLE_EQ(s["gain"].get<double>(), 1.25);
    for (const char* key : {"setup", "micMutedInWindows", "muteMic"}) {
        EXPECT_TRUE(s.contains(key)) << key;
    }
}

TEST(AppState, SetupViewIsReported) {
    UiState s = SampleState();
    s.setup_view = true;
    s.setup_busy = true;
    s.setup_message = "Allow the Windows prompt";
    const json state = json::parse(SerializeStateMessage(s))["state"];
    EXPECT_EQ(state["view"], "setup");
    EXPECT_EQ(state["setup"]["busy"], true);
    EXPECT_EQ(state["setup"]["message"], "Allow the Windows prompt");
}

TEST(AppState, IdenticalStatesSerializeIdentically) {
    UiState a = SampleState();
    UiState b = SampleState();
    b.level = 0.1234f;  // differs only below the published precision
    EXPECT_EQ(SerializeStateMessage(a), SerializeStateMessage(b));
}

TEST(AppState, InvalidUtf8DoesNotThrow) {
    UiState s = SampleState();
    s.sources[0].name = "Bad \xC3";
    EXPECT_NO_THROW(static_cast<void>(SerializeStateMessage(s)));
}

TEST(AppState, IconsAndToastMessages) {
    const json icons = json::parse(SerializeIconsMessage({{"spotify.exe", "data:image/png;base64,AA=="}}));
    EXPECT_EQ(icons["type"], "icons");
    EXPECT_EQ(icons["icons"]["spotify.exe"], "data:image/png;base64,AA==");

    const json toast = json::parse(SerializeToastMessage("Connected", true));
    EXPECT_EQ(toast["type"], "toast");
    EXPECT_EQ(toast["text"], "Connected");
    EXPECT_EQ(toast["tone"], "err");
}

TEST(AppState, ParsesCommands) {
    const auto select = ParseCommand(R"({"cmd":"selectSource","id":"spotify.exe"})");
    ASSERT_TRUE(select);
    EXPECT_EQ(select->cmd, "selectSource");
    EXPECT_EQ(select->id, "spotify.exe");

    const auto gain = ParseCommand(R"({"cmd":"setGain","value":1.5})");
    ASSERT_TRUE(gain && gain->number);
    EXPECT_DOUBLE_EQ(*gain->number, 1.5);

    const auto mute = ParseCommand(R"({"cmd":"setMuteMic","value":true})");
    ASSERT_TRUE(mute && mute->flag);
    EXPECT_TRUE(*mute->flag);

    const auto bare = ParseCommand(R"({"cmd":"toggleLive"})");
    ASSERT_TRUE(bare);
    EXPECT_FALSE(bare->number);
    EXPECT_FALSE(bare->flag);
}

TEST(AppState, RejectsMalformedCommands) {
    EXPECT_FALSE(ParseCommand("not json"));
    EXPECT_FALSE(ParseCommand("[1,2]"));
    EXPECT_FALSE(ParseCommand(R"({"id":"x"})"));
    EXPECT_FALSE(ParseCommand(R"({"cmd":5})"));
    const auto wrong_type = ParseCommand(R"({"cmd":"setGain","value":"loud"})");
    ASSERT_TRUE(wrong_type);
    EXPECT_FALSE(wrong_type->number);
}

}  // namespace
}  // namespace somike::app
