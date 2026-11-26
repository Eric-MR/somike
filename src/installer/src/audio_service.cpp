#include <windows.h>

#include "somike/common/hresult.h"
#include "somike/installer/system_install.h"

namespace somike::installer {

namespace {

class ServiceHandle {
public:
    explicit ServiceHandle(SC_HANDLE handle) noexcept : handle_(handle) {}
    ~ServiceHandle() {
        if (handle_ != nullptr) {
            ::CloseServiceHandle(handle_);
        }
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;

    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    SC_HANDLE handle_;
};

HRESULT WaitForState(SC_HANDLE service, DWORD wanted, DWORD timeout_ms) {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    for (;;) {
        SERVICE_STATUS status{};
        if (!::QueryServiceStatus(service, &status)) {
            return win32::LastErrorAsHResult();
        }
        if (status.dwCurrentState == wanted) {
            return S_OK;
        }
        if (::GetTickCount64() > deadline) {
            return HRESULT_FROM_WIN32(ERROR_SERVICE_REQUEST_TIMEOUT);
        }
        ::Sleep(200);
    }
}

}  // namespace

HRESULT RestartAudioService(Report& report) {
    ServiceHandle manager(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return win32::LastErrorAsHResult();
    }
    ServiceHandle service(::OpenServiceW(manager.get(), L"Audiosrv",
                                         SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS));
    if (!service) {
        return win32::LastErrorAsHResult();
    }

    SERVICE_STATUS status{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
            if (error == ERROR_DEPENDENT_SERVICES_RUNNING) {
                report.emplace_back(
                    L"Other services depend on Windows Audio; please reboot instead.");
            }
            return HRESULT_FROM_WIN32(error);
        }
    }
    if (const HRESULT hr = WaitForState(service.get(), SERVICE_STOPPED, 20000); FAILED(hr)) {
        return hr;
    }
    if (!::StartServiceW(service.get(), 0, nullptr) &&
        ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        return win32::LastErrorAsHResult();
    }
    if (const HRESULT hr = WaitForState(service.get(), SERVICE_RUNNING, 20000); FAILED(hr)) {
        return hr;
    }
    report.emplace_back(L"Windows Audio service restarted.");
    return S_OK;
}

}  // namespace somike::installer
