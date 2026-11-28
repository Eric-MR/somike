// Connects the soundboard to the main window: "sb.*" UI commands, global
// hotkeys, the file picker, dropped files and state pushes to the HTML UI
// (src/app/ui/soundboard.js).
//
// Native -> UI (WebView2 PostWebMessageAsJson):
//   {"type":"sb.library","library":{"ready":true,"settings":{...},"sounds":[...]}}   when it changes
//   {"type":"sb.playing","playing":[{"id":"...","pos":1200,"len":2571}],"loading":["..."]}
//   {"type":"sb.results","query":"air horn","page":1,"pageCount":10,"total":188,"error":"","items":[...]}
//   {"type":"sb.online","id":"<catalog id>","state":"idle|loading|previewing|adding|added|failed"}
// UI -> native (chrome.webview.postMessage):
//   {"cmd":"sb.play","id"}  {"cmd":"sb.stop","id"}  {"cmd":"sb.stopAll"}  {"cmd":"sb.test","id"}
//   {"cmd":"sb.import"}  {"cmd":"sb.drop"} (+ dropped files)  {"cmd":"sb.openFolder"}
//   {"cmd":"sb.rename","id","value":"name"}  {"cmd":"sb.look","id","emoji","color"}
//   {"cmd":"sb.volume","id","value":1.0}  {"cmd":"sb.remove","id"}  {"cmd":"sb.move","id","index":0}
//   {"cmd":"sb.hotkey","id","vk","mods","label"} (id "" = the stop-all key; vk 0 clears)
//   {"cmd":"sb.capture","value":true}   while the UI records a hotkey
//   {"cmd":"sb.settings","micVolume"?,"monitor"?,"monitorVolume"?}
//   {"cmd":"sb.search","query","page"}  {"cmd":"sb.preview","id"}  {"cmd":"sb.add","id"}
//   {"cmd":"sb.openLink","url"}
#pragma once

#include <windows.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "soundboard.h"

namespace somike::app {

class AppController;

class SoundboardBridge final : public SoundboardListener {
public:
    static constexpr UINT kWakeMessage = WM_APP + 7;

    SoundboardBridge(AppController& controller, std::function<void(const std::string&)> post_json);
    ~SoundboardBridge();
    SoundboardBridge(const SoundboardBridge&) = delete;
    SoundboardBridge& operator=(const SoundboardBridge&) = delete;

    void Attach(HWND hwnd);
    void Detach();
    void OnUiReady();
    void HandleCommand(const std::string& cmd, const std::string& json, const std::vector<std::wstring>& files);
    // WM_HOTKEY; returns false for ids that are not the soundboard's.
    bool HandleHotkey(WPARAM id);
    void OnWake();
    void Tick();

private:
    void OnSoundboardToast(const std::string& text, bool error) override;
    void OnSearchResults(const SearchResults& results) override;
    void OnOnlineState(const std::string& item_id, OnlineState state) override;

    void Refresh();
    void ApplyHotkeys();
    void UnregisterHotkeys();
    void PushLibrary(bool force);
    void PushPlaying(bool force);
    void ShowImportDialog();
    void Post(const std::string& json);

    AppController& controller_;
    Soundboard& soundboard_;
    std::function<void(const std::string&)> post_json_;
    HWND hwnd_ = nullptr;
    bool ui_ready_ = false;
    bool capturing_ = false;
    std::vector<std::pair<int, std::string>> registered_;  // hotkey id -> sound id ("" = stop all)
    std::set<std::string> hotkey_failed_;                  // sound ids, "" = the stop-all key
    std::string applied_hotkeys_;
    std::string last_library_;
    std::string last_playing_;
};

}  // namespace somike::app
