#include "somike/core/audio_sessions.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <iterator>
#include <map>

#include "somike/common/unique_handle.h"

namespace somike::core {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring ExecutableName(DWORD pid) {
    win32::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) {
        return {};
    }
    wchar_t path[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(path));
    if (!::QueryFullProcessImageNameW(process.get(), 0, path, &size)) {
        return {};
    }
    std::wstring full(path, size);
    const auto slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

void CollectSessions(IMMDevice* device, DWORD own_pid, std::map<DWORD, AudioSource>& sources) {
    ComPtr<IAudioSessionManager2> manager;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(manager.GetAddressOf())))) {
        return;
    }
    ComPtr<IAudioSessionEnumerator> sessions;
    if (FAILED(manager->GetSessionEnumerator(&sessions))) {
        return;
    }
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(sessions->GetSession(i, &control)) || FAILED(control.As(&control2))) {
            continue;
        }
        if (control2->IsSystemSoundsSession() == S_OK) {
            continue;
        }
        DWORD pid = 0;
        // AUDCLNT_S_NO_SINGLE_PROCESS (a success code) means "several processes".
        if (control2->GetProcessId(&pid) != S_OK || pid == 0 || pid == own_pid) {
            continue;
        }

        AudioSessionState state = AudioSessionStateInactive;
        control->GetState(&state);

        AudioSource& source = sources[pid];
        source.process_id = pid;
        source.active = source.active || state == AudioSessionStateActive;
        if (source.display_name.empty()) {
            LPWSTR name = nullptr;
            if (SUCCEEDED(control->GetDisplayName(&name)) && name != nullptr) {
                // "@%SystemRoot%\..." are resource references, not names.
                if (name[0] != L'\0' && name[0] != L'@') {
                    source.display_name = name;
                }
                ::CoTaskMemFree(name);
            }
        }
    }
}

}  // namespace

std::vector<AudioSource> EnumerateAudioSources() {
    std::map<DWORD, AudioSource> by_pid;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator)))) {
        return {};
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        return {};
    }
    const DWORD own_pid = ::GetCurrentProcessId();
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device))) {
            CollectSessions(device.Get(), own_pid, by_pid);
        }
    }

    std::vector<AudioSource> result;
    for (auto& [pid, source] : by_pid) {
        source.executable = ExecutableName(pid);
        if (source.executable.empty()) {
            continue;  // process exited or is not accessible
        }
        result.push_back(std::move(source));
    }
    std::sort(result.begin(), result.end(), [](const AudioSource& a, const AudioSource& b) {
        if (a.active != b.active) {
            return a.active;
        }
        return ::CompareStringOrdinal(a.executable.c_str(), -1, b.executable.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    return result;
}

}  // namespace somike::core
