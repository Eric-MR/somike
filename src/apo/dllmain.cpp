// COM entry points of somike_apo.dll. Registration is deliberately not done
// here (no DllRegisterServer): somike-apoctl.exe owns all registry changes so
// that install, backup and restore live in one place.
#include <windows.h>
#include <combaseapi.h>

#include <new>

#include "class_factory.h"
#include "module.h"
#include "somike/common/guid.h"
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
