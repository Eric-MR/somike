#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace somike::core {

// User preferences, persisted as UTF-8 "key=value" lines:
//
//   # Somike settings
//   source=Spotify.exe
//   microphone={0.0.1.00000000}.{...}
//   inject_gain=1.00
//   mute_microphone=false
struct Settings {
    std::wstring source_executable;  // empty: all system audio
    std::wstring microphone_id;      // empty: default capture device
    float inject_gain = 1.0f;        // 0.0 .. 2.0
    bool mute_microphone = false;

    bool operator==(const Settings&) const = default;
};

// Pure codec (unit-tested). Unknown keys are ignored, invalid values keep
// their defaults.
[[nodiscard]] Settings ParseSettings(std::string_view text);
[[nodiscard]] std::string SerializeSettings(const Settings& settings);

// %APPDATA%\Somike\settings.ini
[[nodiscard]] std::wstring DefaultSettingsPath();

[[nodiscard]] Settings LoadSettings(const std::wstring& path);
// Atomic replace (write temp file, then rename).
[[nodiscard]] HRESULT SaveSettings(const std::wstring& path, const Settings& settings);

}  // namespace somike::core
