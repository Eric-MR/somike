// The soundboard: a library of short sounds played into the microphone (and,
// optionally, to the user's own speakers), with hotkeys and free sounds to
// download.
//
//   Play(id) --> mic mixer ------> core::Router --> shared ring --> APO --> microphone
//           \--> monitor mixer --> core::RenderStream --> default output device
//
// Files are copied into the soundboard folder on import, so the board keeps
// working when the originals move. Decoded audio is cached in memory (LRU,
// bounded) and preloaded at start, so hotkeys play instantly.
//
// All public methods run on the UI thread. Decoding and downloads run on
// background threads; their results come back through Poll().
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sound_catalog.h"
#include "soundboard_library.h"
#include "somike/core/render_stream.h"
#include "somike/dsp/voice_mixer.h"
#include "task_runner.h"

namespace somike::core {
class Router;
}

namespace somike::app {

// %LOCALAPPDATA%\Somike\Soundboard
[[nodiscard]] std::wstring DefaultSoundboardDirectory();

enum class OnlineState { Idle, Loading, Previewing, Adding, Added, Failed };
[[nodiscard]] const char* ToString(OnlineState state) noexcept;

struct PlayingSound {
    std::string id;
    std::uint32_t position_ms = 0;
    std::uint32_t length_ms = 0;
};

struct SearchResults {
    std::string query;
    std::uint32_t page = 1;
    std::optional<CatalogPage> page_data;  // nullopt: the search failed
    std::string error;                     // user-facing, when it failed
};

// Notifications for the UI, delivered on the UI thread.
class SoundboardListener {
public:
    virtual void OnSoundboardToast(const std::string& text, bool error) = 0;
    virtual void OnSearchResults(const SearchResults& results) = 0;
    virtual void OnOnlineState(const std::string& item_id, OnlineState state) = 0;

protected:
    ~SoundboardListener() = default;
};

class Soundboard {
public:
    Soundboard(std::wstring directory, core::Router& router);
    ~Soundboard();
    Soundboard(const Soundboard&) = delete;
    Soundboard& operator=(const Soundboard&) = delete;

    void SetListener(SoundboardListener* listener) noexcept { listener_ = listener; }
    // Called from a worker thread whenever background work finished; the UI
    // thread should then call Poll() (e.g. post a window message).
    void SetWakeCallback(std::function<void()> wake);
    void Poll();
    void Tick(std::uint64_t now_ms);  // ~4x per second: saving, end of previews

    // --- playback ------------------------------------------------------------
    void Toggle(const std::string& id);  // plays, or stops if it is playing
    void Stop(const std::string& id);
    void StopAll();
    void Test(const std::string& id);  // only to my own speakers

    // --- library -------------------------------------------------------------
    void Import(const std::vector<std::wstring>& paths);
    void Rename(const std::string& id, std::string_view name);
    void SetLook(const std::string& id, std::string_view emoji, std::string_view color);
    void SetVolume(const std::string& id, float volume);
    // Empty `hotkey.vk` clears it. Returns a user-facing error, or "" when set.
    // A combination already used elsewhere moves to this sound.
    std::string SetHotkey(const std::string& id, Hotkey hotkey);
    std::string SetStopHotkey(Hotkey hotkey);
    void Remove(const std::string& id);
    void Move(const std::string& id, std::size_t index);
    void SetMicVolume(float volume);
    void SetMonitor(bool on);
    void SetMonitorVolume(float volume);

    // --- free sounds online ----------------------------------------------------
    void Search(std::string_view query, std::uint32_t page);
    void PreviewOnline(const std::string& item_id);  // toggles
    void AddOnline(const std::string& item_id);
    [[nodiscard]] OnlineState OnlineStateOf(const CatalogItem& item) const;
    [[nodiscard]] const CatalogItem* FindCatalogItem(const std::string& item_id) const;

    // --- state for the UI ------------------------------------------------------
    [[nodiscard]] const SoundLibrary& library() const noexcept { return library_; }
    [[nodiscard]] const SoundEntry* Find(const std::string& id) const;
    [[nodiscard]] bool IsBroken(const std::string& id) const { return broken_.contains(id); }
    [[nodiscard]] bool IsLoading(const std::string& id) const { return play_when_ready_.contains(id); }
    [[nodiscard]] std::vector<PlayingSound> Playing() const;
    [[nodiscard]] const std::wstring& sounds_directory() const noexcept { return sounds_dir_; }

private:
    struct CachedClip {
        dsp::ClipPtr clip;
        std::uint64_t last_used = 0;
    };
    struct ImportResult {
        std::string id;
        std::string name;
        std::string file;
        std::uint32_t duration_ms = 0;
        bool truncated = false;
        dsp::ClipPtr clip;
        std::string error;  // user-facing; empty on success
        std::optional<SoundCredit> credit;
        std::string online_item;
    };
    struct ImportBatch {
        std::size_t pending = 0;
        std::vector<std::string> added;
        std::vector<std::string> failures;
        bool truncated = false;
    };

    class MonitorSource final : public core::IRenderSource {
    public:
        explicit MonitorSource(dsp::VoiceMixer& mixer) noexcept : mixer_(mixer) {}
        bool Render(float* out, std::size_t frames) noexcept override;

    private:
        dsp::VoiceMixer& mixer_;
    };

    // Worker-thread helpers: they only read the immutable folder paths.
    [[nodiscard]] ImportResult ImportFileWork(const std::wstring& path, const std::string& id,
                                              const std::string& name) const;
    [[nodiscard]] std::string DownloadToCache(const CatalogItem& item, std::wstring& path) const;

    void FinishImport(ImportResult result, ImportBatch& batch);
    void AddEntry(ImportResult& result);
    void RequestDecode(const SoundEntry& entry);
    void StartVoice(const SoundEntry& entry, const dsp::ClipPtr& clip);
    [[nodiscard]] dsp::ClipPtr CachedClipFor(const std::string& id);
    void EvictCache();
    [[nodiscard]] std::uint32_t VoiceId(const std::string& id);
    [[nodiscard]] SoundEntry* FindMutable(const std::string& id);
    [[nodiscard]] std::string NewId() const;
    void SetOnline(const std::string& item_id, OnlineState state);
    void StopPreview();
    void Toast(const std::string& text, bool error);
    void MarkDirty();
    void Load();
    void Save();

    const std::wstring directory_;
    const std::wstring sounds_dir_;
    const std::wstring cache_dir_;
    const std::wstring library_path_;
    core::Router& router_;
    SoundboardListener* listener_ = nullptr;

    SoundLibrary library_;
    bool dirty_ = false;
    std::uint64_t save_due_ms_ = 0;

    std::map<std::string, CachedClip> clips_;
    std::set<std::string> decoding_;
    std::set<std::string> play_when_ready_;
    std::set<std::string> broken_;  // files that are missing or unreadable
    std::uint64_t use_counter_ = 0;
    std::map<std::string, std::uint32_t> voice_ids_;
    std::map<std::uint32_t, std::string> voice_names_;
    std::uint32_t next_voice_id_ = 1;
    bool warned_not_ready_ = false;

    std::map<std::string, CatalogItem> catalog_;  // items of recent searches, by id
    std::map<std::string, OnlineState> online_;
    std::string preview_item_;

    dsp::VoiceMixer mic_mixer_;
    dsp::VoiceMixer monitor_mixer_;
    MonitorSource monitor_source_{monitor_mixer_};
    core::RenderStream monitor_;

    // Last members: destroyed first, so no worker outlives what it reads.
    TaskRunner audio_tasks_{1};
    TaskRunner net_tasks_{2};
};

}  // namespace somike::app
