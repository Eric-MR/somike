// UI-independent application logic. The window only renders BuildUiState()
// and forwards user actions; nothing in this class touches HWNDs, so it can be
// driven by the WebView UI, the global hotkey, command-line flags or tests.
//
// Plug and play behaviour lives here:
//   * sources are grouped per app and captured from the root of the app's
//     process tree (multi-process apps such as Spotify, Chrome, Discord);
//   * "live" is a wish, not a one-shot action: while it is on, the route is
//     (re)attached automatically when the app starts, restarts, or the audio
//     service restarts, and shows "waiting" while the app is not running.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app_state.h"
#include "soundboard.h"
#include "somike/core/audio_devices.h"
#include "somike/core/injector_status.h"
#include "somike/core/router.h"
#include "somike/core/settings.h"

namespace somike::app {

inline constexpr wchar_t kSystemSource[] = L"system";

struct SourceEntry {
    std::wstring id;          // lower-case executable name, or kSystemSource
    std::wstring executable;  // "Spotify.exe"
    std::wstring name;        // "Spotify"
    std::wstring image_path;  // for the icon; may be empty
    DWORD root_pid = 0;       // capture target; 0 when not running
    bool running = false;
    bool active = false;      // producing sound right now
    bool system = false;
};

class AppController {
public:
    explicit AppController(std::wstring settings_path);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    // Periodic work, called ~4x per second on the UI thread.
    void Tick(std::uint64_t now_ms);
    // Re-enumerate apps, microphones and the injector state right away.
    void RefreshNow();

    // --- user actions ------------------------------------------------------------
    void SelectSource(const std::wstring& id);  // lower-case exe name or kSystemSource
    void SetLive(bool on);
    void ToggleLive() { SetLive(!live_requested_); }
    void SetInjectGain(float gain);
    void SetMuteMicrophone(bool mute);
    void SelectMicrophone(const std::wstring& id);

    // One-time setup progress; the window runs the elevated installer.
    void SetSetupProgress(bool busy, std::wstring message, bool error);
    [[nodiscard]] bool setup_busy() const noexcept { return setup_busy_; }

    // --- queries --------------------------------------------------------------------
    [[nodiscard]] const std::wstring& selected_microphone_id() const noexcept { return mic_id_; }
    [[nodiscard]] std::wstring selected_microphone_name() const;
    [[nodiscard]] const std::vector<SourceEntry>& sources() const noexcept { return sources_; }
    [[nodiscard]] bool live_requested() const noexcept { return live_requested_; }
    [[nodiscard]] LivePhase live_phase() const noexcept { return phase_; }
    [[nodiscard]] std::wstring selected_source_name() const;
    [[nodiscard]] const core::InjectorStatus& injector() const noexcept { return injector_; }
    // Somike is set up on the selected microphone: routed audio and sounds reach it.
    [[nodiscard]] bool mic_ready() const noexcept;

    [[nodiscard]] Soundboard& soundboard() noexcept { return soundboard_; }

    [[nodiscard]] UiState BuildUiState() const;
    void SaveSettings();

private:
    struct Target {
        DWORD pid = 0;
        bool system = false;
        bool operator==(const Target&) const = default;
    };

    void RefreshSources();
    void RefreshMicrophones();
    void RefreshInjector();
    void UpdateRoute(std::uint64_t now_ms);
    void StopRoute();
    [[nodiscard]] const SourceEntry* SelectedSource() const;
    [[nodiscard]] std::wstring CachedFriendlyName(const std::wstring& image_path,
                                                  const std::wstring& executable);

    std::wstring settings_path_;
    core::Settings settings_;
    bool settings_dirty_ = false;
    core::Router router_;
    Soundboard soundboard_;  // after router_: detaches from it before the router goes

    std::vector<SourceEntry> sources_;
    std::wstring source_id_;  // empty: nothing chosen yet (first run)
    struct SeenApp {
        std::wstring executable;
        std::wstring image_path;
    };
    std::map<std::wstring, std::wstring> names_;  // image path -> friendly name
    std::map<std::wstring, SeenApp> seen_;        // source id -> app seen this session

    std::vector<core::AudioEndpoint> microphones_;
    std::wstring mic_id_;
    bool mic_muted_ = false;
    core::InjectorStatus injector_{};

    bool live_requested_ = false;
    LivePhase phase_ = LivePhase::Off;
    std::wstring live_message_;
    std::optional<Target> active_target_;
    std::uint64_t next_retry_ms_ = 0;
    float level_ = 0.0f;

    bool setup_busy_ = false;
    std::wstring setup_message_;
    bool setup_error_ = false;

    std::uint64_t next_sources_ms_ = 0;
    std::uint64_t next_status_ms_ = 0;
    std::uint64_t next_mics_ms_ = 0;
    std::uint64_t next_save_ms_ = 0;
};

}  // namespace somike::app
