#include "somike/core/settings.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string>

#include "somike/common/hresult.h"
#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"

namespace somike::core {

namespace {

constexpr float kMinGain = 0.0f;
constexpr float kMaxGain = 2.0f;

std::string_view NextLine(std::string_view& text) {
    const auto end = text.find('\n');
    std::string_view line = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    return line;
}

bool ParseBool(std::string_view value, bool fallback) {
    if (value == "true" || value == "1" || value == "yes") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no") {
        return false;
    }
    return fallback;
}

}  // namespace

Settings ParseSettings(std::string_view text) {
    Settings settings;
    // Skip a UTF-8 byte order mark written by some editors.
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    while (!text.empty()) {
        const std::string_view line = text::Trim(NextLine(text));
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        const std::string_view key = text::Trim(line.substr(0, equals));
        const std::string_view value = text::Trim(line.substr(equals + 1));

        if (key == "source") {
            settings.source_executable = text::FromUtf8(value);
        } else if (key == "microphone") {
            settings.microphone_id = text::FromUtf8(value);
        } else if (key == "inject_gain") {
            float gain = 0.0f;
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), gain);
            if (error == std::errc{} && end == value.data() + value.size()) {
                settings.inject_gain = std::clamp(gain, kMinGain, kMaxGain);
            }
        } else if (key == "mute_microphone") {
            settings.mute_microphone = ParseBool(value, settings.mute_microphone);
        }
    }
    return settings;
}

std::string SerializeSettings(const Settings& settings) {
    char gain[32];
    std::snprintf(gain, sizeof(gain), "%.2f", static_cast<double>(settings.inject_gain));

    std::string text = "# Somike settings\n";
    text += "source=" + text::ToUtf8(settings.source_executable) + "\n";
    text += "microphone=" + text::ToUtf8(settings.microphone_id) + "\n";
    text += std::string("inject_gain=") + gain + "\n";
    text += std::string("mute_microphone=") + (settings.mute_microphone ? "true" : "false") + "\n";
    return text;
}

std::wstring DefaultSettingsPath() {
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &folder))) {
        return L"settings.ini";
    }
    std::wstring path = std::wstring(folder) + L"\\Somike";
    ::CoTaskMemFree(folder);
    ::CreateDirectoryW(path.c_str(), nullptr);
    return path + L"\\settings.ini";
}

Settings LoadSettings(const std::wstring& path) {
    win32::UniqueFileHandle file(::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return {};
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024) {
        return {};
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!::ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
        return {};
    }
    text.resize(read);
    return ParseSettings(text);
}

HRESULT SaveSettings(const std::wstring& path, const Settings& settings) {
    const std::string text = SerializeSettings(settings);
    const std::wstring temp = path + L".tmp";
    {
        win32::UniqueFileHandle file(::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return win32::LastErrorAsHResult();
        }
        DWORD written = 0;
        if (!::WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written,
                         nullptr) ||
            written != text.size()) {
            return win32::LastErrorAsHResult();
        }
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return win32::LastErrorAsHResult();
    }
    return S_OK;
}

}  // namespace somike::core
