// The contract between the native app and the HTML UI (src/app/ui/app.js).
//
// Native -> UI (WebView2 PostWebMessageAsJson):
//   {"type":"state","state":{...UiState...}}
//   {"type":"icons","icons":{"spotify.exe":"data:image/png;base64,..."}}
//   {"type":"toast","text":"...","tone":"info"|"err"}
// UI -> native (chrome.webview.postMessage):
//   {"cmd":"ready"|"toggleLive"|"setup"|"repair"|"uninstall"|"refresh"|"openLogs"}
//   {"cmd":"selectSource","id":"spotify.exe"}   {"cmd":"selectMic","id":"{0.0.1...}"}
//   {"cmd":"setGain","value":1.25}              {"cmd":"setMuteMic","value":true}
//
// Pure (no Win32), so it is unit-tested. All strings are UTF-8.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace somike::app {

inline constexpr char kSystemSourceId[] = "system";

struct UiSource {
    std::string id;        // lower-case executable name, or kSystemSourceId
    std::string name;      // "Spotify"
    std::string icon_key;  // key into the icons map (empty: letter avatar)
    bool running = false;  // a process of this app exists
    bool active = false;   // currently producing sound
    bool system = false;   // the "All system audio" entry
};

struct UiMic {
    std::string id;
    std::string name;
    bool is_default = false;
};

enum class LivePhase { Off, Live, Waiting, Error };

struct UiState {
    bool setup_view = false;  // injector not installed on the selected mic
    bool setup_busy = false;
    std::string setup_message;
    bool setup_error = false;

    std::vector<UiMic> mics;
    std::string mic_id;
    bool mic_muted_in_windows = false;

    bool installed = false;
    bool enhancements_off = false;
    bool needs_update = false;  // 0.1.0 install that raw-mode apps (Discord) bypass
    bool mic_in_use = false;    // an app has the microphone open (APO alive)

    std::vector<UiSource> sources;
    std::string source_id;

    bool live_on = false;
    LivePhase live_phase = LivePhase::Off;
    std::string live_message;

    float level = 0.0f;  // 0..1 peak of the routed audio
    float gain = 1.0f;
    bool mute_mic = false;
};

[[nodiscard]] std::string SerializeStateMessage(const UiState& state);
[[nodiscard]] std::string SerializeIconsMessage(const std::map<std::string, std::string>& icons);
[[nodiscard]] std::string SerializeToastMessage(std::string_view text, bool error);

struct UiCommand {
    std::string cmd;
    std::string id;
    std::optional<double> number;
    std::optional<bool> flag;
};

// Returns nullopt for malformed input (not JSON, no "cmd" string).
[[nodiscard]] std::optional<UiCommand> ParseCommand(std::string_view json);

}  // namespace somike::app
