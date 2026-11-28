#include "soundboard_bridge.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <format>

#include <nlohmann/json.hpp>

#include "app_controller.h"
#include "app_state.h"
#include "somike/common/log.h"
#include "somike/common/strings.h"

namespace somike::app {

using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace {

constexpr int kHotkeyBase = 0x2000;  // the window's own hotkey uses id 1
constexpr int kHotkeyLimit = kHotkeyBase + static_cast<int>(kMaxSounds) + 1;

std::string Dump(const json& value) { return value.dump(-1, ' ', false, json::error_handler_t::replace); }

double Round2(float value) { return std::isfinite(value) ? std::round(value * 100.0) / 100.0 : 0.0; }

json HotkeyJson(const Hotkey& hotkey) {
    return {{"vk", hotkey.vk}, {"mods", hotkey.mods}, {"label", hotkey.label}};
}

std::string GetString(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::optional<double> GetNumber(const json& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return std::nullopt;
    }
    const double value = it->get<double>();
    return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
}

std::optional<bool> GetBool(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() ? std::optional<bool>(it->get<bool>()) : std::nullopt;
}

Hotkey HotkeyFrom(const json& message) {
    Hotkey hotkey;
    const auto vk = GetNumber(message, "vk");
    const auto mods = GetNumber(message, "mods");
    if (vk && mods && *vk >= 1 && *vk <= 0xFE && *mods >= 0 && *mods <= 0xF) {
        hotkey.vk = static_cast<std::uint32_t>(*vk);
        hotkey.mods = static_cast<std::uint32_t>(*mods);
        hotkey.label = GetString(message, "label");
    }
    return hotkey;
}

}  // namespace

SoundboardBridge::SoundboardBridge(AppController& controller, std::function<void(const std::string&)> post_json)
    : controller_(controller), soundboard_(controller.soundboard()), post_json_(std::move(post_json)) {
    soundboard_.SetListener(this);
}

SoundboardBridge::~SoundboardBridge() {
    soundboard_.SetListener(nullptr);
    soundboard_.SetWakeCallback(nullptr);
}

void SoundboardBridge::Attach(HWND hwnd) {
    hwnd_ = hwnd;
    soundboard_.SetWakeCallback([hwnd] { ::PostMessageW(hwnd, kWakeMessage, 0, 0); });
    ApplyHotkeys();
}

void SoundboardBridge::Detach() {
    soundboard_.SetWakeCallback(nullptr);
    UnregisterHotkeys();
    hwnd_ = nullptr;
    ui_ready_ = false;
}

void SoundboardBridge::OnUiReady() {
    ui_ready_ = true;
    capturing_ = false;
    last_library_.clear();
    last_playing_.clear();
    ApplyHotkeys();
    PushLibrary(true);
    PushPlaying(true);
}

void SoundboardBridge::Post(const std::string& json_text) {
    if (ui_ready_ && post_json_) {
        post_json_(json_text);
    }
}

void SoundboardBridge::Refresh() {
    ApplyHotkeys();
    PushLibrary(false);
    PushPlaying(false);
}

void SoundboardBridge::OnWake() {
    soundboard_.Poll();
    Refresh();
}

void SoundboardBridge::Tick() { Refresh(); }

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------
void SoundboardBridge::HandleCommand(const std::string& cmd, const std::string& json_text,
                                     const std::vector<std::wstring>& files) {
    const json message = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!message.is_object()) {
        return;
    }
    const std::string id = GetString(message, "id");

    if (cmd == "sb.play") {
        soundboard_.Toggle(id);
    } else if (cmd == "sb.stop") {
        soundboard_.Stop(id);
    } else if (cmd == "sb.stopAll") {
        soundboard_.StopAll();
    } else if (cmd == "sb.test") {
        soundboard_.Test(id);
    } else if (cmd == "sb.import") {
        ShowImportDialog();
    } else if (cmd == "sb.drop") {
        soundboard_.Import(files);
    } else if (cmd == "sb.rename") {
        soundboard_.Rename(id, GetString(message, "value"));
    } else if (cmd == "sb.look") {
        soundboard_.SetLook(id, GetString(message, "emoji"), GetString(message, "color"));
    } else if (cmd == "sb.volume") {
        if (const auto value = GetNumber(message, "value")) {
            soundboard_.SetVolume(id, static_cast<float>(*value));
        }
    } else if (cmd == "sb.remove") {
        soundboard_.Remove(id);
    } else if (cmd == "sb.move") {
        if (const auto index = GetNumber(message, "index"); index && *index >= 0) {
            soundboard_.Move(id, static_cast<std::size_t>(*index));
        }
    } else if (cmd == "sb.hotkey") {
        const Hotkey hotkey = HotkeyFrom(message);
        const std::string error = id.empty() ? soundboard_.SetStopHotkey(hotkey) : soundboard_.SetHotkey(id, hotkey);
        if (!error.empty()) {
            OnSoundboardToast(error, true);
        }
    } else if (cmd == "sb.capture") {
        capturing_ = GetBool(message, "value").value_or(false);
    } else if (cmd == "sb.settings") {
        if (const auto v = GetNumber(message, "micVolume")) {
            soundboard_.SetMicVolume(static_cast<float>(*v));
        }
        if (const auto v = GetBool(message, "monitor")) {
            soundboard_.SetMonitor(*v);
        }
        if (const auto v = GetNumber(message, "monitorVolume")) {
            soundboard_.SetMonitorVolume(static_cast<float>(*v));
        }
    } else if (cmd == "sb.search") {
        const auto page = GetNumber(message, "page").value_or(1.0);
        soundboard_.Search(GetString(message, "query"),
                           static_cast<std::uint32_t>(std::clamp(page, 1.0, static_cast<double>(kCatalogMaxPage))));
    } else if (cmd == "sb.preview") {
        soundboard_.PreviewOnline(id);
    } else if (cmd == "sb.add") {
        soundboard_.AddOnline(id);
    } else if (cmd == "sb.openFolder") {
        ::ShellExecuteW(hwnd_, L"open", soundboard_.sounds_directory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == "sb.openLink") {
        const std::string url = GetString(message, "url");
        if (IsAllowedLinkUrl(url)) {
            ::ShellExecuteW(hwnd_, L"open", text::FromUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else {
        log::Warn(L"Unknown soundboard command '{}'", text::FromUtf8(cmd));
        return;
    }
    Refresh();
}

void SoundboardBridge::ShowImportDialog() {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return;
    }
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC filters[] = {
        {L"Audio files", L"*.mp3;*.wav;*.flac;*.m4a;*.aac;*.wma;*.ogg;*.oga;*.opus;*.mp4;*.webm"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Add sounds to the soundboard");
    if (FAILED(dialog->Show(hwnd_))) {
        return;  // cancelled
    }
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(dialog->GetResults(&items)) || FAILED(items->GetCount(&count))) {
        return;
    }
    std::vector<std::wstring> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            ::CoTaskMemFree(path);
        }
    }
    soundboard_.Import(paths);
}

// ---------------------------------------------------------------------------
// hotkeys
// ---------------------------------------------------------------------------
void SoundboardBridge::UnregisterHotkeys() {
    if (hwnd_ != nullptr) {
        for (const auto& [hotkey_id, sound] : registered_) {
            ::UnregisterHotKey(hwnd_, hotkey_id);
        }
    }
    registered_.clear();
}

void SoundboardBridge::ApplyHotkeys() {
    const SoundLibrary& library = soundboard_.library();
    // Re-register only when the assignments change.
    std::string signature = capturing_ ? "capture;" : "";
    const auto sign = [&](const std::string& id, const Hotkey& key) {
        if (!key.empty()) {
            signature += std::format("{}={}:{};", id, key.vk, key.mods);
        }
    };
    sign("", library.settings.stop_hotkey);
    for (const SoundEntry& sound : library.sounds) {
        sign(sound.id, sound.hotkey);
    }
    if (signature == applied_hotkeys_ && hwnd_ != nullptr) {
        return;
    }
    UnregisterHotkeys();
    hotkey_failed_.clear();
    applied_hotkeys_ = signature;
    if (hwnd_ == nullptr || capturing_) {
        return;  // while the UI records a new hotkey, the old ones must not fire
    }

    int next = kHotkeyBase;
    const auto add = [&](const std::string& id, const Hotkey& key) {
        if (key.empty() || next >= kHotkeyLimit) {
            return;
        }
        if (::RegisterHotKey(hwnd_, next, key.mods | MOD_NOREPEAT, key.vk)) {
            registered_.emplace_back(next, id);
            ++next;
        } else {
            hotkey_failed_.insert(id);
            log::Warn(L"Hotkey {} is already used by another program", text::FromUtf8(key.label));
        }
    };
    add("", library.settings.stop_hotkey);
    for (const SoundEntry& sound : library.sounds) {
        add(sound.id, sound.hotkey);
    }
}

bool SoundboardBridge::HandleHotkey(WPARAM id) {
    const auto it = std::find_if(registered_.begin(), registered_.end(),
                                 [&](const auto& entry) { return static_cast<WPARAM>(entry.first) == id; });
    if (it == registered_.end()) {
        return false;
    }
    if (it->second.empty()) {
        soundboard_.StopAll();
    } else {
        soundboard_.Toggle(it->second);
    }
    PushPlaying(false);
    return true;
}

// ---------------------------------------------------------------------------
// pushes
// ---------------------------------------------------------------------------
void SoundboardBridge::PushLibrary(bool force) {
    if (!ui_ready_) {
        return;
    }
    const SoundLibrary& library = soundboard_.library();
    json sounds = json::array();
    for (const SoundEntry& s : library.sounds) {
        json credit = nullptr;
        if (s.credit) {
            credit = {{"title", s.credit->title},
                      {"creator", s.credit->creator},
                      {"license", s.credit->license},
                      {"url", s.credit->url}};
        }
        sounds.push_back({{"id", s.id},
                          {"name", s.name},
                          {"emoji", s.emoji},
                          {"color", s.color},
                          {"volume", Round2(s.volume)},
                          {"durationMs", s.duration_ms},
                          {"hotkey", HotkeyJson(s.hotkey)},
                          {"hotkeyFailed", hotkey_failed_.contains(s.id)},
                          {"broken", soundboard_.IsBroken(s.id)},
                          {"credit", std::move(credit)}});
    }
    const SoundboardSettings& st = library.settings;
    const json message = {
        {"type", "sb.library"},
        {"library",
         {{"ready", controller_.mic_ready()},
          {"settings",
           {{"micVolume", Round2(st.mic_volume)},
            {"monitor", st.monitor},
            {"monitorVolume", Round2(st.monitor_volume)},
            {"stopHotkey", HotkeyJson(st.stop_hotkey)},
            {"stopHotkeyFailed", hotkey_failed_.contains("")}}},
          {"sounds", std::move(sounds)}}},
    };
    std::string text = Dump(message);
    if (!force && text == last_library_) {
        return;
    }
    last_library_ = text;
    Post(last_library_);
}

void SoundboardBridge::PushPlaying(bool force) {
    if (!ui_ready_) {
        return;
    }
    json playing = json::array();
    for (const PlayingSound& p : soundboard_.Playing()) {
        playing.push_back({{"id", p.id}, {"pos", p.position_ms}, {"len", p.length_ms}});
    }
    json loading = json::array();
    for (const SoundEntry& s : soundboard_.library().sounds) {
        if (soundboard_.IsLoading(s.id)) {
            loading.push_back(s.id);
        }
    }
    std::string text = Dump({{"type", "sb.playing"}, {"playing", std::move(playing)}, {"loading", std::move(loading)}});
    if (!force && text == last_playing_) {
        return;
    }
    last_playing_ = text;
    Post(last_playing_);
}

// ---------------------------------------------------------------------------
// SoundboardListener
// ---------------------------------------------------------------------------
void SoundboardBridge::OnSoundboardToast(const std::string& text, bool error) {
    Post(SerializeToastMessage(text, error));
}

void SoundboardBridge::OnSearchResults(const SearchResults& results) {
    json items = json::array();
    std::uint32_t page_count = 0;
    std::uint32_t total = 0;
    if (results.page_data) {
        page_count = results.page_data->page_count;
        total = results.page_data->result_count;
        for (const CatalogItem& item : results.page_data->items) {
            items.push_back({{"id", item.id},
                             {"title", NameFromTitle(item.title)},
                             {"creator", item.creator},
                             {"license", item.license},
                             {"source", item.source},
                             {"url", item.landing_url},
                             {"durationMs", item.duration_ms},
                             {"state", ToString(soundboard_.OnlineStateOf(item))}});
        }
    }
    Post(Dump({{"type", "sb.results"},
               {"query", results.query},
               {"page", results.page},
               {"pageCount", page_count},
               {"total", total},
               {"error", results.error},
               {"items", std::move(items)}}));
}

void SoundboardBridge::OnOnlineState(const std::string& item_id, OnlineState state) {
    Post(Dump({{"type", "sb.online"}, {"id", item_id}, {"state", ToString(state)}}));
}

}  // namespace somike::app
