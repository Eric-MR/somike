#include "soundboard_library.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

namespace somike::app {

using nlohmann::json;

namespace {

constexpr int kVersion = 1;
constexpr std::size_t kMaxFileNameBytes = 200;
constexpr std::size_t kMaxEmojiBytes = 32;
constexpr std::size_t kMaxLabelChars = 40;
constexpr std::size_t kMaxCreditChars = 200;
constexpr std::size_t kMaxUrlBytes = 2048;

constexpr std::array<const char*, 10> kPalette = {
    "#8b5cf6", "#ec4899", "#f97316", "#eab308", "#22c55e",
    "#14b8a6", "#06b6d4", "#3b82f6", "#a855f7", "#ef4444",
};

float CleanVolume(const json& value, float fallback) {
    if (!value.is_number()) {
        return fallback;
    }
    const double v = value.get<double>();
    if (!std::isfinite(v)) {
        return fallback;
    }
    return static_cast<float>(std::clamp(v, 0.0, 2.0));
}

std::string GetString(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::uint32_t GetUint(const json& object, const char* key, std::uint32_t max) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_unsigned()) {
        return 0;
    }
    const auto value = it->get<std::uint64_t>();
    return value > max ? 0 : static_cast<std::uint32_t>(value);
}

json HotkeyJson(const Hotkey& hotkey) {
    return {{"vk", hotkey.vk}, {"mods", hotkey.mods}, {"label", hotkey.label}};
}

Hotkey ParseHotkey(const json& value) {
    Hotkey hotkey;
    if (!value.is_object()) {
        return hotkey;
    }
    const std::uint32_t vk = GetUint(value, "vk", 0xFE);
    const std::uint32_t mods = GetUint(value, "mods", 0xF);
    if (vk == 0 || !IsAllowedHotkey(vk, mods)) {
        return hotkey;
    }
    hotkey.vk = vk;
    hotkey.mods = mods;
    hotkey.label = CleanText(GetString(value, "label"), kMaxLabelChars);
    return hotkey;
}

bool IsHttpsUrl(std::string_view url) {
    return url.starts_with("https://") && url.size() <= kMaxUrlBytes &&
           std::none_of(url.begin(), url.end(), [](char c) {
               return static_cast<unsigned char>(c) <= 0x20 || c == '"' || c == '<' || c == '>' || c == '\\';
           });
}

// Number of bytes of the UTF-8 sequence starting with `lead`, 0 if invalid.
std::size_t SequenceLength(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return lead >= 0xC2 ? 2 : 0;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return lead <= 0xF4 ? 4 : 0;
    return 0;
}

}  // namespace

bool IsAllowedHotkey(std::uint32_t vk, std::uint32_t mods) noexcept {
    if (vk == 0 || vk > 0xFE || (mods & ~0xFu) != 0) {
        return false;
    }
    // Modifier keys on their own (Shift, Ctrl, Alt, Win, Apps, left/right variants).
    if ((vk >= 0x10 && vk <= 0x12) || vk == 0x5B || vk == 0x5C || vk == 0x5D || (vk >= 0xA0 && vk <= 0xA5)) {
        return false;
    }
    // Mouse buttons.
    if (vk <= 0x06) {
        return false;
    }
    if (vk == 'M' && mods == (kModControl | kModAlt)) {
        return false;  // Somike's own "go live" shortcut
    }
    if (mods != 0 && mods != kModShift) {
        return true;
    }
    // Plain (or Shift+) keys must not get in the way of typing.
    const bool function_key = vk >= 0x70 && vk <= 0x87;            // F1..F24
    const bool numpad = vk >= 0x60 && vk <= 0x6F;                   // Num 0..9, * + , - . /
    const bool media = vk >= 0xAD && vk <= 0xB7;                    // volume, media, app keys
    const bool special = vk == 0x13 || vk == 0x91;                  // Pause, Scroll Lock
    return function_key || numpad || media || special;
}

bool IsSafeFileName(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxFileNameBytes || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false;
        }
    }
    if (name.front() == ' ' || name.back() == ' ' || name.back() == '.') {
        return false;
    }
    // Reserved device names (CON, NUL, COM1, ...), with or without extension.
    std::string stem(name.substr(0, name.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    });
    static const std::set<std::string> reserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return !reserved.contains(stem);
}

std::string CleanText(std::string_view text, std::size_t max_chars) {
    std::string out;
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < text.size() && chars < max_chars) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = SequenceLength(lead);
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            valid = (static_cast<unsigned char>(text[i + k]) & 0xC0) == 0x80;
        }
        if (!valid) {
            out += "\xEF\xBF\xBD";  // U+FFFD
            ++i;
            ++chars;
            continue;
        }
        if (length == 1 && (lead < 0x20 || lead == 0x7F)) {
            if (lead == '\t' || lead == '\n' || lead == '\r') {
                out += ' ';
                ++chars;
            }
            ++i;
            continue;
        }
        out.append(text.substr(i, length));
        i += length;
        ++chars;
    }
    // Trim and collapse runs of spaces.
    std::string collapsed;
    for (const char c : out) {
        if (c == ' ' && (collapsed.empty() || collapsed.back() == ' ')) {
            continue;
        }
        collapsed += c;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

std::string NameFromTitle(std::string_view title) {
    std::string name(title);
    static constexpr std::array<std::string_view, 9> kExtensions = {".wav", ".mp3", ".aif", ".aiff", ".flac",
                                                                    ".ogg", ".m4a", ".wma", ".opus"};
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    });
    for (const std::string_view ext : kExtensions) {
        if (lower.ends_with(ext)) {
            name.resize(name.size() - ext.size());
            break;
        }
    }
    std::replace(name.begin(), name.end(), '_', ' ');
    std::string clean = CleanText(name, kMaxSoundNameChars);
    return clean.empty() ? std::string("Sound") : clean;
}

bool IsValidSoundId(std::string_view id) noexcept {
    return id.size() == 12 &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool IsValidColor(std::string_view color) noexcept {
    return color.size() == 7 && color[0] == '#' &&
           std::all_of(color.begin() + 1, color.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

std::string DefaultColorFor(std::string_view id) {
    std::uint32_t hash = 2166136261u;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    }
    return kPalette[hash % kPalette.size()];
}

std::string SerializeLibrary(const SoundLibrary& library) {
    json sounds = json::array();
    for (const SoundEntry& s : library.sounds) {
        json entry = {{"id", s.id},
                      {"name", s.name},
                      {"file", s.file},
                      {"emoji", s.emoji},
                      {"color", s.color},
                      {"volume", std::round(s.volume * 100.0f) / 100.0f},
                      {"hotkey", HotkeyJson(s.hotkey)},
                      {"durationMs", s.duration_ms}};
        if (s.credit) {
            entry["credit"] = {{"title", s.credit->title},
                               {"creator", s.credit->creator},
                               {"license", s.credit->license},
                               {"url", s.credit->url}};
        }
        sounds.push_back(std::move(entry));
    }
    const SoundboardSettings& st = library.settings;
    const json root = {
        {"version", kVersion},
        {"settings",
         {{"micVolume", std::round(st.mic_volume * 100.0f) / 100.0f},
          {"monitor", st.monitor},
          {"monitorVolume", std::round(st.monitor_volume * 100.0f) / 100.0f},
          {"stopHotkey", HotkeyJson(st.stop_hotkey)}}},
        {"sounds", std::move(sounds)},
    };
    return root.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
}

SoundLibrary ParseLibrary(std::string_view text) {
    SoundLibrary library;
    const json root = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (!root.is_object()) {
        return library;
    }

    if (const auto it = root.find("settings"); it != root.end() && it->is_object()) {
        const json& st = *it;
        SoundboardSettings& out = library.settings;
        out.mic_volume = CleanVolume(st.value("micVolume", json()), out.mic_volume);
        out.monitor_volume = CleanVolume(st.value("monitorVolume", json()), out.monitor_volume);
        if (const auto m = st.find("monitor"); m != st.end() && m->is_boolean()) {
            out.monitor = m->get<bool>();
        }
        out.stop_hotkey = ParseHotkey(st.value("stopHotkey", json()));
    }

    const auto sounds = root.find("sounds");
    if (sounds == root.end() || !sounds->is_array()) {
        return library;
    }
    std::set<std::string> ids;
    std::vector<Hotkey> used_keys;
    if (!library.settings.stop_hotkey.empty()) {
        used_keys.push_back(library.settings.stop_hotkey);
    }
    for (const json& item : *sounds) {
        if (!item.is_object() || library.sounds.size() >= kMaxSounds) {
            continue;
        }
        SoundEntry s;
        s.id = GetString(item, "id");
        s.file = GetString(item, "file");
        if (!IsValidSoundId(s.id) || !IsSafeFileName(s.file) || !ids.insert(s.id).second) {
            continue;
        }
        s.name = CleanText(GetString(item, "name"), kMaxSoundNameChars);
        if (s.name.empty()) {
            s.name = "Sound";
        }
        const std::string emoji = GetString(item, "emoji");
        s.emoji = emoji.size() <= kMaxEmojiBytes ? CleanText(emoji, 8) : std::string{};
        s.color = GetString(item, "color");
        if (!IsValidColor(s.color)) {
            s.color = DefaultColorFor(s.id);
        }
        s.volume = CleanVolume(item.value("volume", json()), 1.0f);
        s.duration_ms = GetUint(item, "durationMs", 24u * 3600u * 1000u);
        s.hotkey = ParseHotkey(item.value("hotkey", json()));
        // A key combination can only trigger one thing.
        if (std::any_of(used_keys.begin(), used_keys.end(), [&](const Hotkey& k) { return k.SameKeys(s.hotkey); })) {
            s.hotkey = {};
        } else if (!s.hotkey.empty()) {
            used_keys.push_back(s.hotkey);
        }
        if (const auto c = item.find("credit"); c != item.end() && c->is_object()) {
            SoundCredit credit;
            credit.title = CleanText(GetString(*c, "title"), kMaxCreditChars);
            credit.creator = CleanText(GetString(*c, "creator"), kMaxCreditChars);
            credit.license = CleanText(GetString(*c, "license"), 32);
            credit.url = GetString(*c, "url");
            if (!IsHttpsUrl(credit.url)) {
                credit.url.clear();
            }
            s.credit = std::move(credit);
        }
        library.sounds.push_back(std::move(s));
    }
    return library;
}

}  // namespace somike::app
