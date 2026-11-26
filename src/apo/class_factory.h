#pragma once

#include <windows.h>
#include <unknwn.h>

#include <atomic>

namespace somike::apo {

// Creates InjectorApo instances for DllGetClassObject.
class ClassFactory final : public IClassFactory {
public:
    ClassFactory() noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid,
                                             void** object) noexcept override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) noexcept override;

private:
    ~ClassFactory();

    std::atomic<ULONG> refs_{1};
};

}  // namespace somike::apo
