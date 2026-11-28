// The soundboard's persistent model and its JSON file
// (%LOCALAPPDATA%\Somike\Soundboard\library.json):
//
//   {"version":1,
//    "settings":{"micVolume":0.8,"monitor":true,"monitorVolume":0.5,
//                "stopHotkey":{"vk":0,"mods":0,"label":""}},
//    "sounds":[{"id":"3f9a0c1d2e4b","name":"Air horn","file":"air-horn-3f9a0c1d2e4b.mp3",
//               "emoji":"📯","color":"#ec4899","volume":1.0,
//               "hotkey":{"vk":97,"mods":0,"label":"Num 1"},"durationMs":2571,
//               "credit":{"title":"...","creator":"...","license":"cc0","url":"https://..."}}]}
//
// Pure (no Win32), so it is unit-tested. The file is user-editable: anything
// invalid is dropped or clamped on load, and file names can never point
// outside the sounds folder.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace somike::app {

// A global hotkey in RegisterHotKey terms.
struct Hotkey {
    std::uint32_t vk = 0;    // virtual-key code; 0 = none
    std::uint32_t mods = 0;  // MOD_ALT 1 | MOD_CONTROL 2 | MOD_SHIFT 4 | MOD_WIN 8
    std::string label;       // as shown to the user, e.g. "Ctrl+Alt+1"

    [[nodiscard]] bool empty() const noexcept { return vk == 0; }
    [[nodiscard]] bool SameKeys(const Hotkey& other) const noexcept {
        return !empty() && vk == other.vk && mods == other.mods;
    }
    bool operator==(const Hotkey&) const = default;
};

inline constexpr std::uint32_t kModAlt = 0x1;
inline constexpr std::uint32_t kModControl = 0x2;
inline constexpr std::uint32_t kModShift = 0x4;
inline constexpr std::uint32_t kModWin = 0x8;

// Keys a soundboard hotkey may use. Without a modifier only keys that do not
// break typing: F1-F24, the numeric keypad, Pause, Scroll Lock and the media
// keys. Modifier keys alone are not hotkeys, and Ctrl+Alt+M (go live) is taken.
[[nodiscard]] bool IsAllowedHotkey(std::uint32_t vk, std::uint32_t mods) noexcept;

// Where a downloaded sound comes from (shown in the app, kept for credit).
struct SoundCredit {
    std::string title;
    std::string creator;
    std::string license;  // "cc0", "pdm", ...
    std::string url;      // page of the sound on its website
    bool operator==(const SoundCredit&) const = default;
};

struct SoundEntry {
    std::string id;     // 12 lower-case hex digits
    std::string name;
    std::string file;   // file name inside the sounds folder
    std::string emoji;  // optional
    std::string color;  // "#rrggbb"
    float volume = 1.0f;  // 0..2, on top of the board volumes
    Hotkey hotkey;
    std::uint32_t duration_ms = 0;
    std::optional<SoundCredit> credit;
    bool operator==(const SoundEntry&) const = default;
};

struct SoundboardSettings {
    float mic_volume = 0.8f;      // 0..2: how loud the sounds are in the microphone
    bool monitor = true;          // also play them on my own speakers/headphones
    float monitor_volume = 0.5f;  // 0..2
    Hotkey stop_hotkey;           // stops every sound
    bool operator==(const SoundboardSettings&) const = default;
};

struct SoundLibrary {
    SoundboardSettings settings;
    std::vector<SoundEntry> sounds;
    bool operator==(const SoundLibrary&) const = default;
};

inline constexpr std::size_t kMaxSoundNameChars = 60;
inline constexpr std::size_t kMaxSounds = 500;

[[nodiscard]] std::string SerializeLibrary(const SoundLibrary& library);
// Never throws; malformed input yields an empty library with default settings.
[[nodiscard]] SoundLibrary ParseLibrary(std::string_view json);

// A plain file name: no path separators, no "..", no drive, no control
// characters, no reserved device names, at most 200 bytes.
[[nodiscard]] bool IsSafeFileName(std::string_view name) noexcept;
// Trimmed, control characters removed, at most `max_chars` code points
// (UTF-8 aware; invalid UTF-8 is replaced).
[[nodiscard]] std::string CleanText(std::string_view text, std::size_t max_chars);
// "Industrial_Air-Horn 01.wav" -> "Industrial Air-Horn 01"
[[nodiscard]] std::string NameFromTitle(std::string_view title);
[[nodiscard]] bool IsValidSoundId(std::string_view id) noexcept;
[[nodiscard]] bool IsValidColor(std::string_view color) noexcept;
// A pleasant colour for a new pad, picked deterministically from the id.
[[nodiscard]] std::string DefaultColorFor(std::string_view id);

}  // namespace somike::app
