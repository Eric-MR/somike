#include "class_factory.h"

#include <new>

#include "injector_apo.h"
#include "module.h"

namespace somike::apo {

ClassFactory::ClassFactory() noexcept { ModuleAddRef(); }

ClassFactory::~ClassFactory() { ModuleRelease(); }

HRESULT ClassFactory::QueryInterface(REFIID riid, void** object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ClassFactory::AddRef() noexcept { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }

ULONG ClassFactory::Release() noexcept {
    const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT ClassFactory::CreateInstance(IUnknown* outer, REFIID riid, void** object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (outer != nullptr) {
        return CLASS_E_NOAGGREGATION;
    }
    auto* apo = new (std::nothrow) InjectorApo();
    if (apo == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = apo->QueryInterface(riid, object);
    apo->Release();  // drop the constructor reference; QI holds its own
    return hr;
}

HRESULT ClassFactory::LockServer(BOOL lock) noexcept {
    ModuleLock(lock != FALSE);
    return S_OK;
}

}  // namespace somike::apo
