#include "app_controller.h"

#include <algorithm>
#include <format>

#include "somike/common/hresult.h"
#include "somike/common/log.h"
#include "somike/common/strings.h"
#include "somike/core/audio_sessions.h"
#include "somike/core/process_tree.h"
#include "somike/installer/fx_config.h"

namespace somike::app {

namespace {

constexpr float kMaxInjectGain = 2.0f;
constexpr std::uint64_t kSourcesIntervalMs = 1500;
constexpr std::uint64_t kStatusIntervalMs = 1000;
constexpr std::uint64_t kMicsIntervalMs = 5000;
constexpr std::uint64_t kRetryDelayMs = 2000;
constexpr std::uint64_t kSaveDelayMs = 1500;

std::wstring Lower(std::wstring value) {
    if (!value.empty()) {
        ::CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    }
    return value;
}

std::string U8(const std::wstring& value) { return text::ToUtf8(value); }

bool FileExists(const std::wstring& path) {
    return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring DescribeRouteError(HRESULT hr, const std::wstring& source) {
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return L"Somike is not set up on a microphone yet";
    }
    if (hr == E_ACCESSDENIED) {
        return L"Windows denied access to the audio of " + source;
    }
    return std::format(L"Could not capture {} ({}), retrying", source, win32::FormatHResult(hr));
}

}  // namespace

AppController::AppController(std::wstring settings_path)
    : settings_path_(std::move(settings_path)),
      settings_(core::LoadSettings(settings_path_)),
      soundboard_(DefaultSoundboardDirectory(), router_) {
    // An empty source in an existing settings file means "all system audio";
    // without a settings file nothing is chosen yet (first run).
    if (FileExists(settings_path_)) {
        source_id_ = settings_.source_executable.empty() ? std::wstring(kSystemSource)
                                                         : Lower(settings_.source_executable);
    }
    RefreshMicrophones();
    RefreshSources();
    RefreshInjector();
    if (source_id_.empty()) {
        // First run: preselect whatever is playing right now.
        const auto playing = std::find_if(sources_.begin(), sources_.end(),
                                          [](const SourceEntry& s) { return !s.system && s.active; });
        if (playing != sources_.end()) {
            SelectSource(playing->id);
        }
    }
}

AppController::~AppController() {
    router_.Stop();
    SaveSettings();
}

// ---------------------------------------------------------------------------
// periodic work
// ---------------------------------------------------------------------------
void AppController::Tick(std::uint64_t now_ms) {
    if (now_ms >= next_mics_ms_) {
        RefreshMicrophones();
        next_mics_ms_ = now_ms + kMicsIntervalMs;
    }
    if (now_ms >= next_sources_ms_) {
        RefreshSources();
        next_sources_ms_ = now_ms + kSourcesIntervalMs;
    }
    if (now_ms >= next_status_ms_) {
        RefreshInjector();
        next_status_ms_ = now_ms + kStatusIntervalMs;
    }
    UpdateRoute(now_ms);
    soundboard_.Tick(now_ms);

    const float peak = router_.routing() ? router_.TakePeak() * settings_.inject_gain : 0.0f;
    level_ = std::max(std::min(peak, 1.0f), level_ * 0.55f);

    if (settings_dirty_ && now_ms >= next_save_ms_) {
        SaveSettings();
    }
}

void AppController::RefreshNow() {
    RefreshMicrophones();
    RefreshSources();
    RefreshInjector();
    next_retry_ms_ = 0;
    UpdateRoute(::GetTickCount64());
}

void AppController::RefreshSources() {
    const std::vector<core::AudioSource> audio = core::EnumerateAudioSources();
    const std::vector<core::ProcessEntry> processes = core::SnapshotProcesses();

    std::vector<SourceEntry> apps;
    for (const core::AudioSource& session : audio) {
        const std::wstring id = Lower(session.executable);
        const auto existing = std::find_if(apps.begin(), apps.end(),
                                           [&](const SourceEntry& e) { return e.id == id; });
        if (existing == apps.end()) {
            SourceEntry entry;
            entry.id = id;
            entry.executable = session.executable;
            entry.image_path = session.image_path;
            entry.name = CachedFriendlyName(session.image_path, session.executable);
            entry.root_pid = core::FindAppRoot(processes, session.process_id);
            entry.running = true;
            entry.active = session.active;
            apps.push_back(std::move(entry));
        } else if (session.active && !existing->active) {
            existing->active = true;
            existing->root_pid = core::FindAppRoot(processes, session.process_id);
        }
    }

    // Apps only have an audio session while (or after) they play, and a paused
    // app loses it when Windows Audio restarts. So: apps seen earlier stay
    // listed while they still run, and the chosen app is always listed -
    // running or not - so the route can attach as soon as it starts.
    std::vector<std::wstring> extra_ids;
    for (const auto& [id, seen] : seen_) {
        extra_ids.push_back(id);
    }
    if (!source_id_.empty() && source_id_ != kSystemSource && !seen_.contains(source_id_)) {
        extra_ids.push_back(source_id_);
    }
    for (const std::wstring& id : extra_ids) {
        if (std::any_of(apps.begin(), apps.end(), [&](const SourceEntry& e) { return e.id == id; })) {
            continue;
        }
        const auto seen = seen_.find(id);
        SourceEntry entry;
        entry.id = id;
        if (seen != seen_.end()) {
            entry.executable = seen->second.executable;
        } else {
            entry.executable = settings_.source_executable.empty() ? id : settings_.source_executable;
        }
        entry.root_pid = core::FindAppRootByExecutable(processes, entry.executable);
        entry.running = entry.root_pid != 0;
        if (!entry.running && id != source_id_) {
            continue;  // closed, and not the chosen one: drop from the list
        }
        if (entry.running) {
            entry.image_path = core::ProcessImagePath(entry.root_pid);
        }
        if (entry.image_path.empty() && seen != seen_.end()) {
            entry.image_path = seen->second.image_path;
        }
        entry.name = CachedFriendlyName(entry.image_path, entry.executable);
        apps.push_back(std::move(entry));
    }

    for (const SourceEntry& entry : apps) {
        if (!entry.image_path.empty()) {
            seen_[entry.id] = {entry.executable, entry.image_path};
        }
    }
    std::stable_sort(apps.begin(), apps.end(), [](const SourceEntry& a, const SourceEntry& b) {
        if (a.active != b.active) {
            return a.active;
        }
        return ::CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });

    SourceEntry system;
    system.id = kSystemSource;
    system.name = L"All system audio";
    system.running = true;
    system.active = true;
    system.system = true;
    apps.push_back(std::move(system));
    sources_ = std::move(apps);
}

void AppController::RefreshMicrophones() {
    microphones_ = core::EnumerateCaptureEndpoints();
    const auto has = [&](const std::wstring& id) {
        return !id.empty() && std::any_of(microphones_.begin(), microphones_.end(),
                                          [&](const core::AudioEndpoint& m) { return m.id == id; });
    };
    if (has(settings_.microphone_id)) {
        mic_id_ = settings_.microphone_id;
        return;
    }
    if (has(mic_id_)) {
        return;
    }
    // Default capture device, else the first one.
    const auto by_default = std::find_if(microphones_.begin(), microphones_.end(),
                                         [](const core::AudioEndpoint& m) { return m.is_default; });
    mic_id_ = by_default != microphones_.end() ? by_default->id
              : microphones_.empty()           ? std::wstring{}
                                               : microphones_.front().id;
}

void AppController::RefreshInjector() {
    if (mic_id_.empty()) {
        injector_ = {};
        mic_muted_ = false;
        return;
    }
    injector_ = core::QueryInjectorStatus(mic_id_, router_.ConsumerStatus());
    mic_muted_ = core::IsCaptureEndpointMuted(mic_id_).value_or(false);
}

// ---------------------------------------------------------------------------
// routing state machine
// ---------------------------------------------------------------------------
void AppController::StopRoute() {
    if (active_target_ || router_.routing()) {
        router_.Stop();
    }
    active_target_.reset();
}

void AppController::UpdateRoute(std::uint64_t now_ms) {
    if (!live_requested_) {
        StopRoute();
        phase_ = LivePhase::Off;
        live_message_.clear();
        return;
    }
    const SourceEntry* source = SelectedSource();
    if (source == nullptr || (!source->system && (!source->running || source->root_pid == 0))) {
        StopRoute();
        phase_ = LivePhase::Waiting;  // attaches as soon as the app runs
        live_message_.clear();
        return;
    }

    const Target desired = source->system ? Target{0, true} : Target{source->root_pid, false};
    const core::RouterStatus status = router_.status();
    if (active_target_ && *active_target_ == desired && status.state == core::RouterState::Routing) {
        phase_ = LivePhase::Live;
        live_message_.clear();
        return;
    }
    if (active_target_ && status.state == core::RouterState::Failed) {
        // The capture stream died (device change, audio service restart).
        log::Warn(L"Capture of {} stopped ({}); reconnecting", source->name,
                  win32::FormatHResult(status.error));
        active_target_.reset();
        next_retry_ms_ = now_ms + 1000;
        phase_ = LivePhase::Error;
        live_message_ = L"Audio changed, reconnecting";
    }
    if (now_ms < next_retry_ms_) {
        return;
    }

    core::RouteConfig config;
    config.target = desired.system ? core::CaptureTarget::EverythingExcept(::GetCurrentProcessId())
                                   : core::CaptureTarget::Application(desired.pid);
    config.inject_gain = settings_.inject_gain;
    config.mute_microphone = settings_.mute_microphone;
    const HRESULT hr = router_.Start(config);
    if (SUCCEEDED(hr)) {
        active_target_ = desired;
        phase_ = LivePhase::Live;
        live_message_.clear();
        log::Info(L"Live: {} (PID {})", source->name, desired.pid);
    } else {
        active_target_.reset();
        phase_ = LivePhase::Error;
        live_message_ = DescribeRouteError(hr, source->name);
        next_retry_ms_ = now_ms + kRetryDelayMs;
    }
}

// ---------------------------------------------------------------------------
// user actions
// ---------------------------------------------------------------------------
void AppController::SelectSource(const std::wstring& id) {
    const std::wstring normalized = Lower(id);
    if (normalized.empty()) {
        return;
    }
    const auto entry = std::find_if(sources_.begin(), sources_.end(),
                                    [&](const SourceEntry& e) { return e.id == normalized; });
    source_id_ = normalized;
    if (normalized == kSystemSource) {
        settings_.source_executable.clear();
    } else {
        settings_.source_executable = entry != sources_.end() ? entry->executable : normalized;
    }
    settings_dirty_ = true;
    next_save_ms_ = ::GetTickCount64() + kSaveDelayMs;
    if (entry == sources_.end()) {
        RefreshSources();  // e.g. --source for an app that is not running yet
    }
    next_retry_ms_ = 0;
    UpdateRoute(::GetTickCount64());
}

void AppController::SetLive(bool on) {
    live_requested_ = on;
    next_retry_ms_ = 0;
    level_ = 0.0f;
    UpdateRoute(::GetTickCount64());
}

void AppController::SetInjectGain(float gain) {
    settings_.inject_gain = std::clamp(gain, 0.0f, kMaxInjectGain);
    router_.SetGains(settings_.inject_gain, settings_.mute_microphone);
    settings_dirty_ = true;
    next_save_ms_ = ::GetTickCount64() + kSaveDelayMs;
}

void AppController::SetMuteMicrophone(bool mute) {
    settings_.mute_microphone = mute;
    router_.SetGains(settings_.inject_gain, settings_.mute_microphone);
    settings_dirty_ = true;
    next_save_ms_ = ::GetTickCount64() + kSaveDelayMs;
}

void AppController::SelectMicrophone(const std::wstring& id) {
    if (std::none_of(microphones_.begin(), microphones_.end(),
                     [&](const core::AudioEndpoint& m) { return m.id == id; })) {
        return;
    }
    mic_id_ = id;
    settings_.microphone_id = id;
    settings_dirty_ = true;
    next_save_ms_ = ::GetTickCount64() + kSaveDelayMs;
    RefreshInjector();
}

void AppController::SetSetupProgress(bool busy, std::wstring message, bool error) {
    setup_busy_ = busy;
    setup_message_ = std::move(message);
    setup_error_ = error;
}

// ---------------------------------------------------------------------------
// queries
// ---------------------------------------------------------------------------
const SourceEntry* AppController::SelectedSource() const {
    const auto it = std::find_if(sources_.begin(), sources_.end(),
                                 [&](const SourceEntry& e) { return e.id == source_id_; });
    return it == sources_.end() ? nullptr : &*it;
}

std::wstring AppController::selected_source_name() const {
    const SourceEntry* source = SelectedSource();
    return source != nullptr ? source->name : std::wstring{};
}

std::wstring AppController::selected_microphone_name() const {
    const auto it = std::find_if(microphones_.begin(), microphones_.end(),
                                 [&](const core::AudioEndpoint& m) { return m.id == mic_id_; });
    return it == microphones_.end() ? std::wstring{} : it->name;
}

std::wstring AppController::CachedFriendlyName(const std::wstring& image_path,
                                               const std::wstring& executable) {
    if (image_path.empty()) {
        return core::StripExeExtension(executable);
    }
    const auto it = names_.find(image_path);
    if (it != names_.end()) {
        return it->second;
    }
    std::wstring name = core::FriendlyAppName(image_path, executable);
    names_.emplace(image_path, name);
    return name;
}

bool AppController::mic_ready() const noexcept {
    return injector_.endpoint_valid && injector_.install_state != installer::InstallState::NotInstalled &&
           injector_.ring_available;
}

UiState AppController::BuildUiState() const {
    UiState s;
    const bool installed = injector_.endpoint_valid &&
                           injector_.install_state != installer::InstallState::NotInstalled;
    s.installed = installed && injector_.ring_available;
    s.setup_view = !installed || microphones_.empty();
    s.setup_busy = setup_busy_;
    s.setup_message = U8(setup_message_);
    s.setup_error = setup_error_;

    for (const core::AudioEndpoint& mic : microphones_) {
        s.mics.push_back({U8(mic.id), U8(mic.name), mic.is_default});
    }
    s.mic_id = U8(mic_id_);
    s.mic_muted_in_windows = mic_muted_;
    s.enhancements_off = installed && injector_.effects_disabled;
    s.needs_update = installed && installer::NeedsUpgrade(injector_.install_state);
    s.mic_in_use = injector_.apo_alive;

    for (const SourceEntry& e : sources_) {
        s.sources.push_back({U8(e.id), U8(e.name), e.image_path.empty() ? std::string{} : U8(e.id),
                             e.running, e.active, e.system});
    }
    s.source_id = U8(source_id_);

    s.live_on = live_requested_;
    s.live_phase = live_requested_ ? phase_ : LivePhase::Off;
    s.live_message = U8(live_message_);
    s.level = phase_ == LivePhase::Live ? level_ : 0.0f;
    s.gain = settings_.inject_gain;
    s.mute_mic = settings_.mute_microphone;
    return s;
}

void AppController::SaveSettings() {
    settings_dirty_ = false;
    if (FAILED(core::SaveSettings(settings_path_, settings_))) {
        log::Warn(L"Could not save settings to {}", settings_path_);
    }
}

}  // namespace somike::app
