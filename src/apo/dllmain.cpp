// COM entry points of somike_apo.dll. Registration is deliberately not done
// here (no DllRegisterServer): somike-apoctl.exe owns all registry changes so
// that install, backup and restore live in one place.
#include <windows.h>
#include <combaseapi.h>

#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

#include "class_factory.h"
#include "module.h"
#include "somike/common/guid.h"
#include "somike/common/log.h"
#include "somike/common/ring_path.h"
#include "somike/protocol/apo_identity.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID clsid, _In_ REFIID riid,
                                        _Outptr_ LPVOID* object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (!IsEqualCLSID(clsid, somike::guid::FromParts(somike::protocol::kApoClsid))) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    // The installer pre-creates the log with audio-service write access; if it
    // is missing, logging silently stays OutputDebugString-only.
    static std::once_flag log_once;
    try {
        std::call_once(log_once, [] {
            // File logging only inside the audio engine (not e.g. unit tests).
            wchar_t host[MAX_PATH];
            const DWORD host_length = ::GetModuleFileNameW(nullptr, host, MAX_PATH);
            const std::wstring_view host_path(host, host_length);
            constexpr std::wstring_view kAudioEngine = L"\\audiodg.exe";
            if (host_path.size() < kAudioEngine.size() ||
                _wcsicmp(host_path.substr(host_path.size() - kAudioEngine.size()).data(),
                         kAudioEngine.data()) != 0) {
                return;
            }
            somike::log::SetDebugOutput(false);
            // A few lines per stream; stop appending once the log reaches 1 MB
            // (the installer truncates it on every install).
            constexpr ULONGLONG kMaxLogBytes = 1u << 20;
            WIN32_FILE_ATTRIBUTE_DATA info{};
            const std::wstring log_path = somike::paths::ApoLogFile();
            if (::GetFileAttributesExW(log_path.c_str(), GetFileExInfoStandard, &info) &&
                ((static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) >= kMaxLogBytes) {
                return;
            }
            if (!somike::log::SetFile(log_path)) {
                const DWORD error = ::GetLastError();
                // Fallback: the audio engine's own temp directory, e.g.
                // C:\Windows\ServiceProfiles\LocalService\AppData\Local\Temp.
                wchar_t temp[MAX_PATH + 1];
                const DWORD length = ::GetTempPathW(MAX_PATH + 1, temp);
                if (length > 0 && length <= MAX_PATH) {
                    somike::log::SetFile(std::wstring(temp, length) + L"somike-apo.log");
                }
                somike::log::Warn(L"cannot open {} (error {})", somike::paths::ApoLogFile(), error);
            }
            somike::log::Info(L"APO module loaded in process {}", ::GetCurrentProcessId());
        });
    } catch (...) {
    }
    auto* factory = new (std::nothrow) somike::apo::ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, object);
    factory->Release();
    return hr;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
    return somike::apo::ModuleCanUnload() ? S_OK : S_FALSE;
}
