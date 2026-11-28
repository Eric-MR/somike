#include "soundboard.h"

#include <windows.h>
#include <knownfolders.h>
#include <mferror.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <random>
#include <utility>

#include "http_client.h"
#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/strings.h"
#include "somike/common/unique_handle.h"
#include "somike/core/audio_file.h"
#include "somike/core/router.h"

namespace somike::app {

namespace {

constexpr std::uint32_t kPreviewVoice = 0xFFFFFFFFu;
constexpr double kMaxDecodeSeconds = 300.0;           // 5 minutes per sound
constexpr double kMaxPreviewSeconds = 60.0;
constexpr std::uint64_t kMaxImportBytes = 200ull << 20;
constexpr std::size_t kMaxDownloadBytes = 25u << 20;
constexpr std::size_t kMaxSearchBytes = 2u << 20;
constexpr std::size_t kCacheBudgetBytes = 512u << 20;  // decoded audio kept in memory
constexpr std::size_t kMaxLibraryBytes = 4u << 20;
constexpr std::uint64_t kSaveDelayMs = 800;
constexpr std::size_t kMaxCatalogItems = 1000;
constexpr std::size_t kMaxImportAtOnce = 100;

std::wstring W(std::string_view text) { return text::FromUtf8(text); }
std::string U8(std::wstring_view text) { return text::ToUtf8(text); }

bool IsRegularFile(const std::wstring& path, std::uint64_t* size = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    if (size != nullptr) {
        *size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }
    return true;
}

std::wstring FileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring StemOf(const std::wstring& name) {
    const auto dot = name.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

// Lower-case extension without the dot if it is short and plain, else "".
std::wstring ExtensionOf(const std::wstring& name) {
    const auto dot = name.rfind(L'.');
    if (dot == std::wstring::npos || name.size() - dot - 1 > 5) {
        return {};
    }
    std::wstring ext = name.substr(dot + 1);
    for (wchar_t& c : ext) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c + 32);
        } else if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))) {
            return {};
        }
    }
    return ext;
}

// File-system-safe ASCII stem for the copy in the sounds folder.
std::wstring SafeStem(const std::wstring& stem) {
    std::wstring out;
    for (const wchar_t c : stem) {
        const bool keep = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (keep) {
            out += c;
        } else if (!out.empty() && out.back() != L'-') {
            out += L'-';
        }
        if (out.size() >= 40) {
            break;
        }
    }
    while (!out.empty() && out.back() == L'-') {
        out.pop_back();
    }
    return out.empty() ? std::wstring(L"sound") : out;
}

bool NeedsStoreCodec(const std::wstring& ext) {
    return ext == L"ogg" || ext == L"oga" || ext == L"opus" || ext == L"webm" || ext == L"mka";
}

std::string DescribeDecodeError(HRESULT hr, const std::wstring& ext) {
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return "file not found";
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) {
        return "the file has no audio in it";
    }
    if (NeedsStoreCodec(ext)) {
        return "Windows needs the free \"Web Media Extensions\" from the Microsoft Store for this format";
    }
    if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_TOPO_CODEC_NOT_FOUND ||
        hr == MF_E_UNSUPPORTED_FORMAT || hr == MF_E_INVALID_FILE_FORMAT || hr == MF_E_NO_MORE_TYPES) {
        return "Windows can't read this file format";
    }
    return "could not read it (" + U8(win32::FormatHResult(hr)) + ")";
}

std::string Quoted(const std::string& name) { return "\xE2\x80\x9C" + name + "\xE2\x80\x9D"; }  // “name”

bool WriteFileAtomically(const std::wstring& path, const std::string& bytes) {
    const std::wstring temp = path + L".part";
    {
        win32::UniqueFileHandle file(::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                                   FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) {
            return false;
        }
        DWORD written = 0;
        if (!::WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size()) {
            file.reset();
            ::DeleteFileW(temp.c_str());
            return false;
        }
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

void DeleteFilesIn(const std::wstring& directory) {
    WIN32_FIND_DATAW data{};
    const HANDLE find = ::FindFirstFileW((directory + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            ::DeleteFileW((directory + L"\\" + data.cFileName).c_str());
        }
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);
}

std::uint32_t FramesToMs(std::size_t frames) { return static_cast<std::uint32_t>(frames / 48); }

}  // namespace

std::wstring DefaultSoundboardDirectory() {
    PWSTR folder = nullptr;
    std::wstring result;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder))) {
        result = std::wstring(folder) + L"\\Somike\\Soundboard";
    }
    ::CoTaskMemFree(folder);
    return result;
}

const char* ToString(OnlineState state) noexcept {
    switch (state) {
        case OnlineState::Loading:
            return "loading";
        case OnlineState::Previewing:
            return "previewing";
        case OnlineState::Adding:
            return "adding";
        case OnlineState::Added:
            return "added";
        case OnlineState::Failed:
            return "failed";
        case OnlineState::Idle:
            break;
    }
    return "idle";
}

bool Soundboard::MonitorSource::Render(float* out, std::size_t frames) noexcept {
    try {
        return mixer_.MixInto(out, frames);
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// lifetime
// ---------------------------------------------------------------------------
Soundboard::Soundboard(std::wstring directory, core::Router& router)
    : directory_(std::move(directory)),
      sounds_dir_(directory_ + L"\\sounds"),
      cache_dir_(directory_ + L"\\cache"),
      library_path_(directory_ + L"\\library.json"),
      router_(router) {
    ::SHCreateDirectoryExW(nullptr, sounds_dir_.c_str(), nullptr);
    ::SHCreateDirectoryExW(nullptr, cache_dir_.c_str(), nullptr);
    DeleteFilesIn(cache_dir_);  // previews of the last session

    Load();
    mic_mixer_.SetMasterGain(library_.settings.mic_volume);
    monitor_mixer_.SetMasterGain(library_.settings.monitor_volume);
    router_.SetSoundSource(&mic_mixer_);

    // Preload (newest first is not needed: the board is short), within budget,
    // so the first press of a hotkey plays at once.
    std::size_t budget = kCacheBudgetBytes;
    for (const SoundEntry& entry : library_.sounds) {
        const std::size_t estimate = std::size_t{entry.duration_ms} * 48 * 2 * sizeof(float);
        if (broken_.contains(entry.id) || estimate > budget) {
            continue;
        }
        budget -= estimate;
        RequestDecode(entry);
    }
}

Soundboard::~Soundboard() {
    router_.SetSoundSource(nullptr);
    monitor_.Stop();
    if (dirty_) {
        Save();
    }
}

void Soundboard::SetWakeCallback(std::function<void()> wake) {
    audio_tasks_.SetNotify(wake);
    net_tasks_.SetNotify(std::move(wake));
}

void Soundboard::Poll() {
    audio_tasks_.RunCompletions();
    net_tasks_.RunCompletions();
}

void Soundboard::Tick(std::uint64_t now_ms) {
    Poll();
    if (!preview_item_.empty() && online_[preview_item_] == OnlineState::Previewing &&
        !monitor_mixer_.IsPlaying(kPreviewVoice)) {
        const std::string item = preview_item_;
        preview_item_.clear();
        const CatalogItem* found = FindCatalogItem(item);
        SetOnline(item, found != nullptr && OnlineStateOf(*found) == OnlineState::Added ? OnlineState::Added
                                                                                         : OnlineState::Idle);
    }
    if (dirty_ && now_ms >= save_due_ms_) {
        Save();
    }
}

void Soundboard::Load() {
    win32::UniqueFileHandle file(::CreateFileW(library_path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return;  // first run
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxLibraryBytes)) {
        log::Warn(L"Ignoring soundboard library {} (size)", library_path_);
        return;
    }
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!::ReadFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &read, nullptr)) {
        return;
    }
    text.resize(read);
    library_ = ParseLibrary(text);
    for (const SoundEntry& entry : library_.sounds) {
        if (!IsRegularFile(sounds_dir_ + L"\\" + W(entry.file))) {
            broken_.insert(entry.id);
        }
    }
    log::Info(L"Soundboard: {} sounds", library_.sounds.size());
}

void Soundboard::Save() {
    dirty_ = false;
    if (!WriteFileAtomically(library_path_, SerializeLibrary(library_))) {
        log::Warn(L"Could not save the soundboard to {}", library_path_);
    }
}

void Soundboard::MarkDirty() {
    dirty_ = true;
    save_due_ms_ = ::GetTickCount64() + kSaveDelayMs;
}

void Soundboard::Toast(const std::string& text, bool error) {
    if (listener_ != nullptr) {
        listener_->OnSoundboardToast(text, error);
    }
}

// ---------------------------------------------------------------------------
// playback
// ---------------------------------------------------------------------------
std::uint32_t Soundboard::VoiceId(const std::string& id) {
    const auto it = voice_ids_.find(id);
    if (it != voice_ids_.end()) {
        return it->second;
    }
    const std::uint32_t voice = next_voice_id_++;
    voice_ids_.emplace(id, voice);
    voice_names_.emplace(voice, id);
    return voice;
}

void Soundboard::Toggle(const std::string& id) {
    const SoundEntry* entry = Find(id);
    if (entry == nullptr) {
        return;
    }
    const std::uint32_t voice = VoiceId(id);
    if (mic_mixer_.IsPlaying(voice) || monitor_mixer_.IsPlaying(voice) || play_when_ready_.contains(id)) {
        Stop(id);
        return;
    }
    if (dsp::ClipPtr clip = CachedClipFor(id)) {
        StartVoice(*entry, clip);
        return;
    }
    play_when_ready_.insert(id);
    RequestDecode(*entry);
}

void Soundboard::Stop(const std::string& id) {
    play_when_ready_.erase(id);
    const auto it = voice_ids_.find(id);
    if (it != voice_ids_.end()) {
        mic_mixer_.Stop(it->second);
        monitor_mixer_.Stop(it->second);
    }
}

void Soundboard::StopAll() {
    play_when_ready_.clear();
    mic_mixer_.StopAll();
    monitor_mixer_.StopAll();
}

void Soundboard::Test(const std::string& id) {
    const SoundEntry* entry = Find(id);
    dsp::ClipPtr clip = entry != nullptr ? CachedClipFor(id) : nullptr;
    if (!clip) {
        if (entry != nullptr) {
            RequestDecode(*entry);  // ready for the next click
        }
        return;
    }
    StopPreview();
    monitor_mixer_.Play(kPreviewVoice, clip, entry->volume);
    monitor_.Start(monitor_source_);
}

void Soundboard::StartVoice(const SoundEntry& entry, const dsp::ClipPtr& clip) {
    const std::uint32_t voice = VoiceId(entry.id);
    mic_mixer_.Play(voice, clip, entry.volume);
    const HRESULT hr = router_.KickSoundSource();
    if (FAILED(hr) && !warned_not_ready_) {
        warned_not_ready_ = true;
        Toast("Set up Somike on your microphone first, then others hear your sounds too.", true);
    }
    if (library_.settings.monitor) {
        monitor_mixer_.Play(voice, clip, entry.volume);
        monitor_.Start(monitor_source_);
    }
}

std::vector<PlayingSound> Soundboard::Playing() const {
    std::vector<PlayingSound> result;
    auto add = [&](const std::vector<dsp::VoiceInfo>& voices) {
        for (const dsp::VoiceInfo& v : voices) {
            const auto name = voice_names_.find(v.sound_id);
            if (name == voice_names_.end() ||
                std::any_of(result.begin(), result.end(), [&](const PlayingSound& p) { return p.id == name->second; })) {
                continue;
            }
            result.push_back({name->second, FramesToMs(v.position), FramesToMs(v.length)});
        }
    };
    add(mic_mixer_.Snapshot());
    add(monitor_mixer_.Snapshot());
    return result;
}

// ---------------------------------------------------------------------------
// decoding and the clip cache
// ---------------------------------------------------------------------------
dsp::ClipPtr Soundboard::CachedClipFor(const std::string& id) {
    const auto it = clips_.find(id);
    if (it == clips_.end()) {
        return nullptr;
    }
    it->second.last_used = ++use_counter_;
    return it->second.clip;
}

void Soundboard::RequestDecode(const SoundEntry& entry) {
    if (decoding_.contains(entry.id) || clips_.contains(entry.id)) {
        return;
    }
    decoding_.insert(entry.id);
    const std::wstring path = sounds_dir_ + L"\\" + W(entry.file);
    const std::string id = entry.id;
    audio_tasks_.Post([this, path, id]() -> TaskRunner::Completion {
        core::DecodedAudio decoded;
        const HRESULT hr = core::DecodeAudioFile(path, kMaxDecodeSeconds, decoded);
        std::string error;
        if (FAILED(hr)) {
            error = DescribeDecodeError(hr, ExtensionOf(path));
            log::Warn(L"Cannot decode {}: {}", path, win32::FormatHResult(hr));
        }
        return [this, id, clip = decoded.clip, error]() {
            decoding_.erase(id);
            const SoundEntry* entry = Find(id);
            if (entry == nullptr) {
                return;  // removed meanwhile
            }
            if (!clip) {
                broken_.insert(id);
                if (play_when_ready_.erase(id) != 0) {
                    Toast("Can't play " + Quoted(entry->name) + ": " + error, true);
                }
                return;
            }
            broken_.erase(id);
            clips_[id] = {clip, ++use_counter_};
            if (play_when_ready_.erase(id) != 0) {
                StartVoice(*entry, clip);
            }
            EvictCache();
        };
    });
}

void Soundboard::EvictCache() {
    std::size_t total = 0;
    for (const auto& [id, cached] : clips_) {
        total += cached.clip->bytes();
    }
    while (total > kCacheBudgetBytes) {
        auto victim = clips_.end();
        for (auto it = clips_.begin(); it != clips_.end(); ++it) {
            const auto voice = voice_ids_.find(it->first);
            const bool playing = voice != voice_ids_.end() &&
                                 (mic_mixer_.IsPlaying(voice->second) || monitor_mixer_.IsPlaying(voice->second));
            if (!playing && (victim == clips_.end() || it->second.last_used < victim->second.last_used)) {
                victim = it;
            }
        }
        if (victim == clips_.end()) {
            return;
        }
        total -= victim->second.clip->bytes();
        clips_.erase(victim);
    }
}

// ---------------------------------------------------------------------------
// library
// ---------------------------------------------------------------------------
const SoundEntry* Soundboard::Find(const std::string& id) const {
    const auto it = std::find_if(library_.sounds.begin(), library_.sounds.end(),
                                 [&](const SoundEntry& s) { return s.id == id; });
    return it == library_.sounds.end() ? nullptr : &*it;
}

SoundEntry* Soundboard::FindMutable(const std::string& id) {
    return const_cast<SoundEntry*>(std::as_const(*this).Find(id));
}

std::string Soundboard::NewId() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device random;
    for (;;) {
        std::string id;
        for (int i = 0; i < 12; ++i) {
            id += kHex[random() & 0xF];
        }
        if (Find(id) == nullptr) {
            return id;
        }
    }
}

Soundboard::ImportResult Soundboard::ImportFileWork(const std::wstring& path, const std::string& id,
                                                    const std::string& name) const {
    ImportResult result;
    result.id = id;
    result.name = name;
    std::uint64_t size = 0;
    if (!IsRegularFile(path, &size)) {
        result.error = "file not found";
        return result;
    }
    if (size > kMaxImportBytes) {
        result.error = "the file is too large";
        return result;
    }

    const std::wstring ext = ExtensionOf(FileNameOf(path));
    core::DecodedAudio decoded;
    const HRESULT hr = core::DecodeAudioFile(path, kMaxDecodeSeconds, decoded);
    if (FAILED(hr)) {
        log::Warn(L"Cannot import {}: {}", path, win32::FormatHResult(hr));
        result.error = DescribeDecodeError(hr, ext);
        return result;
    }

    // Named after the sound, not the source file: downloads come from the
    // cache as "<catalogue id>.mp3".
    const std::wstring file = SafeStem(W(name)) + L"-" + W(id) + L"." + (ext.empty() ? L"audio" : ext);
    if (!::CopyFileW(path.c_str(), (sounds_dir_ + L"\\" + file).c_str(), TRUE)) {
        result.error = "could not copy it into the soundboard folder";
        return result;
    }
    result.file = U8(file);
    result.clip = decoded.clip;
    result.truncated = decoded.truncated;
    result.duration_ms = FramesToMs(decoded.clip->frames());
    return result;
}

void Soundboard::Import(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> files;
    for (const std::wstring& path : paths) {
        if (!path.empty() && files.size() < kMaxImportAtOnce) {
            files.push_back(path);
        }
    }
    const std::size_t room = kMaxSounds > library_.sounds.size() ? kMaxSounds - library_.sounds.size() : 0;
    if (files.size() > room) {
        Toast(std::format("The soundboard holds up to {} sounds.", kMaxSounds), true);
        files.resize(room);
    }
    if (files.empty()) {
        return;
    }
    auto batch = std::make_shared<ImportBatch>();
    batch->pending = files.size();
    std::set<std::string> reserved;
    for (const std::wstring& path : files) {
        std::string id = NewId();
        while (!reserved.insert(id).second) {
            id = NewId();
        }
        const std::string name = NameFromTitle(U8(StemOf(FileNameOf(path))));
        audio_tasks_.Post([this, path, id, name, batch]() -> TaskRunner::Completion {
            ImportResult result = ImportFileWork(path, id, name);
            return [this, result = std::move(result), batch]() mutable { FinishImport(std::move(result), *batch); };
        });
    }
}

void Soundboard::AddEntry(ImportResult& result) {
    SoundEntry entry;
    entry.id = result.id;
    entry.name = result.name;
    entry.file = result.file;
    entry.color = DefaultColorFor(result.id);
    entry.duration_ms = result.duration_ms;
    entry.credit = result.credit;
    library_.sounds.push_back(entry);
    clips_[entry.id] = {result.clip, ++use_counter_};
    EvictCache();
    MarkDirty();
}

void Soundboard::FinishImport(ImportResult result, ImportBatch& batch) {
    --batch.pending;
    if (!result.error.empty() || library_.sounds.size() >= kMaxSounds) {
        batch.failures.push_back(Quoted(result.name) + ": " +
                                 (result.error.empty() ? std::string("the soundboard is full") : result.error));
    } else {
        batch.truncated = batch.truncated || result.truncated;
        batch.added.push_back(result.name);
        AddEntry(result);
    }
    if (batch.pending != 0) {
        return;
    }
    if (!batch.added.empty()) {
        std::string text = batch.added.size() == 1 ? "Added " + Quoted(batch.added.front())
                                                   : std::format("Added {} sounds", batch.added.size());
        if (batch.truncated) {
            text += ". Sounds are limited to 5 minutes; longer files were cut.";
        }
        Toast(text, false);
    }
    if (!batch.failures.empty()) {
        std::string text = "Could not add " + batch.failures.front();
        if (batch.failures.size() > 1) {
            text += std::format(" (and {} more)", batch.failures.size() - 1);
        }
        Toast(text, true);
    }
}

void Soundboard::Rename(const std::string& id, std::string_view name) {
    SoundEntry* entry = FindMutable(id);
    const std::string clean = CleanText(name, kMaxSoundNameChars);
    if (entry != nullptr && !clean.empty() && clean != entry->name) {
        entry->name = clean;
        MarkDirty();
    }
}

void Soundboard::SetLook(const std::string& id, std::string_view emoji, std::string_view color) {
    SoundEntry* entry = FindMutable(id);
    if (entry == nullptr) {
        return;
    }
    if (emoji.size() <= 32) {
        entry->emoji = CleanText(emoji, 8);
    }
    if (IsValidColor(color)) {
        entry->color = std::string(color);
    }
    MarkDirty();
}

void Soundboard::SetVolume(const std::string& id, float volume) {
    SoundEntry* entry = FindMutable(id);
    if (entry == nullptr || !std::isfinite(volume)) {
        return;
    }
    entry->volume = std::clamp(volume, 0.0f, 2.0f);
    if (const auto voice = voice_ids_.find(id); voice != voice_ids_.end()) {
        mic_mixer_.SetVoiceGain(voice->second, entry->volume);
        monitor_mixer_.SetVoiceGain(voice->second, entry->volume);
    }
    MarkDirty();
}

std::string Soundboard::SetHotkey(const std::string& id, Hotkey hotkey) {
    SoundEntry* entry = FindMutable(id);
    if (entry == nullptr) {
        return "This sound no longer exists.";
    }
    if (!hotkey.empty() && !IsAllowedHotkey(hotkey.vk, hotkey.mods)) {
        return "Use a key together with Ctrl, Alt or Win (F-keys and the number pad also work alone).";
    }
    hotkey.label = CleanText(hotkey.label, 40);
    if (!hotkey.empty()) {
        for (SoundEntry& other : library_.sounds) {
            if (other.id != id && other.hotkey.SameKeys(hotkey)) {
                other.hotkey = {};
            }
        }
        if (library_.settings.stop_hotkey.SameKeys(hotkey)) {
            library_.settings.stop_hotkey = {};
        }
    }
    entry->hotkey = hotkey.empty() ? Hotkey{} : hotkey;
    MarkDirty();
    return {};
}

std::string Soundboard::SetStopHotkey(Hotkey hotkey) {
    if (!hotkey.empty() && !IsAllowedHotkey(hotkey.vk, hotkey.mods)) {
        return "Use a key together with Ctrl, Alt or Win (F-keys and the number pad also work alone).";
    }
    hotkey.label = CleanText(hotkey.label, 40);
    if (!hotkey.empty()) {
        for (SoundEntry& other : library_.sounds) {
            if (other.hotkey.SameKeys(hotkey)) {
                other.hotkey = {};
            }
        }
    }
    library_.settings.stop_hotkey = hotkey.empty() ? Hotkey{} : hotkey;
    MarkDirty();
    return {};
}

void Soundboard::Remove(const std::string& id) {
    const auto it = std::find_if(library_.sounds.begin(), library_.sounds.end(),
                                 [&](const SoundEntry& s) { return s.id == id; });
    if (it == library_.sounds.end()) {
        return;
    }
    Stop(id);
    const std::wstring file = sounds_dir_ + L"\\" + W(it->file);
    library_.sounds.erase(it);
    clips_.erase(id);
    broken_.erase(id);
    if (!::DeleteFileW(file.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND) {
        log::Warn(L"Could not delete {}", file);
    }
    MarkDirty();
}

void Soundboard::Move(const std::string& id, std::size_t index) {
    auto& sounds = library_.sounds;
    const auto it = std::find_if(sounds.begin(), sounds.end(), [&](const SoundEntry& s) { return s.id == id; });
    if (it == sounds.end()) {
        return;
    }
    SoundEntry entry = std::move(*it);
    sounds.erase(it);
    index = std::min(index, sounds.size());
    sounds.insert(sounds.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
    MarkDirty();
}

void Soundboard::SetMicVolume(float volume) {
    if (!std::isfinite(volume)) {
        return;
    }
    library_.settings.mic_volume = std::clamp(volume, 0.0f, 2.0f);
    mic_mixer_.SetMasterGain(library_.settings.mic_volume);
    MarkDirty();
}

void Soundboard::SetMonitor(bool on) {
    library_.settings.monitor = on;
    if (!on) {
        for (const auto& [voice, id] : voice_names_) {
            monitor_mixer_.Stop(voice);
        }
    }
    MarkDirty();
}

void Soundboard::SetMonitorVolume(float volume) {
    if (!std::isfinite(volume)) {
        return;
    }
    library_.settings.monitor_volume = std::clamp(volume, 0.0f, 2.0f);
    monitor_mixer_.SetMasterGain(library_.settings.monitor_volume);
    MarkDirty();
}

// ---------------------------------------------------------------------------
// free sounds online
// ---------------------------------------------------------------------------
const CatalogItem* Soundboard::FindCatalogItem(const std::string& item_id) const {
    const auto it = catalog_.find(item_id);
    return it == catalog_.end() ? nullptr : &it->second;
}

OnlineState Soundboard::OnlineStateOf(const CatalogItem& item) const {
    if (const auto it = online_.find(item.id); it != online_.end() && it->second != OnlineState::Idle &&
                                               it->second != OnlineState::Failed) {
        return it->second;
    }
    const bool in_library =
        !item.landing_url.empty() &&
        std::any_of(library_.sounds.begin(), library_.sounds.end(),
                    [&](const SoundEntry& s) { return s.credit && s.credit->url == item.landing_url; });
    return in_library ? OnlineState::Added : OnlineState::Idle;
}

void Soundboard::SetOnline(const std::string& item_id, OnlineState state) {
    online_[item_id] = state;
    if (listener_ != nullptr) {
        listener_->OnOnlineState(item_id, state);
    }
}

void Soundboard::Search(std::string_view query, std::uint32_t page) {
    const std::string q = CleanText(query, kMaxQueryChars);
    if (q.empty()) {
        return;
    }
    const std::string url = OpenverseSearchUrl(q, page);
    net_tasks_.Post([this, url, q, page]() -> TaskRunner::Completion {
        SearchResults results;
        results.query = q;
        results.page = page;
        HttpResponse response;
        const HRESULT hr = HttpGet(W(url), kMaxSearchBytes, response);
        if (FAILED(hr)) {
            log::Warn(L"Sound search failed: {}", win32::FormatHResult(hr));
            results.error = "Can't reach the sound library. Check your internet connection.";
        } else if (response.status == 429) {
            results.error = "Too many searches right now. Wait a minute, then try again.";
        } else if (response.status != 200) {
            results.error = std::format("The sound library is not available right now (HTTP {}).", response.status);
        } else {
            results.page_data = ParseOpenverseResults(response.body);
            if (!results.page_data) {
                results.error = "The sound library sent an unexpected answer.";
            }
        }
        return [this, results = std::move(results)]() {
            if (results.page_data) {
                if (catalog_.size() > kMaxCatalogItems) {
                    catalog_.clear();
                }
                for (const CatalogItem& item : results.page_data->items) {
                    catalog_[item.id] = item;
                }
            }
            if (listener_ != nullptr) {
                listener_->OnSearchResults(results);
            }
        };
    });
}

std::string Soundboard::DownloadToCache(const CatalogItem& item, std::wstring& path) const {
    path = cache_dir_ + L"\\" + W(item.id) + L"." + W(item.filetype);
    std::uint64_t size = 0;
    if (IsRegularFile(path, &size) && size > 0) {
        return {};
    }
    HttpResponse response;
    const HRESULT hr = HttpGet(W(item.download_url), kMaxDownloadBytes, response);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)) {
        return "the file is too large";
    }
    if (FAILED(hr)) {
        log::Warn(L"Download of {} failed: {}", W(item.download_url), win32::FormatHResult(hr));
        return "the download failed. Check your internet connection";
    }
    if (response.status != 200 || response.body.empty()) {
        return std::format("the sound is not available any more (HTTP {})", response.status);
    }
    if (!WriteFileAtomically(path, response.body)) {
        return "could not save the download";
    }
    return {};
}

void Soundboard::StopPreview() {
    monitor_mixer_.Stop(kPreviewVoice);
    if (!preview_item_.empty()) {
        const std::string item = preview_item_;
        preview_item_.clear();
        const CatalogItem* found = FindCatalogItem(item);
        if (found != nullptr && (online_[item] == OnlineState::Previewing || online_[item] == OnlineState::Loading)) {
            SetOnline(item, OnlineStateOf(*found) == OnlineState::Added ? OnlineState::Added : OnlineState::Idle);
        }
    }
}

void Soundboard::PreviewOnline(const std::string& item_id) {
    const CatalogItem* item = FindCatalogItem(item_id);
    if (item == nullptr) {
        return;
    }
    if (preview_item_ == item_id) {
        StopPreview();  // second click stops it
        return;
    }
    StopPreview();
    preview_item_ = item_id;
    SetOnline(item_id, OnlineState::Loading);
    net_tasks_.Post([this, item = *item]() -> TaskRunner::Completion {
        std::wstring path;
        std::string error = DownloadToCache(item, path);
        core::DecodedAudio decoded;
        if (error.empty()) {
            const HRESULT hr = core::DecodeAudioFile(path, kMaxPreviewSeconds, decoded);
            if (FAILED(hr)) {
                error = DescribeDecodeError(hr, W(item.filetype));
            }
        }
        return [this, id = item.id, title = item.title, clip = decoded.clip, error]() {
            if (preview_item_ != id) {
                return;  // the user moved on meanwhile
            }
            if (!error.empty()) {
                preview_item_.clear();
                SetOnline(id, OnlineState::Failed);
                Toast("Can't play " + Quoted(NameFromTitle(title)) + ": " + error, true);
                return;
            }
            monitor_mixer_.Play(kPreviewVoice, clip, 1.0f);
            monitor_.Start(monitor_source_);
            SetOnline(id, OnlineState::Previewing);
        };
    });
}

void Soundboard::AddOnline(const std::string& item_id) {
    const CatalogItem* item = FindCatalogItem(item_id);
    if (item == nullptr) {
        return;
    }
    const OnlineState state = OnlineStateOf(*item);
    if (state == OnlineState::Adding || state == OnlineState::Added) {
        return;
    }
    if (library_.sounds.size() >= kMaxSounds) {
        Toast(std::format("The soundboard holds up to {} sounds.", kMaxSounds), true);
        return;
    }
    SetOnline(item_id, OnlineState::Adding);
    const std::string id = NewId();
    net_tasks_.Post([this, item = *item, id]() -> TaskRunner::Completion {
        std::wstring path;
        ImportResult result;
        result.id = id;
        result.name = NameFromTitle(item.title);
        result.error = DownloadToCache(item, path);
        if (result.error.empty()) {
            result = ImportFileWork(path, id, NameFromTitle(item.title));
        }
        result.online_item = item.id;
        result.credit = SoundCredit{item.title, item.creator, item.license, item.landing_url};
        return [this, result = std::move(result)]() mutable {
            if (!result.error.empty() || library_.sounds.size() >= kMaxSounds || Find(result.id) != nullptr) {
                SetOnline(result.online_item, OnlineState::Failed);
                Toast("Could not add " + Quoted(result.name) + ": " + result.error, true);
                return;
            }
            const std::string item_id = result.online_item;
            const std::string name = result.name;
            AddEntry(result);
            SetOnline(item_id, OnlineState::Added);
            Toast("Added " + Quoted(name) + " to your soundboard", false);
        };
    });
}

}  // namespace somike::app
