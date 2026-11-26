// UI-independent application logic behind the main window. The view only
// renders state from here and forwards user actions; nothing in this class
// touches HWNDs, so it can be driven by a tray icon, hotkeys or tests alike.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "somike/core/audio_devices.h"
#include "somike/core/injector_status.h"
#include "somike/core/router.h"
#include "somike/core/settings.h"

namespace somike::app {

// One entry of the "audio source" list.
struct SourceItem {
    std::wstring label;
    std::wstring executable;  // empty for the system-wide entry
    DWORD process_id = 0;     // 0: system-wide, or the selected app is not running
    bool system_wide = false;
};

class AppController {
public:
    explicit AppController(std::wstring settings_path);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // --- what to route -----------------------------------------------------------
    void RefreshSources();
    [[nodiscard]] const std::vector<SourceItem>& sources() const noexcept { return sources_; }
    [[nodiscard]] int selected_source() const noexcept { return selected_source_; }
    void SelectSource(int index);

    // --- where to inject -----------------------------------------------------------
    void RefreshMicrophones();
    [[nodiscard]] const std::vector<core::AudioEndpoint>& microphones() const noexcept {
        return microphones_;
    }
    [[nodiscard]] int selected_microphone() const noexcept { return selected_microphone_; }
    void SelectMicrophone(int index);
    [[nodiscard]] std::wstring selected_microphone_id() const;
    [[nodiscard]] std::wstring selected_microphone_name() const;

    // --- mixing ------------------------------------------------------------------------
    [[nodiscard]] float inject_gain() const noexcept { return settings_.inject_gain; }
    [[nodiscard]] bool mute_microphone() const noexcept { return settings_.mute_microphone; }
    void SetInjectGain(float gain);
    void SetMuteMicrophone(bool mute);

    // --- routing -----------------------------------------------------------------------
    [[nodiscard]] HRESULT StartRouting();
    void StopRouting();
    [[nodiscard]] bool routing() const noexcept { return router_.routing(); }
    [[nodiscard]] core::RouterStatus router_status() const noexcept { return router_.status(); }
    [[nodiscard]] std::wstring routing_label() const;

    // --- injector (APO) ------------------------------------------------------------------
    [[nodiscard]] core::InjectorStatus QueryInjectorStatus();

    void SaveSettings();

private:
    std::wstring settings_path_;
    core::Settings settings_;
    core::Router router_;

    std::vector<SourceItem> sources_;
    int selected_source_ = 0;
    std::vector<core::AudioEndpoint> microphones_;
    int selected_microphone_ = -1;
};

}  // namespace somike::app
