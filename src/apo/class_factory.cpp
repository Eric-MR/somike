#include "class_factory.h"

#include <new>

#include "injector_apo.h"
#include "module.h"
#include "somike/common/guid.h"
#include "somike/common/log.h"

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
    // COM aggregation rule: an aggregated object must be asked for IUnknown
    // (it returns its non-delegating unknown). The audio engine aggregates
    // every APO it creates.
    if (outer != nullptr && riid != __uuidof(IUnknown)) {
        log::Warn(L"APO: aggregated CreateInstance for {} refused", guid::ToString(riid));
        return CLASS_E_NOAGGREGATION;
    }
    auto* apo = new (std::nothrow) InjectorApo(outer);
    if (apo == nullptr) {
        return E_OUTOFMEMORY;
    }
    IUnknown* inner = apo->inner();
    const HRESULT hr = inner->QueryInterface(riid, object);
    inner->Release();  // drop the constructor reference; QI holds its own
    if (FAILED(hr)) {
        log::Warn(L"APO: CreateInstance({}) failed: 0x{:08X}", guid::ToString(riid),
                  static_cast<unsigned>(hr));
    }
    return hr;
}

HRESULT ClassFactory::LockServer(BOOL lock) noexcept {
    ModuleLock(lock != FALSE);
    return S_OK;
}

}  // namespace somike::apo
