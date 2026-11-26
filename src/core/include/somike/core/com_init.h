#pragma once

#include <windows.h>
#include <objbase.h>

namespace somike::core {

// Scoped CoInitializeEx/CoUninitialize for the current thread.
class ComInit {
public:
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) noexcept
        : hr_(::CoInitializeEx(nullptr, model)) {}
    ~ComInit() {
        if (SUCCEEDED(hr_)) {
            ::CoUninitialize();
        }
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

    // RPC_E_CHANGED_MODE means COM is already initialised differently on this
    // thread - still usable, we just must not uninitialise it.
    [[nodiscard]] bool usable() const noexcept { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }
    [[nodiscard]] HRESULT result() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

}  // namespace somike::core
