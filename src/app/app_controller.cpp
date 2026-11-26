#include "app_controller.h"

#include <algorithm>
#include <format>

#include "somike/common/log.h"
#include "somike/common/strings.h"
#include "somike/core/audio_sessions.h"

namespace somike::app {

namespace {

constexpr float kMaxInjectGain = 2.0f;

}  // namespace

AppController::AppController(std::wstring settings_path)
    : settings_path_(std::move(settings_path)), settings_(core::LoadSettings(settings_path_)) {
    RefreshSources();
    RefreshMicrophones();
}

AppController::~AppController() {
    router_.Stop();
    SaveSettings();
}

void AppController::RefreshSources() {
    std::vector<SourceItem> items;
    items.push_back({L"All system audio (everything you hear)", L"", 0, true});

    for (const core::AudioSource& source : core::EnumerateAudioSources()) {
        SourceItem item;
        item.executable = source.executable;
        item.process_id = source.process_id;
        item.label = source.executable;
        if (!source.display_name.empty() &&
            !text::EqualsIgnoreCase(source.display_name, source.executable)) {
            item.label += L" - " + source.display_name;
        }
        item.label += std::format(L"  (PID {}{})", source.process_id,
                                  source.active ? L", playing" : L"");
        items.push_back(std::move(item));
    }

    // Keep the remembered app selected even if it is not running right now,
    // instead of silently falling back to "all system audio".
    int selected = 0;
    if (!settings_.source_executable.empty()) {
        const auto it = std::find_if(items.begin(), items.end(), [&](const SourceItem& item) {
            return !item.system_wide && text::EqualsIgnoreCase(item.executable, settings_.source_executable);
        });
        if (it != items.end()) {
            selected = static_cast<int>(it - items.begin());
        } else {
            items.push_back({settings_.source_executable + L"  (not running)",
                             settings_.source_executable, 0, false});
            selected = static_cast<int>(items.size()) - 1;
        }
    }
    sources_ = std::move(items);
    selected_source_ = selected;
}

void AppController::SelectSource(int index) {
    if (index < 0 || index >= static_cast<int>(sources_.size())) {
        return;
    }
    selected_source_ = index;
    const SourceItem& item = sources_[static_cast<std::size_t>(index)];
    settings_.source_executable = item.system_wide ? std::wstring{} : item.executable;

    if (router_.routing()) {
        // Switch the live route to the new source; if that is impossible
        // (e.g. the app is not running), stop rather than keep routing the
        // previous source behind the user's back.
        if (FAILED(StartRouting())) {
            log::Warn(L"Re-routing to {} failed; routing stopped", item.label);
            router_.Stop();
        }
    }
}

void AppController::RefreshMicrophones() {
    microphones_ = core::EnumerateCaptureEndpoints();
    selected_microphone_ = -1;
    for (std::size_t i = 0; i < microphones_.size(); ++i) {
        const bool remembered = !settings_.microphone_id.empty() &&
                                text::EqualsIgnoreCase(microphones_[i].id, settings_.microphone_id);
        if (remembered) {
            selected_microphone_ = static_cast<int>(i);
            break;
        }
        if (microphones_[i].is_default && selected_microphone_ < 0) {
            selected_microphone_ = static_cast<int>(i);
        }
    }
    if (selected_microphone_ < 0 && !microphones_.empty()) {
        selected_microphone_ = 0;
    }
}

void AppController::SelectMicrophone(int index) {
    if (index < 0 || index >= static_cast<int>(microphones_.size())) {
        return;
    }
    selected_microphone_ = index;
    settings_.microphone_id = microphones_[static_cast<std::size_t>(index)].id;
}

std::wstring AppController::selected_microphone_id() const {
    if (selected_microphone_ < 0 || selected_microphone_ >= static_cast<int>(microphones_.size())) {
        return {};
    }
    return microphones_[static_cast<std::size_t>(selected_microphone_)].id;
}

std::wstring AppController::selected_microphone_name() const {
    if (selected_microphone_ < 0 || selected_microphone_ >= static_cast<int>(microphones_.size())) {
        return {};
    }
    return microphones_[static_cast<std::size_t>(selected_microphone_)].name;
}

void AppController::SetInjectGain(float gain) {
    settings_.inject_gain = std::clamp(gain, 0.0f, kMaxInjectGain);
    router_.SetGains(settings_.inject_gain, settings_.mute_microphone);
}

void AppController::SetMuteMicrophone(bool mute) {
    settings_.mute_microphone = mute;
    router_.SetGains(settings_.inject_gain, settings_.mute_microphone);
}

HRESULT AppController::StartRouting() {
    if (selected_source_ < 0 || selected_source_ >= static_cast<int>(sources_.size())) {
        return E_INVALIDARG;
    }
    const SourceItem& source = sources_[static_cast<std::size_t>(selected_source_)];
    if (!source.system_wide && source.process_id == 0) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);  // remembered app is not running
    }

    core::RouteConfig config;
    config.target = source.system_wide
                        ? core::CaptureTarget::EverythingExcept(::GetCurrentProcessId())
                        : core::CaptureTarget::Application(source.process_id);
    config.inject_gain = settings_.inject_gain;
    config.mute_microphone = settings_.mute_microphone;
    return router_.Start(config);
}

void AppController::StopRouting() { router_.Stop(); }

std::wstring AppController::routing_label() const {
    if (selected_source_ < 0 || selected_source_ >= static_cast<int>(sources_.size())) {
        return {};
    }
    const SourceItem& source = sources_[static_cast<std::size_t>(selected_source_)];
    return source.system_wide ? L"all system audio" : source.executable;
}

core::InjectorStatus AppController::QueryInjectorStatus() {
    return core::QueryInjectorStatus(selected_microphone_id(), router_.ConsumerStatus());
}

void AppController::SaveSettings() {
    if (FAILED(core::SaveSettings(settings_path_, settings_))) {
        log::Warn(L"Could not save settings to {}", settings_path_);
    }
}

}  // namespace somike::app
